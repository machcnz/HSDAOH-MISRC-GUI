/*
 * MISRC GUI - Simulated Device
 *
 * Provides a simulated capture device for development and testing without hardware.
 * Generates PAL CVBS video on Channel A and VHS RF head signal on Channel B.
 */

//-----------------------------------------------------------------------------
// Simulation Feature Flags
//
// Set to 0 to disable non-ideal behaviors for cleaner test signals
//-----------------------------------------------------------------------------

// CVBS signal impairments
#define SIM_ENABLE_CVBS_NOISE           0   // Random noise on CVBS output

// VHS RF signal impairments
#define SIM_ENABLE_VHS_RF_NOISE         0   // Random noise on VHS RF output
#define SIM_ENABLE_VHS_HEAD_SWITCH      0   // Head switching noise at field boundaries
#define SIM_ENABLE_VHS_TRACKING_NOISE   0   // Low-level tracking jitter

// Analog circuit simulation
#define SIM_ENABLE_SOFT_CLIPPING        0   // Soft saturation near signal limits

//-----------------------------------------------------------------------------

#include "gui_simulated.h"
#include "gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_cvbs.h"
#include "gui_extract.h"
#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//-----------------------------------------------------------------------------
// PAL Timing Constants (in samples at 40 MSPS)
// Using PAL-B/G standard (625 lines, 50 Hz, 4.43361875 MHz subcarrier)
//-----------------------------------------------------------------------------

#define PAL_LINE_DURATION_US     64.0      // One horizontal line (1/15625 Hz)
#define PAL_LINE_SAMPLES         ((int)(PAL_LINE_DURATION_US * 40.0))   // 2560 samples per line
#define PAL_HALF_LINE_SAMPLES    (PAL_LINE_SAMPLES / 2)                 // 1280 samples

// Horizontal timing
#define PAL_HSYNC_US             4.7       // H-sync pulse width
#define PAL_HSYNC_SAMPLES        ((int)(PAL_HSYNC_US * 40.0))           // ~188 samples
#define PAL_BACK_PORCH_US        5.7       // Back porch (includes colorburst)
#define PAL_BACK_PORCH_SAMPLES   ((int)(PAL_BACK_PORCH_US * 40.0))
#define PAL_FRONT_PORCH_US       1.65      // Front porch
#define PAL_FRONT_PORCH_SAMPLES  ((int)(PAL_FRONT_PORCH_US * 40.0))
#define PAL_COLORBURST_CYCLES    10        // Number of colorburst cycles
#define PAL_COLORBURST_FREQ      4433618.75 // 4.43361875 MHz color subcarrier

// Vertical timing (PAL interlaced)
#define PAL_LINES_PER_FRAME      625       // Total lines per frame (both fields)
#define PAL_LINES_PER_FIELD      312       // Lines per field (312.5 lines, alternating)
#define PAL_FRAME_SAMPLES        ((uint64_t)PAL_LINE_SAMPLES * PAL_LINES_PER_FRAME)

// Vertical blanking structure (per field)
#define PAL_VBLANK_PRE_EQ_LINES       2     // Pre-equalizing pulses
#define PAL_VBLANK_VSYNC_LINES        2     // Vertical sync pulses
#define PAL_VBLANK_POST_EQ_LINES      2     // Post-equalizing pulses
#define PAL_VBLANK_BLANK_LINES        17    // Remaining VBI
#define PAL_FIRST_ACTIVE_LINE         23    // First line with active video (0-indexed)

// Equalizing and serration pulse widths
#define PAL_EQ_PULSE_US              2.35   // Equalizing pulse width
#define PAL_EQ_PULSE_SAMPLES         ((int)(PAL_EQ_PULSE_US * 40.0))
#define PAL_SERR_PULSE_US            4.7    // Serration pulse width
#define PAL_SERR_PULSE_SAMPLES       ((int)(PAL_SERR_PULSE_US * 40.0))

// Use PAL constants throughout (aliased for compatibility with existing code)
#define LINE_SAMPLES             PAL_LINE_SAMPLES
#define HALF_LINE_SAMPLES        PAL_HALF_LINE_SAMPLES
#define HSYNC_SAMPLES            PAL_HSYNC_SAMPLES
#define BACK_PORCH_SAMPLES       PAL_BACK_PORCH_SAMPLES
#define FRONT_PORCH_SAMPLES      PAL_FRONT_PORCH_SAMPLES
#define LINES_PER_FRAME          PAL_LINES_PER_FRAME
#define LINES_PER_FIELD          PAL_LINES_PER_FIELD
#define FRAME_SAMPLES            PAL_FRAME_SAMPLES
#define FIRST_ACTIVE_LINE        PAL_FIRST_ACTIVE_LINE
#define EQ_PULSE_SAMPLES         PAL_EQ_PULSE_SAMPLES
#define COLORBURST_FREQ          PAL_COLORBURST_FREQ
#define COLORBURST_CYCLES        PAL_COLORBURST_CYCLES

