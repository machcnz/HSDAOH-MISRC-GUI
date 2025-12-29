/*
 * MISRC GUI - Panel Abstraction Implementation
 *
 * Provides render dispatch and state management for panel views.
 * This file orchestrates panel rendering by delegating to specialized
 * panel render functions in their respective modules.
 */

#include "gui_panel.h"
#include "panel_interface.h"
#include "../core/gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_fft.h"
#include "gui_histogram_panel.h"
#include "../signal/gui_cvbs.h"
#include "../signal/gui_histogram.h"
#include "../ui/gui_ui.h"
#include <stdio.h>
#include <stdlib.h>

//-----------------------------------------------------------------------------
// View Type Names (for UI dropdowns)
//-----------------------------------------------------------------------------

static const char* s_view_names[] = {
    [PANEL_VIEW_WAVEFORM_LINE] = "Line",
    [PANEL_VIEW_WAVEFORM_PHOSPHOR] = "Phosphor",
    [PANEL_VIEW_FFT] = "FFT",
    [PANEL_VIEW_CVBS] = "CVBS",
    [PANEL_VIEW_HISTOGRAM] = "Histogram",
};

const char* panel_view_type_name(panel_view_type_t type) {
    if (type < PANEL_VIEW_COUNT) return s_view_names[type];
    return "Unknown";
}

bool panel_view_type_available(panel_view_type_t type) {
    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
        case PANEL_VIEW_CVBS:
        case PANEL_VIEW_HISTOGRAM:
            return true;
        case PANEL_VIEW_FFT:
            return gui_fft_available();
        default:
            return false;
    }
}

//-----------------------------------------------------------------------------
// View State Management
//-----------------------------------------------------------------------------

void* panel_create_view_state(panel_view_type_t type) {
    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            // Waveform views use shared phosphor from app, no per-panel state
            return NULL;
        case PANEL_VIEW_FFT: {
            fft_state_t *fft = malloc(sizeof(fft_state_t));
            if (fft) {
                if (gui_fft_init(fft)) {
                    return fft;
                }
                free(fft);
            }
            return NULL;
        }
        case PANEL_VIEW_CVBS: {
            // Use vtable factory to create CVBS decoder state
            const panel_vtable_t *vtable = panel_get_vtable(PANEL_VIEW_CVBS);
            if (vtable && vtable->create) {
                return vtable->create();
            }
            return NULL;
        }
        case PANEL_VIEW_HISTOGRAM: {
            histogram_state_t *hist = malloc(sizeof(histogram_state_t));
            if (hist) {
                if (histogram_init(hist, HISTOGRAM_DEFAULT_NUM_BINS)) {
                    return hist;
                }
                free(hist);
            }
            return NULL;
        }
        default:
            return NULL;
    }
}

void panel_destroy_view_state(panel_view_type_t type, void *state) {
    if (!state) return;

    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            // No state to destroy
            break;
        case PANEL_VIEW_FFT: {
            fft_state_t *fft = (fft_state_t*)state;
            gui_fft_cleanup(fft);
            free(fft);
            break;
        }
        case PANEL_VIEW_CVBS: {
            // Use vtable destroy to cleanup CVBS decoder state
            const panel_vtable_t *vtable = panel_get_vtable(PANEL_VIEW_CVBS);
            if (vtable && vtable->destroy) {
                vtable->destroy(state);
            }
            break;
        }
        case PANEL_VIEW_HISTOGRAM: {
            histogram_state_t *hist = (histogram_state_t*)state;
            histogram_cleanup(hist);
            free(hist);
            break;
        }
        default:
            break;
    }
}

void panel_clear_view_state(panel_view_type_t type, void *state) {
    if (!state) return;

    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            break;
        case PANEL_VIEW_FFT: {
            fft_state_t *fft = (fft_state_t*)state;
            gui_fft_clear(fft);
            break;
        }
        case PANEL_VIEW_CVBS: {
            // Use vtable clear to reset CVBS decoder state
            const panel_vtable_t *vtable = panel_get_vtable(PANEL_VIEW_CVBS);
            if (vtable && vtable->clear) {
                vtable->clear(state);
            }
            break;
        }
        case PANEL_VIEW_HISTOGRAM:
            histogram_clear((histogram_state_t*)state);
            break;
        default:
            break;
    }
}

// NOTE: Legacy panel_render_fn and s_render_fns[] have been removed.
// All panel rendering now uses vtable dispatch via panel_get_vtable().

//-----------------------------------------------------------------------------
// Channel Panel Rendering (Main Entry Point)
//-----------------------------------------------------------------------------

// Helper: Render a single panel using vtable dispatch
static void render_single_panel(gui_app_t *app, int channel,
                                 panel_view_type_t type, void *state,
                                 Rectangle bounds, Color color) {
    const panel_vtable_t *vtable = panel_get_vtable(type);

    if (vtable && vtable->render) {
        vtable->render(state, app, channel, bounds, color);

        // Render overlay if present
        if (vtable->render_overlay) {
            vtable->render_overlay(state, bounds);
        }
    } else {
        // No vtable - draw placeholder
        DrawRectangleRec(bounds, (Color){20, 20, 20, 255});
        DrawRectangleLinesEx(bounds, 1, (Color){60, 60, 60, 255});
        const char *name = panel_view_type_name(type);
        int w = MeasureText(name, 14);
        DrawText(name, (int)(bounds.x + bounds.width/2 - w/2),
                 (int)(bounds.y + bounds.height/2 - 7), 14, COLOR_TEXT_DIM);
    }
}

