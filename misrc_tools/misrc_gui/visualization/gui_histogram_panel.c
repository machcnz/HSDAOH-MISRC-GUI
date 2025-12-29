/*
 * MISRC GUI - Histogram Panel Rendering Implementation
 *
 * Renders amplitude histogram with heatmap coloring consistent
 * with the oscilloscope/FFT phosphor displays.
 */

#include "gui_histogram_panel.h"
#include "../core/gui_app.h"
#include "../processing/gui_display_thread.h"
#include "../ui/gui_ui.h"
#include "../ui/gui_dropdown.h"
#include "gui_text.h"
#include <math.h>
#include <stdio.h>

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
    Rectangle options_rect[7];  // Up to 7 bin options
    bool is_visible;
    histogram_state_t *hist;    // Pointer to histogram state for this panel
} histogram_overlay_hitbox_t;

static histogram_overlay_hitbox_t s_histogram_overlay[2];  // One per channel

//-----------------------------------------------------------------------------
// Heatmap Color Conversion (matches phosphor shader for consistency)
//-----------------------------------------------------------------------------

// Convert intensity (0.0-1.0) to heatmap color
// Blue -> Cyan -> Green -> Yellow -> Red
static Color intensity_to_heatmap(float intensity) {
    if (intensity < 0.02f) {
        return (Color){0, 0, 0, 0};  // Transparent for very low values
    }
    if (intensity > 1.0f) intensity = 1.0f;

    float r, g, b;

    if (intensity < 0.25f) {
        // Dark blue to cyan
        float t = intensity / 0.25f;
        r = 0.0f;
        g = 0.078f * t;
        b = 0.392f + 0.608f * t;
    } else if (intensity < 0.5f) {
        // Cyan to green
        float t = (intensity - 0.25f) / 0.25f;
        r = 0.0f;
        g = 0.078f + 0.922f * t;
        b = 1.0f - 0.784f * t;
    } else if (intensity < 0.75f) {
        // Green to yellow
        float t = (intensity - 0.5f) / 0.25f;
        r = t;
        g = 1.0f;
        b = 0.216f - 0.216f * t;
    } else {
        // Yellow to red
        float t = (intensity - 0.75f) / 0.25f;
        r = 1.0f;
        g = 1.0f - 0.706f * t;
        b = 0.0f;
    }

    // Alpha: quadratic ramp for better visibility at low intensities
    float alpha = 0.3f + 0.7f * intensity * intensity;

    return (Color){
        (unsigned char)(r * 255.0f),
        (unsigned char)(g * 255.0f),
        (unsigned char)(b * 255.0f),
        (unsigned char)(alpha * 255.0f)
    };
}

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
    (void)color;  // Currently unused, could be used for opacity mode

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

    // Get raw samples from display thread (not the decimated display_samples)
    display_thread_t *dt = app->display_thread;
    const int16_t *samples = NULL;
    size_t samples_available = 0;

    if (dt && dt->samples.sample_count > 0) {
        samples = (channel == 0) ? dt->samples.samples_a : dt->samples.samples_b;
        samples_available = dt->samples.sample_count;
    }

    // Process raw samples into histogram
    if (samples && samples_available > 0) {
        histogram_process(hist, samples, samples_available);
    }

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

    // Scale factor: tallest bar reaches 75% of drawable height
    // height = (bin_value / actual_max) * drawable_height * 0.75
    // Simplify: height = bin_value * (drawable_height * 0.75 / actual_max)
    float height_per_unit = (drawable_height * 0.75f) / actual_max;

    // Draw histogram bars - two rendering modes based on bin count vs panel width
    if (num_bins <= panel_width) {
        // Few bins: each bin gets its own bar with scaled width
        float bar_width = (float)panel_width / (float)num_bins;

        for (int bin = 0; bin < num_bins; bin++) {
            float intensity = hist->bins[bin];

            // Compute bar height: intensity * height_per_unit
            // This ensures max bin value maps to 75% of drawable height
            int bar_height = (int)(intensity * height_per_unit);
            if (bar_height < 1 && intensity > 0.02f) bar_height = 1;

            if (bar_height > 0) {
                // Normalized value for color (0.0 to 1.0 based on actual_max)
                float normalized = intensity / actual_max;
                Color bar_color = intensity_to_heatmap(normalized);

                // Calculate bar position and width to fill panel without gaps
                int bar_x = (int)(x + bin * bar_width);
                int bar_x_next = (int)(x + (bin + 1) * bar_width);
                int actual_width = bar_x_next - bar_x;
                if (actual_width < 1) actual_width = 1;

                int bar_y = (int)(y + margin_top + drawable_height - bar_height);
                DrawRectangle(bar_x, bar_y, actual_width, bar_height, bar_color);
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

            // Compute bar height: max_val * height_per_unit
            // This ensures max bin value maps to 75% of drawable height
            int bar_height = (int)(max_val * height_per_unit);
            if (bar_height < 1 && max_val > 0.02f) bar_height = 1;

            if (bar_height > 0) {
                // Normalized value for color (0.0 to 1.0 based on actual_max)
                float normalized = max_val / actual_max;
                Color bar_color = intensity_to_heatmap(normalized);

                int bar_x = (int)x + px;
                int bar_y = (int)(y + margin_top + drawable_height - bar_height);
                DrawRectangle(bar_x, bar_y, 1, bar_height, bar_color);
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