//-----------------------------------------------------------------------------
// Video Signal Levels
//-----------------------------------------------------------------------------

#define SYNC_LEVEL      (-0.4)    // Sync tip
#define BLANKING_LEVEL  (0.0)     // Blanking/black level
#define BLACK_LEVEL     (0.075)   // Setup/pedestal (7.5 IRE)
#define WHITE_LEVEL     (1.0)     // Peak white (100 IRE)

//-----------------------------------------------------------------------------
// VHS RF Parameters
//-----------------------------------------------------------------------------

#define VHS_LUMA_CARRIER_SYNC    3400000.0   // 3.4 MHz at sync tip
#define VHS_LUMA_CARRIER_WHITE   4400000.0   // 4.4 MHz at white
#define VHS_CHROMA_CARRIER       629000.0    // 629 kHz down-converted chroma
#define VHS_FM_DEVIATION         (VHS_LUMA_CARRIER_WHITE - VHS_LUMA_CARRIER_SYNC)

//-----------------------------------------------------------------------------
// Signal Generation State (based on hacktv approach)
//-----------------------------------------------------------------------------

static uint64_t s_sim_sample_count = 0;
static double s_vhs_fm_phase = 0.0;  // FM phase must be accumulated (frequency varies with signal)

// Color carrier lookup table (hacktv approach)
// The table length equals sample_rate / carrier_freq in lowest terms
// For 40MHz / 3.579545MHz ≈ 11.177, but we use integer math
// hacktv uses { 39375000, 11 } = 3579545.4545... Hz exactly
// At 40 MSPS: lookup_width = 40000000 * 11 / 39375000 = 440000000 / 39375000 ≈ 11.175
// To get exact integer cycles, we'd need GCD, but for now use a large enough table
#define COLOR_CARRIER_NUM  39375000   // Numerator (frequency * 11)
#define COLOR_CARRIER_DEN  11         // Denominator
// Actual carrier freq = 39375000 / 11 = 3579545.454545... Hz

// Lookup table: stores cos and sin for each sample position in one color cycle period
// Table width = sample_rate * denominator / numerator = 40000000 * 11 / 39375000
// Simplified: 440000000 / 39375000 = 8800 / 787.5 ≈ 11.175
// We'll compute this properly at init time
static int16_t *s_colour_lookup_i = NULL;  // cos(phase) * 32767
static int16_t *s_colour_lookup_q = NULL;  // sin(phase) * 32767
static int s_colour_lookup_width = 0;
static int s_colour_lookup_offset = 0;

// Simple xorshift PRNG for noise
static uint32_t s_sim_rng_state = 12345;

static uint32_t sim_rand(void) {
    uint32_t x = s_sim_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_sim_rng_state = x;
    return x;
}

// Generate random float in range [-1, 1]
static float sim_noise(void) {
    return ((float)(sim_rand() & 0xFFFF) / 32768.0f) - 1.0f;
}

