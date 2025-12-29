/*
 * MISRC GUI - Panel Abstraction Implementation
 *
 * Provides render dispatch and state management for panel views.
 * This file orchestrates panel rendering by delegating to specialized
 * panel render functions in their respective modules.
 */

#include "gui_panel.h"
#include "../core/gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_fft.h"
#include "../signal/gui_cvbs.h"
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
        case PANEL_VIEW_CVBS:
            // CVBS decoder state is stored on gui_app_t (app->cvbs_a/b)
            return NULL;
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
        case PANEL_VIEW_CVBS:
            // No per-panel state
            break;
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
        case PANEL_VIEW_CVBS:
            // No per-panel state
            break;
        default:
            break;
    }
}

//-----------------------------------------------------------------------------
// Panel Render Functions
//-----------------------------------------------------------------------------

// Forward declaration of internal render functions
static void render_waveform_line_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);
static void render_waveform_phosphor_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);
static void render_fft_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);
static void render_cvbs_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);

// Render function table
static panel_render_fn s_render_fns[] = {
    [PANEL_VIEW_WAVEFORM_LINE] = render_waveform_line_panel,
    [PANEL_VIEW_WAVEFORM_PHOSPHOR] = render_waveform_phosphor_panel,
    [PANEL_VIEW_FFT] = render_fft_panel,
    [PANEL_VIEW_CVBS] = render_cvbs_panel,
};

panel_render_fn panel_get_render_fn(panel_view_type_t type) {
    if (type < PANEL_VIEW_COUNT) return s_render_fns[type];
    return NULL;
}

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (Line Mode) - Thin wrapper for panel dispatch
//-----------------------------------------------------------------------------

static void render_waveform_line_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    (void)state;  // Line mode doesn't use per-panel state
    render_waveform_line(app, channel, x, y, w, h, color);
}

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (Phosphor Mode) - Thin wrapper for panel dispatch
//-----------------------------------------------------------------------------

static void render_waveform_phosphor_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    (void)state;  // Uses shared phosphor from app
    render_waveform_phosphor(app, channel, x, y, w, h, color);
}

//-----------------------------------------------------------------------------
// FFT Panel Rendering - Delegates to gui_fft module
//-----------------------------------------------------------------------------

static void render_fft_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    gui_fft_render_panel(app, channel, x, y, w, h, state, color);
}

//-----------------------------------------------------------------------------
// CVBS Panel Rendering - Delegates to gui_cvbs module
//-----------------------------------------------------------------------------

static void render_cvbs_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    gui_cvbs_render_panel(app, channel, x, y, w, h, state, color);
}

//-----------------------------------------------------------------------------
// Channel Panel Rendering (Main Entry Point)
//-----------------------------------------------------------------------------

void render_channel_panels(gui_app_t *app, int channel,
                           float x, float y, float width, float height,
                           Color channel_color) {

    // Access the inline struct from gui_app_t (uses int for view types)
    bool split;
    panel_view_type_t left_view, right_view;
    void *left_state, *right_state;

    if (channel == 0) {
        split = app->panel_config_a.split;
        left_view = (panel_view_type_t)app->panel_config_a.left_view;
        right_view = (panel_view_type_t)app->panel_config_a.right_view;
        left_state = app->panel_config_a.left_state;
        right_state = app->panel_config_a.right_state;
    } else {
        split = app->panel_config_b.split;
        left_view = (panel_view_type_t)app->panel_config_b.left_view;
        right_view = (panel_view_type_t)app->panel_config_b.right_view;
        left_state = app->panel_config_b.left_state;
        right_state = app->panel_config_b.right_state;
    }

    if (!split) {
        // Single panel - render at full width
        panel_render_fn fn = panel_get_render_fn(left_view);
        if (fn) {
            fn(app, channel, x, y, width, height, left_state, channel_color);
        }
    } else {
        // Split panels - divide width between left and right
        float half_width = width / 2.0f;
        float divider_x = x + half_width;

        // Left panel
        panel_render_fn left_fn = panel_get_render_fn(left_view);
        if (left_fn) {
            left_fn(app, channel, x, y, half_width - 1, height,
                   left_state, channel_color);
        }

        // Divider line
        DrawLineEx((Vector2){divider_x, y}, (Vector2){divider_x, y + height},
                   2.0f, COLOR_GRID_MAJOR);

        // Right panel
        panel_render_fn right_fn = panel_get_render_fn(right_view);
        if (right_fn) {
            right_fn(app, channel, divider_x + 2, y, half_width - 3, height,
                    right_state, channel_color);
        }
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
// CVBS Panel Overlay Interaction - Delegates to gui_cvbs module
//-----------------------------------------------------------------------------

bool panel_cvbs_overlay_handle_click(gui_app_t *app, int channel, Vector2 mouse_pos) {
    return gui_cvbs_overlay_handle_click(app, channel, mouse_pos);
}
