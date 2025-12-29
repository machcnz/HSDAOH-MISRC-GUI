/*
 * MISRC GUI - Waveform Panel Implementation
 *
 * Waveform panel vtable (line and phosphor modes), trigger detection,
 * per-panel resampling, and grid rendering.
 */

#include "gui_oscilloscope.h"
#include "gui_phosphor_rt.h"
#include "gui_fft.h"
#include "../signal/gui_trigger.h"
#include "../ui/gui_popup.h"
#include "gui_text.h"
#include "../ui/gui_ui.h"
#include "gui_panel.h"
#include "../ui/gui_dropdown.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LIBSOXR_ENABLED
#include <soxr.h>
#endif

//-----------------------------------------------------------------------------
// Trigger Mode Labels (for dropdown UI)
//-----------------------------------------------------------------------------

static const char *s_trigger_mode_labels[] = {
    [TRIGGER_MODE_RISING] = "Rising",
    [TRIGGER_MODE_FALLING] = "Falling",
    [TRIGGER_MODE_CVBS_HSYNC] = "CVBS",
};

//-----------------------------------------------------------------------------
// Waveform Panel State (per-panel trigger, resampling, and display)
//-----------------------------------------------------------------------------

typedef struct {
    // Trigger settings (per-panel, independent)
    bool trigger_enabled;
    int16_t trigger_level;
    trigger_mode_t trigger_mode;
    float zoom_scale;
    int trigger_display_pos;

    // Resampler state (per-panel)
    void *resampler;           // soxr_t handle
    float resampler_ratio;     // Current decimation ratio

    // Display buffer (per-panel)
    waveform_sample_t display_samples[DISPLAY_BUFFER_SIZE];
    size_t display_samples_available;
    atomic_int display_width;

    // Phosphor state (for phosphor mode only)
    phosphor_rt_t *phosphor;

    // Phosphor color mode
    phosphor_color_mode_t phosphor_color;

    // UI overlay state (trigger mode dropdown)
    Rectangle button_rect;
    Rectangle options_rect[TRIGGER_MODE_COUNT];
    bool dropdown_open;
    panel_menu_item_t menu_items[TRIGGER_MODE_COUNT];

    // Drag state for trigger level
    bool dragging;

    // Initialization flag
    bool initialized;
} waveform_panel_state_t;

// Forward declarations for static helper functions
static void waveform_cleanup_resampler(waveform_panel_state_t *state);
static size_t waveform_resample_to_buffer(waveform_panel_state_t *state,
                                           const int16_t *buf, size_t num_samples,
                                           size_t start_idx, float decimation,
                                           size_t target_width);
static bool waveform_process_display(waveform_panel_state_t *state,
                                      const int16_t *buf, size_t num_samples);

//-----------------------------------------------------------------------------
// Grid Settings
//-----------------------------------------------------------------------------

#define GRID_DIVISIONS_Y 4  // Per channel (amplitude)
#define GRID_MIN_SPACING_PX 120  // Minimum pixels between time grid lines
#define GRID_MAX_DIVISIONS 20   // Maximum number of time divisions

//-----------------------------------------------------------------------------
// Cleanup
//-----------------------------------------------------------------------------

void gui_oscilloscope_cleanup(void) {
    // No static resources - per-panel cleanup handled by vtable destroy()
}

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

// Snap to 1-2-5 log scale sequence
// Given a rough time division, find the nearest "nice" value in the sequence:
// ...0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100...
static double snap_to_125(double value) {
    if (value <= 0) return 1.0;

    // Find the order of magnitude (power of 10)
    double log_val = log10(value);
    double magnitude = pow(10.0, floor(log_val));
    double normalized = value / magnitude;  // Will be between 1 and 10

    // Snap to 1, 2, or 5 within this magnitude
    double snapped;
    if (normalized < 1.5) {
        snapped = 1.0;
    } else if (normalized < 3.5) {
        snapped = 2.0;
    } else if (normalized < 7.5) {
        snapped = 5.0;
    } else {
        snapped = 10.0;
    }

    return snapped * magnitude;
}

// Format time value with appropriate unit (ns, us, ms, s)
static void format_time_label(char *buf, size_t buf_size, double seconds) {
    if (seconds >= 1.0) {
        snprintf(buf, buf_size, "%.3gs", seconds);
    } else if (seconds >= 0.001) {
        snprintf(buf, buf_size, "%.3gms", seconds * 1000.0);
    } else if (seconds >= 0.000001) {
        snprintf(buf, buf_size, "%.3gus", seconds * 1000000.0);
    } else {
        snprintf(buf, buf_size, "%.3gns", seconds * 1000000000.0);
    }
}