// Initialize the color carrier lookup table for PAL (call once at startup)
static void init_colour_lookup(void) {
    if (s_colour_lookup_i != NULL) return;  // Already initialized

    // PAL carrier: 4.43361875 MHz = 17734475/4 Hz (hacktv uses this rational)
    // At 40 MSPS: samples per cycle = 40000000 / 4433618.75 ≈ 9.022
    // For exact repeat: 40000000 * 4 / 17734475 = 160000000 / 17734475
    // GCD(160000000, 17734475) = 25 -> 6400000 / 709379
    // This doesn't simplify nicely, so use a large table
    // hacktv uses: colour_lookup_width = a.num where a = pixel_rate / carrier
    // For PAL at various sample rates, they compute the exact rational

    // For simplicity, use enough samples to cover many complete cycles
    // 709379 samples would give exact 6400000 cycles, but that's huge
    // Instead use a reasonable size that covers several lines worth
    // At 9.022 samples/cycle, 2560 samples = ~284 cycles (close enough)

    s_colour_lookup_width = 2560;  // One line width for PAL
    int total_size = s_colour_lookup_width + LINE_SAMPLES;

    s_colour_lookup_i = (int16_t *)malloc(total_size * sizeof(int16_t));
    s_colour_lookup_q = (int16_t *)malloc(total_size * sizeof(int16_t));

    if (!s_colour_lookup_i || !s_colour_lookup_q) {
        fprintf(stderr, "[SIM] Failed to allocate color lookup tables\n");
        return;
    }

    // Phase increment per sample = 2*PI * (carrier_freq / sample_rate)
    // PAL: 2*PI * 4433618.75 / 40000000
    double phase_inc = 2.0 * M_PI * PAL_COLORBURST_FREQ / (double)SIM_SAMPLE_RATE;

    for (int i = 0; i < total_size; i++) {
        double phase = phase_inc * i;
        s_colour_lookup_i[i] = (int16_t)round(cos(phase) * 32767.0);
        s_colour_lookup_q[i] = (int16_t)round(sin(phase) * 32767.0);
    }

    s_colour_lookup_offset = 0;

    fprintf(stderr, "[SIM] PAL color carrier lookup table initialized: %d samples (%.1f MHz)\n",
            s_colour_lookup_width, PAL_COLORBURST_FREQ / 1000000.0);
}

// Cleanup color lookup table
static void cleanup_colour_lookup(void) {
    if (s_colour_lookup_i) {
        free(s_colour_lookup_i);
        s_colour_lookup_i = NULL;
    }
    if (s_colour_lookup_q) {
        free(s_colour_lookup_q);
        s_colour_lookup_q = NULL;
    }
    s_colour_lookup_width = 0;
    s_colour_lookup_offset = 0;
}

//-----------------------------------------------------------------------------
// NTSC Line Type Classification
//-----------------------------------------------------------------------------

typedef enum {
    LINE_TYPE_PRE_EQ,       // Pre-equalizing pulses (half-line rate)
    LINE_TYPE_VSYNC,        // Vertical sync with serrations
    LINE_TYPE_POST_EQ,      // Post-equalizing pulses (half-line rate)
    LINE_TYPE_VBLANK,       // Vertical blanking (normal H-sync, black video)
    LINE_TYPE_ACTIVE        // Active video line
} line_type_t;

// Get the line type and field information for a given sample position
static line_type_t sim_get_line_type(uint64_t sample_index, int *out_field, int *out_line_in_field,
                                     int *out_sample_in_line, bool *out_is_half_line) {
    uint64_t sample_in_frame = sample_index % FRAME_SAMPLES;
    int line_in_frame = (int)(sample_in_frame / LINE_SAMPLES);
    int sample_in_line = (int)(sample_in_frame % LINE_SAMPLES);

    int field, line_in_field;
    bool is_half_line = false;

    if (line_in_frame < LINES_PER_FIELD) {
        field = 0;
        line_in_field = line_in_frame;
    } else if (line_in_frame == LINES_PER_FIELD) {
        if (sample_in_line < HALF_LINE_SAMPLES) {
            field = 0;
            line_in_field = 262;
            is_half_line = true;
        } else {
            field = 1;
            line_in_field = 0;
            sample_in_line -= HALF_LINE_SAMPLES;
            is_half_line = true;
        }
    } else {
        field = 1;
        line_in_field = line_in_frame - LINES_PER_FIELD;
    }

    *out_field = field;
    *out_line_in_field = line_in_field;
    *out_sample_in_line = sample_in_line;
    *out_is_half_line = is_half_line;

    if (line_in_field < PAL_VBLANK_PRE_EQ_LINES) {
        return LINE_TYPE_PRE_EQ;
    } else if (line_in_field < PAL_VBLANK_PRE_EQ_LINES + PAL_VBLANK_VSYNC_LINES) {
        return LINE_TYPE_VSYNC;
    } else if (line_in_field < PAL_VBLANK_PRE_EQ_LINES + PAL_VBLANK_VSYNC_LINES + PAL_VBLANK_POST_EQ_LINES) {
        return LINE_TYPE_POST_EQ;
    } else if (line_in_field < FIRST_ACTIVE_LINE) {
        return LINE_TYPE_VBLANK;
    } else {
        return LINE_TYPE_ACTIVE;
    }
}

//-----------------------------------------------------------------------------
// Sync Pulse Generation
//-----------------------------------------------------------------------------

