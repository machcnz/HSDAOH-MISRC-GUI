/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#ifndef GUI_CVBS_H
#define GUI_CVBS_H

#include "../core/gui_app.h"
#include "gui_trigger.h"
#include "../visualization/panel_interface.h"
#include "raylib.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the CVBS panel vtable with the panel registry.
// Call this once at startup.
void gui_cvbs_panel_register(void);

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

// Frame dimensions
#define CVBS_FRAME_WIDTH      720   // Active-area horizontal resolution
#define CVBS_PAL_HEIGHT       576   // 625-line PAL/SECAM active lines
#define CVBS_NTSC_HEIGHT      486   // 525-line NTSC active lines
#define CVBS_PAL_FULL_WIDTH_4FSC   1135  // Full PAL line width at 4fsc
#define CVBS_NTSC_FULL_WIDTH_4FSC   910  // Full NTSC line width at 4fsc
#define CVBS_PAL_FULL_HEIGHT        625  // Full PAL frame lines
#define CVBS_NTSC_FULL_HEIGHT       525  // Full NTSC frame lines
#define CVBS_MAX_WIDTH         CVBS_PAL_FULL_WIDTH_4FSC
#define CVBS_MAX_HEIGHT        CVBS_PAL_FULL_HEIGHT

// Line counts (used for 625-line / 525-line mode naming)
#define CVBS_PAL_TOTAL_LINES  625   // 625-line PAL/SECAM
#define CVBS_NTSC_TOTAL_LINES 525   // 525-line NTSC
#define CVBS_PAL_ACTIVE_LINES 576
#define CVBS_NTSC_ACTIVE_LINES 486

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
    int active_lines;              // Active video lines (486/576)
    int current_line;              // Current line being decoded (0-based)
    int current_field;             // Current field (0=odd/first, 1=even/second)
    bool in_vsync;                 // Currently in vertical sync region
    bool frame_complete;           // A complete frame is ready for display
    int frames_decoded;            // Total frames decoded
} cvbs_frame_state_t;

// OSD mode controls how much status text is overlaid on the video.
typedef enum {
    CVBS_OSD_OFF     = 0,  // no overlay
    CVBS_OSD_MINIMAL = 1,  // deinterlacer + timing
    CVBS_OSD_STATS   = 2,  // detailed statistics
} cvbs_osd_mode_t;

// Frame view mode controls active-area vs full-frame geometry.
typedef enum {
    CVBS_FRAME_VIEW_ACTIVE = 0,  // Active video only (720x576 / 720x486)
    CVBS_FRAME_VIEW_FULL   = 1,  // Full line/frame geometry (4fsc width, 625/525 lines)
} cvbs_frame_view_t;

// Video level mode controls whether black/white/sync levels keep adapting
// each field (AUTO) or are held fixed after lock (FIXED).
typedef enum {
    CVBS_LEVEL_MODE_AUTO  = 0,
    CVBS_LEVEL_MODE_FIXED = 1,
} cvbs_level_mode_t;
// Sync mode controls whether H/V lock timing keeps adapting (AUTO)
// or uses a fixed lock profile once stable (FIXED).
typedef enum {
    CVBS_SYNC_MODE_AUTO  = 0,
    CVBS_SYNC_MODE_FIXED = 1,
} cvbs_sync_mode_t;

// Decoder family used by preview path.
// CVBS preserves existing behavior; TAPE enables tape-oriented preview tuning.
typedef enum {
    CVBS_DECODER_MODE_CVBS = 0,
    CVBS_DECODER_MODE_TAPE = 1,
} cvbs_decoder_mode_t;

// Tape family selector (aligned with tape-decode-rs family naming where practical).
typedef enum {
    CVBS_TAPE_FORMAT_VHS = 0,
    CVBS_TAPE_FORMAT_SVHS,
    CVBS_TAPE_FORMAT_BETAMAX,
    CVBS_TAPE_FORMAT_VIDEO8,
    CVBS_TAPE_FORMAT_HI8,
    CVBS_TAPE_FORMAT_UMATIC,
} cvbs_tape_format_t;

// Tape speed mode selector.
typedef enum {
    CVBS_TAPE_MODE_SP = 0,
    CVBS_TAPE_MODE_LP,
    CVBS_TAPE_MODE_ELP,
} cvbs_tape_mode_t;

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

// Histogram-based level detection buffer size
#define CVBS_LEVEL_SAMPLE_BUFFER_SIZE  16384  // ~0.4ms of samples at 40 MSPS

// Adaptive threshold state (histogram-based)
typedef struct {
    int16_t sync_tip;              // Estimated sync tip level (lowest ~5%)
    int16_t blanking;              // Estimated blanking level (~30%)
    int16_t black;                 // Estimated black level
    int16_t white;                 // Estimated white level (highest ~95%)
    int16_t threshold;             // Current sync threshold
    // Per-field sample buffer for histogram analysis
    int16_t *level_sample_buf;     // Buffer for histogram samples (allocated)
    size_t level_sample_count;     // Current count of samples in buffer
    int subsample_counter;         // Counter for subsampling
} cvbs_adaptive_levels_t;

// Line buffer size - stores one complete line of samples for decoding
// We need this because H-sync edges may not align with buffer boundaries
#define CVBS_LINE_BUFFER_SIZE   3000  // Slightly more than max line period (2560 PAL)

