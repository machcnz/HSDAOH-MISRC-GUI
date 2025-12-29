/*
 * MISRC GUI - Histogram Computation Implementation
 *
 * Computes amplitude distribution histogram from waveform samples
 * using EMA (exponential moving average) for smoothing.
 */

#include "gui_histogram.h"
#include <stdlib.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Histogram Lifecycle Functions
//-----------------------------------------------------------------------------

bool histogram_init(histogram_state_t *state, int num_bins) {
    if (!state) return false;

    // Clamp bin count to valid range
    if (num_bins < HISTOGRAM_MIN_BINS) num_bins = HISTOGRAM_MIN_BINS;
    if (num_bins > HISTOGRAM_MAX_BINS) num_bins = HISTOGRAM_MAX_BINS;

    // Allocate bins array
    state->bins = (float *)calloc(num_bins, sizeof(float));
    if (!state->bins) {
        state->initialized = false;
        return false;
    }

    state->num_bins = num_bins;
    state->max_intensity = 0.0f;
    state->initialized = true;
    return true;
}

void histogram_clear(histogram_state_t *state) {
    if (!state || !state->bins) return;

    memset(state->bins, 0, state->num_bins * sizeof(float));
    state->max_intensity = 0.0f;
}

void histogram_cleanup(histogram_state_t *state) {
    if (!state) return;

    if (state->bins) {
        free(state->bins);
        state->bins = NULL;
    }
    state->num_bins = 0;
    state->max_intensity = 0.0f;
    state->initialized = false;
}

bool histogram_set_num_bins(histogram_state_t *state, int num_bins) {
    if (!state) return false;

    // Clamp bin count to valid range
    if (num_bins < HISTOGRAM_MIN_BINS) num_bins = HISTOGRAM_MIN_BINS;
    if (num_bins > HISTOGRAM_MAX_BINS) num_bins = HISTOGRAM_MAX_BINS;

    // If same size, just clear
    if (state->bins && state->num_bins == num_bins) {
        histogram_clear(state);
        return true;
    }

    // Free old bins if any
    if (state->bins) {
        free(state->bins);
        state->bins = NULL;
    }

    // Allocate new bins
    state->bins = (float *)calloc(num_bins, sizeof(float));
    if (!state->bins) {
        state->num_bins = 0;
        state->initialized = false;
        return false;
    }

    state->num_bins = num_bins;
    state->max_intensity = 0.0f;
    state->initialized = true;
    return true;
}

//-----------------------------------------------------------------------------
// Histogram Processing Functions
//-----------------------------------------------------------------------------

void histogram_process(histogram_state_t *state,
                       const int16_t *samples,
                       size_t count) {
    if (!state || !state->initialized || !state->bins || !samples || count == 0) return;

    const int num_bins = state->num_bins;
    const float alpha = HISTOGRAM_EMA_ALPHA;
    const float one_minus_alpha = 1.0f - alpha;

    // Step 1: Build current frame's histogram (count samples per bin)
    // Use a temporary buffer on the stack for small bin counts, heap for large
    float *current_frame;
    float stack_buffer[1024];
    bool use_heap = (num_bins > 1024);

    if (use_heap) {
        current_frame = (float *)calloc(num_bins, sizeof(float));
        if (!current_frame) return;
    } else {
        current_frame = stack_buffer;
        memset(current_frame, 0, num_bins * sizeof(float));
    }

    // Count samples into bins
    // Raw int16_t samples are 12-bit ADC values: range -2048 to +2047
    // Map to bin index: 0 to num_bins-1
    const float scale = (float)num_bins / 4096.0f;

    for (size_t i = 0; i < count; i++) {
        // Shift from [-2048, 2047] to [0, 4095] then scale to bin index
        // Clamp input to 12-bit range first
        int sample = samples[i];
        if (sample < -2048) sample = -2048;
        if (sample > 2047) sample = 2047;

        int bin_index = (int)(((float)(sample + 2048)) * scale);

        // Clamp index to valid range
        if (bin_index < 0) bin_index = 0;
        if (bin_index >= num_bins) bin_index = num_bins - 1;

        current_frame[bin_index] += 1.0f;
    }

    // Step 2: Normalize current frame by sample count
    float inv_count = 1.0f / (float)count;
    for (int i = 0; i < num_bins; i++) {
        current_frame[i] *= inv_count;
    }

    // Step 3: Apply EMA: new_value = alpha * current + (1 - alpha) * previous
    for (int i = 0; i < num_bins; i++) {
        state->bins[i] = alpha * current_frame[i] + one_minus_alpha * state->bins[i];
    }

    if (use_heap) {
        free(current_frame);
    }

    // Step 4: Find maximum intensity for normalization during rendering
    float max_val = 0.0f;
    for (int i = 0; i < num_bins; i++) {
        if (state->bins[i] > max_val) {
            max_val = state->bins[i];
        }
    }
    state->max_intensity = max_val;
}
