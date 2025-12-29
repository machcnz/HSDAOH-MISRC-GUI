/*
 * MISRC GUI - Oscilloscope and Trigger Implementation
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 */

#include "gui_oscilloscope.h"
#include "gui_phosphor_rt.h"
#include "gui_fft.h"
#include "../signal/gui_trigger.h"
#include "../ui/gui_popup.h"
#include "gui_text.h"
#include "../ui/gui_ui.h"
#include "gui_panel.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LIBSOXR_ENABLED
#include <soxr.h>
// Forward declaration for cleanup function
static void gui_oscilloscope_cleanup_resampler(channel_trigger_t *trig);
#endif

//-----------------------------------------------------------------------------
// Grid Settings
//-----------------------------------------------------------------------------

#define GRID_DIVISIONS_Y 4  // Per channel (amplitude)
#define GRID_MIN_SPACING_PX 120  // Minimum pixels between time grid lines
#define GRID_MAX_DIVISIONS 20   // Maximum number of time divisions

//-----------------------------------------------------------------------------
// Static State
//-----------------------------------------------------------------------------

// Waveform panel bounds for mouse interaction (stored per channel)
// In split mode, there can be up to 2 waveform panels per channel (left + right)
// We track bounds for waveform panels specifically, not FFT panels
typedef struct {
    Rectangle bounds;
    bool valid;
} panel_bounds_t;

static panel_bounds_t s_waveform_bounds[2][2] = {0};  // [channel][panel_index]
static int s_waveform_panel_count[2] = {0, 0};        // How many waveform panels per channel
static int s_dragging_channel = -1;  // Which channel is being dragged (-1 = none)

// Clear waveform bounds for a channel (call before rendering panels)
void gui_oscilloscope_clear_bounds(int channel) {
    if (channel >= 0 && channel < 2) {
        s_waveform_panel_count[channel] = 0;
        s_waveform_bounds[channel][0].valid = false;
        s_waveform_bounds[channel][1].valid = false;
    }
}

// Register waveform panel bounds (call from waveform renderers)
static void register_waveform_bounds(int channel, float x, float y, float w, float h) {
    if (channel >= 0 && channel < 2) {
        int idx = s_waveform_panel_count[channel];
        if (idx < 2) {
            s_waveform_bounds[channel][idx].bounds = (Rectangle){x, y, w, h};
            s_waveform_bounds[channel][idx].valid = true;
            s_waveform_panel_count[channel]++;
        }
    }
}

// Cleanup oscilloscope resources (static state)
void gui_oscilloscope_cleanup(void) {
    // No static resources to clean up - phosphor cleanup is in gui_phosphor module
}