// Generate equalizing pulse signal (2 pulses per line at half-line rate)
static double sim_generate_eq_pulse(int sample_in_line, bool is_half_line) {
    int half_line = HALF_LINE_SAMPLES;

    if (sample_in_line < EQ_PULSE_SAMPLES) {
        return SYNC_LEVEL;
    }
    if (!is_half_line && sample_in_line >= half_line &&
        sample_in_line < half_line + EQ_PULSE_SAMPLES) {
        return SYNC_LEVEL;
    }
    return BLANKING_LEVEL;
}

// Generate vertical sync with serrations
static double sim_generate_vsync_serration(int sample_in_line, bool is_half_line) {
    int serr_start1 = HALF_LINE_SAMPLES - PAL_SERR_PULSE_SAMPLES;
    if (sample_in_line >= serr_start1 && sample_in_line < HALF_LINE_SAMPLES) {
        return BLANKING_LEVEL;
    }
    if (!is_half_line) {
        int serr_start2 = LINE_SAMPLES - PAL_SERR_PULSE_SAMPLES;
        if (sample_in_line >= serr_start2) {
            return BLANKING_LEVEL;
        }
    }
    return SYNC_LEVEL;
}

//-----------------------------------------------------------------------------
// Color Space Conversion
//-----------------------------------------------------------------------------

// RGB to YUV conversion (standard for vhs-decode compatibility)
// Same formula as hacktv: U = (B-Y) * 0.493, V = (R-Y) * 0.877
static inline void rgb_to_yuv(double r, double g, double b,
                              double *y, double *u, double *v) {
    *y = 0.299 * r + 0.587 * g + 0.114 * b;
    *u = 0.493 * (b - *y);  // (B-Y) scaled - same as hacktv eu_co
    *v = 0.877 * (r - *y);  // (R-Y) scaled - same as hacktv ev_co
}

// 75% SMPTE color bars as RGB
// Order: white, yellow, cyan, green, magenta, red, blue, black
static const double bar_rgb[8][3] = {
    {0.75, 0.75, 0.75},  // white
    {0.75, 0.75, 0.00},  // yellow
    {0.00, 0.75, 0.75},  // cyan
    {0.00, 0.75, 0.00},  // green
    {0.75, 0.00, 0.75},  // magenta
    {0.75, 0.00, 0.00},  // red
    {0.00, 0.00, 0.75},  // blue
    {0.00, 0.00, 0.00},  // black
};

//-----------------------------------------------------------------------------
// Test Pattern Generation
//-----------------------------------------------------------------------------

// Test mode: set to 1-7 to output a single solid color bar, 0 for normal bars
#define TEST_SINGLE_COLOR 1  // 0=normal, 1=yellow, 2=cyan, 3=green, 4=magenta, 5=red, 6=blue

// Get Y, U, V values for current bar
static void sim_get_bar_yuv(int bar, double *y, double *u, double *v) {
#if TEST_SINGLE_COLOR > 0
    bar = TEST_SINGLE_COLOR;  // Force single color for testing
#else
    bar = bar % 8;
#endif
    rgb_to_yuv(bar_rgb[bar][0], bar_rgb[bar][1], bar_rgb[bar][2], y, u, v);

    // Debug: print YUV values once
    static int printed = 0;
    if (!printed) {
        fprintf(stderr, "[SIM] Color bar %d: Y=%.3f U=%.3f V=%.3f\n", bar, *y, *u, *v);
        printed = 1;
    }
}

static double sim_generate_test_pattern(int pixel_in_line, int field, int active_width) {
    (void)field;
    int bar = (pixel_in_line * 8) / active_width;

    double y, u, v;
    sim_get_bar_yuv(bar, &y, &u, &v);
    (void)u; (void)v;  // Luma only for this function

    return y;
}

//-----------------------------------------------------------------------------
// CVBS Signal Generation (rewritten based on hacktv approach)
//-----------------------------------------------------------------------------

// Track the current line for lookup table offset management
static int s_current_line = -1;
static int s_line_lookup_offset = 0;  // Offset into lookup table for current line

// Called at the start of each line to update the lookup table offset
static void advance_to_line(int line_in_frame) {
    if (line_in_frame != s_current_line) {
        // New line - advance the lookup offset
        if (s_current_line >= 0 && line_in_frame == s_current_line + 1) {
            // Sequential line - advance by line width
            s_line_lookup_offset += LINE_SAMPLES;
            s_line_lookup_offset %= s_colour_lookup_width;
        } else {
            // Non-sequential (frame wrap or init) - compute from line number
            // Each line adds LINE_SAMPLES to the offset
            s_line_lookup_offset = (line_in_frame * LINE_SAMPLES) % s_colour_lookup_width;
        }
        s_current_line = line_in_frame;
    }
}

