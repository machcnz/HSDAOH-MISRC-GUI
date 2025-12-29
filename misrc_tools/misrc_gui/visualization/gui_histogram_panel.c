/*
 * MISRC GUI - Histogram Panel Rendering Implementation
 *
 * Renders amplitude histogram with heatmap coloring consistent
 * with the oscilloscope/FFT phosphor displays.
 */

#include "gui_histogram_panel.h"
#include "../core/gui_app.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_dropdown.h"
#include "gui_text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

//-----------------------------------------------------------------------------
// Rendering Configuration
//-----------------------------------------------------------------------------

// Set to true for logarithmic Y-axis scaling, false for linear
static const bool s_use_log_scale = false;

//-----------------------------------------------------------------------------
// Bin count options for dropdown
//-----------------------------------------------------------------------------

static const int s_bin_options[] = {64, 128, 256, 512, 1024, 2048, 4096};
static const int s_num_bin_options = sizeof(s_bin_options) / sizeof(s_bin_options[0]);

//-----------------------------------------------------------------------------
// Extended Histogram State (includes overlay/menu state)
//-----------------------------------------------------------------------------

typedef struct {
    histogram_state_t hist;         // Base histogram state

    // Overlay UI state (self-contained, no static arrays)
    Rectangle button_rect;
    Rectangle options_rect[7];      // Must match s_num_bin_options
    bool dropdown_open;

    // Menu items for vtable interface
    panel_menu_item_t menu_items[7];
} histogram_panel_state_t;

// NOTE: Legacy s_histogram_overlay[] array has been removed.
// All click handling now uses vtable dispatch with self-contained state.


// NOTE: Legacy render_histogram_bins_overlay() has been removed.
// Overlay rendering now uses histogram_vtable_render_overlay() with self-contained state.

// NOTE: Legacy gui_histogram_render_panel() and gui_histogram_overlay_handle_click()
// have been removed. All rendering and click handling now uses vtable dispatch.

//-----------------------------------------------------------------------------
// Histogram Panel Processing (called from display thread)
//-----------------------------------------------------------------------------

// Helper to process histogram for a single panel config
static void process_panel_histograms(const channel_panel_config_t *config,
                                     const int16_t *samples,
                                     size_t count) {
    if (!config) return;

    // Check left panel
    if (config->left_view == PANEL_VIEW_HISTOGRAM && config->left_state) {
        histogram_process((histogram_state_t *)config->left_state, samples, count);
    }

    // Check right panel (only if split mode is active)
    if (config->split && config->right_view == PANEL_VIEW_HISTOGRAM && config->right_state) {
        histogram_process((histogram_state_t *)config->right_state, samples, count);
    }
}

void gui_histogram_panel_process_all(struct gui_app *app,
                                     const int16_t *samples_a,
                                     const int16_t *samples_b,
                                     size_t count) {
    if (!app || count == 0) return;

    // Process channel A histograms
    if (samples_a) {
        process_panel_histograms(&app->panel_config_a, samples_a, count);
    }

    // Process channel B histograms
    if (samples_b) {
        process_panel_histograms(&app->panel_config_b, samples_b, count);
    }
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================

//-----------------------------------------------------------------------------
// Lifecycle Functions
//-----------------------------------------------------------------------------

static void *histogram_vtable_create(void) {
    histogram_panel_state_t *state = calloc(1, sizeof(histogram_panel_state_t));
    if (!state) return NULL;

    // Initialize with default bins
    if (!histogram_init(&state->hist, HISTOGRAM_DEFAULT_NUM_BINS)) {
        free(state);
        return NULL;
    }

    state->dropdown_open = false;
    return state;
}

static void histogram_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_cleanup(&state->hist);
    free(state);
}

static void histogram_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_clear(&state->hist);
}

//-----------------------------------------------------------------------------
// Processing Function (called from display thread)
//-----------------------------------------------------------------------------

static void histogram_vtable_process(void *state_ptr, const int16_t *samples, size_t count, uint32_t sample_rate) {
    (void)sample_rate;  // Histogram doesn't need sample rate
    if (!state_ptr || !samples || count == 0) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_process(&state->hist, samples, count);
}

//-----------------------------------------------------------------------------
// Rendering Functions
//-----------------------------------------------------------------------------