void render_channel_panels(gui_app_t *app, int channel,
                           float x, float y, float width, float height,
                           Color channel_color) {

    // Get the config for this channel
    channel_panel_config_t *config = (channel == 0)
        ? &app->panel_config_a
        : &app->panel_config_b;

    if (!config->split) {
        // Single panel - render at full width
        Rectangle bounds = {x, y, width, height};
        config->left_bounds = bounds;
        config->right_bounds = (Rectangle){0, 0, 0, 0};

        render_single_panel(app, channel, config->left_view,
                           config->left_state, bounds, channel_color);
    } else {
        // Split panels - divide width between left and right
        float half_width = width / 2.0f;
        float divider_x = x + half_width;

        // Cache bounds for click handling
        Rectangle left_bounds = {x, y, half_width - 1, height};
        Rectangle right_bounds = {divider_x + 2, y, half_width - 3, height};
        config->left_bounds = left_bounds;
        config->right_bounds = right_bounds;

        // Left panel
        render_single_panel(app, channel, config->left_view,
                           config->left_state, left_bounds, channel_color);

        // Divider line
        DrawLineEx((Vector2){divider_x, y}, (Vector2){divider_x, y + height},
                   2.0f, COLOR_GRID_MAJOR);

        // Right panel
        render_single_panel(app, channel, config->right_view,
                           config->right_state, right_bounds, channel_color);
    }
}

//-----------------------------------------------------------------------------
// Panel Configuration Helpers
//-----------------------------------------------------------------------------

void panel_config_init_default(channel_panel_config_t *config) {
    config->split = false;
    config->left_view = PANEL_VIEW_WAVEFORM_PHOSPHOR;
    config->right_view = PANEL_VIEW_FFT;
    config->left_state = NULL;
    config->right_state = NULL;
}

void panel_config_cleanup(channel_panel_config_t *config) {
    if (config->left_state) {
        panel_destroy_view_state(config->left_view, config->left_state);
        config->left_state = NULL;
    }
    if (config->right_state) {
        panel_destroy_view_state(config->right_view, config->right_state);
        config->right_state = NULL;
    }
}

void panel_config_set_left_view(channel_panel_config_t *config, panel_view_type_t type) {
    if (config->left_view == type) return;

    // Destroy old state
    if (config->left_state) {
        panel_destroy_view_state(config->left_view, config->left_state);
        config->left_state = NULL;
    }

    // Set new view and create state
    config->left_view = type;
    config->left_state = panel_create_view_state(type);
}

void panel_config_set_right_view(channel_panel_config_t *config, panel_view_type_t type) {
    if (config->right_view == type) return;

    // Destroy old state
    if (config->right_state) {
        panel_destroy_view_state(config->right_view, config->right_state);
        config->right_state = NULL;
    }

    // Set new view and create state
    config->right_view = type;
    if (config->split) {
        config->right_state = panel_create_view_state(type);
    }
}

void panel_config_set_split(channel_panel_config_t *config, bool split) {
    if (config->split == split) return;

    config->split = split;

    if (split) {
        // Entering split mode - create right panel state
        config->right_state = panel_create_view_state(config->right_view);
    } else {
        // Leaving split mode - destroy right panel state
        if (config->right_state) {
            panel_destroy_view_state(config->right_view, config->right_state);
            config->right_state = NULL;
        }
    }
}

//-----------------------------------------------------------------------------
// Unified Panel Click Handling
//-----------------------------------------------------------------------------

// Helper: Try vtable click handler for a panel
static bool try_panel_click(gui_app_t *app, int channel, panel_view_type_t type,
                            void *state, Rectangle bounds, Vector2 mouse_pos) {
    // First check if click is within bounds
    if (!CheckCollisionPointRec(mouse_pos, bounds)) {
        return false;
    }

    // Try vtable handler
    // Note: state may be NULL for panels like CVBS that store state on gui_app_t
    // Individual handlers must check for NULL state if they need it
    const panel_vtable_t *vtable = panel_get_vtable(type);
    if (vtable && vtable->handle_click) {
        return vtable->handle_click(state, app, channel, mouse_pos, bounds);
    }

    return false;
}

bool panel_handle_all_clicks(gui_app_t *app, Vector2 mouse_pos) {
    if (!app) return false;

    // Process both channels
    for (int ch = 0; ch < 2; ch++) {
        channel_panel_config_t *config = (ch == 0)
            ? &app->panel_config_a
            : &app->panel_config_b;

        // Try left panel
        if (try_panel_click(app, ch, config->left_view, config->left_state,
                            config->left_bounds, mouse_pos)) {
            return true;
        }

        // Try right panel if in split mode
        if (config->split) {
            if (try_panel_click(app, ch, config->right_view, config->right_state,
                                config->right_bounds, mouse_pos)) {
                return true;
            }
        }
    }

    return false;
}
