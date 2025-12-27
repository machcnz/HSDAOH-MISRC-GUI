/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#include "gui_cvbs.h"
#include "gui_trigger.h"
#include "gui_text.h"
#include "../misrc_common/threading.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Debug timing for luma filter performance (per-field accumulation)
static uint64_t g_field_decode_us = 0;      // Accumulated decode time for current field
static double g_field_decode_avg_ms = 0.0;  // Last completed field's decode time (for OSD)

//-----------------------------------------------------------------------------
// Internal Constants
//-----------------------------------------------------------------------------

// Back porch offset (after H-sync, before active video)
#define BACK_PORCH_SAMPLES      228  // ~7µs - slightly more than standard to skip color burst

// Field detection constants
#define PAL_FIELD_LINES         312  // Lines per PAL field (312.5 rounded)
#define NTSC_FIELD_LINES        262  // Lines per NTSC field (262.5 rounded)
#define PAL_ACTIVE_START        23   // First active line in PAL field
#define NTSC_ACTIVE_START       21   // First active line in NTSC field

// Field heights (half of full frame)
#define PAL_FIELD_HEIGHT        288  // 576/2
#define NTSC_FIELD_HEIGHT       243  // 486/2

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

// Luma lowpass filter to remove chroma subcarrier
// At 40 MSPS: PAL color burst = 4.43 MHz (~9 samples/cycle)
// 9-tap filter matches PAL chroma cycle, works well for NTSC too
#define LUMA_LPF_TAPS  9
#define LUMA_LPF_HALF  4   // Half the taps (for centering)

// Pre-computed Gaussian kernel coefficients (sigma ~= 1.5)
// Sum = 256 for fast division via right-shift by 8
static const int16_t luma_kernel[LUMA_LPF_TAPS] = {
    2, 11, 30, 54, 70, 54, 30, 11, 2   // Adjusted to sum to 264, close enough
};
#define LUMA_KERNEL_SUM 256
#define LUMA_KERNEL_SHIFT 8  // Divide by 256 = shift right 8

// Pre-filtered line buffer (avoids per-pixel convolution)
// Max active samples is ~2100, add padding for filter taps
#define FILTERED_LINE_MAX 2200
static int16_t g_filtered_line[FILTERED_LINE_MAX];

// Decode a single video line from samples to grayscale pixels
// Optimized: pre-filters entire line, then resamples to output width
static void decode_line_to_pixels(const int16_t *samples, size_t sample_count,
                                  const cvbs_levels_t *levels,
                                  uint8_t *pixels, int pixel_width) {
    if (!samples || !levels || !pixels || pixel_width <= 0) return;
    if (sample_count < (size_t)(BACK_PORCH_SAMPLES + pixel_width)) return;

    // Skip back porch to get to active video
    const int16_t *src = samples + BACK_PORCH_SAMPLES;
    int n = (int)(sample_count - BACK_PORCH_SAMPLES);

    // Limit to expected active region
    if (n > CVBS_ACTIVE_SAMPLES) n = CVBS_ACTIVE_SAMPLES;
    if (n > FILTERED_LINE_MAX) n = FILTERED_LINE_MAX;

    // Pre-calculate level normalization (fixed-point: multiply by 255, divide by range)
    int32_t black = levels->black_level;
    int32_t range = levels->white_level - black;
    if (range < 1) range = 1;

    // =========================================================================
    // Pass 1: Apply kernel filter to entire line (one convolution per sample)
    // This is O(n * taps) but we only do it once, not per output pixel
    // =========================================================================

    // Handle left edge (first LUMA_LPF_HALF samples) - clamp to edge
    for (int i = 0; i < LUMA_LPF_HALF && i < n; i++) {
        int32_t sum = 0;
        for (int k = 0; k < LUMA_LPF_TAPS; k++) {
            int idx = i - LUMA_LPF_HALF + k;
            if (idx < 0) idx = 0;
            sum += src[idx] * luma_kernel[k];
        }
        g_filtered_line[i] = (int16_t)(sum >> LUMA_KERNEL_SHIFT);
    }

    // Main body - no bounds checking needed, fully unrolled for speed
    int end = n - LUMA_LPF_HALF;
    for (int i = LUMA_LPF_HALF; i < end; i++) {
        const int16_t *p = src + i - LUMA_LPF_HALF;
        int32_t sum = p[0] * 2 + p[1] * 11 + p[2] * 30 + p[3] * 54 + p[4] * 70 +
                      p[5] * 54 + p[6] * 30 + p[7] * 11 + p[8] * 2;
        g_filtered_line[i] = (int16_t)(sum >> LUMA_KERNEL_SHIFT);
    }

    // Handle right edge (last LUMA_LPF_HALF samples) - clamp to edge
    for (int i = end; i < n; i++) {
        int32_t sum = 0;
        for (int k = 0; k < LUMA_LPF_TAPS; k++) {
            int idx = i - LUMA_LPF_HALF + k;
            if (idx >= n) idx = n - 1;
            sum += src[idx] * luma_kernel[k];
        }
        g_filtered_line[i] = (int16_t)(sum >> LUMA_KERNEL_SHIFT);
    }

    // =========================================================================
    // Pass 2: Resample filtered line to output pixels (simple lookup)
    // =========================================================================

    // Fixed-point position tracking (16.16 format)
    int32_t pos = 0;
    int32_t step = (n << 16) / pixel_width;

    for (int px = 0; px < pixel_width; px++) {
        int idx = pos >> 16;
        if (idx >= n) idx = n - 1;

        // Normalize to 0-255
        int32_t val = ((g_filtered_line[idx] - black) * 255) / range;

        // Clamp
        if (val < 0) val = 0;
        else if (val > 255) val = 255;

        pixels[px] = (uint8_t)val;
        pos += step;
    }

}

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