static double sim_generate_cvbs(uint64_t sample_index, int *out_line_number, int *out_field,
                                 double *out_chroma_u, double *out_chroma_v, double *out_subcarrier_phase) {
    int field, line_in_field, sample_in_line;
    bool is_half_line;

    line_type_t line_type = sim_get_line_type(sample_index, &field, &line_in_field,
                                               &sample_in_line, &is_half_line);

    int line_in_frame = (field == 0) ? line_in_field : (LINES_PER_FIELD + line_in_field);
    if (out_line_number) *out_line_number = line_in_frame;
    if (out_field) *out_field = field;

    // Update lookup table offset for this line
    advance_to_line(line_in_frame);

    // Get carrier lookup values for this sample position
    int lut_idx = s_line_lookup_offset + sample_in_line;
    int16_t carrier_i = s_colour_lookup_i[lut_idx];  // cos(phase)
    int16_t carrier_q = s_colour_lookup_q[lut_idx];  // sin(phase)

    // Output subcarrier phase for VHS (approximate from lookup)
    if (out_subcarrier_phase) {
        *out_subcarrier_phase = atan2((double)carrier_q, (double)carrier_i);
    }

    double signal = 0.0;
    double chroma_u = 0.0;
    double chroma_v = 0.0;

    switch (line_type) {
        case LINE_TYPE_PRE_EQ:
        case LINE_TYPE_POST_EQ:
            signal = sim_generate_eq_pulse(sample_in_line, is_half_line);
            break;

        case LINE_TYPE_VSYNC:
            signal = sim_generate_vsync_serration(sample_in_line, is_half_line);
            break;

        case LINE_TYPE_VBLANK:
            if (sample_in_line < HSYNC_SAMPLES) {
                signal = SYNC_LEVEL;
            } else {
                signal = BLANKING_LEVEL;
            }
            break;

        case LINE_TYPE_ACTIVE: {
            int active_start = HSYNC_SAMPLES + BACK_PORCH_SAMPLES;
            int active_end = LINE_SAMPLES - FRONT_PORCH_SAMPLES;
            int active_width = active_end - active_start;

            if (sample_in_line < HSYNC_SAMPLES) {
                signal = SYNC_LEVEL;
            }
            else if (sample_in_line < active_start) {
                // Back porch - includes color burst
                int back_porch_pos = sample_in_line - HSYNC_SAMPLES;
                int burst_start = (int)(0.6 * 40);   // 0.6µs after hsync
                int burst_duration = (int)(COLORBURST_CYCLES * 40.0 / (COLORBURST_FREQ / 1000000.0));

                if (back_porch_pos >= burst_start && back_porch_pos < burst_start + burst_duration) {
                    // PAL colorburst: 135° phase (swinging ±45° from line to line)
                    // cos(135°) = -sqrt(2)/2 ≈ -0.707
                    // sin(135°) = +sqrt(2)/2 ≈ +0.707
                    // PAL burst swings between 135° and 225° on alternating lines
                    // Line n:   phase = 135° -> burst_i = -0.707, burst_q = +0.707
                    // Line n+1: phase = 225° -> burst_i = -0.707, burst_q = -0.707
                    // The swing is based on (frame + line) & 1

                    double burst_level = 0.15;  // About 40 IRE peak-to-peak
                    double burst_i = -0.7071;   // cos(135°) = -sqrt(2)/2
                    double burst_q = 0.7071;    // sin(135°) = +sqrt(2)/2

                    // PAL burst phase alternation: swing ±45° on alternating lines
                    // On odd lines (when pal_switch = -1), burst swings to 225°
                    int pal_switch = ((line_in_frame) & 1) ? -1 : 1;
                    burst_q *= pal_switch;

                    // Modulation: burst = burst_i * carrier_i + burst_q * carrier_q
                    //           = burst_i * cos(wt) + burst_q * sin(wt)
                    double burst = burst_level * (burst_i * ((double)carrier_i / 32767.0)
                                                + burst_q * ((double)carrier_q / 32767.0));
                    signal = BLANKING_LEVEL + burst;
                } else {
                    signal = BLANKING_LEVEL;
                }
            }
            else if (sample_in_line < active_end) {
                // Active video region
                int pixel = sample_in_line - active_start;
                int bar = (pixel * 8) / active_width;

                // Get Y, U, V from RGB color bars
                double y, u, v;
                sim_get_bar_yuv(bar, &y, &u, &v);

                // Store U/V for VHS generation
                chroma_u = u;
                chroma_v = v;

                // PAL chroma modulation with V-axis switching:
                // Formula: chroma = cos(wt) * V * pal_switch + sin(wt) * U
                // The V component sign alternates on each line (PAL = Phase Alternation Line)
                // This is the key difference from NTSC which doesn't alternate
                int pal_switch = ((line_in_frame) & 1) ? -1 : 1;
                double chroma = ((double)carrier_i / 32767.0) * v * pal_switch
                              + ((double)carrier_q / 32767.0) * u;

                // Scale chroma (hacktv doesn't scale here, but we need to match levels)
                signal = BLACK_LEVEL + y * (WHITE_LEVEL - BLACK_LEVEL) + chroma;
            }
            else {
                signal = BLANKING_LEVEL;
            }
            break;
        }
    }

    // Output U/V components for VHS generation
    if (out_chroma_u) *out_chroma_u = chroma_u;
    if (out_chroma_v) *out_chroma_v = chroma_v;

    return signal;
}

