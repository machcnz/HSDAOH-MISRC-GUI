/*
 * MISRC GUI - Histogram Computation Implementation
 *
 * Computes amplitude distribution histogram from sample data
 * using configurable sample range and optional EMA smoothing.
 *
 * This module is purely computational - it has no knowledge of
 * the application structure, panels, or rendering.
 */

#include "gui_histogram.h"
#include <stdlib.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Configuration Helpers
//-----------------------------------------------------------------------------

void histogram_config_init_default(histogram_config_t *config) {
    if (!config) return;

    config->sample_min = HISTOGRAM_DEFAULT_SAMPLE_MIN;
    config->sample_max = HISTOGRAM_DEFAULT_SAMPLE_MAX;
    config->ema_enabled = true;
    config->ema_alpha = HISTOGRAM_DEFAULT_EMA_ALPHA;
}

//-----------------------------------------------------------------------------
// Histogram Lifecycle Functions
//-----------------------------------------------------------------------------

bool histogram_init(histogram_state_t *state, int num_bins) {
    histogram_config_t default_config;
    histogram_config_init_default(&default_config);
    return histogram_init_with_config(state, num_bins, &default_config);
}

bool histogram_init_with_config(histogram_state_t *state, int num_bins,
                                const histogram_config_t *config) {
    if (!state) return false;

    memset(state, 0, sizeof(*state));

    // Clamp bin count to valid range
    if (num_bins < HISTOGRAM_MIN_BINS) num_bins = HISTOGRAM_MIN_BINS;
    if (num_bins > HISTOGRAM_MAX_BINS) num_bins = HISTOGRAM_MAX_BINS;

    // Allocate bins array
    state->bins = (float *)calloc(num_bins, sizeof(float));
    if (!state->bins) {
        return false;
    }

    // Allocate scratch buffer for per-frame integer counts
    state->scratch = (uint32_t *)calloc(num_bins, sizeof(uint32_t));
    if (!state->scratch) {
        free(state->bins);
        state->bins = NULL;
        return false;
    }

    state->num_bins = num_bins;
    state->max_intensity = 0.0f;

    // Copy configuration
    if (config) {
        state->config = *config;
    } else {
        histogram_config_init_default(&state->config);
    }

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
    if (state->scratch) {
        free(state->scratch);
        state->scratch = NULL;
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

    // Free old buffers if any
    if (state->bins) {
        free(state->bins);
        state->bins = NULL;
    }
    if (state->scratch) {
        free(state->scratch);
        state->scratch = NULL;
    }

    // Allocate new bins
    state->bins = (float *)calloc(num_bins, sizeof(float));
    if (!state->bins) {
        state->num_bins = 0;
        state->initialized = false;
        return false;
    }

    // Allocate new scratch buffer
    state->scratch = (uint32_t *)calloc(num_bins, sizeof(uint32_t));
    if (!state->scratch) {
        free(state->bins);
        state->bins = NULL;
        state->num_bins = 0;
        state->initialized = false;
        return false;
    }

    state->num_bins = num_bins;
    state->max_intensity = 0.0f;
    state->initialized = true;
    return true;
}

void histogram_set_config(histogram_state_t *state, const histogram_config_t *config) {
    if (!state || !config) return;

    state->config = *config;
    histogram_clear(state);  // Clear bins when config changes
}

const histogram_config_t* histogram_get_config(const histogram_state_t *state) {
    if (!state) return NULL;
    return &state->config;
}

//-----------------------------------------------------------------------------
// Histogram Processing Functions
//-----------------------------------------------------------------------------

void histogram_process(histogram_state_t *state,
                       const int16_t *samples,
                       size_t count) {
    if (!state || !state->initialized || !state->bins || !state->scratch ||
        !samples || count == 0) return;

    const int num_bins = state->num_bins;
    const histogram_config_t *cfg = &state->config;

    // Validate sample range configuration
    const int sample_min = cfg->sample_min;
    const int sample_max = cfg->sample_max;
    if (sample_max <= sample_min) return;  // Invalid config

    const int sample_range = sample_max - sample_min + 1;  // e.g., 4096 for 12-bit

    // EMA parameters (only used if enabled)
    const bool ema_enabled = cfg->ema_enabled;
    const float alpha = cfg->ema_alpha;
    const float one_minus_alpha = 1.0f - alpha;

    // Step 1: Clear scratch buffer and count samples into bins using integers
    uint32_t *counts = state->scratch;
    memset(counts, 0, num_bins * sizeof(uint32_t));

    // Map sample value to bin index: 0 to num_bins-1
    const float scale = (float)num_bins / (float)sample_range;

    for (size_t i = 0; i < count; i++) {
        // Clamp sample to configured range
        int sample = samples[i];
        if (sample < sample_min) sample = sample_min;
        if (sample > sample_max) sample = sample_max;

        // Map [sample_min, sample_max] to [0, num_bins-1]
        int bin_index = (int)(((float)(sample - sample_min)) * scale);

        // Clamp index to valid range (handles edge case at sample_max)
        if (bin_index >= num_bins) bin_index = num_bins - 1;

        counts[bin_index]++;
    }

    // Step 2: Normalize and apply EMA or direct copy
    const float inv_count = 1.0f / (float)count;

    if (ema_enabled) {
        // EMA: new_value = alpha * current + (1 - alpha) * previous
        for (int i = 0; i < num_bins; i++) {
            float current_val = (float)counts[i] * inv_count;
            state->bins[i] = alpha * current_val + one_minus_alpha * state->bins[i];
        }
    } else {
        // No smoothing: directly normalize counts to bins
        for (int i = 0; i < num_bins; i++) {
            state->bins[i] = (float)counts[i] * inv_count;
        }
    }

    // Step 3: Find maximum intensity for normalization during rendering
    float max_val = 0.0f;
    for (int i = 0; i < num_bins; i++) {
        if (state->bins[i] > max_val) {
            max_val = state->bins[i];
        }
    }
    state->max_intensity = max_val;
}
