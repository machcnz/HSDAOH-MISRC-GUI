/*
 * MISRC GUI - FFT Line Spectrum Display with GPU Phosphor Persistence
 *
 * Computes FFT of resampled display waveform and displays as a line-based spectrum
 * using GPU-accelerated phosphor persistence (same as oscilloscope phosphor).
 *
 * Requires FFTW3 single-precision library (fftw3f).
 */

#ifndef GUI_FFT_H
#define GUI_FFT_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Include gui_app.h for waveform_sample_t definition
// (it's an anonymous struct so can't be forward-declared)
#include "../core/gui_app.h"
#include "gui_phosphor_rt.h"

//-----------------------------------------------------------------------------
// FFT Configuration Constants
//-----------------------------------------------------------------------------

// FFT size limits (dynamically sized based on available samples)
#define FFT_SIZE_MIN          128    // Minimum FFT size
#define FFT_SIZE_MAX          4096   // Maximum FFT size (must be <= DISPLAY_BUFFER_SIZE)

// dB range for magnitude normalization
#define FFT_DB_MIN           -100.0f   // Minimum dB (bottom of display)
#define FFT_DB_MAX             0.0f   // Maximum dB (top of display)

#define FFT_DECAY_RATE        0.6f   // Per-frame decay (higher = slower fade)
#define FFT_HIT_INCREMENT     0.1f    // Intensity added per FFT hit
#define FFT_BLOOM             1.0f    // Bloom Factor
#define FFT_EMA_ALPHA         0.70f

//-----------------------------------------------------------------------------
// FFT State Structure
//-----------------------------------------------------------------------------

typedef struct fft_state {
    // FFTW state (opaque pointers to avoid requiring fftw3.h in header)
    void *fftw_plan;           // fftwf_plan
    float *fftw_input;         // Input buffer (fft_size floats)
    void *fftw_output;         // Output buffer (fftwf_complex, fft_bins)

    // Current FFT size (power of 2, dynamically adjusted)
    int fft_size;              // Current FFT window size
    int fft_bins;              // Output bins (fft_size/2 + 1)
    int allocated_size;        // Size of allocated buffers (to know when to reallocate)

    // Hanning window coefficients (precomputed for current size)
    float *window;             // fft_size floats

    // Current FFT magnitude output (normalized 0-1)
    float *magnitude;          // fft_bins floats, latest FFT result normalized

    // GPU phosphor render texture (shared module)
    phosphor_rt_t phosphor;    // Reusable phosphor persistence effect

    // Indicates new FFT data is available for rendering
    bool data_ready;

    // Initialization state
    bool initialized;
} fft_state_t;

//-----------------------------------------------------------------------------
// FFT Availability Check
//-----------------------------------------------------------------------------

// Returns true if FFT support is compiled in (FFTW available)
bool gui_fft_available(void);

//-----------------------------------------------------------------------------
// FFT Lifecycle Management
//-----------------------------------------------------------------------------

// Initialize FFT state with FFTW plan and buffers
// Returns true on success, false on failure or if FFTW not available
bool gui_fft_init(fft_state_t *state);

// Clear FFT buffers (reset to empty state)
void gui_fft_clear(fft_state_t *state);

// Free all FFT resources
void gui_fft_cleanup(fft_state_t *state);

//-----------------------------------------------------------------------------
// FFT Processing (called from render thread with display samples)
//-----------------------------------------------------------------------------

// Compute FFT from resampled display waveform samples
// Called each frame with the current display buffer
// Parameters:
//   state: FFT state structure
//   samples: Resampled waveform samples (from display buffer)
//   count: Number of samples available
//   display_sample_rate: Effective sample rate of display data (pixels/sec)
void gui_fft_process_display(fft_state_t *state, const waveform_sample_t *samples,
                             size_t count, float display_sample_rate);

//-----------------------------------------------------------------------------
// FFT Rendering (called from main/render thread)
//-----------------------------------------------------------------------------

// Render FFT spectrum with GPU phosphor persistence
// Uses same shader system as oscilloscope phosphor
// Parameters:
//   state: FFT state structure
//   x, y: Screen position
//   width, height: Display size
//   display_sample_rate: Effective sample rate of display data (for freq axis)
//   color: Line color for spectrum
//   fonts: Font array (index 0 = Inter, index 1 = Space Mono), NULL for default
void gui_fft_render(fft_state_t *state, float x, float y,
                    float width, float height, float display_sample_rate,
                    Color color, Font *fonts);

//-----------------------------------------------------------------------------
// FFT Panel Rendering (for panel system integration)
//-----------------------------------------------------------------------------

// Render FFT as a panel, handling state lookup and unavailability messages.
// This is the high-level entry point for the panel dispatch system.
// Parameters:
//   app: Application state
//   channel: Channel number (0 or 1)
//   x, y: Panel position
//   w, h: Panel dimensions
//   state: Per-panel FFT state (or NULL to use app's FFT state)
//   color: Channel color (currently unused)
void gui_fft_render_panel(struct gui_app *app, int channel,
                          float x, float y, float w, float h,
                          void *state, Color color);

#endif // GUI_FFT_H