// Cleanup per-channel resampler resources
void gui_oscilloscope_cleanup_resamplers(gui_app_t *app) {
#if LIBSOXR_ENABLED
    if (app) {
        gui_oscilloscope_cleanup_resampler(&app->trigger_a);
        gui_oscilloscope_cleanup_resampler(&app->trigger_b);
    }
#else
    (void)app;
#endif
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

            // Calculate pixels per division
            double pixels_per_div = time_division / time_per_pixel;

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
// Trigger Marker Drawing (shared by waveform panels)
//-----------------------------------------------------------------------------

static void draw_trigger_markers(float x, float y, float w, float h,
                                  channel_trigger_t *trig, float amplitude_scale, Color color) {
    if (!trig->enabled) return;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * amplitude_scale;

    // Convert trigger level (-2048 to +2047) to normalized (-1 to +1)
    float level_norm = trig->level / 2048.0f;
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
    if (trig->trigger_display_pos >= 0 && trig->trigger_display_pos < (int)w) {
        float trigger_x = x + (float)trig->trigger_display_pos;

        Color marker_color = { color.r, color.g, color.b, 80 };
        DrawLineEx((Vector2){trigger_x, y}, (Vector2){trigger_x, y + h}, 1.0f, marker_color);

        // Draw small "T" marker at the trigger intersection
        Color t_marker_color = { color.r, color.g, color.b, 200 };
        DrawLineEx((Vector2){trigger_x - 4, level_y - 8}, (Vector2){trigger_x + 4, level_y - 8}, 2.0f, t_marker_color);
        DrawLineEx((Vector2){trigger_x, level_y - 8}, (Vector2){trigger_x, level_y - 2}, 2.0f, t_marker_color);
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (Line Mode)
//-----------------------------------------------------------------------------

void render_waveform_line(gui_app_t *app, int channel,
                          float x, float y, float w, float h, Color color) {
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    const char *label = (channel == 0) ? "CH A" : "CH B";

    // Register bounds for mouse interaction (trigger level drag)
    register_waveform_bounds(channel, x, y, w, h);

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      trig->zoom_scale, sample_rate,
                      trig->enabled, trig->trigger_display_pos);

    // Draw trigger level and position markers
    draw_trigger_markers(x, y, w, h, trig, app->settings.amplitude_scale, color);

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

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (Phosphor Mode)
//-----------------------------------------------------------------------------

void render_waveform_phosphor(gui_app_t *app, int channel,
                              float x, float y, float w, float h, Color color) {
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    const char *label = (channel == 0) ? "CH A" : "CH B";

    // Register bounds for mouse interaction (trigger level drag)
    register_waveform_bounds(channel, x, y, w, h);

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      trig->zoom_scale, sample_rate,
                      trig->enabled, trig->trigger_display_pos);

    // Draw trigger level and position markers
    draw_trigger_markers(x, y, w, h, trig, app->settings.amplitude_scale, color);

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

    if (samples_available == 0) return;

    int buf_width = (int)w;
    int buf_height = (int)h;
    if (buf_width <= 0 || buf_height <= 0) return;

    int samples_to_draw = (samples_available < (size_t)buf_width) ?
                          (int)samples_available : buf_width;

    // Get phosphor state for this channel
    phosphor_rt_t *prt = (channel == 0) ? app->phosphor_a : app->phosphor_b;
    if (prt) {
        // Initialize/resize phosphor if needed
        phosphor_rt_init(prt, buf_width, buf_height);

        // Update phosphor
        phosphor_rt_begin_frame(prt);
        phosphor_rt_draw_waveform(prt, samples, samples_to_draw, app->settings.amplitude_scale);
        phosphor_rt_end_frame(prt);

        // Render phosphor to screen
        if (trig->phosphor_color == PHOSPHOR_COLOR_OPACITY) {
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
// Oscilloscope Rendering
//-----------------------------------------------------------------------------

void render_oscilloscope_channel(gui_app_t *app, float x, float y, float width, float height,
                                  int channel, const char *label, Color channel_color) {
    (void)label;  // Label is now drawn by individual panel renderers

    // Initialize text helpers with app (for font access)
    gui_text_set_app(app);

    // Get trigger state for this channel
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;

    // Clear waveform bounds before rendering (waveform renderers will register their bounds)
    gui_oscilloscope_clear_bounds(channel);

    // Store actual display width for the processing thread (atomic for thread safety)
    if (channel >= 0 && channel < 2) {
        int new_display_width = (int)width;
        if (new_display_width < 100) new_display_width = 100;  // Minimum reasonable width
        if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
        atomic_store(&trig->display_width, new_display_width);
    }

    // Render panels using the panel abstraction system
    // Each panel draws its own grid, waveform, and labels
    // Waveform panels will register their bounds for mouse interaction
    render_channel_panels(app, channel, x, y, width, height, channel_color);
}

//-----------------------------------------------------------------------------
// Mouse Interaction
//-----------------------------------------------------------------------------

void handle_oscilloscope_interaction(gui_app_t *app) {
    if (!app) return;

    // Don't process clicks if UI already consumed them (dropdown, popup, etc.)
    if (gui_ui_click_consumed()) return;

    Vector2 mouse = GetMousePosition();
    bool mouse_down = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    bool mouse_pressed = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
    bool mouse_released = IsMouseButtonReleased(MOUSE_LEFT_BUTTON);

    // Check if mouse is over a waveform panel (not FFT)
    int hover_channel = -1;
    for (int ch = 0; ch < 2; ch++) {
        for (int p = 0; p < s_waveform_panel_count[ch]; p++) {
            if (s_waveform_bounds[ch][p].valid) {
                Rectangle bounds = s_waveform_bounds[ch][p].bounds;
                if (mouse.x >= bounds.x && mouse.x < bounds.x + bounds.width &&
                    mouse.y >= bounds.y && mouse.y < bounds.y + bounds.height) {
                    hover_channel = ch;
                    break;
                }
            }
        }
        if (hover_channel >= 0) break;
    }

    // Start dragging on mouse press over waveform panel
    if (mouse_pressed && hover_channel >= 0) {
        s_dragging_channel = hover_channel;
        // Enable trigger when starting to drag
        channel_trigger_t *trig = (hover_channel == 0) ? &app->trigger_a : &app->trigger_b;
        trig->enabled = true;
    }

    // Update trigger level while dragging
    if (mouse_down && s_dragging_channel >= 0) {
        int ch = s_dragging_channel;
        // Use the first waveform panel bounds for this channel (they all share the same height)
        if (s_waveform_panel_count[ch] > 0 && s_waveform_bounds[ch][0].valid) {
            Rectangle bounds = s_waveform_bounds[ch][0].bounds;
            channel_trigger_t *trig = (ch == 0) ? &app->trigger_a : &app->trigger_b;

            // Convert mouse Y to trigger level
            // bounds.y is top (level = +2047)
            // bounds.y + bounds.height is bottom (level = -2048)
            // center is level = 0

            float center_y = bounds.y + bounds.height / 2.0f;
            float half_height = (bounds.height / 2.0f) * app->settings.amplitude_scale;

            // Calculate normalized level (-1 to +1)
            float level_norm = (center_y - mouse.y) / half_height;

            // Clamp to valid range
            if (level_norm > 1.0f) level_norm = 1.0f;
            if (level_norm < -1.0f) level_norm = -1.0f;

            // Convert to 12-bit signed value
            trig->level = (int16_t)(level_norm * 2047.0f);
        }
    }

    // Stop dragging on mouse release
    if (mouse_released) {
        s_dragging_channel = -1;
    }

    // Note: Mouse wheel zoom is now handled via vtable (waveform_handle_scroll)
    // through the unified panel scroll handling system (panel_handle_all_scrolls)

    // Change cursor when hovering over oscilloscope (but not when popup is open)
    if (gui_popup_is_open()) {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    } else if (hover_channel >= 0 || s_dragging_channel >= 0) {
        SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
    } else {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }
}

//-----------------------------------------------------------------------------
// Trigger Detection (wrappers to gui_trigger module)
//-----------------------------------------------------------------------------

ssize_t find_trigger_point_from(const int16_t *buf, size_t count,
                                 const channel_trigger_t *trig, size_t min_index) {
    return trigger_find_from_config(buf, count, trig, min_index);
}

ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig) {
    return trigger_find_from_config(buf, count, trig, 1);
}

//-----------------------------------------------------------------------------
// Decimation and Display Buffer Processing (with libsoxr resampling)
//-----------------------------------------------------------------------------

#if LIBSOXR_ENABLED
// Ensure resampler is initialized with correct decimation ratio
// Returns the resampler handle, creating/recreating if needed
// decimation: the zoom_scale value (samples per pixel, continuous)
static soxr_t ensure_resampler(channel_trigger_t *trig, float decimation) {
    // Check if we need to create or recreate the resampler
    // Recreate if ratio changed by more than 0.1% (to avoid floating point noise)
    float ratio_diff = fabsf(trig->resampler_ratio - decimation);
    bool need_recreate = (trig->resampler == NULL) ||
                         (ratio_diff > decimation * 0.001f);

    if (!need_recreate) {
        return (soxr_t)trig->resampler;
    }

    // Destroy old resampler if exists
    if (trig->resampler) {
        soxr_delete((soxr_t)trig->resampler);
        trig->resampler = NULL;
    }

    // Create new resampler for this decimation ratio
    // in_rate:out_rate = decimation:1
    // printf("Creating soxr resampler for decimation %.3f\n", decimation);
    const double in_rate = (double)decimation;
    const double out_rate = 1.0;

    soxr_error_t soxr_err = NULL;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    // Use SOXR_LQ - low latency works better for non-streaming frame-by-frame processing
    soxr_quality_spec_t qual_spec = soxr_quality_spec(SOXR_QQ, 0);

    soxr_t resampler = soxr_create(in_rate, out_rate, 1, &soxr_err, &io_spec, &qual_spec, NULL);
    if (!resampler || soxr_err) {
        return NULL;
    }

    trig->resampler = resampler;
    trig->resampler_ratio = decimation;

    return resampler;
}

// Cleanup resampler for a channel (call on shutdown)
static void gui_oscilloscope_cleanup_resampler(channel_trigger_t *trig) {
    if (trig && trig->resampler) {
        soxr_delete((soxr_t)trig->resampler);
        trig->resampler = NULL;
        trig->resampler_ratio = 0.0f;
    }
}
#endif

// Resample a single channel from source buffer to display buffer using libsoxr
// Proper anti-alias filtering is applied during resampling
static size_t resample_to_buffer_smooth(channel_trigger_t *trig, waveform_sample_t *dest,
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

    // Calculate how many source samples we actually need for this display width
    size_t source_samples_needed = (size_t)ceilf((float)display_count * decimation);
    if (source_samples_needed > available) source_samples_needed = available;

#if LIBSOXR_ENABLED
    // Bypass soxr for 1:1 ratio (no resampling needed)
    // Use small epsilon to handle floating point imprecision
    if (decimation >= 0.999f && decimation <= 1.001f) {
        size_t count = (display_count < source_samples_needed) ? display_count : source_samples_needed;
        for (size_t i = 0; i < count; i++) {
            dest[i].value = (float)buf[start_idx + i] * scale;
        }
        return count;
    }

    // Temporary buffers for float conversion
    static float temp_input[DISPLAY_BUFFER_SIZE * 256];  // Max ~256x decimation
    static float temp_output[DISPLAY_BUFFER_SIZE];

    // Limit input size to our temp buffer
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
    soxr_t resampler = ensure_resampler(trig, decimation);
    if (!resampler) {
        return 0;
    }

    // Clear resampler state for fresh data each frame
    // (we're not doing continuous streaming, each frame is independent)
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
    // No libsoxr: simple point sampling (no anti-aliasing)
    for (size_t i = 0; i < display_count; i++) {
        size_t src_idx = start_idx + (size_t)((float)i * decimation);
        if (src_idx >= num_samples) src_idx = num_samples - 1;
        dest[i].value = (float)buf[src_idx] * scale;
    }
    return display_count;
#endif
}

bool process_channel_display(gui_app_t *app, const int16_t *buf, size_t num_samples,
                             waveform_sample_t *display_buf, size_t *display_count,
                             channel_trigger_t *trig, int channel) {
    (void)app;      // Unused parameter
    (void)channel;  // Unused parameter (kept for API compatibility)

    // Get display width (set by renderer, defaults to DISPLAY_BUFFER_SIZE)
    // Use atomic_load for thread safety since renderer runs on main thread
    size_t display_width = (size_t)atomic_load(&trig->display_width);
    if (display_width == 0 || display_width > DISPLAY_BUFFER_SIZE) {
        display_width = DISPLAY_BUFFER_SIZE;
    }

    // Get decimation factor from zoom_scale (samples per pixel)
    // Clamp to valid range
    float decimation = trig->zoom_scale;
    if (decimation < ZOOM_SCALE_MIN) decimation = ZOOM_SCALE_MIN;
    if (decimation > ZOOM_SCALE_MAX) decimation = ZOOM_SCALE_MAX;

    // How many raw samples we need for the full display at this zoom
    float display_window = (float)display_width * decimation;

    // If trigger is disabled, just show the start of the buffer
    if (!trig->enabled) {
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // When zoomed out so far that display_window >= 90% of buffer,
    // there's no room for trigger positioning - just show from start
    if (display_window >= (float)num_samples * 0.9f) {
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Trigger point should appear at 10% across the display
    size_t trigger_display_pos = display_width / 10;
    float pre_trigger_raw_samples = (float)trigger_display_pos * decimation;
    float post_trigger_raw_samples = display_window - pre_trigger_raw_samples;

    // Calculate the valid search range for triggers
    // Trigger must be at least pre_trigger_raw_samples into the buffer
    // and have enough room for post_trigger_raw_samples after it
    size_t min_trig_pos = (size_t)pre_trigger_raw_samples;
    size_t max_trig_pos = num_samples - (size_t)post_trigger_raw_samples;

    // Check if there's a valid search range
    if (min_trig_pos >= max_trig_pos) {
        // No valid range - display window too large for this buffer
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Find trigger point starting from minimum valid position
    ssize_t trig_pos = find_trigger_point_from(buf, max_trig_pos, trig, min_trig_pos);

    if (trig_pos < 0) {
        // No trigger found in valid range - hold previous display
        return false;
    }

    // Trigger found in valid range - place it at desired position
    size_t start_pos = (size_t)((float)trig_pos - pre_trigger_raw_samples);
    trig->trigger_display_pos = (int)trigger_display_pos;
    *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, start_pos, decimation, display_width);
    return true;
}

void gui_oscilloscope_update_display(gui_app_t *app, const int16_t *buf_a,
                                      const int16_t *buf_b, size_t num_samples) {
    // Process channel A
    size_t count_a = app->display_samples_available_a;
    if (process_channel_display(app, buf_a, num_samples,
                                app->display_samples_a, &count_a, &app->trigger_a, 0)) {
        app->display_samples_available_a = count_a;
    }

    // Process channel B
    size_t count_b = app->display_samples_available_b;
    if (process_channel_display(app, buf_b, num_samples,
                                app->display_samples_b, &count_b, &app->trigger_b, 1)) {
        app->display_samples_available_b = count_b;
    }
}

//=============================================================================
// Panel Interface (vtable) Implementation
//=============================================================================

// Note: Waveform panels use shared state from gui_app_t (phosphor, trigger,
// display samples). The vtable lifecycle functions are no-ops because the
// actual state is managed at the app level, not per-panel.
//
// The render function receives display_samples from the caller, but also
// needs access to app state for grids, triggers, phosphor, etc.
// This is a limitation of the current vtable interface.

//-----------------------------------------------------------------------------
// Waveform Panel Scroll Handler (shared by line and phosphor)
//-----------------------------------------------------------------------------

static bool waveform_handle_scroll(void *state, float delta, Rectangle bounds) {
    (void)state;  // Waveform uses app->trigger state, not per-panel state

    if (delta == 0.0f) return false;

    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, bounds)) return false;

    // Determine which channel this panel belongs to by checking bounds
    // This is a workaround since we don't have channel info in scroll handler
    // The panel system stores bounds per-channel, so we check both
    extern gui_app_t *g_app_ptr;  // Set during render
    if (!g_app_ptr) return false;

    // Check which channel's bounds this matches
    int channel = -1;
    if (CheckCollisionPointRec(mouse, g_app_ptr->panel_config_a.left_bounds) ||
        CheckCollisionPointRec(mouse, g_app_ptr->panel_config_a.right_bounds)) {
        channel = 0;
    } else if (CheckCollisionPointRec(mouse, g_app_ptr->panel_config_b.left_bounds) ||
               CheckCollisionPointRec(mouse, g_app_ptr->panel_config_b.right_bounds)) {
        channel = 1;
    }

    if (channel < 0) return false;

    channel_trigger_t *trig = (channel == 0) ? &g_app_ptr->trigger_a : &g_app_ptr->trigger_b;

    // Smooth zoom: multiply/divide by a factor for each scroll step
    const float zoom_factor = 1.10f;

    if (delta > 0.0f) {
        // Scroll up = zoom in (fewer samples per pixel)
        trig->zoom_scale /= zoom_factor;
        if (trig->zoom_scale < 0.5f) {
            trig->zoom_scale = 0.5f;
        }
    } else {
        // Scroll down = zoom out (more samples per pixel)
        trig->zoom_scale *= zoom_factor;
        if (trig->zoom_scale > ZOOM_SCALE_MAX) {
            trig->zoom_scale = ZOOM_SCALE_MAX;
        }
    }

    return true;
}

// Global app pointer for waveform scroll handler (set during render)
gui_app_t *g_app_ptr = NULL;

//-----------------------------------------------------------------------------
// Waveform Line Panel Vtable
//-----------------------------------------------------------------------------

static void *waveform_line_create(void) {
    // No per-panel state needed - uses shared app state
    return NULL;
}

static void waveform_line_destroy(void *state) {
    (void)state;
}

static void waveform_line_clear(void *state) {
    (void)state;
}

static void waveform_line_render(void *state, gui_app_t *app, int channel,
                                  Rectangle bounds, Color channel_color) {
    (void)state;

    if (!app) return;

    // Set global app pointer for scroll handler
    g_app_ptr = app;

    // Delegate to existing render function
    render_waveform_line(app, channel, bounds.x, bounds.y,
                         bounds.width, bounds.height, channel_color);
}

static const panel_vtable_t s_waveform_line_vtable = {
    .name = "Line",
    .create = waveform_line_create,
    .destroy = waveform_line_destroy,
    .clear = waveform_line_clear,
    .process = NULL,  // Waveform uses resampled display samples
    .render = waveform_line_render,
    .render_overlay = NULL,
    .handle_click = NULL,  // Trigger drag handled by handle_oscilloscope_interaction()
    .handle_scroll = waveform_handle_scroll,  // Time-axis zoom
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
    // No per-panel state - uses shared app->phosphor_a/b
    return NULL;
}

static void waveform_phosphor_destroy(void *state) {
    (void)state;
}

static void waveform_phosphor_clear(void *state) {
    (void)state;
}

static void waveform_phosphor_render(void *state, gui_app_t *app, int channel,
                                      Rectangle bounds, Color channel_color) {
    (void)state;

    if (!app) return;

    // Set global app pointer for scroll handler
    g_app_ptr = app;

    // Delegate to existing render function
    render_waveform_phosphor(app, channel, bounds.x, bounds.y,
                             bounds.width, bounds.height, channel_color);
}

static const panel_vtable_t s_waveform_phosphor_vtable = {
    .name = "Phosphor",
    .create = waveform_phosphor_create,
    .destroy = waveform_phosphor_destroy,
    .clear = waveform_phosphor_clear,
    .process = NULL,
    .render = waveform_phosphor_render,
    .render_overlay = NULL,
    .handle_click = NULL,  // Trigger drag handled by handle_oscilloscope_interaction()
    .handle_scroll = waveform_handle_scroll,  // Time-axis zoom
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_waveform_phosphor_panel_register(void) {
    panel_register(PANEL_VIEW_WAVEFORM_PHOSPHOR, &s_waveform_phosphor_vtable);
}