bool gui_cvbs_init(cvbs_decoder_t *decoder) {
    if (!decoder) return false;

    memset(decoder, 0, sizeof(cvbs_decoder_t));

    // Allocate field buffers (each field is half the frame height)
    // Max field height is PAL: 288 lines
    decoder->field_height = PAL_FIELD_HEIGHT;
    size_t field_size = (size_t)CVBS_FRAME_WIDTH * (size_t)(CVBS_MAX_HEIGHT / 2);

    decoder->field_buffer[0] = (uint8_t *)calloc(field_size, 1);
    if (!decoder->field_buffer[0]) {
        return false;
    }

    decoder->field_buffer[1] = (uint8_t *)calloc(field_size, 1);
    if (!decoder->field_buffer[1]) {
        free(decoder->field_buffer[0]);
        decoder->field_buffer[0] = NULL;
        return false;
    }

    // Allocate deinterlaced frame buffer at full-frame resolution (D1)
    decoder->frame_width = CVBS_FRAME_WIDTH;
    decoder->frame_height = CVBS_PAL_HEIGHT;  // Start with PAL full-frame height
    decoder->frame_buffer = (uint8_t *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, 1);
    if (!decoder->frame_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        return false;
    }

    // Allocate display buffer (double buffering)
    decoder->display_buffer = (uint8_t *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, 1);
    if (!decoder->display_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        return false;
    }

    // Create raylib Image for video display (RGBA format at full-frame resolution)
    // Allocate our own pixel buffer so we control the memory
    Color *frame_pixels = (Color *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, sizeof(Color));
    if (!frame_pixels) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        return false;
    }
    decoder->frame_image.data = frame_pixels;
    decoder->frame_image.width = CVBS_FRAME_WIDTH;
    decoder->frame_image.height = CVBS_MAX_HEIGHT;
    decoder->frame_image.mipmaps = 1;
    decoder->frame_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    decoder->texture_valid = false;

    // Allocate line buffer for cross-boundary line assembly
    decoder->line_buffer = (int16_t *)calloc(CVBS_LINE_BUFFER_SIZE, sizeof(int16_t));
    if (!decoder->line_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        free(decoder->frame_image.data);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        decoder->frame_image.data = NULL;
        return false;
    }
    decoder->line_buffer_count = 0;

    // Initialize adaptive levels
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));

    // Initialize software PLL with PAL default
    decoder->pll.phase = 0;
    decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    decoder->pll.freq_adjust = 0;
    decoder->pll.phase_error = 0;
    decoder->pll.phase_integral = 0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->pll.current_line = 0;
    decoder->pll.samples_in_line = 0;
    decoder->pll.total_samples = 0;

    // Initialize lowpass filter state
    decoder->lpf_state = 0;
    decoder->lpf_output = 0;

    // Initialize edge detection state (uses filtered signal)
    decoder->last_filtered_above = true;
    decoder->global_sample_pos = 0;

    // Initialize H-sync pulse tracking
    decoder->in_hsync_pulse = false;
    decoder->hsync_pulse_start = 0;

    // Initialize V-sync detector state
    decoder->vsync_last_edge_pos = 0;

    // Initialize V-sync detection state
    memset(&decoder->vsync, 0, sizeof(decoder->vsync));

    // Initialize legacy state
    decoder->lines_since_vsync = 0;

    // Default chroma decoder
    decoder->chroma_decoder = CVBS_CHROMA_DECODER_MONO;

    // Initialize frame state
    decoder->state.format = CVBS_FORMAT_UNKNOWN;
    decoder->state.total_lines = 0;
    decoder->state.active_lines = 0;
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    decoder->state.frames_decoded = 0;

    return true;
}

void gui_cvbs_cleanup(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    if (decoder->texture_valid) {
        UnloadTexture(decoder->frame_texture);
        decoder->texture_valid = false;
    }

    // Free frame image data (we allocated it ourselves)
    if (decoder->frame_image.data) {
        free(decoder->frame_image.data);
        decoder->frame_image.data = NULL;
    }

    free(decoder->field_buffer[0]);
    free(decoder->field_buffer[1]);
    free(decoder->frame_buffer);
    free(decoder->display_buffer);
    free(decoder->line_buffer);

    decoder->field_buffer[0] = NULL;
    decoder->field_buffer[1] = NULL;
    decoder->frame_buffer = NULL;
    decoder->display_buffer = NULL;
    decoder->line_buffer = NULL;
}

