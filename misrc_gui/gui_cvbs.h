/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#ifndef GUI_CVBS_H
#define GUI_CVBS_H

#include "gui_app.h"
#include "gui_trigger.h"
#include "raylib.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// Video Format Constants
//-----------------------------------------------------------------------------

// Video format detection
typedef enum {
    CVBS_FORMAT_UNKNOWN,
    CVBS_FORMAT_PAL,
    CVBS_FORMAT_NTSC,
    CVBS_FORMAT_SECAM
} cvbs_format_t;

// Chroma decoder selection
typedef enum {
    CVBS_CHROMA_DECODER_MONO = 0,     // luma-only (clean mono)
    CVBS_CHROMA_DECODER_SIMPLEPAL = 1 // Simple PAL colour decode (WIP)
} cvbs_chroma_decoder_t;

// Frame dimensions
#define CVBS_FRAME_WIDTH      720   // Standard horizontal resolution
#define CVBS_PAL_HEIGHT       576   // PAL (D1) active lines
#define CVBS_NTSC_HEIGHT      486   // NTSC (D1) active lines
#define CVBS_MAX_HEIGHT       576   // Maximum (PAL/SECAM)

// Line counts
#define CVBS_PAL_TOTAL_LINES  625
#define CVBS_NTSC_TOTAL_LINES 525
#define CVBS_PAL_ACTIVE_LINES 576
#define CVBS_NTSC_ACTIVE_LINES 480

// Timing at 40 MSPS (from gui_trigger.h constants)
#define CVBS_SAMPLES_PER_LINE_PAL   CVBS_PAL_LINE_SAMPLES    // 2560
#define CVBS_SAMPLES_PER_LINE_NTSC  CVBS_NTSC_LINE_SAMPLES   // 2540

//-----------------------------------------------------------------------------
// Decoder State Structures
//-----------------------------------------------------------------------------

// Frame synchronization state
typedef struct {
    cvbs_format_t format;          // Detected video format
    int total_lines;               // Total lines per frame (525/625)
    int active_lines;              // Active video lines (480/576)
    int current_line;              // Current line being decoded (0-based)
    int current_field;             // Current field (0=odd/first, 1=even/second)
    bool in_vsync;                 // Currently in vertical sync region
    bool frame_complete;           // A complete frame is ready for display
    int frames_decoded;            // Total frames decoded
} cvbs_frame_state_t;

// Software PLL state for H-sync tracking
typedef struct {
    // Core PLL state
    double phase;                  // Current phase within line (0 to line_period)
    double line_period;            // Nominal line period in samples (2560 PAL, 2540 NTSC)
    double freq_adjust;            // Fine frequency adjustment (±small value)

    // Phase correction
    double phase_error;            // Last phase error (for derivative term)
    double phase_integral;         // Integrated phase error (for I term)

    // Lock detection
    int good_sync_count;           // Consecutive syncs within tolerance
    int bad_sync_count;            // Consecutive syncs out of tolerance
    bool locked;                   // True if PLL is locked

    // Line tracking
    int current_line;              // Current line number in field (0-312 for PAL)
    int samples_in_line;           // Samples processed in current line
    size_t total_samples;          // Total samples processed (for debug)
} cvbs_pll_state_t;

// V-sync detection state (persistent across buffers)
typedef struct {
    int half_line_count;           // Count of consecutive half-line intervals
    int total_half_lines;          // Total half-line pulses in V-sync (for field detection)
    bool in_vsync;                 // Currently in V-sync region
} cvbs_vsync_state_t;

// Adaptive threshold state (percentile-based)
typedef struct {
    int16_t sync_tip;              // Estimated sync tip level (lowest ~5%)
    int16_t blanking;              // Estimated blanking level (~30%)
    int16_t black;                 // Estimated black level
    int16_t white;                 // Estimated white level (highest ~95%)
    int16_t threshold;             // Current sync threshold
    // Per-field accumulators (to avoid mid-field level changes causing shimmer)
    int16_t field_min;             // Minimum sample value in current field
    int16_t field_max;             // Maximum sample value in current field
} cvbs_adaptive_levels_t;