//-----------------------------------------------------------------------------
// VHS RF Signal Generation
//-----------------------------------------------------------------------------

// VHS color-under: generate 629 kHz QAM chroma signal from U/V components

static double sim_generate_vhs_rf(double cvbs_luma, double chroma_u, double chroma_v,
                                   int line_in_frame, int field, int sample_in_line) {
    (void)field;

    // === FM Luminance ===
    // VHS FM-encodes the luminance signal
    double normalized = (cvbs_luma - SYNC_LEVEL) / (WHITE_LEVEL - SYNC_LEVEL);
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;

    double fm_freq = VHS_LUMA_CARRIER_SYNC + normalized * VHS_FM_DEVIATION;

    // FM phase must be accumulated since frequency varies with signal
    double fm_signal = sin(s_vhs_fm_phase) * 0.7;
    s_vhs_fm_phase += 2.0 * M_PI * fm_freq / SIM_SAMPLE_RATE;
    if (s_vhs_fm_phase > 2.0 * M_PI) s_vhs_fm_phase -= 2.0 * M_PI;

    // === VHS Color-Under (629 kHz QAM) ===
    // VHS color-under is a quadrature amplitude modulated signal at 629 kHz.
    // Per-line phase for 629 kHz carrier
    double vhs_cycles_in_line = VHS_CHROMA_CARRIER * (double)sample_in_line / (double)SIM_SAMPLE_RATE;
    double vhs_phase = 2.0 * M_PI * vhs_cycles_in_line;

    // VHS 90° phase rotation on odd lines for crosstalk cancellation
    double vhs_line_rotation = (line_in_frame % 2) ? (M_PI / 2.0) : 0.0;
    vhs_phase += vhs_line_rotation;

    // Generate 629 kHz QAM chroma: cos(wt)*V + sin(wt)*U
    // Note: PAL V-axis switching was already applied in the CVBS generation
    double chroma_under = cos(vhs_phase) * chroma_v + sin(vhs_phase) * chroma_u;

    // Scale down VHS chroma (color-under level is lower than NTSC broadcast)
    chroma_under *= 0.5;

    double head_noise = 0.0;
#if SIM_ENABLE_VHS_HEAD_SWITCH
    int line_in_field = (field == 0) ? line_in_frame : (line_in_frame - LINES_PER_FIELD);
    if (line_in_field <= 6) {
        double switch_intensity = 1.0 - (line_in_field / 6.0);
        head_noise = sim_noise() * 0.4 * switch_intensity;

        if (line_in_field <= 2 && (sim_rand() % 100) < 5) {
            head_noise += (sim_noise() > 0 ? 0.5 : -0.5);
        }
    }
#endif

    double tracking_noise = 0.0;
#if SIM_ENABLE_VHS_TRACKING_NOISE
    tracking_noise = sim_noise() * 0.02 * (field == 0 ? 1.0 : 1.1);
#endif

    return fm_signal + chroma_under + head_noise + tracking_noise;
}

//-----------------------------------------------------------------------------
// Simulated Capture Thread
//-----------------------------------------------------------------------------

static int simulated_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;

    int16_t *buf_a = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[SIM] Failed to allocate buffers\n");
        free(buf_a);
        free(buf_b);
        return -1;
    }

    fprintf(stderr, "[SIM] Simulated capture thread started at %d MSPS\n", SIM_SAMPLE_RATE / 1000000);
    fprintf(stderr, "[SIM] Channel A: CVBS composite video (NTSC color bars)\n");
    fprintf(stderr, "[SIM] Channel B: VHS RF head signal (FM luma + 629kHz chroma)\n");

    gui_extract_init_record_rbs();

