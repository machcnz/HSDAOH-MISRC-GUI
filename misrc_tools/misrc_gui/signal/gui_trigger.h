/*
 * MISRC GUI - Trigger Detection Module
 *
 * Centralized trigger detection for oscilloscope and CVBS decoder.
 * Supports edge triggers (rising/falling) and CVBS sync detection.
 */

#ifndef GUI_TRIGGER_H
#define GUI_TRIGGER_H

#include "../core/gui_app.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

//-----------------------------------------------------------------------------
// CVBS Signal Constants (for ~40 MSPS sample rate)
//-----------------------------------------------------------------------------

// H-sync pulse timing
#define CVBS_HSYNC_MIN_WIDTH  100    // Minimum H-sync samples (~2.5µs)
#define CVBS_HSYNC_MAX_WIDTH  280    // Maximum H-sync samples (~7µs)
#define CVBS_SYNC_MARGIN      0.25f  // Threshold at 25% above minimum

// V-sync broad pulse timing (for future use)
#define CVBS_VSYNC_MIN_WIDTH  800    // Minimum V-sync broad pulse (~20µs)
#define CVBS_VSYNC_MAX_WIDTH  1200   // Maximum V-sync broad pulse (~30µs)

// Line timing at 40 MSPS
#define CVBS_PAL_LINE_SAMPLES   2560  // 64µs PAL line
#define CVBS_NTSC_LINE_SAMPLES  2540  // 63.5µs NTSC line
#define CVBS_BACK_PORCH_SAMPLES 228   // ~5.7µs back porch
#define CVBS_ACTIVE_SAMPLES     2080  // ~52µs active video

//-----------------------------------------------------------------------------
// CVBS Signal Level Analysis
//-----------------------------------------------------------------------------

typedef struct {
    int16_t sig_min;           // Signal minimum (sync tip level)
    int16_t sig_max;           // Signal maximum (white level)
    int16_t range;             // sig_max - sig_min
    int16_t sync_threshold;    // Threshold for sync detection
    int16_t black_level;       // Calculated black level (~30% above min)
    int16_t white_level;       // Calculated white level (near max)
} cvbs_levels_t;

// Analyze CVBS signal levels from a buffer
// Samples every 8th value for efficiency
void trigger_analyze_cvbs_levels(const int16_t *buf, size_t count,
                                  cvbs_levels_t *levels);

//-----------------------------------------------------------------------------
// Edge Trigger Detection
//-----------------------------------------------------------------------------

// Find rising edge crossing level, starting search from min_index
// Returns sample index of crossing, or -1 if not found
ssize_t trigger_find_rising_edge(const int16_t *buf, size_t count,
                                  int16_t level, size_t min_index);

// Find falling edge crossing level, starting search from min_index
// Returns sample index of crossing, or -1 if not found
ssize_t trigger_find_falling_edge(const int16_t *buf, size_t count,
                                   int16_t level, size_t min_index);

//-----------------------------------------------------------------------------
// CVBS Sync Detection
//-----------------------------------------------------------------------------

// Find CVBS H-sync trigger point (rising edge at end of sync pulse)
// Uses adaptive threshold based on signal min/max
// Returns sample index of sync end, or -1 if not found
ssize_t trigger_find_cvbs_hsync(const int16_t *buf, size_t count,
                                 size_t min_index);

// Find CVBS H-sync with pre-analyzed levels (more efficient for repeated calls)
ssize_t trigger_find_cvbs_hsync_with_levels(const int16_t *buf, size_t count,
                                             size_t min_index,
                                             const cvbs_levels_t *levels);

// Find CVBS V-sync (broad pulse detection for frame sync)
// Returns sample index of V-sync start, or -1 if not found
ssize_t trigger_find_cvbs_vsync(const int16_t *buf, size_t count,
                                 size_t min_index,
                                 const cvbs_levels_t *levels);

//-----------------------------------------------------------------------------
// Main Trigger Dispatch
//-----------------------------------------------------------------------------

// Find trigger point based on channel configuration
// Dispatches to appropriate trigger function based on mode
// Uses trigger_mode_t enum from gui_app.h
ssize_t trigger_find(const int16_t *buf, size_t count,
                     int16_t level, trigger_mode_t mode,
                     bool enabled, size_t min_index);

// Convenience wrapper that takes channel_trigger_t pointer
ssize_t trigger_find_from_config(const int16_t *buf, size_t count,
                                  const channel_trigger_t *trig, size_t min_index);

#endif // GUI_TRIGGER_H
