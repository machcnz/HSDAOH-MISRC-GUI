/*
 * MISRC GUI - Trigger Detection Module
 *
 * Centralized trigger detection for oscilloscope and CVBS decoder.
 */

#include "gui_trigger.h"

//-----------------------------------------------------------------------------
// CVBS Signal Level Analysis
//-----------------------------------------------------------------------------

void trigger_analyze_cvbs_levels(const int16_t *buf, size_t count,
                                  cvbs_levels_t *levels) {
    if (!buf || count == 0 || !levels) return;

    // Sample every 8th value for efficiency
    int16_t sig_min = buf[0];
    int16_t sig_max = buf[0];

    for (size_t i = 0; i < count; i += 8) {
        if (buf[i] < sig_min) sig_min = buf[i];
        if (buf[i] > sig_max) sig_max = buf[i];
    }

    levels->sig_min = sig_min;
    levels->sig_max = sig_max;
    levels->range = sig_max - sig_min;

    // Calculate derived levels
    // Sync threshold: 25% above minimum (into sync pulse region)
    levels->sync_threshold = sig_min + (int16_t)((float)levels->range * CVBS_SYNC_MARGIN);

    // Black level: approximately 30% of range (blanking level)
    levels->black_level = sig_min + (int16_t)((float)levels->range * 0.30f);

    // White level: approximately 95% of range
    levels->white_level = sig_min + (int16_t)((float)levels->range * 0.95f);
}

//-----------------------------------------------------------------------------
// Edge Trigger Detection
//-----------------------------------------------------------------------------

ssize_t trigger_find_rising_edge(const int16_t *buf, size_t count,
                                  int16_t level, size_t min_index) {
    if (!buf || count < 2) return -1;

    size_t start = (min_index > 1) ? min_index : 1;

    for (size_t i = start; i < count; i++) {
        int16_t prev = buf[i - 1];
        int16_t curr = buf[i];

        // Rising edge: cross from below to at-or-above level
        if (prev < level && curr >= level) {
            return (ssize_t)i;
        }
    }

    return -1;
}

ssize_t trigger_find_falling_edge(const int16_t *buf, size_t count,
                                   int16_t level, size_t min_index) {
    if (!buf || count < 2) return -1;

    size_t start = (min_index > 1) ? min_index : 1;

    for (size_t i = start; i < count; i++) {
        int16_t prev = buf[i - 1];
        int16_t curr = buf[i];

        // Falling edge: cross from above to at-or-below level
        if (prev > level && curr <= level) {
            return (ssize_t)i;
        }
    }

    return -1;
}

//-----------------------------------------------------------------------------
// CVBS Sync Detection
//-----------------------------------------------------------------------------

ssize_t trigger_find_cvbs_hsync_with_levels(const int16_t *buf, size_t count,
                                             size_t min_index,
                                             const cvbs_levels_t *levels) {
    if (!buf || !levels || count < CVBS_HSYNC_MAX_WIDTH + 2) return -1;
    if (levels->range < 100) return -1;  // Signal too weak

    int16_t sync_threshold = levels->sync_threshold;
    size_t start = (min_index > 1) ? min_index : 1;
    size_t search_limit = count - CVBS_HSYNC_MAX_WIDTH;

    for (size_t i = start; i < search_limit; i++) {
        // Check for falling edge into sync (entering sync pulse)
        if (buf[i - 1] > sync_threshold && buf[i] <= sync_threshold) {
            // Measure how long signal stays below threshold
            size_t sync_end = i;
            while (sync_end < count && buf[sync_end] <= sync_threshold) {
                sync_end++;
            }

            size_t pulse_width = sync_end - i;

            // Valid H-sync pulse width? (not too short = noise, not too long = vsync)
            if (pulse_width >= CVBS_HSYNC_MIN_WIDTH && pulse_width <= CVBS_HSYNC_MAX_WIDTH) {
                // Trigger at rising edge (end of sync pulse = start of back porch)
                return (ssize_t)sync_end;
            }

            // Skip past this pulse for next iteration
            i = sync_end;
        }
    }

    return -1;
}

ssize_t trigger_find_cvbs_hsync(const int16_t *buf, size_t count,
                                 size_t min_index) {
    if (!buf || count < CVBS_HSYNC_MAX_WIDTH + 2) return -1;

    // Analyze signal levels first
    cvbs_levels_t levels;
    trigger_analyze_cvbs_levels(buf, count, &levels);

    return trigger_find_cvbs_hsync_with_levels(buf, count, min_index, &levels);
}

ssize_t trigger_find_cvbs_vsync(const int16_t *buf, size_t count,
                                 size_t min_index,
                                 const cvbs_levels_t *levels) {
    if (!buf || !levels || count < CVBS_VSYNC_MAX_WIDTH + 2) return -1;
    if (levels->range < 100) return -1;  // Signal too weak

    int16_t sync_threshold = levels->sync_threshold;
    size_t start = (min_index > 1) ? min_index : 1;
    size_t search_limit = count - CVBS_VSYNC_MAX_WIDTH;

    for (size_t i = start; i < search_limit; i++) {
        // Check for falling edge into sync
        if (buf[i - 1] > sync_threshold && buf[i] <= sync_threshold) {
            // Measure pulse width
            size_t sync_end = i;
            while (sync_end < count && buf[sync_end] <= sync_threshold) {
                sync_end++;
            }

            size_t pulse_width = sync_end - i;

            // V-sync broad pulse? (longer than H-sync)
            if (pulse_width >= CVBS_VSYNC_MIN_WIDTH && pulse_width <= CVBS_VSYNC_MAX_WIDTH) {
                // Return start of V-sync pulse
                return (ssize_t)i;
            }

            // Skip past this pulse
            i = sync_end;
        }
    }

    return -1;
}

//-----------------------------------------------------------------------------
// Main Trigger Dispatch
//-----------------------------------------------------------------------------

ssize_t trigger_find(const int16_t *buf, size_t count,
                     int16_t level, trigger_mode_t mode,
                     bool enabled, size_t min_index) {
    if (!enabled || !buf || count < 2) return -1;

    switch (mode) {
        case TRIGGER_MODE_RISING:
            return trigger_find_rising_edge(buf, count, level, min_index);

        case TRIGGER_MODE_FALLING:
            return trigger_find_falling_edge(buf, count, level, min_index);

        case TRIGGER_MODE_CVBS_HSYNC:
            return trigger_find_cvbs_hsync(buf, count, min_index);

        default:
            return trigger_find_rising_edge(buf, count, level, min_index);
    }
}

ssize_t trigger_find_from_config(const int16_t *buf, size_t count,
                                  const channel_trigger_t *trig, size_t min_index) {
    if (!trig) return -1;
    return trigger_find(buf, count, trig->level, trig->trigger_mode,
                        trig->enabled, min_index);
}