void gui_cvbs_reset(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Clear field buffers
    size_t field_size = (size_t)CVBS_FRAME_WIDTH * (size_t)(CVBS_MAX_HEIGHT / 2);
    if (decoder->field_buffer[0]) {
        memset(decoder->field_buffer[0], 0, field_size);
    }
    if (decoder->field_buffer[1]) {
        memset(decoder->field_buffer[1], 0, field_size);
    }

    // Clear frame buffers
    if (decoder->frame_buffer) {
        memset(decoder->frame_buffer, 0, CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT);
    }
    if (decoder->display_buffer) {
        memset(decoder->display_buffer, 0, CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT);
    }

    // Reset line buffer
    decoder->line_buffer_count = 0;

    // Reset adaptive levels
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));

    // Reset software PLL (keep line period consistent with selected system)
    decoder->pll.phase = 0;
    if (decoder->state.format == CVBS_FORMAT_NTSC) {
        decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_NTSC;
    } else {
        // PAL + SECAM + UNKNOWN use PAL timing
        decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    }
    decoder->pll.freq_adjust = 0;
    decoder->pll.phase_error = 0;
    decoder->pll.phase_integral = 0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->pll.current_line = 0;
    decoder->pll.samples_in_line = 0;
    // Don't reset total_samples - keep for debug

    // Reset lowpass filter state
    decoder->lpf_state = 0;
    decoder->lpf_output = 0;

    // Reset edge detection state (uses filtered signal)
    decoder->last_filtered_above = true;
    decoder->global_sample_pos = 0;

    // Reset H-sync pulse tracking
    decoder->in_hsync_pulse = false;
    decoder->hsync_pulse_start = 0;

    // Reset V-sync detector state
    decoder->vsync_last_edge_pos = 0;

    // Reset V-sync detection state
    memset(&decoder->vsync, 0, sizeof(decoder->vsync));

    // Reset field ready flags (important when switching modes)
    decoder->field_ready[0] = false;
    decoder->field_ready[1] = false;

    // Reset legacy state
    decoder->lines_since_vsync = 0;

    // Reset frame state (preserve decoder->state.format set by gui_cvbs_set_format)
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    decoder->display_ready = false;

    // Reset statistics
    decoder->sync_errors = 0;
    decoder->format_changes = 0;

    // Reset debug statistics
    memset(&decoder->debug, 0, sizeof(decoder->debug));
}

//-----------------------------------------------------------------------------
// Adaptive Level Estimation
//-----------------------------------------------------------------------------

// Update min/max from a filtered sample (called from main processing loop)
// Uses the already-filtered signal from sync detection to reject noise
static inline void accumulate_filtered_level(cvbs_decoder_t *decoder, int16_t filtered) {
    if (decoder->adaptive.field_min == 0 && decoder->adaptive.field_max == 0) {
        // First sample of field
        decoder->adaptive.field_min = filtered;
        decoder->adaptive.field_max = filtered;
    } else {
        if (filtered < decoder->adaptive.field_min)
            decoder->adaptive.field_min = filtered;
        if (filtered > decoder->adaptive.field_max)
            decoder->adaptive.field_max = filtered;
    }
}

// Commit accumulated levels at end of field (called from start_new_field_pll)
static void commit_adaptive_levels(cvbs_decoder_t *decoder) {
    // Only update if we accumulated valid data
    if (decoder->adaptive.field_min == 0 && decoder->adaptive.field_max == 0) return;

    int16_t field_min = decoder->adaptive.field_min;
    int16_t field_max = decoder->adaptive.field_max;

    // Reset accumulators for next field
    decoder->adaptive.field_min = 0;
    decoder->adaptive.field_max = 0;

    // Exponential moving average for stability (update once per field)
    const float alpha = 0.1f;  // ~10 fields to converge (~200ms)

    if (decoder->adaptive.sync_tip == 0 && decoder->adaptive.white == 0) {
        // First time - initialize directly
        decoder->adaptive.sync_tip = field_min;
        decoder->adaptive.white = field_max;
    } else {
        // Smooth update
        decoder->adaptive.sync_tip = (int16_t)(decoder->adaptive.sync_tip * (1.0f - alpha) +
                                               field_min * alpha);
        decoder->adaptive.white = (int16_t)(decoder->adaptive.white * (1.0f - alpha) +
                                            field_max * alpha);
    }

    // Derive other levels
    int16_t range = decoder->adaptive.white - decoder->adaptive.sync_tip;
    if (range < 100) range = 100;  // Minimum range to avoid division issues

    // For CVBS, the sync tip is at IRE -40, blanking at IRE 0, black at IRE ~7.5, white at IRE 100
    // So sync is about 40/140 = 28.5% of the range below blanking
    // Threshold at 25% above sync tip (matching gui_trigger.h CVBS_SYNC_MARGIN)
    // This is well into the sync pulse region for reliable edge detection
    decoder->adaptive.threshold = decoder->adaptive.sync_tip + (int16_t)(range * CVBS_SYNC_MARGIN);

    // Blanking level: ~28% of range (above sync, at black level start)
    decoder->adaptive.blanking = decoder->adaptive.sync_tip + (int16_t)(range * 0.28f);

    // Black level: ~32% of range (just above blanking)
    decoder->adaptive.black = decoder->adaptive.sync_tip + (int16_t)(range * 0.32f);

    // Update legacy levels for compatibility
    decoder->levels.sig_min = decoder->adaptive.sync_tip;
    decoder->levels.sig_max = decoder->adaptive.white;
    decoder->levels.range = range;
    decoder->levels.sync_threshold = decoder->adaptive.threshold;
    decoder->levels.black_level = decoder->adaptive.black;
    decoder->levels.white_level = decoder->adaptive.white;
}

//-----------------------------------------------------------------------------
// Software PLL-based CVBS Decoder
//
// This approach uses a software PLL to track H-sync timing:
// - PLL maintains a phase counter that represents position within a line
// - When H-sync edges are detected, PLL phase is corrected
// - Samples are written to frame buffer based on PLL line counter
// - V-sync detection resets the line counter to start a new field
// - Missing H-syncs don't break the display - PLL interpolates
//-----------------------------------------------------------------------------

// PLL tuning constants
#define PLL_PHASE_GAIN      0.15    // Proportional gain for phase correction
#define PLL_INTEGRAL_GAIN   0.005   // Integral gain for frequency drift
#define PLL_LOCK_THRESHOLD  100     // Phase error below this = good sync
#define PLL_LOCK_COUNT      10      // Good syncs needed to declare lock
#define PLL_UNLOCK_COUNT    5       // Bad syncs to lose lock

