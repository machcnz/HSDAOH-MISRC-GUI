/*
 * MISRC GUI - Oscilloscope and Trigger
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 * Uses libsoxr for high-quality waveform resampling
 */

#ifndef GUI_OSCILLOSCOPE_H
#define GUI_OSCILLOSCOPE_H

#include "../core/gui_app.h"
#include "raylib.h"
#include <sys/types.h>
#include "panel_interface.h"

//-----------------------------------------------------------------------------
// Panel Interface Registration
//-----------------------------------------------------------------------------

// Register the waveform line panel vtable with the panel registry.
void gui_waveform_line_panel_register(void);

// Register the waveform phosphor panel vtable with the panel registry.
void gui_waveform_phosphor_panel_register(void);

//-----------------------------------------------------------------------------
// Oscilloscope Phosphor Parameters
//-----------------------------------------------------------------------------

#define SCOPE_DECAY_RATE        0.75f   // Per-frame decay (higher = slower fade)
#define SCOPE_HIT_INCREMENT     0.25f    // Intensity added per waveform hit
#define SCOPE_BLOOM             2.0f    // Bloom factor

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

// Cleanup oscilloscope resources (static state only)
// Call on application exit
void gui_oscilloscope_cleanup(void);

// Cleanup per-channel resampler resources
// Call when app is being destroyed
void gui_oscilloscope_cleanup_resamplers(gui_app_t *app);

//-----------------------------------------------------------------------------
// Oscilloscope Rendering
//-----------------------------------------------------------------------------

// Draw grid for a single channel with amplitude scale ticks
// zoom_scale: samples per pixel, sample_rate: samples per second
// trigger_enabled: if true, use trigger_display_pos as t=0 reference
// trigger_display_pos: pixel position of trigger point (-1 if not triggered)
void draw_channel_grid(float x, float y, float width, float height,
                       const char *label, Color channel_color, bool show_grid,
                       float zoom_scale, uint32_t sample_rate,
                       bool trigger_enabled, int trigger_display_pos);

// Render waveform in line mode (simple connected line)
void render_waveform_line(gui_app_t *app, int channel,
                          float x, float y, float w, float h, Color color);

// Render waveform in phosphor mode (with persistence/decay effect)
void render_waveform_phosphor(gui_app_t *app, int channel,
                              float x, float y, float w, float h, Color color);

// Render a single channel's waveform with grid and trigger line
void render_oscilloscope_channel(gui_app_t *app, float x, float y, float width, float height,
                                  int channel, const char *label, Color channel_color);

// Note: Mouse interaction (click/drag, scroll) is now handled via panel vtable
// system - see panel_handle_all_clicks() and panel_handle_all_scrolls()

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

// Find first trigger point in sample buffer
// Returns sample index of trigger point, or -1 if no trigger found
ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig);

// Process a single channel: find trigger, resample, update display buffer
// Returns true if display was updated, false if held (Normal mode, no trigger)
// Note: trig is non-const because trigger_display_pos is updated
bool process_channel_display(gui_app_t *app, const int16_t *buf, size_t num_samples,
                             waveform_sample_t *display_buf, size_t *display_count,
                             channel_trigger_t *trig, int channel);

// Update display buffers for both channels (called from extraction thread)
void gui_oscilloscope_update_display(gui_app_t *app, const int16_t *buf_a,
                                      const int16_t *buf_b, size_t num_samples);

#endif // GUI_OSCILLOSCOPE_H
