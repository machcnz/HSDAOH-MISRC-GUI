/*
 * MISRC GUI - Histogram Computation
 *
 * Computes amplitude distribution histogram from waveform samples.
 * Uses EMA (exponential moving average) to smooth results over time.
 *
 * This module handles the computational logic and can be reused
 * by other parts of the application independent of rendering.
 */

#ifndef GUI_HISTOGRAM_H
#define GUI_HISTOGRAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// Histogram Configuration Constants
//-----------------------------------------------------------------------------

// Default number of histogram bins (12-bit ADC = 4096 levels)
#define HISTOGRAM_DEFAULT_NUM_BINS 4096

// Maximum supported bins (memory limit)
#define HISTOGRAM_MAX_BINS 16384

// Minimum supported bins
#define HISTOGRAM_MIN_BINS 16

// EMA smoothing factor (0.0-1.0, lower = more smoothing/slower response)
#define HISTOGRAM_EMA_ALPHA 0.1f

//-----------------------------------------------------------------------------
// Histogram State Structure
//-----------------------------------------------------------------------------

typedef struct histogram_state {
    float *bins;                     // Dynamically allocated bin intensities (0.0-1.0)
    int num_bins;                    // Current number of bins
    float max_intensity;             // Current maximum bin value for normalization
    bool initialized;                // Initialization flag
} histogram_state_t;

//-----------------------------------------------------------------------------
// Histogram Lifecycle Functions
//-----------------------------------------------------------------------------

// Initialize histogram state with specified number of bins
// Returns true on success, false on allocation failure
bool histogram_init(histogram_state_t *state, int num_bins);

// Clear histogram (reset bins to zero without full reinitialization)
void histogram_clear(histogram_state_t *state);

// Cleanup histogram (free allocated memory)
void histogram_cleanup(histogram_state_t *state);

// Change the number of bins (reallocates and clears)
// Returns true on success, false on allocation failure
bool histogram_set_num_bins(histogram_state_t *state, int num_bins);

//-----------------------------------------------------------------------------
// Histogram Processing Functions
//-----------------------------------------------------------------------------

// Process raw int16_t samples and update histogram bins using EMA.
// Computes current frame histogram and blends with previous result.
//
// Parameters:
//   state: Histogram state structure
//   samples: Array of raw int16_t samples (full ADC range)
//   count: Number of samples to process
void histogram_process(histogram_state_t *state,
                       const int16_t *samples,
                       size_t count);

#endif // GUI_HISTOGRAM_H