// Internal render function (shared by legacy and vtable paths)
static void histogram_render_internal(histogram_state_t *hist, Rectangle bounds, Color color) {
    if (!hist || !hist->initialized) {
        // Draw "No Histogram" message if state is invalid
        const char *text = "No Histogram";
        int text_width = MeasureText(text, FONT_SIZE_OSC_MSG);
        DrawText(text, (int)(bounds.x + bounds.width/2 - text_width/2),
                 (int)(bounds.y + bounds.height/2 - 12),
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;

    // Draw background
    DrawRectangle((int)x, (int)y, (int)w, (int)h, COLOR_METER_BG);
    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);

    // Calculate bin rendering parameters
    int panel_width = (int)w;
    int num_bins = hist->num_bins;
    float margin_bottom = 20.0f;
    float margin_top = 10.0f;
    float drawable_height = h - margin_bottom - margin_top;

    // Find actual maximum
    float actual_max = 0.0f;
    for (int i = 0; i < num_bins; i++) {
        if (hist->bins[i] > actual_max) actual_max = hist->bins[i];
    }
    if (actual_max < 0.001f) actual_max = 1.0f;

    // Calculate scaling
    float height_scale, log_max = 0.0f;
    if (s_use_log_scale) {
        log_max = log10f(1.0f + actual_max * 1000.0f);
        if (log_max < 0.001f) log_max = 1.0f;
        height_scale = drawable_height / log_max;
    } else {
        height_scale = drawable_height / actual_max;
    }

    // Draw bars
    if (num_bins <= panel_width) {
        float bar_width = (float)panel_width / (float)num_bins;
        for (int bin = 0; bin < num_bins; bin++) {
            float intensity = hist->bins[bin];
            int bar_height;
            if (s_use_log_scale) {
                bar_height = (int)(log10f(1.0f + intensity * 1000.0f) * height_scale);
            } else {
                bar_height = (int)(intensity * height_scale);
            }
            if (bar_height < 1 && intensity > 0.0001f) bar_height = 1;

            if (bar_height > 0) {
                int bar_x = (int)(x + bin * bar_width);
                int bar_x_next = (int)(x + (bin + 1) * bar_width);
                int actual_width = bar_x_next - bar_x;
                if (actual_width < 1) actual_width = 1;
                int bar_y = (int)(y + margin_top + drawable_height - bar_height);
                DrawRectangle(bar_x, bar_y, actual_width, bar_height, color);
            }
        }
    } else {
        for (int px = 0; px < panel_width; px++) {
            int bin_start = (px * num_bins) / panel_width;
            int bin_end = ((px + 1) * num_bins) / panel_width;
            if (bin_end > num_bins) bin_end = num_bins;
            if (bin_end <= bin_start) bin_end = bin_start + 1;

            float max_val = 0.0f;
            for (int b = bin_start; b < bin_end && b < num_bins; b++) {
                if (hist->bins[b] > max_val) max_val = hist->bins[b];
            }

            int bar_height;
            if (s_use_log_scale) {
                bar_height = (int)(log10f(1.0f + max_val * 1000.0f) * height_scale);
            } else {
                bar_height = (int)(max_val * height_scale);
            }
            if (bar_height < 1 && max_val > 0.0001f) bar_height = 1;

            if (bar_height > 0) {
                int bar_x = (int)x + px;
                int bar_y = (int)(y + margin_top + drawable_height - bar_height);
                DrawRectangle(bar_x, bar_y, 1, bar_height, color);
            }
        }
    }

    // Draw center line
    int center_x = (int)(x + w / 2);
    DrawLine(center_x, (int)(y + margin_top), center_x,
             (int)(y + margin_top + drawable_height), COLOR_GRID);

    // Draw quarter grid lines
    for (int i = 1; i < 4; i++) {
        int grid_x = (int)(x + (w * i) / 4);
        DrawLine(grid_x, (int)(y + margin_top), grid_x,
                 (int)(y + margin_top + drawable_height),
                 (Color){COLOR_GRID.r, COLOR_GRID.g, COLOR_GRID.b, 60});
    }

    // Draw axis labels
    Font font = GetFontDefault();
    int font_size = FONT_SIZE_OSC_SCALE;

    DrawTextEx(font, "-1", (Vector2){x + 2, y + h - margin_bottom + 2},
               font_size, 1, COLOR_TEXT_DIM);

    Vector2 zero_size = MeasureTextEx(font, "0", font_size, 1);
    DrawTextEx(font, "0", (Vector2){x + w/2 - zero_size.x/2, y + h - margin_bottom + 2},
               font_size, 1, COLOR_TEXT_DIM);

    Vector2 pos_size = MeasureTextEx(font, "+1", font_size, 1);
    DrawTextEx(font, "+1", (Vector2){x + w - pos_size.x - 2, y + h - margin_bottom + 2},
               font_size, 1, COLOR_TEXT_DIM);
}

static void histogram_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                    Rectangle bounds, Color channel_color) {
    (void)app;
    (void)channel;

    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_render_internal(&state->hist, bounds, channel_color);
}