// Line buffer size - stores one complete line of samples for decoding
// We need this because H-sync edges may not align with buffer boundaries
#define CVBS_LINE_BUFFER_SIZE   3000  // Slightly more than max line period (2560 PAL)

// Main decoder structure
typedef struct cvbs_decoder {
    // Track whether we've received each field for the current frame
    bool field_ready[2];
    // Frame state
    cvbs_frame_state_t state;

    // Field buffers (grayscale, 720 x field_height each)
    // Each field buffer stores half the vertical resolution
    uint8_t *field_buffer[2];      // [0]=even/first field, [1]=odd/second field
    int field_height;              // Height of each field (288 PAL, 243 NTSC)

    // Deinterlaced frame buffer (720 x full_height)
    uint8_t *frame_buffer;         // Deinterlaced output frame
    int frame_width;               // Always CVBS_FRAME_WIDTH (720)
    int frame_height;              // Full frame height (576 PAL, 486 NTSC)

    // Double buffering for display
    uint8_t *display_buffer;       // Buffer currently being displayed
    bool display_ready;            // Display buffer has valid data

    // Chroma decoder selection
    cvbs_chroma_decoder_t chroma_decoder;

    // GPU resources for video display
    Image frame_image;
    Texture2D frame_texture;
    bool texture_valid;

    // Line buffer for cross-boundary line assembly
    int16_t *line_buffer;          // Buffer for current line samples
    int line_buffer_count;         // Samples currently in line buffer

    // Software PLL and sync detection (persistent across buffers)
    cvbs_adaptive_levels_t adaptive;   // Adaptive threshold tracking
    cvbs_pll_state_t pll;              // Software PLL for H-sync
    cvbs_vsync_state_t vsync;          // V-sync detection state

    // Lowpass filter state for sync detection (fixed-point Q16)
    int32_t lpf_state;             // IIR lowpass filter accumulator (Q16 fixed-point)
    int32_t lpf_output;            // Filtered signal value (unused, kept for ABI)

    // Edge detection state (persistent across buffers)
    bool last_filtered_above;      // Was last filtered sample above threshold?
    size_t global_sample_pos;      // Global sample position counter

    // H-sync pulse tracking
    bool in_hsync_pulse;           // Currently inside a potential H-sync pulse
    size_t hsync_pulse_start;      // Sample position where H-sync pulse started

    // V-sync detector state (separate from H-sync)
    size_t vsync_last_edge_pos;    // Position of last falling edge for V-sync

    // Legacy sync tracking (kept for compatibility)
    cvbs_levels_t levels;          // Current signal levels
    int lines_since_vsync;         // Lines counted since last V-sync detection

    // Statistics
    int sync_errors;               // Count of sync detection failures
    int format_changes;            // Count of format auto-detections

    // Debug statistics
    struct {
        int fields_decoded;            // Total fields successfully decoded
        int vsync_found;               // V-sync detection count
        int hsyncs_last_field;         // H-syncs found in last field
        int last_half_line_count;      // Half-line count from last V-sync
        int log_counter;               // Per-instance debug log counter
    } debug;
} cvbs_decoder_t;

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

// Initialize decoder with maximum dimensions
// Returns true on success, false on allocation failure
bool gui_cvbs_init(cvbs_decoder_t *decoder);

// Cleanup decoder resources
void gui_cvbs_cleanup(cvbs_decoder_t *decoder);

// Reset decoder state (clear frame, reset sync)
void gui_cvbs_reset(cvbs_decoder_t *decoder);

//-----------------------------------------------------------------------------
// Decoding
//-----------------------------------------------------------------------------

// Process a buffer of raw ADC samples
// Call this from the extraction thread with each new buffer
void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count);

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

// Render the decoded video frame
// Scales to fit within the given rectangle while maintaining aspect ratio
void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height);

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

// Set video format manually (PAL/NTSC)
// Uses cvbs_format_select_t from gui_app.h (CVBS_SELECT_PAL/CVBS_SELECT_NTSC)
void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select);

// Set chroma decoder mode
void gui_cvbs_set_chroma_decoder(cvbs_decoder_t *decoder, int chroma_decoder);

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

// Get detected format
cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder);

// Get format name string
const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder);

#endif // GUI_CVBS_H