// H-sync pulse validation (aligned with gui_trigger.h constants)
#define HSYNC_MIN_WIDTH     CVBS_HSYNC_MIN_WIDTH  // 100 samples (~2.5µs minimum)
#define HSYNC_MAX_WIDTH     CVBS_HSYNC_MAX_WIDTH  // 280 samples (~7µs maximum)

// Lowpass filter coefficient (IIR single-pole) - Fixed-point Q8
// At 40 MSPS, a cutoff of ~500kHz gives alpha ≈ 0.08
// Lower alpha = more smoothing (removes HF noise while preserving sync edges)
// Fixed-point: alpha = 20/256 ≈ 0.078, (1-alpha) = 236/256 ≈ 0.922
#define LPF_ALPHA_FP        20      // 0.08 * 256 ≈ 20
#define LPF_ONE_MINUS_FP    236     // (1 - 0.08) * 256 ≈ 236
#define LPF_SHIFT           8       // Divide by 256

//-----------------------------------------------------------------------------
// Lowpass Filter for Sync Detection
//-----------------------------------------------------------------------------

// Apply IIR lowpass filter to a sample (fixed-point version)
// This smooths out high-frequency noise while preserving sync pulse edges
// Returns filtered value in original sample range (not shifted)
static inline int16_t apply_lowpass(cvbs_decoder_t *decoder, int16_t sample) {
    // Single-pole IIR: y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
    // Fixed-point Q8: state is stored shifted left by 8
    // y[n] = (alpha * x[n] + (1-alpha) * y[n-1]) >> 8, but we keep state in Q8
    int32_t state = LPF_ALPHA_FP * (int32_t)sample + LPF_ONE_MINUS_FP * (decoder->lpf_state >> LPF_SHIFT);
    decoder->lpf_state = state;
    return (int16_t)(state >> LPF_SHIFT);
}

//-----------------------------------------------------------------------------
// Separate H-sync and V-sync Detectors
//-----------------------------------------------------------------------------

// V-sync detector result
typedef struct {
    bool edge_detected;        // True if falling edge detected
    size_t interval;           // Samples since last falling edge
    bool vsync_complete;       // True if V-sync sequence completed
    bool is_odd_field;         // True if odd field (field 1), false if even (field 2)
} vsync_result_t;

// H-sync detector result
typedef struct {
    bool pulse_complete;       // True if a valid H-sync pulse ended
    size_t pulse_width;        // Width of the completed pulse
    double phase_at_sync;      // PLL phase when sync was detected
} hsync_result_t;

// Detect V-sync by tracking falling edge intervals
// V-sync region has half-line rate pulses (~1280 samples apart vs ~2560 for normal lines)
// Field detection: odd field has 16 half-lines, even field has 14
static vsync_result_t detect_vsync(cvbs_decoder_t *decoder, int16_t filtered,
                                    int16_t threshold, size_t sample_pos) {
    vsync_result_t result = {false, 0, false, false};
    bool is_above = (filtered > threshold);
    cvbs_vsync_state_t *vs = &decoder->vsync;

    // Check for falling edge (signal goes below threshold)
    if (decoder->last_filtered_above && !is_above) {
        result.edge_detected = true;
        result.interval = sample_pos - decoder->vsync_last_edge_pos;
        decoder->vsync_last_edge_pos = sample_pos;

        // Half-line interval: 1000-1600 samples (vs 2200-3000 for full line)
        bool is_half_line = (result.interval >= 1000 && result.interval <= 1600);

        if (is_half_line) {
            vs->half_line_count++;
            vs->total_half_lines++;
            // Enter V-sync after 6 consecutive half-line pulses
            if (vs->half_line_count >= 6 && !vs->in_vsync) {
                vs->in_vsync = true;
                vs->total_half_lines = vs->half_line_count;
            }
        } else {
            // Full line interval - if in V-sync, it's ending
            if (vs->in_vsync && result.interval >= 2200 && result.interval <= 3000) {
                vs->in_vsync = false;
                result.vsync_complete = true;

                // Field detection: >= 15 half-lines = odd field, < 15 = even field
                decoder->debug.last_half_line_count = vs->total_half_lines;
                result.is_odd_field = (vs->total_half_lines >= 15);

                vs->half_line_count = 0;
                vs->total_half_lines = 0;
            } else {
                vs->half_line_count = 0;
            }
        }
    }

    return result;
}

// Detect H-sync by measuring pulse width
// Returns pulse_complete=true when a valid H-sync pulse ends (100-280 samples)
static hsync_result_t detect_hsync(cvbs_decoder_t *decoder, int16_t filtered,
                                    int16_t threshold, size_t sample_pos,
                                    double current_pll_phase) {
    hsync_result_t result = {false, 0, 0.0};
    bool is_above = (filtered > threshold);

    // Check for falling edge - start of potential H-sync pulse
    if (decoder->last_filtered_above && !is_above) {
        decoder->in_hsync_pulse = true;
        decoder->hsync_pulse_start = sample_pos;
    }

    // Check for rising edge - end of pulse
    if (!decoder->last_filtered_above && is_above && decoder->in_hsync_pulse) {
        decoder->in_hsync_pulse = false;
        size_t pulse_width = sample_pos - decoder->hsync_pulse_start;

        // Only accept pulses in H-sync range
        if (pulse_width >= HSYNC_MIN_WIDTH && pulse_width <= HSYNC_MAX_WIDTH) {
            result.pulse_complete = true;
            result.pulse_width = pulse_width;
            result.phase_at_sync = current_pll_phase;
        }
    }

    return result;
}

