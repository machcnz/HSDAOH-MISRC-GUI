/*
 * MISRC GUI - Histogram Panel Rendering
 *
 * Renders amplitude histogram as a panel view with heatmap coloring.
 * Integrates with the panel dispatch system.
 */

#ifndef GUI_HISTOGRAM_PANEL_H
#define GUI_HISTOGRAM_PANEL_H

#include "raylib.h"
#include "../signal/gui_histogram.h"

// Forward declaration
struct gui_app;

//-----------------------------------------------------------------------------
// Histogram Panel Rendering
//-----------------------------------------------------------------------------

// Render histogram as a panel view.
// This is the entry point for the panel dispatch system.
//
// Parameters:
//   app: Application state (for accessing display samples)
//   channel: Channel number (0 = A, 1 = B)
//   x, y: Panel position (top-left corner)
//   w, h: Panel dimensions
//   state: Per-panel histogram state (histogram_state_t*)
//   color: Channel color (used for styling)
void gui_histogram_render_panel(struct gui_app *app, int channel,
                                float x, float y, float w, float h,
                                void *state, Color color);

//-----------------------------------------------------------------------------
// Histogram Panel Overlay Interaction
//-----------------------------------------------------------------------------

// Handle click on histogram panel overlay (bins selector dropdown)
// Returns true if click was handled, false otherwise
bool gui_histogram_overlay_handle_click(struct gui_app *app, int channel,
                                        Vector2 mouse_pos);

//-----------------------------------------------------------------------------
// Histogram Panel Processing (called from display thread)
//-----------------------------------------------------------------------------

// Process histograms for all active histogram panels.
// Iterates through panel configs and calls histogram_process for each.
// Call this from the display thread with the raw sample data.
//
// Parameters:
//   app: Application state (contains panel configurations)
//   samples_a: Channel A samples
//   samples_b: Channel B samples
//   count: Number of samples per channel
void gui_histogram_panel_process_all(struct gui_app *app,
                                     const int16_t *samples_a,
                                     const int16_t *samples_b,
                                     size_t count);

#endif // GUI_HISTOGRAM_PANEL_H
