/*
 * MISRC GUI - Histogram Computation
 *
 * Computes amplitude distribution histogram from sample data.
 * Uses EMA (exponential moving average) for optional temporal smoothing.
 *
 * This module handles the computational logic and is designed to be
 * reusable by other parts of the application independent of rendering.
 * The histogram core is configurable for different sample formats and
 * smoothing parameters at runtime.
 */

#ifndef GUI_HISTOGRAM_H
#define GUI_HISTOGRAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// Histogram Configuration Constants (defaults)
//-----------------------------------------------------------------------------

// Default number of histogram bins (12-bit ADC = 4096 levels)
#define HISTOGRAM_DEFAULT_NUM_BINS 4096

// Maximum supported bins (memory limit)
#define HISTOGRAM_MAX_BINS 4096

// Minimum supported bins
#define HISTOGRAM_MIN_BINS 16

// Default EMA smoothing factor (0.0-1.0, lower = more smoothing)
#define HISTOGRAM_DEFAULT_EMA_ALPHA 0.5f

// Default sample range for 12-bit ADC: [-2048, +2047]
#define HISTOGRAM_DEFAULT_SAMPLE_MIN -2048
#define HISTOGRAM_DEFAULT_SAMPLE_MAX 2047

//-----------------------------------------------------------------------------
// Histogram Configuration Structure (runtime parameters)
//-----------------------------------------------------------------------------

typedef struct histogram_config {
    // Sample value range (inclusive)
    int sample_min;                  // Minimum expected sample value
    int sample_max;                  // Maximum expected sample value

    // EMA smoothing
    bool ema_enabled;                // Enable temporal smoothing
    float ema_alpha;                 // Smoothing factor (0.0-1.0, higher = faster response)
} histogram_config_t;

//-----------------------------------------------------------------------------
// Histogram State Structure
//-----------------------------------------------------------------------------

typedef struct histogram_state {
    // Bin data
    float *bins;                     // Dynamically allocated bin intensities (0.0-1.0)
    uint32_t *scratch;               // Scratch buffer for per-frame integer counts
    int num_bins;                    // Current number of bins

    // Runtime configuration
    histogram_config_t config;       // Sample range and EMA parameters

    // Computed values
    float max_intensity;             // Current maximum bin value for normalization
    bool initialized;                // Initialization flag
} histogram_state_t;

//-----------------------------------------------------------------------------
// Configuration Helpers
//-----------------------------------------------------------------------------

// Initialize config with default values (12-bit ADC, EMA enabled)
void histogram_config_init_default(histogram_config_t *config);

//-----------------------------------------------------------------------------
// Histogram Lifecycle Functions
//-----------------------------------------------------------------------------

// Initialize histogram state with specified number of bins and default config
// Returns true on success, false on allocation failure
bool histogram_init(histogram_state_t *state, int num_bins);

// Initialize histogram with custom configuration
// Returns true on success, false on allocation failure
bool histogram_init_with_config(histogram_state_t *state, int num_bins,
                                const histogram_config_t *config);

// Clear histogram (reset bins to zero without full reinitialization)
void histogram_clear(histogram_state_t *state);

// Cleanup histogram (free allocated memory)
void histogram_cleanup(histogram_state_t *state);

// Change the number of bins (reallocates and clears)
// Returns true on success, false on allocation failure
bool histogram_set_num_bins(histogram_state_t *state, int num_bins);

// Update runtime configuration (does not reallocate, clears bins)
void histogram_set_config(histogram_state_t *state, const histogram_config_t *config);

// Get current configuration
const histogram_config_t* histogram_get_config(const histogram_state_t *state);

//-----------------------------------------------------------------------------
// Histogram Processing Functions
//-----------------------------------------------------------------------------

// Process int16_t samples and update histogram bins.
// Uses the configured sample range and EMA settings.
//
// Parameters:
//   state: Histogram state structure (must be initialized)
//   samples: Array of int16_t samples
//   count: Number of samples to process
//
// Note: Call this from a processing thread, NOT the render thread.
void histogram_process(histogram_state_t *state,
                       const int16_t *samples,
                       size_t count);

#endif // GUI_HISTOGRAM_H