#if SIM_ENABLE_CVBS_NOISE
    double cvbs_noise = 0.02;
#endif
#if SIM_ENABLE_VHS_RF_NOISE
    double rf_noise = 0.03;
#endif

    s_sim_rng_state = (uint32_t)get_time_ms();

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);

    uint64_t batch_count = 0;

    while (atomic_load(&app->sim_running)) {
        for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
            int line_in_frame = 0;
            int field = 0;
            double chroma_u = 0.0;
            double chroma_v = 0.0;
            double subcarrier_phase = 0.0;

            // Generate CVBS signal and get U/V chroma components for VHS
            double cvbs = sim_generate_cvbs(s_sim_sample_count, &line_in_frame, &field,
                                            &chroma_u, &chroma_v, &subcarrier_phase);
#if SIM_ENABLE_CVBS_NOISE
            cvbs += sim_noise() * cvbs_noise;
#endif

            // For VHS, compute luma from CVBS (subtract the chroma we added)
            // Chroma was: cos(phase) * V + sin(phase) * U
            double chroma_at_sample = cos(subcarrier_phase) * chroma_v
                                    + sin(subcarrier_phase) * chroma_u;
            double cvbs_luma = cvbs - chroma_at_sample;

            // Get sample_in_line for VHS RF generation
            int field_tmp, line_in_field_tmp, sample_in_line;
            bool is_half_line_tmp;
            sim_get_line_type(s_sim_sample_count, &field_tmp, &line_in_field_tmp, &sample_in_line, &is_half_line_tmp);

            // Generate VHS RF with U/V quadrature chroma at 629 kHz
            double vhs_rf = sim_generate_vhs_rf(cvbs_luma, chroma_u, chroma_v, line_in_frame, field, sample_in_line);
#if SIM_ENABLE_VHS_RF_NOISE
            vhs_rf += sim_noise() * rf_noise;
#endif

#if SIM_ENABLE_SOFT_CLIPPING
            // Soft clipping - analog-style saturation near limits
            if (cvbs > 0.95) cvbs = 0.95 + 0.05 * tanh((cvbs - 0.95) * 10.0);
            if (cvbs < -0.95) cvbs = -0.95 + 0.05 * tanh((cvbs + 0.95) * 10.0);
            if (vhs_rf > 0.95) vhs_rf = 0.95 + 0.05 * tanh((vhs_rf - 0.95) * 10.0);
            if (vhs_rf < -0.95) vhs_rf = -0.95 + 0.05 * tanh((vhs_rf + 0.95) * 10.0);
#endif

            buf_a[i] = (int16_t)(cvbs * 1400.0);
            buf_b[i] = (int16_t)(vhs_rf * 1024.0);  // 50% of full scale

            s_sim_sample_count++;
        }

        gui_oscilloscope_update_display(app, buf_a, buf_b, SIM_BUFFER_SIZE);

        // CVBS decode (if enabled)
        cvbs_decoder_t *cvbs_a = atomic_load(&app->cvbs_a);
        if (cvbs_a) {
            atomic_fetch_add(&app->cvbs_busy_a, 1);
            int sys = atomic_load(&app->cvbs_system_a);
            int dec = atomic_load(&app->cvbs_chroma_decoder_a);
            gui_cvbs_set_format(cvbs_a, sys);
            gui_cvbs_set_chroma_decoder(cvbs_a, dec);
            gui_cvbs_process_buffer(cvbs_a, buf_a, SIM_BUFFER_SIZE);
            atomic_fetch_sub(&app->cvbs_busy_a, 1);
        }
        cvbs_decoder_t *cvbs_b = atomic_load(&app->cvbs_b);
        if (cvbs_b) {
            atomic_fetch_add(&app->cvbs_busy_b, 1);
            int sys = atomic_load(&app->cvbs_system_b);
            int dec = atomic_load(&app->cvbs_chroma_decoder_b);
            gui_cvbs_set_format(cvbs_b, sys);
            gui_cvbs_set_chroma_decoder(cvbs_b, dec);
            gui_cvbs_process_buffer(cvbs_b, buf_b, SIM_BUFFER_SIZE);
            atomic_fetch_sub(&app->cvbs_busy_b, 1);
        }

        atomic_fetch_add(&app->total_samples, SIM_BUFFER_SIZE);
        atomic_fetch_add(&app->samples_a, SIM_BUFFER_SIZE);
        atomic_fetch_add(&app->samples_b, SIM_BUFFER_SIZE);
        atomic_fetch_add(&app->frame_count, 1);
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        // Update peak values for VU meters
        int16_t peak_a_pos = 0, peak_a_neg = 0;
        int16_t peak_b_pos = 0, peak_b_neg = 0;
        for (int i = 0; i < SIM_BUFFER_SIZE; i += 16) {
            if (buf_a[i] > peak_a_pos) peak_a_pos = buf_a[i];
            if (buf_a[i] < peak_a_neg) peak_a_neg = buf_a[i];
            if (buf_b[i] > peak_b_pos) peak_b_pos = buf_b[i];
            if (buf_b[i] < peak_b_neg) peak_b_neg = buf_b[i];
        }
        atomic_store(&app->peak_a_pos, (uint16_t)peak_a_pos);
        atomic_store(&app->peak_a_neg, (uint16_t)(-peak_a_neg));
        atomic_store(&app->peak_b_pos, (uint16_t)peak_b_pos);
        atomic_store(&app->peak_b_neg, (uint16_t)(-peak_b_neg));

        // Write to record ringbuffers if recording is enabled
        bool use_flac = false;
        if (gui_extract_is_recording(&use_flac)) {
            ringbuffer_t *rb_a = gui_extract_get_record_rb_a();
            ringbuffer_t *rb_b = gui_extract_get_record_rb_b();

            if (rb_a && rb_b) {
                if (use_flac) {
                    size_t sample_bytes = SIM_BUFFER_SIZE * sizeof(int32_t);

                    int32_t *write_a = (int32_t *)rb_write_ptr(rb_a, sample_bytes);
                    int32_t *write_b = (int32_t *)rb_write_ptr(rb_b, sample_bytes);

                    if (write_a && write_b) {
                        for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
                            write_a[i] = (int32_t)buf_a[i] << 4;
                            write_b[i] = (int32_t)buf_b[i] << 4;
                        }
                        rb_write_finished(rb_a, sample_bytes);
                        rb_write_finished(rb_b, sample_bytes);
                    }
                } else {
                    size_t sample_bytes = SIM_BUFFER_SIZE * sizeof(int16_t);

                    void *write_a = rb_write_ptr(rb_a, sample_bytes);
                    void *write_b = rb_write_ptr(rb_b, sample_bytes);

                    if (write_a && write_b) {
                        memcpy(write_a, buf_a, sample_bytes);
                        memcpy(write_b, buf_b, sample_bytes);
                        rb_write_finished(rb_a, sample_bytes);
                        rb_write_finished(rb_b, sample_bytes);
                    }
                }
            }
        }

        batch_count++;
        thrd_sleep_ms(SIM_UPDATE_INTERVAL_MS);
    }

    fprintf(stderr, "[SIM] Simulated capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(buf_a);
    free(buf_b);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_simulated_start(gui_app_t *app) {
    fprintf(stderr, "[SIM] Starting simulated capture\n");

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize color carrier lookup table
    init_colour_lookup();

    // Reset signal generation state
    s_sim_sample_count = 0;
    s_vhs_fm_phase = 0.0;
    s_current_line = -1;
    s_line_lookup_offset = 0;

    // Start thread
    atomic_store(&app->sim_running, true);
    thrd_t thread;
    if (thrd_create(&thread, simulated_capture_thread, app) != thrd_success) {
        fprintf(stderr, "[SIM] Failed to create simulated capture thread\n");
        atomic_store(&app->sim_running, false);
        return -1;
    }
    app->sim_thread = (void *)(uintptr_t)thread;

    app->is_capturing = true;
    gui_app_set_status(app, "Simulated capture running");

    return 0;
}

void gui_simulated_stop(gui_app_t *app) {
    if (!atomic_load(&app->sim_running)) return;

    fprintf(stderr, "[SIM] Stopping simulated capture\n");

    atomic_store(&app->sim_running, false);

    if (app->sim_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->sim_thread;
        thrd_join(thread, NULL);
        app->sim_thread = NULL;
    }

    atomic_store(&app->stream_synced, false);
    app->is_capturing = false;

    gui_app_set_status(app, "Simulated capture stopped");
}

bool gui_simulated_is_running(gui_app_t *app) {
    return atomic_load(&app->sim_running);
}
