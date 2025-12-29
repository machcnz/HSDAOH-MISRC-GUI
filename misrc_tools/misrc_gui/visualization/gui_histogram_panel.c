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
// Overlay hitbox state for click detection (per channel)
//-----------------------------------------------------------------------------

typedef struct {
    Rectangle button_rect;
    Rectangle options_rect[7];  // Must match s_num_bin_options
    bool is_visible;
    histogram_state_t *hist;    // Pointer to histogram state for this panel
} histogram_overlay_hitbox_t;

static histogram_overlay_hitbox_t s_histogram_overlay[2];  // One per channel


//-----------------------------------------------------------------------------
// Bins Selector Overlay Rendering
//-----------------------------------------------------------------------------

static void render_histogram_bins_overlay(int channel, histogram_state_t *hist,
                                          float panel_x, float panel_y, float panel_w) {
    if (!hist || !hist->initialized) return;

    // Format current bin count as string
    char bins_str[16];
    snprintf(bins_str, sizeof(bins_str), "%d", hist->num_bins);

    // Button dimensions matching sidebar style
    float btn_w = 55;
    float btn_h = 18;
    float btn_x = panel_x + panel_w - btn_w - 8;
    float btn_y = panel_y + 8;

    // Store hit box for click detection
    s_histogram_overlay[channel].button_rect = (Rectangle){btn_x, btn_y, btn_w, btn_h};
    s_histogram_overlay[channel].is_visible = true;
    s_histogram_overlay[channel].hist = hist;

    // Draw dropdown button
    bool is_open = gui_dropdown_is_open(DROPDOWN_HISTOGRAM_BINS, channel);
    Color btn_bg = is_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;

    DrawRectangleRounded((Rectangle){btn_x, btn_y, btn_w, btn_h}, 0.15f, 4, btn_bg);

    // Draw text centered, leaving room for arrow
    int text_w = gui_text_measure(bins_str, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8;
    float total_w = text_w + arrow_w + 4;
    float text_x = btn_x + (btn_w - total_w) / 2;
    float text_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(bins_str, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

    // Draw small triangle arrow indicator
    float arrow_size = 5.0f;
    float arrow_x = text_x + text_w + 6;
    float arrow_cy = btn_y + btn_h / 2;
    if (is_open) {
        // Up arrow
        Vector2 top = { arrow_x + arrow_size/2, arrow_cy - arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy + arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy + arrow_size/2 };
        DrawTriangle(top, left, right, COLOR_TEXT);
    } else {
        // Down arrow
        Vector2 bottom = { arrow_x + arrow_size/2, arrow_cy + arrow_size/2 };
        Vector2 left = { arrow_x, arrow_cy - arrow_size/2 };
        Vector2 right = { arrow_x + arrow_size, arrow_cy - arrow_size/2 };
        DrawTriangle(bottom, right, left, COLOR_TEXT);
    }

    // Draw dropdown options if open
    if (is_open) {
        float opt_y = btn_y + btn_h;
        float opt_h = 20;

        // Background for dropdown container
        DrawRectangleRounded((Rectangle){btn_x, opt_y, btn_w, opt_h * s_num_bin_options},
                             0.1f, 4, COLOR_PANEL_BG);

        for (int i = 0; i < s_num_bin_options; i++) {
            Rectangle opt_rect = {btn_x, opt_y + i * opt_h, btn_w, opt_h};
            s_histogram_overlay[channel].options_rect[i] = opt_rect;

            bool is_selected = (hist->num_bins == s_bin_options[i]);

            // Check hover
            Vector2 mouse = GetMousePosition();
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            // Use dropdown option color for consistent styling
            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            // Format option text
            char opt_str[16];
            snprintf(opt_str, sizeof(opt_str), "%d", s_bin_options[i]);

            // Center text in option
            int opt_text_w = gui_text_measure(opt_str, FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = btn_x + btn_w/2 - opt_text_w/2;
            float opt_text_y = opt_y + i * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(opt_str, opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

//-----------------------------------------------------------------------------
// Histogram Panel Rendering
//-----------------------------------------------------------------------------

void gui_histogram_render_panel(struct gui_app *app, int channel,
                                float x, float y, float w, float h,
                                void *state, Color color) {
    (void)app;  // Processing moved to display thread

    // Reset overlay visibility at start
    s_histogram_overlay[channel].is_visible = false;
    s_histogram_overlay[channel].hist = NULL;

    histogram_state_t *hist = (histogram_state_t *)state;
    if (!hist || !hist->initialized) {
        // Draw "No Histogram" message if state is invalid
        const char *text = "No Histogram";
        int text_width = MeasureText(text, FONT_SIZE_OSC_MSG);
        DrawText(text, (int)(x + w/2 - text_width/2), (int)(y + h/2 - 12),
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    // Note: histogram_process() is now called from the display thread,
    // not here in the render function. This separates computation from rendering.

    // Draw background (same as oscilloscope/FFT panels)
    DrawRectangle((int)x, (int)y, (int)w, (int)h, COLOR_METER_BG);

    // Draw border
    DrawRectangleLinesEx((Rectangle){x, y, w, h}, 1, COLOR_GRID_MAJOR);

    // Calculate bin rendering parameters
    int panel_width = (int)w;
    int num_bins = hist->num_bins;
    float margin_bottom = 20.0f;  // Space for axis label
    float margin_top = 10.0f;
    float drawable_height = h - margin_bottom - margin_top;

    // Find the actual maximum bin value right now for accurate scaling
    float actual_max = 0.0f;
    for (int i = 0; i < num_bins; i++) {
        if (hist->bins[i] > actual_max) {
            actual_max = hist->bins[i];
        }
    }
    if (actual_max < 0.001f) actual_max = 1.0f;  // Avoid division by zero

    // Calculate scaling factor based on scale mode
    float height_scale = 0.0f;
    float log_max = 0.0f;
    if (s_use_log_scale) {
        // Log scale: use log10(1 + value * 1000) for better dynamic range
        log_max = log10f(1.0f + actual_max * 1000.0f);
        if (log_max < 0.001f) log_max = 1.0f;
        height_scale = drawable_height / log_max;
    } else {
        // Linear scale: tallest bar fills drawable height
        height_scale = drawable_height / actual_max;
    }

    // Draw histogram bars - two rendering modes based on bin count vs panel width
    if (num_bins <= panel_width) {
        // Few bins: each bin gets its own bar with scaled width
        float bar_width = (float)panel_width / (float)num_bins;

        for (int bin = 0; bin < num_bins; bin++) {
            float intensity = hist->bins[bin];

            int bar_height;
            if (s_use_log_scale) {
                float log_val = log10f(1.0f + intensity * 1000.0f);
                bar_height = (int)(log_val * height_scale);
            } else {
                bar_height = (int)(intensity * height_scale);
            }
            if (bar_height < 1 && intensity > 0.0001f) bar_height = 1;

            if (bar_height > 0) {
                // Calculate bar position and width to fill panel without gaps
                int bar_x = (int)(x + bin * bar_width);
                int bar_x_next = (int)(x + (bin + 1) * bar_width);
                int actual_width = bar_x_next - bar_x;
                if (actual_width < 1) actual_width = 1;

                int bar_y = (int)(y + margin_top + drawable_height - bar_height);
                DrawRectangle(bar_x, bar_y, actual_width, bar_height, color);
            }
        }
    } else {
        // Many bins: combine multiple bins per pixel column
        for (int px = 0; px < panel_width; px++) {
            // Determine which bins this pixel column represents
            int bin_start = (px * num_bins) / panel_width;
            int bin_end = ((px + 1) * num_bins) / panel_width;
            if (bin_end > num_bins) bin_end = num_bins;
            if (bin_end <= bin_start) bin_end = bin_start + 1;

            // Find maximum intensity in this bin range
            float max_val = 0.0f;
            for (int b = bin_start; b < bin_end && b < num_bins; b++) {
                if (hist->bins[b] > max_val) {
                    max_val = hist->bins[b];
                }
            }

            int bar_height;
            if (s_use_log_scale) {
                float log_val = log10f(1.0f + max_val * 1000.0f);
                bar_height = (int)(log_val * height_scale);
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

    // Draw center line (0 amplitude reference)
    int center_x = (int)(x + w / 2);
    DrawLine(center_x, (int)(y + margin_top), center_x,
             (int)(y + margin_top + drawable_height), COLOR_GRID);

    // Draw grid lines at quarter points
    for (int i = 1; i < 4; i++) {
        int grid_x = (int)(x + (w * i) / 4);
        DrawLine(grid_x, (int)(y + margin_top), grid_x,
                 (int)(y + margin_top + drawable_height),
                 (Color){COLOR_GRID.r, COLOR_GRID.g, COLOR_GRID.b, 60});
    }

    // Draw axis labels
    Font font = app->fonts ? app->fonts[1] : GetFontDefault();  // Use mono font
    int font_size = FONT_SIZE_OSC_SCALE;

    // "-1" at left edge
    const char *label_neg = "-1";
    DrawTextEx(font, label_neg,
               (Vector2){x + 2, y + h - margin_bottom + 2},
               font_size, 1, COLOR_TEXT_DIM);

    // "0" at center
    const char *label_zero = "0";
    Vector2 zero_size = MeasureTextEx(font, label_zero, font_size, 1);
    DrawTextEx(font, label_zero,
               (Vector2){x + w/2 - zero_size.x/2, y + h - margin_bottom + 2},
               font_size, 1, COLOR_TEXT_DIM);

    // "+1" at right edge
    const char *label_pos = "+1";
    Vector2 pos_size = MeasureTextEx(font, label_pos, font_size, 1);
    DrawTextEx(font, label_pos,
               (Vector2){x + w - pos_size.x - 2, y + h - margin_bottom + 2},
               font_size, 1, COLOR_TEXT_DIM);

    // Render bins selector overlay in top-right corner
    render_histogram_bins_overlay(channel, hist, x, y, w);
}

//-----------------------------------------------------------------------------
// Histogram Panel Overlay Click Handler
//-----------------------------------------------------------------------------

bool gui_histogram_overlay_handle_click(struct gui_app *app, int channel,
                                        Vector2 mouse_pos) {
    (void)app;  // Not needed since we store hist pointer in overlay state

    if (channel < 0 || channel > 1) return false;
    if (!s_histogram_overlay[channel].is_visible) return false;
    if (!s_histogram_overlay[channel].hist) return false;

    histogram_state_t *hist = s_histogram_overlay[channel].hist;

    // Check button click - toggle dropdown
    if (CheckCollisionPointRec(mouse_pos, s_histogram_overlay[channel].button_rect)) {
        gui_dropdown_toggle(DROPDOWN_HISTOGRAM_BINS, channel);
        return true;
    }

    // Check option clicks if dropdown is open
    if (gui_dropdown_is_open(DROPDOWN_HISTOGRAM_BINS, channel)) {
        for (int i = 0; i < s_num_bin_options; i++) {
            if (CheckCollisionPointRec(mouse_pos, s_histogram_overlay[channel].options_rect[i])) {
                histogram_set_num_bins(hist, s_bin_options[i]);
                gui_dropdown_close_all();
                return true;
            }
        }
    }

    return false;
}

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