static void histogram_vtable_render_overlay(void *state_ptr, Rectangle bounds) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_state_t *hist = &state->hist;

    if (!hist->initialized) return;

    // Format current bin count
    char bins_str[16];
    snprintf(bins_str, sizeof(bins_str), "%d", hist->num_bins);

    // Button dimensions
    float btn_w = 55, btn_h = 18;
    float btn_x = bounds.x + bounds.width - btn_w - 8;
    float btn_y = bounds.y + 8;

    state->button_rect = (Rectangle){btn_x, btn_y, btn_w, btn_h};

    // Draw button
    Color btn_bg = state->dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(state->button_rect, 0.15f, 4, btn_bg);

    // Draw text centered with arrow
    int text_w = gui_text_measure(bins_str, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8;
    float total_w = text_w + arrow_w + 4;
    float text_x = btn_x + (btn_w - total_w) / 2;
    float text_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(bins_str, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Draw arrow
    float arrow_size = 5.0f;
    float arrow_x = text_x + text_w + 6;
    float arrow_cy = btn_y + btn_h / 2;
    if (state->dropdown_open) {
        Vector2 top = { arrow_x + arrow_size/2, arrow_cy - arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy + arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        Vector2 bottom = { arrow_x + arrow_size/2, arrow_cy + arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy - arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy - arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }

    // Draw dropdown options if open
    if (state->dropdown_open) {
        float opt_y = btn_y + btn_h;
        float opt_h = 20;

        DrawRectangleRounded((Rectangle){btn_x, opt_y, btn_w, opt_h * s_num_bin_options},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < s_num_bin_options; i++) {
            Rectangle opt_rect = {btn_x, opt_y + i * opt_h, btn_w, opt_h};
            state->options_rect[i] = opt_rect;

            bool is_selected = (hist->num_bins == s_bin_options[i]);
            Vector2 mouse = GetMousePosition();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            char opt_str[16];
            snprintf(opt_str, sizeof(opt_str), "%d", s_bin_options[i]);

            int opt_text_w = gui_text_measure(opt_str, FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = btn_x + btn_w/2 - opt_text_w/2;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(opt_str, opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

//-----------------------------------------------------------------------------
// Interaction Functions
//-----------------------------------------------------------------------------

static bool histogram_vtable_handle_click(void *state_ptr, gui_app_t *app, int channel,
                                          Vector2 click, Rectangle bounds) {
    (void)app;
    (void)channel;
    (void)bounds;

    if (!state_ptr) return false;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;

    // Check button click
    if (CheckCollisionPointRec(click, state->button_rect)) {
        state->dropdown_open = !state->dropdown_open;
        return true;
    }

    // Check option clicks if open
    if (state->dropdown_open) {
        for (int i = 0; i < s_num_bin_options; i++) {
            if (CheckCollisionPointRec(click, state->options_rect[i])) {
                histogram_set_num_bins(&state->hist, s_bin_options[i]);
                state->dropdown_open = false;
                return true;
            }
        }
        // Click outside dropdown closes it
        state->dropdown_open = false;
    }

    return false;
}

//-----------------------------------------------------------------------------
// Menu Functions
//-----------------------------------------------------------------------------

static void histogram_menu_on_select(void *state_ptr, int value) {
    if (!state_ptr) return;
    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_set_num_bins(&state->hist, value);
    state->dropdown_open = false;
}

static size_t histogram_vtable_get_menu_count(void *state_ptr) {
    (void)state_ptr;
    return 1;  // One menu: bin count selector
}

static panel_menu_t histogram_vtable_get_menu(void *state_ptr, size_t index) {
    panel_menu_t empty = {0};
    if (index != 0 || !state_ptr) return empty;

    histogram_panel_state_t *state = (histogram_panel_state_t *)state_ptr;
    histogram_state_t *hist = &state->hist;

    // Populate menu items
    for (int i = 0; i < s_num_bin_options; i++) {
        char label[16];
        snprintf(label, sizeof(label), "%d", s_bin_options[i]);
        // Note: label string is static, but we need persistent storage
        // For now, we'll use the format in the caller
        state->menu_items[i].label = NULL;  // Caller formats
        state->menu_items[i].value = s_bin_options[i];
        state->menu_items[i].selected = (hist->num_bins == s_bin_options[i]);
    }

    panel_menu_t menu = {
        .title = "Bins",
        .items = state->menu_items,
        .count = s_num_bin_options,
        .on_select = histogram_menu_on_select
    };

    return menu;
}

//-----------------------------------------------------------------------------
// Vtable Definition
//-----------------------------------------------------------------------------

static const panel_vtable_t s_histogram_vtable = {
    .name = "Histogram",

    // Lifecycle
    .create = histogram_vtable_create,
    .destroy = histogram_vtable_destroy,
    .clear = histogram_vtable_clear,

    // Processing
    .process = histogram_vtable_process,

    // Rendering
    .render = histogram_vtable_render,
    .render_overlay = histogram_vtable_render_overlay,

    // Interaction
    .handle_click = histogram_vtable_handle_click,
    .handle_scroll = NULL,

    // Menus
    .get_menu_count = histogram_vtable_get_menu_count,
    .get_menu = histogram_vtable_get_menu,
};

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------

void gui_histogram_panel_register(void) {
    panel_register(PANEL_VIEW_HISTOGRAM, &s_histogram_vtable);
}