// Draw grid for a single channel with amplitude scale ticks
// zoom_scale: samples per pixel, sample_rate: samples per second
// trigger_enabled: if true, use trigger_display_pos as t=0 reference
// trigger_display_pos: pixel position of trigger point (-1 if not triggered)
void draw_channel_grid(float x, float y, float width, float height,
                       const char *label, Color channel_color, bool show_grid,
                       float zoom_scale, uint32_t sample_rate,
                       bool trigger_enabled, int trigger_display_pos) {
    // Background slightly darker than main bg
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){25, 25, 30, 255});

    float center_y = y + height / 2;

    if (show_grid) {
        // Time-based vertical grid lines (if we have sample rate info)
        if (sample_rate > 0 && zoom_scale > 0) {
            // Calculate time per pixel
            double time_per_pixel = (double)zoom_scale / (double)sample_rate;

            // Calculate rough time division to get reasonable spacing
            double rough_division = time_per_pixel * (double)GRID_MIN_SPACING_PX;

            // Snap to 1-2-5 sequence
            double time_division = snap_to_125(rough_division);

            // Determine the reference point (t=0) in pixels from left edge
            // If trigger is enabled and we have a valid position, use that as t=0
            // Otherwise, t=0 is at the left edge
            double t0_pixel = 0.0;
            if (trigger_enabled && trigger_display_pos >= 0 && trigger_display_pos < (int)width) {
                t0_pixel = (double)trigger_display_pos;
            }

            // Calculate the time offset at the left edge (will be negative if trigger is after left edge)
            double time_at_left = -t0_pixel * time_per_pixel;

            // Find the first grid line position (snap to division boundary)
            // We want the first t such that t >= time_at_left and t is a multiple of time_division
            double first_grid_time;
            if (time_at_left >= 0) {
                first_grid_time = ceil(time_at_left / time_division) * time_division;
            } else {
                first_grid_time = ceil(time_at_left / time_division) * time_division;
            }

            // Draw vertical grid lines at time intervals
            char time_buf[8];
            int division_count = 0;
            for (double t = first_grid_time; division_count < GRID_MAX_DIVISIONS; t += time_division) {
                // Convert time to pixel position
                double px = (t - time_at_left) / time_per_pixel;
                if (px >= (double)width) break;
                if (px < 0) continue;

                float gx = x + (float)px;

                // Draw grid line (use major color for t=0)
                bool is_zero = (fabs(t) < time_division * 0.01);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height},
                         is_zero ? COLOR_GRID_MAJOR : COLOR_GRID);

                // Draw time label (skip if too close to edges)
                if (gx > x + 40 && gx < x + width - 40) {
                    if (is_zero) {
                        // Draw "0" for the trigger point
                        int label_w = gui_text_measure_mono("0", FONT_SIZE_OSC_SCALE);
                        gui_text_draw_mono("0", gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
                    } else {
                        format_time_label(time_buf, sizeof(time_buf), fabs(t));
                        int label_w = gui_text_measure_mono(time_buf, FONT_SIZE_OSC_SCALE);
                        gui_text_draw_mono(time_buf, gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
                    }
                }
                division_count++;
            }

            // Show time per division in top-right corner (below channel label)
            format_time_label(time_buf, sizeof(time_buf), time_division);
            char div_label[48];
            snprintf(div_label, sizeof(div_label), "%s/div", time_buf);
            int div_label_w = gui_text_measure_mono(div_label, FONT_SIZE_OSC_DIV);
            gui_text_draw_mono(div_label, x + width - div_label_w - 8, y + 26, FONT_SIZE_OSC_DIV, COLOR_TEXT);
        } else {
            // Fallback: fixed divisions when no sample rate available
            const int fixed_divisions = 10;
            for (int i = 1; i < fixed_divisions; i++) {
                float gx = x + (width * i / fixed_divisions);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height}, COLOR_GRID);
            }
        }

        // Horizontal grid lines (amplitude divisions)
        for (int i = 1; i < GRID_DIVISIONS_Y; i++) {
            float gy = y + (height * i / GRID_DIVISIONS_Y);
            DrawLineV((Vector2){x, gy}, (Vector2){x + width, gy}, COLOR_GRID);
        }
    }

    // Center line (0V reference) - always show
    DrawLineEx((Vector2){x, center_y}, (Vector2){x + width, center_y}, 1.0f, COLOR_GRID_MAJOR);

    // Border
    DrawRectangleLinesEx((Rectangle){x, y, width, height}, 1, COLOR_GRID_MAJOR);

    // Amplitude scale ticks on left side (use mono font for numbers)
    const char *tick_labels[] = { "+0.5", "0", "-0.5" };
    float tick_positions[] = { 0.25f, 0.5f, 0.75f };
    for (int i = 0; i < 3; i++) {
        float tick_y = y + height * tick_positions[i];
        // Tick mark
        DrawLineEx((Vector2){x, tick_y}, (Vector2){x + 4, tick_y}, 1.0f, COLOR_GRID_MAJOR);
        // Label (offset to not overlap with border)
        gui_text_draw_mono(tick_labels[i], x + 6, tick_y - 7, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
    }

    // Channel label in top-right corner
    int label_width = gui_text_measure(label, FONT_SIZE_OSC_LABEL);
    gui_text_draw(label, x + width - label_width - 8, y + 4, FONT_SIZE_OSC_LABEL, channel_color);
}