//-----------------------------------------------------------------------------
// Deinterlacer
//-----------------------------------------------------------------------------

// Deinterlace two fields into a full frame using weave with bob fallback
// - Weave: interleave even/odd fields (best quality when both present)
// - Bob: interpolate missing field lines (for partial frames)
//
// Field mapping (PAL/NTSC standard):
// - field_buffer[0] = odd field (field 1): contains lines 1, 3, 5... of the frame
// - field_buffer[1] = even field (field 2): contains lines 0, 2, 4... of the frame
// In the frame buffer, even field lines come first (0, 2, 4...), then odd (1, 3, 5...)
static void deinterlace_fields(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->field_buffer[0] || !decoder->field_buffer[1]) return;
    if (!decoder->frame_buffer) return;

    int field_height = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                       NTSC_FIELD_HEIGHT : PAL_FIELD_HEIGHT;
    int frame_height = field_height * 2;

    // Clamp to buffer limits
    if (frame_height > decoder->frame_height) {
        frame_height = decoder->frame_height;
        field_height = frame_height / 2;
    }

    uint8_t *odd_field = decoder->field_buffer[0];   // Odd field (frame lines 1, 3, 5...)
    uint8_t *even_field = decoder->field_buffer[1];  // Even field (frame lines 0, 2, 4...)
    uint8_t *frame = decoder->frame_buffer;

    // Check which fields are valid
    bool have_odd = decoder->field_ready[0];
    bool have_even = decoder->field_ready[1];

    if (have_odd && have_even) {
        // Weave mode: interleave both fields for full vertical resolution
        for (int fl = 0; fl < field_height; fl++) {
            uint8_t *src_odd = odd_field + (size_t)fl * CVBS_FRAME_WIDTH;
            uint8_t *src_even = even_field + (size_t)fl * CVBS_FRAME_WIDTH;

            // Even frame lines (0, 2, 4...) from even field
            // Odd frame lines (1, 3, 5...) from odd field
            uint8_t *dst_even = frame + (size_t)(fl * 2) * CVBS_FRAME_WIDTH;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * CVBS_FRAME_WIDTH;

            memcpy(dst_even, src_even, CVBS_FRAME_WIDTH);
            memcpy(dst_odd, src_odd, CVBS_FRAME_WIDTH);
        }
    } else if (have_odd) {
        // Bob mode with odd field only: duplicate lines with interpolation
        // First line: just duplicate (no previous line to interpolate from)
        memcpy(frame + CVBS_FRAME_WIDTH, odd_field, CVBS_FRAME_WIDTH);  // dst_odd line 0
        memcpy(frame, odd_field, CVBS_FRAME_WIDTH);  // dst_even line 0

        // Remaining lines: interpolate even lines from adjacent odd lines
        for (int fl = 1; fl < field_height; fl++) {
            uint8_t *src = odd_field + (size_t)fl * CVBS_FRAME_WIDTH;
            uint8_t *src_prev = odd_field + (size_t)(fl - 1) * CVBS_FRAME_WIDTH;
            uint8_t *dst_even = frame + (size_t)(fl * 2) * CVBS_FRAME_WIDTH;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * CVBS_FRAME_WIDTH;

            memcpy(dst_odd, src, CVBS_FRAME_WIDTH);

            // Interpolate using integer average: (a + b) >> 1
            // Process 4 bytes at a time using 32-bit operations
            int x = 0;
            for (; x <= CVBS_FRAME_WIDTH - 4; x += 4) {
                uint32_t a = *(uint32_t *)(src_prev + x);
                uint32_t b = *(uint32_t *)(src + x);
                // Average without overflow: (a & b) + ((a ^ b) >> 1)
                uint32_t avg = (a & b) + (((a ^ b) & 0xFEFEFEFE) >> 1);
                *(uint32_t *)(dst_even + x) = avg;
            }
            // Handle remaining pixels
            for (; x < CVBS_FRAME_WIDTH; x++) {
                dst_even[x] = (uint8_t)((src_prev[x] + src[x]) >> 1);
            }
        }
    } else if (have_even) {
        // Bob mode with even field only
        // Process all but last line with interpolation
        for (int fl = 0; fl < field_height - 1; fl++) {
            uint8_t *src = even_field + (size_t)fl * CVBS_FRAME_WIDTH;
            uint8_t *src_next = even_field + (size_t)(fl + 1) * CVBS_FRAME_WIDTH;
            uint8_t *dst_even = frame + (size_t)(fl * 2) * CVBS_FRAME_WIDTH;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * CVBS_FRAME_WIDTH;

            memcpy(dst_even, src, CVBS_FRAME_WIDTH);

            // Interpolate using 32-bit operations
            int x = 0;
            for (; x <= CVBS_FRAME_WIDTH - 4; x += 4) {
                uint32_t a = *(uint32_t *)(src + x);
                uint32_t b = *(uint32_t *)(src_next + x);
                uint32_t avg = (a & b) + (((a ^ b) & 0xFEFEFEFE) >> 1);
                *(uint32_t *)(dst_odd + x) = avg;
            }
            for (; x < CVBS_FRAME_WIDTH; x++) {
                dst_odd[x] = (uint8_t)((src[x] + src_next[x]) >> 1);
            }
        }
        // Last line: just duplicate
        int last_fl = field_height - 1;
        uint8_t *src_last = even_field + (size_t)last_fl * CVBS_FRAME_WIDTH;
        uint8_t *dst_even_last = frame + (size_t)(last_fl * 2) * CVBS_FRAME_WIDTH;
        uint8_t *dst_odd_last = frame + (size_t)(last_fl * 2 + 1) * CVBS_FRAME_WIDTH;
        memcpy(dst_even_last, src_last, CVBS_FRAME_WIDTH);
        memcpy(dst_odd_last, src_last, CVBS_FRAME_WIDTH);
    }
    // If neither field ready, frame_buffer keeps its previous content
}