// Main decoder structure
typedef struct cvbs_decoder {
    // Track whether we've received each field for the current frame
    bool field_ready[2];
    uint64_t field_sequence;          // Monotonic sequence number for completed fields
    uint64_t field_last_update_seq[2];// Last sequence value written to each field buffer
    // Frame state
    cvbs_frame_state_t state;

    // Decoder configuration
    cvbs_decoder_mode_t decoder_mode;   // CVBS vs Tape decoder path
    cvbs_tape_format_t tape_format;     // Tape family preset
    cvbs_tape_mode_t tape_mode;         // SP/LP/ELP
    cvbs_osd_mode_t osd_mode;           // Off / Minimal / Stats
    cvbs_frame_view_t frame_view_mode;  // Active-only vs full-frame view
    cvbs_level_mode_t level_mode;       // Auto-adaptive vs fixed video levels
    cvbs_sync_mode_t sync_mode;         // Auto-adaptive vs fixed H/V sync behavior
    bool fixed_levels_valid;            // Fixed mode has latched levels from signal
    int16_t fixed_black_level;          // Latched black level used in fixed mode
    int16_t fixed_white_level;          // Latched white level used in fixed mode
    bool fixed_sync_locked;             // FIXED mode has acquired lock and frozen line period
    double fixed_line_period;           // Frozen line period used while fixed sync lock is active
    int fixed_bad_syncs;                // Consecutive bad H-syncs while fixed lock is active
    int16_t fixed_sync_threshold;       // Latched sync threshold used while fixed lock is active

    // Field buffers (grayscale, 720 x field_height each)
    // Each field buffer stores half the vertical resolution
    uint8_t *field_buffer[2];      // [0]=even/first field, [1]=odd/second field
    int field_height;              // Height of each field (288 PAL, 243 NTSC)

    // Deinterlaced frame buffer (720 x full_height)
    uint8_t *frame_buffer;         // Deinterlaced output frame
    int frame_width;               // Active: 720, Full: 910/1135 (NTSC/PAL 4fsc)
    int frame_height;              // Active: 486/576, Full: 525/625

    // Double buffering for thread-safe display
    // Display thread writes to back buffer, render thread reads from front
    uint8_t *display_front;        // Front buffer - read by render thread
    uint8_t *display_back;         // Back buffer - written by display thread
    atomic_int display_ready;      // 1 when back buffer has new frame to swap

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

    // Edge detection state (persistent across buffers)
    bool last_filtered_above;      // Was last filtered sample above threshold?
    size_t global_sample_pos;      // Global sample position counter

    // H-sync pulse tracking
    bool in_hsync_pulse;           // Currently inside a potential H-sync pulse
    size_t hsync_pulse_start;      // Sample position where H-sync pulse started

    // V-sync detector state (separate from H-sync)
    size_t vsync_last_edge_pos;    // Position of last falling edge for V-sync

    // Sync tracking state
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
        bool mode_trace_initialized;   // CVBS mode transition trace initialized
        int last_decoder_mode;         // Last observed cvbs_decoder_mode_t value
        int last_tape_format;          // Last observed cvbs_tape_format_t value
        int last_tape_mode;            // Last observed cvbs_tape_mode_t value
        int last_level_mode;           // Last observed cvbs_level_mode_t value
        int last_sync_mode;            // Last observed cvbs_sync_mode_t value
        bool last_fixed_sync_locked;   // Last observed fixed-sync lock state
        bool last_fixed_levels_valid;  // Last observed fixed-level latch state
    } debug;

    // UI overlay state (for panel-top controls)
    struct {
        // Buttons (right-aligned vertical): [Decoder], [OSD], [Frame], [Levels], [Sync], [System]
        Rectangle decoder_btn_rect;    // Hit box for decoder mode button
        Rectangle osd_btn_rect;        // Hit box for OSD mode button
        Rectangle frame_btn_rect;      // Hit box for frame mode button
        Rectangle level_btn_rect;      // Hit box for levels mode button
        Rectangle sync_btn_rect;       // Hit box for sync mode button
        Rectangle system_btn_rect;     // Hit box for system selector button
        Rectangle tape_btn_rect;       // Hit box for tape family selector button
        Rectangle tape_format_btn_rect;// Hit box for tape format selector button
        Rectangle tape_mode_btn_rect;  // Hit box for tape mode cycle button

        // Dropdown options for system selector (0=625-line PAL/SECAM, 1=525-line NTSC)
        Rectangle system_options_rect[2];
        // Dropdown options for tape family selector.
        Rectangle tape_options_rect[6];
        // Dropdown options for tape format selector (PAL/NTSC/SECAM).
        Rectangle tape_format_options_rect[3];

        bool is_visible;               // Whether overlay was rendered (for click detection)
        bool system_dropdown_open;     // Whether system dropdown is currently open
        bool tape_dropdown_open;       // Whether tape family dropdown is currently open
        bool tape_format_dropdown_open;// Whether tape format dropdown is currently open
        int selected_system;           // 0=625-line PAL/SECAM, 1=525-line NTSC
        int selected_tape_format;      // 0=PAL, 1=NTSC, 2=SECAM
    } overlay;
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

// Swap buffers if new frame available (called from render thread before rendering)
// Copies back buffer to front buffer if display_ready flag is set.
void gui_cvbs_swap_buffers(cvbs_decoder_t *decoder);

// Render the decoded video frame
// Scales to fit within the given rectangle while maintaining aspect ratio
// Call gui_cvbs_swap_buffers() before this to get latest frame.
void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height);

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

// Set video format manually (PAL/NTSC)
// Uses cvbs_format_select_t from gui_app.h (CVBS_SELECT_PAL/CVBS_SELECT_NTSC)
void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select);

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

// Get detected format
cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder);

// Get format name string
const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder);

#endif // GUI_CVBS_H