//-----------------------------------------------------------------------------
// Trigger Marker Drawing (for panel state)
//-----------------------------------------------------------------------------

static void draw_panel_trigger_markers(float x, float y, float w, float h,
                                        const waveform_panel_state_t *state,
                                        float amplitude_scale, Color color) {
    if (!state->trigger_enabled) return;

    // Skip drawing trigger markers in CVBS mode (level is auto-detected)
    if (state->trigger_mode == TRIGGER_MODE_CVBS_HSYNC) return;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * amplitude_scale;

    // Convert trigger level (-2048 to +2047) to normalized (-1 to +1)
    float level_norm = state->trigger_level / 2048.0f;
    float level_y = center_y - level_norm * scale;

    // Clamp to panel bounds
    if (level_y < y) level_y = y;
    if (level_y > y + h) level_y = y + h;

    // Draw dashed trigger level line (semi-transparent channel color)
    Color trig_color = { color.r, color.g, color.b, 128 };
    float dash_len = 8.0f;
    float gap_len = 4.0f;
    for (float dx = 0; dx < w; dx += dash_len + gap_len) {
        float dash_end = dx + dash_len;
        if (dash_end > w) dash_end = w;
        DrawLineEx((Vector2){x + dx, level_y}, (Vector2){x + dash_end, level_y}, 1.0f, trig_color);
    }

    // Draw small trigger arrow on the left edge
    float arrow_size = 6.0f;
    Vector2 arrow_tip = { x + 2, level_y };
    Vector2 arrow_top = { x + 2 + arrow_size, level_y - arrow_size/2 };
    Vector2 arrow_bot = { x + 2 + arrow_size, level_y + arrow_size/2 };
    DrawTriangle(arrow_tip, arrow_bot, arrow_top, trig_color);

    // Draw vertical trigger position marker at actual trigger position (if triggered)
    if (state->trigger_display_pos >= 0 && state->trigger_display_pos < (int)w) {
        float trigger_x = x + (float)state->trigger_display_pos;

        Color marker_color = { color.r, color.g, color.b, 80 };
        DrawLineEx((Vector2){trigger_x, y}, (Vector2){trigger_x, y + h}, 1.0f, marker_color);

        // Draw small "T" marker at the trigger intersection
        Color t_marker_color = { color.r, color.g, color.b, 200 };
        DrawLineEx((Vector2){trigger_x - 4, level_y - 8}, (Vector2){trigger_x + 4, level_y - 8}, 2.0f, t_marker_color);
        DrawLineEx((Vector2){trigger_x, level_y - 8}, (Vector2){trigger_x, level_y - 2}, 2.0f, t_marker_color);
    }
}


//-----------------------------------------------------------------------------
// Waveform Panel Rendering (using panel state)
//-----------------------------------------------------------------------------

// Internal render for line mode using panel state
static void render_waveform_line_internal(waveform_panel_state_t *state,
                                           gui_app_t *app, int channel,
                                           Rectangle bounds, Color color) {
    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
    const char *label = (channel == 0) ? "CH A" : "CH B";

    // Update display width for processing thread
    int new_display_width = (int)w;
    if (new_display_width < 100) new_display_width = 100;
    if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
    atomic_store(&state->display_width, new_display_width);

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      state->zoom_scale, sample_rate,
                      state->trigger_enabled, state->trigger_display_pos);

    // Draw trigger level and position markers
    draw_panel_trigger_markers(x, y, w, h, state, app->settings.amplitude_scale, color);

    // Get display samples from panel state
    waveform_sample_t *samples = state->display_samples;
    size_t samples_available = state->display_samples_available;

    if (samples_available == 0) return;

    int display_width = (int)w;
    if (display_width > DISPLAY_BUFFER_SIZE) display_width = DISPLAY_BUFFER_SIZE;
    int samples_to_draw = (samples_available < (size_t)display_width) ?
                          (int)samples_available : display_width;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;

    // Draw waveform as connected line
    float prev_py = center_y;
    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + px;
        float py = center_y - samples[px].value * scale;

        // Clamp to bounds
        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, color);
        }
        prev_py = py;
    }
}