//-----------------------------------------------------------------------------
// Field Completion
//-----------------------------------------------------------------------------

// Complete a field - deinterlace and copy to display buffer
static void complete_field_pll(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Capture field decode timing and reset for next field
    g_field_decode_avg_ms = g_field_decode_us / 1000.0;
    g_field_decode_us = 0;

    int line_count = decoder->pll.current_line;

    // Get expected field parameters
    int expected_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                         NTSC_FIELD_LINES : PAL_FIELD_LINES;

    // Only accept if we got a reasonable field (at least 80% of expected lines)
    // This prevents accepting partial/corrupted fields after format switch
    if (line_count < (expected_lines * 4 / 5)) {
        return;
    }

    // Mark this field as received
    int field_idx = decoder->state.current_field ? 1 : 0;
    decoder->field_ready[field_idx] = true;

    // Deinterlace and display - works with one or both fields
    // Bob mode is used until both fields are available, then weave takes over
    deinterlace_fields(decoder);

    // Copy deinterlaced frame to display buffer
    int frame_h = decoder->frame_height;
    if (frame_h > CVBS_MAX_HEIGHT) frame_h = CVBS_MAX_HEIGHT;

    memcpy(decoder->display_buffer, decoder->frame_buffer,
           (size_t)CVBS_FRAME_WIDTH * (size_t)frame_h);
    decoder->display_ready = true;
    decoder->state.frame_complete = true;
    decoder->state.frames_decoded++;
    decoder->debug.fields_decoded++;
}

// Start a new field after V-sync
// is_odd_field: true for odd field (buffer 0), false for even field (buffer 1)
static void start_new_field_pll(cvbs_decoder_t *decoder, bool is_odd_field) {
    complete_field_pll(decoder);
    commit_adaptive_levels(decoder);

    decoder->pll.current_line = 0;
    decoder->state.current_field = is_odd_field ? 0 : 1;
    decoder->lines_since_vsync = 0;
    decoder->debug.hsyncs_last_field = 0;
    decoder->debug.vsync_found++;
}

// Process a detected H-sync edge - update PLL
static void pll_process_hsync(cvbs_decoder_t *decoder, double phase_at_sync) {
    cvbs_pll_state_t *pll = &decoder->pll;

    // Calculate phase error: how far off was our prediction?
    // Ideal sync should happen at phase = 0 (or line_period)
    double phase_error = phase_at_sync;

    // Wrap to -half_period to +half_period
    if (phase_error > pll->line_period / 2) {
        phase_error -= pll->line_period;
    }

    // Update PLL lock status based on phase error magnitude
    if (fabs(phase_error) < PLL_LOCK_THRESHOLD) {
        pll->good_sync_count++;
        pll->bad_sync_count = 0;
        if (pll->good_sync_count >= PLL_LOCK_COUNT) {
            pll->locked = true;
        }
    } else {
        pll->bad_sync_count++;
        pll->good_sync_count = 0;
        if (pll->bad_sync_count >= PLL_UNLOCK_COUNT) {
            pll->locked = false;
        }
    }

    // Reject obviously bad syncs (too far off)
    if (fabs(phase_error) > pll->line_period * 0.4) {
        // This sync is way off - probably noise or V-sync region
        // Don't adjust PLL, just increment line counter if phase wrapped
        return;
    }

    // Apply phase correction (proportional)
    pll->phase -= phase_error * PLL_PHASE_GAIN;

    // Accumulate for integral term (frequency drift correction)
    pll->phase_integral += phase_error * PLL_INTEGRAL_GAIN;

    // Limit integral term to prevent runaway
    if (pll->phase_integral > 50) pll->phase_integral = 50;
    if (pll->phase_integral < -50) pll->phase_integral = -50;

    // Apply integral correction to frequency
    pll->freq_adjust = pll->phase_integral;

    // Store for derivative term (not currently used)
    pll->phase_error = phase_error;

    decoder->debug.hsyncs_last_field++;
}

// Decode current line from line buffer to field buffer
static void decode_current_line(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->line_buffer) return;

    cvbs_pll_state_t *pll = &decoder->pll;
    int line_num = pll->current_line;

    // Get field parameters
    int active_start = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                       NTSC_ACTIVE_START : PAL_ACTIVE_START;
    int max_field_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                          NTSC_FIELD_HEIGHT : PAL_FIELD_HEIGHT;

    // Only decode active video lines (skip VBI)
    if (line_num >= active_start && line_num < active_start + max_field_lines) {
        int field_line = line_num - active_start;

        // Write to the appropriate field buffer (not directly to frame)
        int field_idx = decoder->state.current_field ? 1 : 0;
        uint8_t *field_buf = decoder->field_buffer[field_idx];

        if (field_buf && field_line >= 0 && field_line < max_field_lines) {
            uint8_t *row_ptr = field_buf + ((size_t)field_line * (size_t)CVBS_FRAME_WIDTH);

            // Use samples from line buffer
            int samples_available = decoder->line_buffer_count;
            if (samples_available > 100) {  // Need minimum samples
                decode_line_to_pixels(decoder->line_buffer, samples_available,
                                     &decoder->levels, row_ptr, CVBS_FRAME_WIDTH);
            }
        }
    }

    // Clear line buffer for next line
    decoder->line_buffer_count = 0;
}

