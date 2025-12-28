/*
 * MISRC GUI - Panel Abstraction Implementation
 *
 * Provides render dispatch and state management for panel views.
 */

#include "gui_panel.h"
#include "../core/gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_fft.h"
#include "../signal/gui_cvbs.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_dropdown.h"
#include "gui_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// CVBS Overlay State (for panel-based system selector)
//-----------------------------------------------------------------------------

typedef struct {
    Rectangle button_rect;
    Rectangle options_rect[3];  // PAL, NTSC, SECAM
    bool is_visible;
} cvbs_overlay_hitbox_t;

static cvbs_overlay_hitbox_t s_cvbs_overlay[2];  // One per channel

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
// FFT Panel Rendering
//-----------------------------------------------------------------------------

static void render_fft_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {

    fft_state_t *fft = (fft_state_t*)state;

    // If no state provided, try to use the app's FFT state (for backward compat)
    if (!fft) {
        fft = (channel == 0) ? app->fft_a : app->fft_b;
    }

    if (!fft || !fft->initialized) {
        // FFT not available - show message
        const char *text = gui_fft_available() ? "FFT Initializing..." : "FFT Not Available";
        int text_width = MeasureText(text, FONT_SIZE_OSC_MSG);
        DrawText(text, (int)(x + w/2 - text_width/2), (int)(y + h/2 - 12),
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;

    // Get display samples
    waveform_sample_t *samples;
    size_t samples_available;
    if (channel == 0) {
        samples = app->display_samples_a;
        samples_available = app->display_samples_available_a;
    } else {
        samples = app->display_samples_b;
        samples_available = app->display_samples_available_b;
    }

    // Calculate display sample rate
    uint32_t sr = atomic_load(&app->sample_rate);
    float display_sample_rate = (trig->zoom_scale > 0 && sr > 0) ?
                                (float)sr / trig->zoom_scale : 0;

    // Process and render FFT
    gui_fft_process_display(fft, samples, samples_available, display_sample_rate);
    gui_fft_render(fft, x, y, w, h, display_sample_rate, color, app->fonts);
}

//-----------------------------------------------------------------------------
// CVBS Panel Rendering
//-----------------------------------------------------------------------------

// Render the CVBS system selector overlay in the top-right corner of the panel
// Uses the same style as sidebar dropdowns (FONT_SIZE_DROPDOWN_OPT, COLOR_BUTTON, etc.)
static void render_cvbs_system_overlay(gui_app_t *app, int channel,
                                        float panel_x, float panel_y, float panel_w) {
    int sys = (channel == 0) ? atomic_load(&app->cvbs_system_a) : atomic_load(&app->cvbs_system_b);
    const char *sys_name = (sys == 0) ? "PAL" : (sys == 2) ? "SECAM" : "NTSC";

    // Button dimensions matching sidebar style (65x18 with corner radius 3)
    float btn_w = 65;
    float btn_h = 18;
    float btn_x = panel_x + panel_w - btn_w - 8;
    float btn_y = panel_y + 8;

    // Store hit box for click detection
    s_cvbs_overlay[channel].button_rect = (Rectangle){btn_x, btn_y, btn_w, btn_h};
    s_cvbs_overlay[channel].is_visible = true;

    // Draw dropdown button (matching sidebar style)
    bool is_open = gui_dropdown_is_open(DROPDOWN_CVBS_SYSTEM, channel);
    Color btn_bg = is_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;

    DrawRectangleRounded((Rectangle){btn_x, btn_y, btn_w, btn_h}, 0.15f, 4, btn_bg);

    // Draw text centered, leaving room for arrow
    int text_w = gui_text_measure(sys_name, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8;  // Space for arrow
    float total_w = text_w + arrow_w + 4;  // text + gap + arrow
    float text_x = btn_x + (btn_w - total_w) / 2;
    float text_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(sys_name, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Draw small triangle arrow indicator
    float arrow_size = 5.0f;
    float arrow_x = text_x + text_w + 6;
    float arrow_cy = btn_y + btn_h / 2;
    if (is_open) {
        // Up arrow (triangle pointing up)
        Vector2 top = { arrow_x + arrow_size/2, arrow_cy - arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy + arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        // Down arrow (triangle pointing down)
        Vector2 bottom = { arrow_x + arrow_size/2, arrow_cy + arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy - arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy - arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }

    // Draw dropdown options if open
    if (is_open) {
        float opt_y = btn_y + btn_h;
        const char *options[] = {"PAL", "NTSC", "SECAM"};
        int sys_values[] = {0, 1, 2};  // PAL=0, NTSC=1, SECAM=2
        float opt_h = 20;  // Option height matching sidebar

        // Background for dropdown container
        DrawRectangleRounded((Rectangle){btn_x, opt_y, btn_w, opt_h * 3}, 0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < 3; i++) {
            Rectangle opt_rect = {btn_x, opt_y + i * opt_h, btn_w, opt_h};
            s_cvbs_overlay[channel].options_rect[i] = opt_rect;

            bool is_selected = (sys == sys_values[i]);

            // Check hover
            Vector2 mouse = GetMousePosition();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            // Use gui_dropdown_option_color for consistent styling
            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            // Center text in option using app font
            int opt_text_w = gui_text_measure(options[i], FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = btn_x + btn_w/2 - opt_text_w/2;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(options[i], opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

static void render_cvbs_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    (void)state;
    (void)color;

    // Reset overlay visibility at start (will be set if decoder is active)
    s_cvbs_overlay[channel].is_visible = false;

    cvbs_decoder_t *dec = (channel == 0) ? atomic_load(&app->cvbs_a) : atomic_load(&app->cvbs_b);
    if (!dec) {
        const char *text = "CVBS disabled";
        int text_width = MeasureText(text, FONT_SIZE_OSC_MSG);
        DrawText(text, (int)(x + w/2 - text_width/2), (int)(y + h/2 - 12),
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    // Render decoded video frame (grayscale)
    gui_cvbs_render_frame(dec, x, y, w, h);

    // Render system selector overlay in top-right corner
    render_cvbs_system_overlay(app, channel, x, y, w);
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
// CVBS Panel Overlay Interaction
//-----------------------------------------------------------------------------

bool panel_cvbs_overlay_handle_click(gui_app_t *app, int channel, Vector2 mouse_pos) {
    if (channel < 0 || channel > 1) return false;
    if (!s_cvbs_overlay[channel].is_visible) return false;

    // Check button click - toggle dropdown
    if (CheckCollisionPointRec(mouse_pos, s_cvbs_overlay[channel].button_rect)) {
        gui_dropdown_toggle(DROPDOWN_CVBS_SYSTEM, channel);
        return true;
    }

    // Check option clicks if dropdown is open
    if (gui_dropdown_is_open(DROPDOWN_CVBS_SYSTEM, channel)) {
        int sys_values[] = {0, 1, 2};  // PAL, NTSC, SECAM
        for (int i = 0; i < 3; i++) {
            if (CheckCollisionPointRec(mouse_pos, s_cvbs_overlay[channel].options_rect[i])) {
                if (channel == 0) atomic_store(&app->cvbs_system_a, sys_values[i]);
                else atomic_store(&app->cvbs_system_b, sys_values[i]);
                gui_dropdown_close_all();
                return true;
            }
        }
    }

    return false;
}