// Internal render for phosphor mode using panel state
static void render_waveform_phosphor_internal(waveform_panel_state_t *state,
                                               gui_app_t *app, int channel,
                                               Rectangle bounds, Color color) {
    float x = bounds.x, y = bounds.y, w = bounds.width, h = bounds.height;
    const char *label = (channel == 0) ? "CH A" : "CH B";

    // Update display width for processing thread
    int new_display_width = (int)w;
    if (new_display_width < 100) new_display_width = 100;
    if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
    atomic_store(&state->display_width, new_display_width);

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      state->zoom_scale, sample_rate,
                      state->trigger_enabled, state->trigger_display_pos);

    // Draw trigger level and position markers
    draw_panel_trigger_markers(x, y, w, h, state, app->settings.amplitude_scale, color);

    // Get display samples from panel state
    waveform_sample_t *samples = state->display_samples;
    size_t samples_available = state->display_samples_available;

    if (samples_available == 0) return;

    int buf_width = (int)w;
    int buf_height = (int)h;
    if (buf_width <= 0 || buf_height <= 0) return;

    int samples_to_draw = (samples_available < (size_t)buf_width) ?
                          (int)samples_available : buf_width;

    // Get phosphor state from panel
    phosphor_rt_t *prt = state->phosphor;
    if (prt) {
        // Initialize/resize phosphor if needed
        phosphor_rt_init(prt, buf_width, buf_height);

        // Update phosphor
        phosphor_rt_begin_frame(prt);
        phosphor_rt_draw_waveform(prt, samples, samples_to_draw, app->settings.amplitude_scale);
        phosphor_rt_end_frame(prt);

        // Render phosphor to screen
        if (state->phosphor_color == PHOSPHOR_COLOR_OPACITY) {
            phosphor_rt_render_opacity(prt, x, y);
        } else {
            phosphor_rt_render(prt, x, y, false);
        }
    }

    // Draw line overlay on phosphor (semi-transparent)
    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;
    float prev_py = center_y;
    Color waveform_color = {color.r, color.g, color.b, 200};

    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + px;
        float py = center_y - samples[px].value * scale;

        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, waveform_color);
        }
        prev_py = py;
    }
}

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