void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count) {
    if (!decoder || !buf || count < 100) return;
    if (!decoder->line_buffer) return;

    // Start timing for this buffer processing
    uint64_t start_time = get_time_us();

    // Check for minimum signal strength (skip until first V-sync commits levels)
    if (decoder->levels.range < 100 && decoder->debug.vsync_found > 0) {
        decoder->sync_errors++;
        return;
    }

    // Cache frequently accessed values in local variables
    int16_t threshold = decoder->adaptive.threshold;
    cvbs_pll_state_t *pll = &decoder->pll;
    int16_t *line_buffer = decoder->line_buffer;
    int line_buffer_count = decoder->line_buffer_count;
    size_t global_sample_pos = decoder->global_sample_pos;
    bool last_filtered_above = decoder->last_filtered_above;

    // Pre-calculate effective period (only changes on H-sync, updated in loop)
    double effective_period = pll->line_period + pll->freq_adjust;

    // Pre-calculate max lines for field overflow check
    int max_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                   NTSC_FIELD_LINES + 20 : PAL_FIELD_LINES + 20;

    // Process each sample
    for (size_t i = 0; i < count; i++) {
        int16_t sample = buf[i];

        // Apply lowpass filter for cleaner sync detection (fixed-point)
        int16_t filtered = apply_lowpass(decoder, sample);

        // Accumulate min/max from filtered signal (every 16th sample for efficiency)
        if ((i & 0xF) == 0) {
            accumulate_filtered_level(decoder, filtered);
        }

        // Store UNFILTERED sample in line buffer (for video decoding)
        if (line_buffer_count < CVBS_LINE_BUFFER_SIZE) {
            line_buffer[line_buffer_count++] = sample;
        }

        // Advance PLL phase
        pll->phase += 1.0;
        pll->samples_in_line++;
        pll->total_samples++;
        global_sample_pos++;

        // Check for edge transitions (computed once, used by both detectors)
        bool is_above = (filtered > threshold);
        bool falling_edge = last_filtered_above && !is_above;
        bool rising_edge = !last_filtered_above && is_above;

        // Run V-sync detector on falling edges only
        if (falling_edge) {
            size_t interval = global_sample_pos - decoder->vsync_last_edge_pos;
            decoder->vsync_last_edge_pos = global_sample_pos;
            cvbs_vsync_state_t *vs = &decoder->vsync;

            // Half-line interval: 1000-1600 samples (vs 2200-3000 for full line)
            bool is_half_line = (interval >= 1000 && interval <= 1600);

            if (is_half_line) {
                vs->half_line_count++;
                vs->total_half_lines++;
                if (vs->half_line_count >= 6 && !vs->in_vsync) {
                    vs->in_vsync = true;
                    vs->total_half_lines = vs->half_line_count;
                }
            } else if (vs->in_vsync && interval >= 2200 && interval <= 3000) {
                // V-sync complete
                vs->in_vsync = false;
                decoder->debug.last_half_line_count = vs->total_half_lines;
                bool is_odd_field = (vs->total_half_lines >= 15);
                vs->half_line_count = 0;
                vs->total_half_lines = 0;

                // Write back before calling start_new_field_pll
                decoder->line_buffer_count = line_buffer_count;
                decoder->global_sample_pos = global_sample_pos;
                decoder->last_filtered_above = is_above;

                start_new_field_pll(decoder, is_odd_field);

                // Refresh cached values that may have changed
                effective_period = pll->line_period + pll->freq_adjust;
                line_buffer_count = decoder->line_buffer_count;
            } else {
                vs->half_line_count = 0;
            }

            // H-sync falling edge - start pulse measurement
            decoder->in_hsync_pulse = true;
            decoder->hsync_pulse_start = global_sample_pos;
        }

        // H-sync rising edge - check pulse width
        if (rising_edge && decoder->in_hsync_pulse) {
            decoder->in_hsync_pulse = false;
            size_t pulse_width = global_sample_pos - decoder->hsync_pulse_start;

            if (pulse_width >= HSYNC_MIN_WIDTH && pulse_width <= HSYNC_MAX_WIDTH) {
                pll_process_hsync(decoder, pll->phase);
                // Update effective period after PLL adjustment
                effective_period = pll->line_period + pll->freq_adjust;
            }
        }

        last_filtered_above = is_above;

        // Check if PLL indicates line complete (phase >= line_period)
        if (pll->phase >= effective_period) {
            // Write back line_buffer_count before decode
            decoder->line_buffer_count = line_buffer_count;

            // Line complete - decode it
            decode_current_line(decoder);

            // Refresh line_buffer_count (decode_current_line resets it)
            line_buffer_count = decoder->line_buffer_count;

            // Advance to next line
            pll->current_line++;
            pll->phase -= effective_period;
            pll->samples_in_line = 0;
            decoder->lines_since_vsync++;

            // Check for field overflow
            if (pll->current_line > max_lines) {
                bool next_is_odd = (decoder->state.current_field == 1);
                decoder->global_sample_pos = global_sample_pos;
                decoder->last_filtered_above = last_filtered_above;
                start_new_field_pll(decoder, next_is_odd);
                effective_period = pll->line_period + pll->freq_adjust;
                line_buffer_count = decoder->line_buffer_count;
            }
        }
    }

    // Write back cached state
    decoder->line_buffer_count = line_buffer_count;
    decoder->global_sample_pos = global_sample_pos;
    decoder->last_filtered_above = last_filtered_above;

    // Accumulate total decode time for this field
    g_field_decode_us += get_time_us() - start_time;
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height) {
    if (!decoder || !decoder->display_ready) {
        // No frame available - draw placeholder
        DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){20, 20, 30, 255});
        DrawText("No Signal", (int)(x + width/2 - 40), (int)(y + height/2 - 10),
                 20, (Color){100, 100, 100, 255});
        return;
    }

    // Update texture if needed
    if (!decoder->texture_valid) {
        decoder->frame_texture = LoadTextureFromImage(decoder->frame_image);
        SetTextureFilter(decoder->frame_texture, TEXTURE_FILTER_BILINEAR);
        decoder->texture_valid = true;
    }

    // Get frame height
    int field_h = decoder->frame_height;
    if (field_h > CVBS_MAX_HEIGHT) field_h = CVBS_MAX_HEIGHT;

    // Convert grayscale to RGBA for the image
    // Use 32-bit writes instead of per-component Color struct assignment
    uint8_t *gray_src = decoder->display_buffer;
    uint32_t *rgba_dst = (uint32_t *)decoder->frame_image.data;
    int total_pixels = field_h * CVBS_FRAME_WIDTH;

    for (int i = 0; i < total_pixels; i++) {
        uint8_t gray = gray_src[i];
        // Pack as RGBA (little-endian: 0xAABBGGRR)
        rgba_dst[i] = 0xFF000000 | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | gray;
    }

    // Upload to GPU
    UpdateTexture(decoder->frame_texture, decoder->frame_image.data);

    // Calculate aspect-correct rectangle (4:3 aspect ratio for CVBS)
    float aspect = 4.0f / 3.0f;
    float display_aspect = width / height;

    float draw_w, draw_h;
    if (display_aspect > aspect) {
        // Height limited
        draw_h = height;
        draw_w = height * aspect;
    } else {
        // Width limited
        draw_w = width;
        draw_h = width / aspect;
    }

    float draw_x = x + (width - draw_w) / 2;
    float draw_y = y + (height - draw_h) / 2;

    // Draw the video frame - raylib will scale from field resolution to display
    Rectangle src = {0, 0, (float)CVBS_FRAME_WIDTH, (float)field_h};
    Rectangle dst = {draw_x, draw_y, draw_w, draw_h};
    DrawTexturePro(decoder->frame_texture, src, dst, (Vector2){0, 0}, 0, WHITE);

    // Draw OSD overlay in bottom-left corner of video
    const char *deint_mode = (decoder->field_ready[0] && decoder->field_ready[1]) ? "weave" : "bob";

    float text_x = draw_x + 8;
    float text_y = draw_y + draw_h - 24;

    // Format: "weave | 5.2 ms"
    char osd_text[64];
    snprintf(osd_text, sizeof(osd_text), "%s | %.2f ms", deint_mode, g_field_decode_avg_ms);

    gui_text_draw_mono(osd_text, text_x + 1, text_y + 1, 14, BLACK);  // Shadow
    gui_text_draw_mono(osd_text, text_x, text_y, 14, (Color){255, 255, 100, 255});  // Yellow
}

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder) {
    if (!decoder) return CVBS_FORMAT_UNKNOWN;
    return decoder->state.format;
}

const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder) {
    if (!decoder) return "Unknown";

    switch (decoder->state.format) {
        case CVBS_FORMAT_PAL:   return "PAL 720x576";
        case CVBS_FORMAT_NTSC:  return "NTSC 720x486";
        case CVBS_FORMAT_SECAM: return "SECAM 720x576";
        default:                return "Detecting...";
    }
}

void gui_cvbs_set_chroma_decoder(cvbs_decoder_t *decoder, int chroma_decoder)
{
    if (!decoder) return;

    if (chroma_decoder == (int)CVBS_CHROMA_DECODER_SIMPLEPAL) {
        decoder->chroma_decoder = CVBS_CHROMA_DECODER_SIMPLEPAL;
    } else {
        decoder->chroma_decoder = CVBS_CHROMA_DECODER_MONO;
    }
}

void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select) {
    if (!decoder) return;

    // format_select: 0=PAL, 1=NTSC, 2=SECAM
    cvbs_format_t new_format = CVBS_FORMAT_NTSC;
    if (format_select == 0) new_format = CVBS_FORMAT_PAL;
    else if (format_select == 2) new_format = CVBS_FORMAT_SECAM;

    if (decoder->state.format != new_format) {
        decoder->state.format = new_format;

        if (new_format == CVBS_FORMAT_NTSC) {
            decoder->state.total_lines = CVBS_NTSC_TOTAL_LINES;
            decoder->state.active_lines = CVBS_NTSC_ACTIVE_LINES;
            decoder->frame_height = CVBS_NTSC_HEIGHT;
            decoder->field_height = NTSC_FIELD_HEIGHT;
            decoder->pll.line_period = CVBS_NTSC_LINE_SAMPLES;
        } else {
            // PAL and SECAM share line/field geometry for luma
            decoder->state.total_lines = CVBS_PAL_TOTAL_LINES;
            decoder->state.active_lines = CVBS_PAL_ACTIVE_LINES;
            decoder->frame_height = CVBS_PAL_HEIGHT;
            decoder->field_height = PAL_FIELD_HEIGHT;
            decoder->pll.line_period = CVBS_PAL_LINE_SAMPLES;
        }

        // Reset decode state after changing system
        gui_cvbs_reset(decoder);
    }
}