ssize_t find_trigger_point_from(const int16_t *buf, size_t count,
                                 const channel_trigger_t *trig, size_t min_index) {
    return trigger_find_from_config(buf, count, trig, min_index);
}

ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig) {
    return trigger_find_from_config(buf, count, trig, 1);
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================

//-----------------------------------------------------------------------------
// Waveform Panel Resampler (per-panel libsoxr)
//-----------------------------------------------------------------------------

#if LIBSOXR_ENABLED
// Ensure resampler is initialized with correct decimation ratio
static soxr_t waveform_ensure_resampler(waveform_panel_state_t *state, float decimation) {
    // Check if we need to create or recreate the resampler
    float ratio_diff = fabsf(state->resampler_ratio - decimation);
    bool need_recreate = (state->resampler == NULL) ||
                         (ratio_diff > decimation * 0.001f);

    if (!need_recreate) {
        return (soxr_t)state->resampler;
    }

    // Destroy old resampler if exists
    if (state->resampler) {
        soxr_delete((soxr_t)state->resampler);
        state->resampler = NULL;
    }

    // Create new resampler for this decimation ratio
    const double in_rate = (double)decimation;
    const double out_rate = 1.0;

    soxr_error_t soxr_err = NULL;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t qual_spec = soxr_quality_spec(SOXR_QQ, 0);

    soxr_t resampler = soxr_create(in_rate, out_rate, 1, &soxr_err, &io_spec, &qual_spec, NULL);
    if (!resampler || soxr_err) {
        return NULL;
    }

    state->resampler = resampler;
    state->resampler_ratio = decimation;

    return resampler;
}
#endif

static void waveform_cleanup_resampler(waveform_panel_state_t *state) {
#if LIBSOXR_ENABLED
    if (state && state->resampler) {
        soxr_delete((soxr_t)state->resampler);
        state->resampler = NULL;
        state->resampler_ratio = 0.0f;
    }
#else
    (void)state;
#endif
}

//-----------------------------------------------------------------------------
// Waveform Panel Resampling (per-panel display buffer)
//-----------------------------------------------------------------------------

static size_t waveform_resample_to_buffer(waveform_panel_state_t *state,
                                           const int16_t *buf, size_t num_samples,
                                           size_t start_idx, float decimation,
                                           size_t target_width) {
    const float scale = 1.0f / 2048.0f;

    // Clamp target width to buffer size
    if (target_width > DISPLAY_BUFFER_SIZE) target_width = DISPLAY_BUFFER_SIZE;
    if (target_width == 0) target_width = DISPLAY_BUFFER_SIZE;

    // Calculate how many source samples we have available
    size_t available = (start_idx < num_samples) ? (num_samples - start_idx) : 0;
    if (available == 0) return 0;

    // Calculate display count based on available samples and decimation
    size_t display_count = (size_t)((float)available / decimation);
    if (display_count > target_width) display_count = target_width;
    if (display_count == 0) return 0;

    // Calculate how many source samples we actually need
    size_t source_samples_needed = (size_t)ceilf((float)display_count * decimation);
    if (source_samples_needed > available) source_samples_needed = available;

    waveform_sample_t *dest = state->display_samples;

#if LIBSOXR_ENABLED
    // Bypass soxr for 1:1 ratio
    if (decimation >= 0.999f && decimation <= 1.001f) {
        size_t count = (display_count < source_samples_needed) ? display_count : source_samples_needed;
        for (size_t i = 0; i < count; i++) {
            dest[i].value = (float)buf[start_idx + i] * scale;
        }
        return count;
    }

    // Temporary buffers for float conversion
    static float temp_input[DISPLAY_BUFFER_SIZE * 256];
    static float temp_output[DISPLAY_BUFFER_SIZE];

    size_t max_input = sizeof(temp_input) / sizeof(temp_input[0]);
    if (source_samples_needed > max_input) {
        source_samples_needed = max_input;
        display_count = (size_t)((float)source_samples_needed / decimation);
        if (display_count == 0) display_count = 1;
    }

    // Convert input to float with scaling
    for (size_t i = 0; i < source_samples_needed; i++) {
        temp_input[i] = (float)buf[start_idx + i] * scale;
    }

    // Get or create resampler for this decimation ratio
    soxr_t resampler = waveform_ensure_resampler(state, decimation);
    if (!resampler) {
        return 0;
    }

    // Clear resampler state for fresh data each frame
    soxr_clear(resampler);

    // Process through resampler
    size_t in_done = 0, out_done = 0;
    soxr_error_t soxr_err = soxr_process(resampler,
                                          temp_input, source_samples_needed, &in_done,
                                          temp_output, display_count, &out_done);

    if (soxr_err || out_done == 0) {
        return 0;
    }

    // Copy to destination
    for (size_t i = 0; i < out_done; i++) {
        dest[i].value = temp_output[i];
    }

    return out_done;
#else
    // No libsoxr: simple point sampling
    for (size_t i = 0; i < display_count; i++) {
        size_t src_idx = start_idx + (size_t)((float)i * decimation);
        if (src_idx >= num_samples) src_idx = num_samples - 1;
        dest[i].value = (float)buf[src_idx] * scale;
    }
    return display_count;
#endif
}

//-----------------------------------------------------------------------------
// Waveform Panel Processing (per-panel trigger detection and resampling)
//-----------------------------------------------------------------------------

// Find trigger point using panel's trigger configuration
static ssize_t waveform_find_trigger_from(const int16_t *buf, size_t count,
                                           const waveform_panel_state_t *state,
                                           size_t min_index) {
    if (!state->trigger_enabled) return -1;
    if (count < 2 || min_index >= count) return -1;

    // Use the gui_trigger module for actual trigger detection
    // Create a temporary channel_trigger_t to use with existing trigger code
    channel_trigger_t temp_trig = {
        .enabled = state->trigger_enabled,
        .level = state->trigger_level,
        .trigger_mode = state->trigger_mode,
    };

    return trigger_find_from_config(buf, count, &temp_trig, min_index);
}

// Process raw samples and update panel's display buffer
static bool waveform_process_display(waveform_panel_state_t *state,
                                      const int16_t *buf, size_t num_samples) {
    if (!state || !buf || num_samples == 0) return false;

    // Get display width (set by renderer, defaults to DISPLAY_BUFFER_SIZE)
    size_t display_width = (size_t)atomic_load(&state->display_width);
    if (display_width == 0 || display_width > DISPLAY_BUFFER_SIZE) {
        display_width = DISPLAY_BUFFER_SIZE;
    }

    // Get decimation factor from zoom_scale
    float decimation = state->zoom_scale;
    if (decimation < ZOOM_SCALE_MIN) decimation = ZOOM_SCALE_MIN;
    if (decimation > ZOOM_SCALE_MAX) decimation = ZOOM_SCALE_MAX;

    // How many raw samples we need for the full display at this zoom
    float display_window = (float)display_width * decimation;

    // If trigger is disabled, just show the start of the buffer
    if (!state->trigger_enabled) {
        state->trigger_display_pos = -1;
        state->display_samples_available = waveform_resample_to_buffer(
            state, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // When zoomed out so far that display_window >= 90% of buffer,
    // there's no room for trigger positioning - just show from start
    if (display_window >= (float)num_samples * 0.9f) {
        state->trigger_display_pos = -1;
        state->display_samples_available = waveform_resample_to_buffer(
            state, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Trigger point should appear at 10% across the display
    size_t trigger_display_pos = display_width / 10;
    float pre_trigger_raw_samples = (float)trigger_display_pos * decimation;
    float post_trigger_raw_samples = display_window - pre_trigger_raw_samples;

    // Calculate the valid search range for triggers
    size_t min_trig_pos = (size_t)pre_trigger_raw_samples;
    size_t max_trig_pos = num_samples - (size_t)post_trigger_raw_samples;

    // Check if there's a valid search range
    if (min_trig_pos >= max_trig_pos) {
        state->trigger_display_pos = -1;
        state->display_samples_available = waveform_resample_to_buffer(
            state, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Find trigger point starting from minimum valid position
    ssize_t trig_pos = waveform_find_trigger_from(buf, max_trig_pos, state, min_trig_pos);

    if (trig_pos < 0) {
        // No trigger found - hold previous display
        return false;
    }

    // Trigger found - place it at desired position
    size_t start_pos = (size_t)((float)trig_pos - pre_trigger_raw_samples);
    state->trigger_display_pos = (int)trigger_display_pos;
    state->display_samples_available = waveform_resample_to_buffer(
        state, buf, num_samples, start_pos, decimation, display_width);
    return true;
}

// Vtable process function (called from display thread via panel_process_all)
static void waveform_vtable_process(void *state_ptr, const int16_t *samples,
                                     size_t count, uint32_t sample_rate) {
    (void)sample_rate;  // Currently unused but available for future use
    if (!state_ptr || !samples || count == 0) return;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    waveform_process_display(state, samples, count);
}

//-----------------------------------------------------------------------------
// Waveform Line Panel Vtable
//-----------------------------------------------------------------------------

static void *waveform_line_create(void) {
    waveform_panel_state_t *state = calloc(1, sizeof(waveform_panel_state_t));
    if (!state) return NULL;

    // Initialize trigger settings with defaults
    state->trigger_enabled = false;
    state->trigger_level = 0;
    state->trigger_mode = TRIGGER_MODE_RISING;
    state->zoom_scale = ZOOM_SCALE_DEFAULT;
    state->trigger_display_pos = -1;

    // Initialize display state
    atomic_init(&state->display_width, DISPLAY_BUFFER_SIZE);
    state->display_samples_available = 0;

    // Resampler initialized on demand
    state->resampler = NULL;
    state->resampler_ratio = 0.0f;

    // No phosphor for line mode
    state->phosphor = NULL;
    state->phosphor_color = PHOSPHOR_COLOR_HEATMAP;

    // UI state
    state->dropdown_open = false;
    state->dragging = false;

    state->initialized = true;
    return state;
}

static void waveform_line_destroy(void *state_ptr) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    waveform_cleanup_resampler(state);
    // Line mode has no phosphor to clean up

    free(state);
}

static void waveform_line_clear(void *state_ptr) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    state->display_samples_available = 0;
    state->trigger_display_pos = -1;
}

//-----------------------------------------------------------------------------
// Waveform Panel Overlay (Trigger Mode Dropdown)
//-----------------------------------------------------------------------------

static void waveform_render_overlay(void *state_ptr, Rectangle bounds) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    if (!state->initialized) return;

    // Format current trigger mode
    const char *mode_label;
    if (!state->trigger_enabled) {
        mode_label = "Off";
    } else {
        mode_label = s_trigger_mode_labels[state->trigger_mode];
    }

    // Button dimensions (similar to histogram)
    float btn_w = 60, btn_h = 18;
    float btn_x = bounds.x + bounds.width - btn_w - 8;
    float btn_y = bounds.y + 8;

    // Draw "Trigger:" label before the dropdown button
    const char *trig_label = "Trig:";
    int trig_label_w = gui_text_measure(trig_label, FONT_SIZE_DROPDOWN_OPT);
    float label_x = btn_x - trig_label_w - 6;
    float label_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(trig_label, label_x, label_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT_DIM);

    state->button_rect = (Rectangle){btn_x, btn_y, btn_w, btn_h};

    // Draw button
    Color btn_bg = state->dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    DrawRectangleRounded(state->button_rect, 0.15f, 4, btn_bg);

    // Draw text centered with arrow
    int text_w = gui_text_measure(mode_label, FONT_SIZE_DROPDOWN_OPT);
    float arrow_w = 8;
    float total_w = text_w + arrow_w + 4;
    float text_x = btn_x + (btn_w - total_w) / 2;
    float text_y = btn_y + (btn_h - FONT_SIZE_DROPDOWN_OPT) / 2;
    gui_text_draw(mode_label, text_x, text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

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
        int num_options = TRIGGER_MODE_COUNT + 1;  // +1 for "Off" option

        DrawRectangleRounded((Rectangle){btn_x, opt_y, btn_w, opt_h * num_options},
                             0.1f, 4, COLOR_PANEL_BG);

        // Option 0: Off
        Rectangle off_rect = {btn_x, opt_y, btn_w, opt_h};
        state->options_rect[0] = off_rect;  // Store in first slot (repurpose)

        bool is_off = !state->trigger_enabled;
        Vector2 mouse = GetMousePosition();
        bool hover_off = CheckCollisionPointRec(mouse, off_rect);

        Color off_bg = gui_dropdown_option_color(is_off, hover_off);
        DrawRectangleRec(off_rect, off_bg);

        const char *off_label = "Off";
        int off_text_w = gui_text_measure(off_label, FONT_SIZE_DROPDOWN_OPT);
        float off_text_x = btn_x + btn_w/2 - off_text_w/2;
        float off_text_y = opt_y + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
        gui_text_draw(off_label, off_text_x, off_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);

        // Options 1..N: Trigger modes
        for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
            Rectangle opt_rect = {btn_x, opt_y + (i + 1) * opt_h, btn_w, opt_h};
            state->options_rect[i] = opt_rect;

            bool is_selected = state->trigger_enabled && (state->trigger_mode == (trigger_mode_t)i);
            bool hover = CheckCollisionPointRec(mouse, opt_rect);

            Color opt_bg = gui_dropdown_option_color(is_selected, hover);
            DrawRectangleRec(opt_rect, opt_bg);

            const char *opt_label = s_trigger_mode_labels[i];
            int opt_text_w = gui_text_measure(opt_label, FONT_SIZE_DROPDOWN_OPT);
            float opt_text_x = btn_x + btn_w/2 - opt_text_w/2;
            float opt_text_y = opt_y + (i + 1) * opt_h + (opt_h - FONT_SIZE_DROPDOWN_OPT) / 2;
            gui_text_draw(opt_label, opt_text_x, opt_text_y, FONT_SIZE_DROPDOWN_OPT, COLOR_TEXT);
        }
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Click Handler (with dropdown support)
//-----------------------------------------------------------------------------

static bool waveform_panel_handle_click(void *state_ptr, struct gui_app *app, int channel,
                                         Vector2 click, Rectangle bounds) {
    (void)channel;  // Unused since we now use per-panel state

    if (!state_ptr || !app) return false;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    // Check button click (toggle dropdown)
    if (CheckCollisionPointRec(click, state->button_rect)) {
        state->dropdown_open = !state->dropdown_open;
        return true;
    }

    // Check option clicks if dropdown is open
    if (state->dropdown_open) {
        // Check "Off" option (stored in options_rect[0] when dropdown open)
        Rectangle off_rect = {state->button_rect.x, state->button_rect.y + state->button_rect.height,
                              state->button_rect.width, 20};
        if (CheckCollisionPointRec(click, off_rect)) {
            state->trigger_enabled = false;
            state->dropdown_open = false;
            return true;
        }

        // Check trigger mode options
        for (int i = 0; i < TRIGGER_MODE_COUNT; i++) {
            Rectangle opt_rect = {state->button_rect.x,
                                  state->button_rect.y + state->button_rect.height + (i + 1) * 20,
                                  state->button_rect.width, 20};
            if (CheckCollisionPointRec(click, opt_rect)) {
                state->trigger_enabled = true;
                state->trigger_mode = (trigger_mode_t)i;
                state->dropdown_open = false;
                return true;
            }
        }

        // Click outside dropdown closes it
        state->dropdown_open = false;
        return true;
    }

    // Check if click is within bounds for trigger level drag
    if (!CheckCollisionPointRec(click, bounds)) {
        return false;
    }

    // Don't allow trigger level drag in CVBS mode (level is auto-detected)
    if (state->trigger_mode == TRIGGER_MODE_CVBS_HSYNC) {
        return false;
    }

    // Start dragging on mouse press
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        state->dragging = true;

        // Enable trigger when starting to drag
        state->trigger_enabled = true;

        // Calculate trigger level from click position
        float center_y = bounds.y + bounds.height / 2.0f;
        float half_height = (bounds.height / 2.0f) * app->settings.amplitude_scale;

        float level_norm = (center_y - click.y) / half_height;
        if (level_norm > 1.0f) level_norm = 1.0f;
        if (level_norm < -1.0f) level_norm = -1.0f;

        state->trigger_level = (int16_t)(level_norm * 2047.0f);

        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// Waveform Panel Drag Update (called during render for continuous drag)
//-----------------------------------------------------------------------------

static void waveform_panel_update_drag(waveform_panel_state_t *state, gui_app_t *app, Rectangle bounds) {
    if (!state || !app || !state->dragging) return;

    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        Vector2 mouse = GetMousePosition();

        // Calculate trigger level from mouse position
        float center_y = bounds.y + bounds.height / 2.0f;
        float half_height = (bounds.height / 2.0f) * app->settings.amplitude_scale;

        float level_norm = (center_y - mouse.y) / half_height;
        if (level_norm > 1.0f) level_norm = 1.0f;
        if (level_norm < -1.0f) level_norm = -1.0f;

        state->trigger_level = (int16_t)(level_norm * 2047.0f);
    } else {
        // Mouse released - stop dragging
        state->dragging = false;
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Scroll Handler (per-panel zoom)
//-----------------------------------------------------------------------------

static bool waveform_panel_handle_scroll(void *state_ptr, float delta, Rectangle bounds) {
    if (!state_ptr || delta == 0.0f) return false;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, bounds)) return false;

    // Smooth zoom: multiply/divide by a factor for each scroll step
    const float zoom_factor = 1.10f;

    if (delta > 0.0f) {
        // Scroll up = zoom in (fewer samples per pixel)
        state->zoom_scale /= zoom_factor;
        if (state->zoom_scale < ZOOM_SCALE_MIN) {
            state->zoom_scale = ZOOM_SCALE_MIN;
        }
    } else {
        // Scroll down = zoom out (more samples per pixel)
        state->zoom_scale *= zoom_factor;
        if (state->zoom_scale > ZOOM_SCALE_MAX) {
            state->zoom_scale = ZOOM_SCALE_MAX;
        }
    }

    return true;
}

static void waveform_line_render(void *state_ptr, gui_app_t *app, int channel,
                                  Rectangle bounds, Color channel_color) {
    if (!app || !state_ptr) return;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    if (!state->initialized) return;

    // Update drag state for trigger level (continuous while mouse is held)
    waveform_panel_update_drag(state, app, bounds);

    // Set cursor when hovering over waveform panel or dragging
    Vector2 mouse = GetMousePosition();
    if (!gui_popup_is_open()) {
        if (CheckCollisionPointRec(mouse, bounds) || state->dragging) {
            SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
        }
    }

    render_waveform_line_internal(state, app, channel, bounds, channel_color);
}

static const panel_vtable_t s_waveform_line_vtable = {
    .name = "Line",
    .create = waveform_line_create,
    .destroy = waveform_line_destroy,
    .clear = waveform_line_clear,
    .process = waveform_vtable_process,
    .render = waveform_line_render,
    .render_overlay = waveform_render_overlay,
    .handle_click = waveform_panel_handle_click,
    .handle_scroll = waveform_panel_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_waveform_line_panel_register(void) {
    panel_register(PANEL_VIEW_WAVEFORM_LINE, &s_waveform_line_vtable);
}

//-----------------------------------------------------------------------------
// Waveform Phosphor Panel Vtable
//-----------------------------------------------------------------------------

static void *waveform_phosphor_create(void) {
    waveform_panel_state_t *state = calloc(1, sizeof(waveform_panel_state_t));
    if (!state) return NULL;

    // Initialize trigger settings with defaults
    state->trigger_enabled = false;
    state->trigger_level = 0;
    state->trigger_mode = TRIGGER_MODE_RISING;
    state->zoom_scale = ZOOM_SCALE_DEFAULT;
    state->trigger_display_pos = -1;

    // Initialize display state
    atomic_init(&state->display_width, DISPLAY_BUFFER_SIZE);
    state->display_samples_available = 0;

    // Resampler initialized on demand
    state->resampler = NULL;
    state->resampler_ratio = 0.0f;

    // Allocate phosphor state for phosphor mode
    state->phosphor = calloc(1, sizeof(phosphor_rt_t));
    if (!state->phosphor) {
        free(state);
        return NULL;
    }
    state->phosphor_color = PHOSPHOR_COLOR_HEATMAP;

    // UI state
    state->dropdown_open = false;
    state->dragging = false;

    state->initialized = true;
    return state;
}

static void waveform_phosphor_destroy(void *state_ptr) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    waveform_cleanup_resampler(state);

    // Clean up phosphor
    if (state->phosphor) {
        phosphor_rt_cleanup(state->phosphor);
        free(state->phosphor);
        state->phosphor = NULL;
    }

    free(state);
}

static void waveform_phosphor_clear(void *state_ptr) {
    if (!state_ptr) return;
    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;

    state->display_samples_available = 0;
    state->trigger_display_pos = -1;

    // Clear phosphor persistence
    if (state->phosphor) {
        phosphor_rt_clear(state->phosphor);
    }
}

static void waveform_phosphor_render(void *state_ptr, gui_app_t *app, int channel,
                                      Rectangle bounds, Color channel_color) {
    if (!app || !state_ptr) return;

    waveform_panel_state_t *state = (waveform_panel_state_t *)state_ptr;
    if (!state->initialized) return;

    // Update drag state for trigger level (continuous while mouse is held)
    waveform_panel_update_drag(state, app, bounds);

    // Set cursor when hovering over waveform panel or dragging
    Vector2 mouse = GetMousePosition();
    if (!gui_popup_is_open()) {
        if (CheckCollisionPointRec(mouse, bounds) || state->dragging) {
            SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
        }
    }

    render_waveform_phosphor_internal(state, app, channel, bounds, channel_color);
}

static const panel_vtable_t s_waveform_phosphor_vtable = {
    .name = "Phosphor",
    .create = waveform_phosphor_create,
    .destroy = waveform_phosphor_destroy,
    .clear = waveform_phosphor_clear,
    .process = waveform_vtable_process,
    .render = waveform_phosphor_render,
    .render_overlay = waveform_render_overlay,
    .handle_click = waveform_panel_handle_click,
    .handle_scroll = waveform_panel_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_waveform_phosphor_panel_register(void) {
    panel_register(PANEL_VIEW_WAVEFORM_PHOSPHOR, &s_waveform_phosphor_vtable);
}
