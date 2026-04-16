/*
 * MISRC GUI - UI Layout Implementation
 * Clay-based declarative UI layout (Clay v0.14 API)
 */

#include "gui_ui.h"
#include "gui_dropdown.h"
#include "gui_popup.h"
#include "../visualization/gui_fft.h"
#include "../signal/gui_cvbs.h"
#include "../visualization/gui_panel.h"
#include "../input/gui_playback.h"
#include "../output/gui_audio.h"
#include "../input/gui_capture.h" // Support hsdoah-rp2350 Error & stats
#include "version.h"
#include "../visualization/gui_custom_elements.h"
#include "../../common/buffer_manager.h"
#include <clay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

// Track if UI consumed the current frame's click (prevents click-through)
static bool s_ui_consumed_click = false;

// Simple in-place text editing state (settings panel)
static bool s_edit_output_base_name = false;
static bool s_edit_output_path = false; // 080226 - added to make editable
static double s_base_name_backspace_repeat_at = 0.0;
static double s_output_path_backspace_repeat_at = 0.0; // 080226 - added to make editable

// Audio label in-place editing state (settings panel)
static int s_edit_audio_label_idx = -1; // 0..3, or -1
static double s_audio_label_backspace_repeat_at = 0.0;
// Non-mono audio output tag editing state (4ch, 2ch12, 2ch34)
static int s_edit_audio_output_tag_idx = -1; // 0..2, or -1
static double s_audio_output_tag_backspace_repeat_at = 0.0;
// RF tag in-place editing state (settings panel)
static int s_edit_rf_tag_idx = -1; // 0..1, or -1
static double s_rf_tag_backspace_repeat_at = 0.0;


bool gui_ui_click_consumed(void) {
    return s_ui_consumed_click;
}

static inline void gui_ui_set_click_consumed(void) { // 130226 - added
    s_ui_consumed_click = true;
}
// Color conversions
static inline Clay_Color to_clay_color(Color c) {
    return (Clay_Color){ c.r, c.g, c.b, c.a };
}

// Format helpers - use separate buffers to avoid overwriting
static char temp_buf1[64];
static char temp_buf2[64];
static char temp_buf3[64];
static char temp_buf4[64];
static char temp_buf5[64];
static char temp_buf6[64];
static char temp_buf7[64];
static char temp_buf8[64];
static char device_dropdown_buf[64];
static char temp_title_buf[64];

// Per-channel stat buffers (separate for A and B to avoid overwrite)
static char stat_a_peak_pos[16];
static char stat_a_peak_neg[16];
static char stat_a_clip_pos[16];
static char stat_a_clip_neg[16];
static char stat_a_errors[16];
static char stat_b_peak_pos[16];
static char stat_b_peak_neg[16];
static char stat_b_clip_pos[16];
static char stat_b_clip_neg[16];
static char stat_b_errors[16];
static char stat_rec_raw[2][32];
static char stat_rec_flac[2][32];
static char stat_rec_ratio[2][24];
static char stat_rec_duration[2][24];

// Playback file display buffers
static char playback_file_a_display[64];
static char playback_file_b_display[64];
static char settings_output_path_display[256]; // 080226 - separate buffer for output path display

// Audio meter channel labels (static buffers)
static char audio_ch_label[4][8];

// Settings panel stable display buffers (avoid reuse of temp_buf* across layout)
static char settings_base_name_display[256];
static char settings_rf_bits_a_display[16];
static char settings_rf_bits_b_display[16];
static char settings_rf_tag_a_display[64];
static char settings_rf_tag_b_display[64];
static char settings_audio_tag_4ch_display[64];
static char settings_audio_tag_12_display[64];
static char settings_audio_tag_34_display[64];
static char settings_flac_level_display[64];
static char settings_flac_threads_display[64];
static char settings_resample_a_display[32];
static char settings_resample_b_display[32];
static char status_sample_rate_display[32];
static char status_samples_display[32];
static char status_frames_display[32];
static char status_missed_display[16];
static char status_errors_display[16];
static char status_rf_buf_display[16];
static char status_aud_buf_display[16];


static Clay_String make_string(const char *str) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(str), .chars = str };
}

static Color ui_disabled_color(Color c) {
    // Dim and slightly transparent.
    return (Color){ (unsigned char)(c.r * 0.55f), (unsigned char)(c.g * 0.55f), (unsigned char)(c.b * 0.55f), (unsigned char)(c.a * 0.80f) };
}

static const char *rf_bits_label(uint8_t bits) {
    switch (bits) {
        case 8: return "8";
        case 12: return "12";
        default: return "16";
    }
}

static void format_msps_label(char *dst, size_t dst_len, float khz) {
    if (!dst || dst_len == 0) return;
    double msps = (double)khz / 1000.0;
    // Trim trailing .0
    if (fabs(msps - (double)((int)msps)) < 1e-6) {
        snprintf(dst, dst_len, "%d MSPS", (int)msps);
    } else {
        snprintf(dst, dst_len, "%.1f MSPS", msps);
    }
}



static float cycle_resample_khz(float current_khz) {
    // User-facing presets: 5/10/14.3/17.9/20 MSPS, stored as kHz.
    static const float presets_khz[] = { 5000.0f, 10000.0f, 14300.0f, 17900.0f, 20000.0f };
    const int n = (int)(sizeof(presets_khz) / sizeof(presets_khz[0]));

    // Find nearest preset (within 1 kHz), otherwise start from first.
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (fabsf(current_khz - presets_khz[i]) < 1.0f) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return presets_khz[0];
    return presets_khz[(idx + 1) % n];
}

// Static storage for custom element data (persists during render)
static CustomLayoutElement s_osc_a_element;
static CustomLayoutElement s_osc_b_element;
static CustomLayoutElement s_vu_a_element;
static CustomLayoutElement s_vu_b_element;
static CustomLayoutElement s_settings_icon_element;

static int toolbar_title_font_size(void) {
    int width = GetScreenWidth();
    if (width <= 1280) return 18;
    if (width <= 1440) return 20;
    if (width <= 1680) return 22;
    return 24;
}

// Render settings panel (floating modal)
static void render_settings_panel(gui_app_t *app) {
    if (!app->settings_panel_open) return;

    // Backdrop
    CLAY(CLAY_ID("SettingsBackdrop"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP }
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 140}
    }) {}

    // Panel
    CLAY(CLAY_ID("SettingsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(.min = 760, .max = 1080), CLAY_SIZING_FIT(.min = 520, .max = 780) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 14, 14, 14, 14 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        // Header row
        CLAY(CLAY_ID("SettingsHeader"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Settings"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));

            CLAY(CLAY_ID("SettingsHeaderSpacer"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
            }) {}

            CLAY(CLAY_ID("SettingsCloseButton"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Auto naming (moved to top segment, above Output folder)
        CLAY_TEXT(CLAY_STRING("Auto naming:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        CLAY(CLAY_ID("AutoNameToggleRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
            CLAY(CLAY_ID("ToggleAutoNames"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.auto_names_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.auto_names_enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY_TEXT(CLAY_STRING("Generate filenames automatically"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

            // Add Time/Date toggle on the right side of the same row
            Color ts_bg = app->settings.auto_names_enabled ? (app->settings.append_timestamp_on_capture_start ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON) : ui_disabled_color(COLOR_BUTTON);
            Color ts_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
            CLAY(CLAY_ID("AppendTimestampToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(ts_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.append_timestamp_on_capture_start ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ts_fg) }));
            }
            CLAY_TEXT(CLAY_STRING("Add Time/Date"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(ts_fg) }));

            Color stop_drop_bg = app->settings.stop_on_dropout ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            Color stop_drop_fg = COLOR_TEXT;
            CLAY(CLAY_ID("StopOnDropoutToggle"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(stop_drop_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.stop_on_dropout ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(stop_drop_fg) }));
            }
            CLAY_TEXT(CLAY_STRING("Stop on Dropout"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(stop_drop_fg) }));
        }

        CLAY(CLAY_ID("BaseNameRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
            CLAY_TEXT(CLAY_STRING("Capture Name:"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

            Color base_box_bg = (Color){25,25,30,255};
            Color base_box_fg = COLOR_TEXT;
            if (!app->settings.auto_names_enabled) {
                base_box_bg = ui_disabled_color(base_box_bg);
                base_box_fg = ui_disabled_color(base_box_fg);
            }

            CLAY(CLAY_ID("OutputBaseNameField"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color(base_box_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                const char *base = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";
                // Visual caret indicator when editing.
                if (s_edit_output_base_name && app->settings.auto_names_enabled) {
                    snprintf(settings_base_name_display, sizeof(settings_base_name_display), "%s_", base);
                    CLAY_TEXT(make_string(settings_base_name_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(base_box_fg) }));
                } else {
                    CLAY_TEXT(make_string(base), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(base_box_fg) }));
                }
            }

            CLAY(CLAY_ID("OutputBaseNameHint"), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(CLAY_STRING("(click to edit)"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

// Output path display + choose button
CLAY(CLAY_ID("SettingsOutputPath"), {
    .layout = {
        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
        .layoutDirection = CLAY_TOP_TO_BOTTOM,
        .childGap = 6
    }
}) {
    CLAY_TEXT(CLAY_STRING("Output folder:"),
        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

    CLAY(CLAY_ID("OutputPathRow"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = 8
        }
    }) {
        // Editable path box (click to edit)
        CLAY(CLAY_ID("OutputPathBox"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 10, 10, 0, 0 }
            },
            .backgroundColor = to_clay_color((Color){25, 25, 30, 255}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            if (s_edit_output_path) {
                snprintf(settings_output_path_display, sizeof(settings_output_path_display), "%s_", app->settings.output_path);
                CLAY_TEXT(make_string(settings_output_path_display),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            } else {
                CLAY_TEXT(make_string(app->settings.output_path),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Choose output folder button (so the handler has a real element)
        CLAY(CLAY_ID("ChooseOutputFolderButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(CLAY_STRING("Choose..."),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }
    }
}


        // Scrollable settings body
        CLAY(CLAY_ID("SettingsScroll"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .childGap = 10
            },
            .clip = {
                .vertical = true,
                .horizontal = false,
                .childOffset = Clay_GetScrollOffset()
            }
        }) {
            // Two-column layout to reduce vertical overflow
            CLAY(CLAY_ID("SettingsColumns"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 18
                }
            }) {
                // Left column
                CLAY(CLAY_ID("SettingsColLeft"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 8
                    }
                }) {
                    // helper-like rows
                    CLAY_TEXT(CLAY_STRING("Capture:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    CLAY(CLAY_ID("ToggleRowCaptureA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleCaptureA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.capture_a ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.capture_a ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("RF A"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                        // RF bit depth selector (moved up into Capture segment)
                        CLAY(CLAY_ID("CaptureRowSpacerA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
                        snprintf(settings_rf_bits_a_display, sizeof(settings_rf_bits_a_display), "%s-bit", rf_bits_label(app->settings.rf_bits_a));
                        CLAY(CLAY_ID("RfBitsABox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_rf_bits_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        Color rf_tag_a_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color rf_tag_a_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("RfTagAField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(rf_tag_a_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *rf_tag_a = app->settings.rf_channel_tags[0][0] ? app->settings.rf_channel_tags[0] : "(tag)";
                            if (s_edit_rf_tag_idx == 0 && app->settings.auto_names_enabled) {
                                snprintf(settings_rf_tag_a_display, sizeof(settings_rf_tag_a_display), "%s_", rf_tag_a);
                                CLAY_TEXT(make_string(settings_rf_tag_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(rf_tag_a_fg) }));
                            } else {
                                CLAY_TEXT(make_string(rf_tag_a), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(rf_tag_a_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowCaptureB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleCaptureB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.capture_b ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.capture_b ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("RF B"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                        CLAY(CLAY_ID("CaptureRowSpacerB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } } }) { }
                        snprintf(settings_rf_bits_b_display, sizeof(settings_rf_bits_b_display), "%s-bit", rf_bits_label(app->settings.rf_bits_b));
                        CLAY(CLAY_ID("RfBitsBBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_rf_bits_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        Color rf_tag_b_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color rf_tag_b_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("RfTagBField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(rf_tag_b_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *rf_tag_b = app->settings.rf_channel_tags[1][0] ? app->settings.rf_channel_tags[1] : "(tag)";
                            if (s_edit_rf_tag_idx == 1 && app->settings.auto_names_enabled) {
                                snprintf(settings_rf_tag_b_display, sizeof(settings_rf_tag_b_display), "%s_", rf_tag_b);
                                CLAY_TEXT(make_string(settings_rf_tag_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(rf_tag_b_fg) }));
                            } else {
                                CLAY_TEXT(make_string(rf_tag_b), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(rf_tag_b_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowFlac"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleUseFlac"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.use_flac ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.use_flac ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("RF FLAC compression"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }

                    // FLAC verify toggle (moved directly under enable)
                    CLAY(CLAY_ID("ToggleRowFlacVerify"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleFlacVerify"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.flac_verification ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.flac_verification ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Verify FLAC output"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }

                    CLAY(CLAY_ID("ToggleRowOverwrite"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleOverwrite"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.overwrite_files ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.overwrite_files ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Overwrite output files"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                    // Compression section
                    CLAY_TEXT(CLAY_STRING("Compression (RF):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    // FLAC level stepper
                    CLAY(CLAY_ID("FlacLevelRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("FlacLevelMinus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        snprintf(settings_flac_level_display, sizeof(settings_flac_level_display), "FLAC level: %d", app->settings.flac_level);
                        CLAY(CLAY_ID("FlacLevelValue"), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(make_string(settings_flac_level_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        CLAY(CLAY_ID("FlacLevelPlus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                    }

                    // FLAC threads stepper (0=auto)
                    CLAY(CLAY_ID("FlacThreadsRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("FlacThreadsMinus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        snprintf(settings_flac_threads_display, sizeof(settings_flac_threads_display), "FLAC threads: %d", app->settings.flac_threads);
                        CLAY(CLAY_ID("FlacThreadsValue"), { .layout = { .sizing = { CLAY_SIZING_FIXED(170), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(make_string(settings_flac_threads_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                        CLAY(CLAY_ID("FlacThreadsPlus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) { CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) })); }
                    }

                    // Resample section
                    CLAY_TEXT(CLAY_STRING("Resample (RF):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    CLAY(CLAY_ID("ToggleRowResampleA"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleResampleA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_resample_a ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_resample_a ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Resample A"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                        // Rate selector (kHz stored; display MSPS)
                        format_msps_label(settings_resample_a_display, sizeof(settings_resample_a_display), app->settings.resample_rate_a);
                        Color rate_bg = app->settings.enable_resample_a ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON);
                        Color rate_fg = app->settings.enable_resample_a ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("ResampleRateABox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rate_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_resample_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rate_fg) }));
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowResampleB"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleResampleB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_resample_b ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_resample_b ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Resample B"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                        format_msps_label(settings_resample_b_display, sizeof(settings_resample_b_display), app->settings.resample_rate_b);
                        Color rate_bg = app->settings.enable_resample_b ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON);
                        Color rate_fg = app->settings.enable_resample_b ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("ResampleRateBBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rate_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(settings_resample_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rate_fg) }));
                        }
                    }

                }

                // Right column
                CLAY(CLAY_ID("SettingsColRight"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 8
                    }
                }) {
                    // Audio outputs
                    CLAY_TEXT(CLAY_STRING("Audio output (WAV):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    CLAY(CLAY_ID("ToggleRowAudio4ch"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleAudio4ch"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_4ch ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_audio_4ch ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Quad Ch1-4"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        Color audio_tag_4ch_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color audio_tag_4ch_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("AudioTag4chField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(audio_tag_4ch_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *tag4 = app->settings.audio_output_tags[0][0] ? app->settings.audio_output_tags[0] : "(tag)";
                            if (s_edit_audio_output_tag_idx == 0 && app->settings.auto_names_enabled) {
                                snprintf(settings_audio_tag_4ch_display, sizeof(settings_audio_tag_4ch_display), "%s_", tag4);
                                CLAY_TEXT(make_string(settings_audio_tag_4ch_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_4ch_fg) }));
                            } else {
                                CLAY_TEXT(make_string(tag4), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_4ch_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowAudio2ch12"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleAudio2ch12"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_2ch_12 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_audio_2ch_12 ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Stereo Ch1/Ch2"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        Color audio_tag_12_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color audio_tag_12_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("AudioTag2ch12Field"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(audio_tag_12_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *tag12 = app->settings.audio_output_tags[1][0] ? app->settings.audio_output_tags[1] : "(tag)";
                            if (s_edit_audio_output_tag_idx == 1 && app->settings.auto_names_enabled) {
                                snprintf(settings_audio_tag_12_display, sizeof(settings_audio_tag_12_display), "%s_", tag12);
                                CLAY_TEXT(make_string(settings_audio_tag_12_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_12_fg) }));
                            } else {
                                CLAY_TEXT(make_string(tag12), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_12_fg) }));
                            }
                        }
                    }

                    CLAY(CLAY_ID("ToggleRowAudio2ch34"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                        CLAY(CLAY_ID("ToggleAudio2ch34"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_2ch_34 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(app->settings.enable_audio_2ch_34 ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        CLAY_TEXT(CLAY_STRING("Stereo Ch3/Ch4"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                        Color audio_tag_34_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                        Color audio_tag_34_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                        CLAY(CLAY_ID("AudioTag2ch34Field"), { .layout = { .sizing = { CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(audio_tag_34_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            const char *tag34 = app->settings.audio_output_tags[2][0] ? app->settings.audio_output_tags[2] : "(tag)";
                            if (s_edit_audio_output_tag_idx == 2 && app->settings.auto_names_enabled) {
                                snprintf(settings_audio_tag_34_display, sizeof(settings_audio_tag_34_display), "%s_", tag34);
                                CLAY_TEXT(make_string(settings_audio_tag_34_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_34_fg) }));
                            } else {
                                CLAY_TEXT(make_string(tag34), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(audio_tag_34_fg) }));
                            }
                        }
                    }

                    // Audio 1ch (WAV) - mono CH1/CH2/CH3/CH4 list (do not alter)
                    CLAY_TEXT(CLAY_STRING("Audio 1ch (WAV):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    for (int i = 0; i < 4; i++) {
                        Clay_ElementId row_id = CLAY_IDI("ToggleRowAudio1ch", i);
                        Clay_ElementId toggle_id = CLAY_IDI("ToggleAudio1ch", i);
                        Clay_ElementId label_id = CLAY_IDI("Audio1chLabelField", i);

                        CLAY(row_id, { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                            CLAY(toggle_id, { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(app->settings.enable_audio_1ch[i] ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                CLAY_TEXT(app->settings.enable_audio_1ch[i] ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            }

                            if (i == 0) CLAY_TEXT(CLAY_STRING("CH1"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            else if (i == 1) CLAY_TEXT(CLAY_STRING("CH2"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            else if (i == 2) CLAY_TEXT(CLAY_STRING("CH3"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                            else CLAY_TEXT(CLAY_STRING("CH4"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));

                            // Per-channel audio tag (used in auto naming)
                            Color tag_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
                            Color tag_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                            CLAY(label_id, { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(tag_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                const char *tag = app->settings.audio_1ch_labels[i][0] ? app->settings.audio_1ch_labels[i] : "(tag)";
                                if (s_edit_audio_label_idx == i && app->settings.auto_names_enabled) {
                                    snprintf(temp_buf8, sizeof(temp_buf8), "%s_", tag);
                                    CLAY_TEXT(make_string(temp_buf8), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(tag_fg) }));
                                } else {
                                    CLAY_TEXT(make_string(tag), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(tag_fg) }));
                                }
                            }

                            // Filename preview intentionally hidden to keep settings rows compact.
                        }
                    }

                    // Playback files section
                    CLAY_TEXT(CLAY_STRING("Playback files (FLAC):"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

                    // Channel A playback file
                    CLAY(CLAY_ID("PlaybackFileARow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                        CLAY(CLAY_ID("PlaybackFileBrowseA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("Ch A..."), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        // Show filename or "(none)"
                        const char *file_a = app->settings.playback_file_a[0] ? app->settings.playback_file_a : "(none)";
                        // Truncate long paths for display
                        size_t len_a = strlen(file_a);
                        if (len_a > 30) {
                            snprintf(playback_file_a_display, sizeof(playback_file_a_display), "...%s", file_a + len_a - 27);
                        } else {
                            snprintf(playback_file_a_display, sizeof(playback_file_a_display), "%s", file_a);
                        }
                        CLAY(CLAY_ID("PlaybackFileAPath"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(playback_file_a_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(app->settings.playback_file_a[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                        }
                        CLAY(CLAY_ID("PlaybackFileClearA"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }

                    // Channel B playback file
                    CLAY(CLAY_ID("PlaybackFileBRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                        CLAY(CLAY_ID("PlaybackFileBrowseB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("Ch B..."), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                        const char *file_b = app->settings.playback_file_b[0] ? app->settings.playback_file_b : "(none)";
                        size_t len_b = strlen(file_b);
                        if (len_b > 30) {
                            snprintf(playback_file_b_display, sizeof(playback_file_b_display), "...%s", file_b + len_b - 27);
                        } else {
                            snprintf(playback_file_b_display, sizeof(playback_file_b_display), "%s", file_b);
                        }
                        CLAY(CLAY_ID("PlaybackFileBPath"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(make_string(playback_file_b_display), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(app->settings.playback_file_b[0] ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                        }
                        CLAY(CLAY_ID("PlaybackFileClearB"), { .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                }
            }
        }
    }
}

// Render the toolbar
static void render_toolbar(gui_app_t *app) {
    CLAY(CLAY_ID("Toolbar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(48) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .padding = { 8, 8, 8, 8 },
            .childGap = 12
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        // Title
        snprintf(temp_title_buf, sizeof(temp_title_buf), "MISRC Capture %s", MIRSC_TOOLS_VERSION);
        CLAY_TEXT(make_string(temp_title_buf),
            CLAY_TEXT_CONFIG({ .fontSize = toolbar_title_font_size(), .textColor = to_clay_color(COLOR_TEXT) }));

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer1"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_GROW(0) } }
        }) {}

        // Device label
        CLAY_TEXT(CLAY_STRING("Device:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        // Device dropdown button
        bool device_dropdown_open = gui_dropdown_is_open(DROPDOWN_DEVICE, 0);
        Color dropdown_color = device_dropdown_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("DeviceDropdown"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(250), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 10, 10, 0, 0 }
            },
            .backgroundColor = to_clay_color(dropdown_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            const char *device_name = app->device_count > 0 ?
                app->devices[app->selected_device].name : "No devices";
            snprintf(device_dropdown_buf, sizeof(device_dropdown_buf), "%s", device_name);
            CLAY_TEXT(make_string(device_dropdown_buf),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Connect/Disconnect button (next to device dropdown)
        Color connect_color = app->is_capturing ? COLOR_CLIP_RED : COLOR_SYNC_GREEN;
        CLAY(CLAY_ID("ConnectButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(connect_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->is_capturing ? CLAY_STRING("Disconnect") : CLAY_STRING("Connect"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = { 255, 255, 255, 255 } }));
        }
        // Capture mode toggle (MISRC default: swapped A/B; HSDAOH: normal A/B)
        Color mode_bg = app->settings.misrc_mode ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("CaptureModeToggle"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(125), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(mode_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->settings.misrc_mode ? CLAY_STRING("Mode: MISRC") : CLAY_STRING("Mode: HSDAOH"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer2"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

        // Audio playback monitoring toggle
        Color mon_bg = app->settings.audio_monitor_playback ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("AudioPlaybackToggle"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(32) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = to_clay_color(mon_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(CLAY_STRING("Audio Mon"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }
        
        // Audio channel select (CH1/2 vs CH3/4)
        Color ch_bg = app->settings.audio_monitor_ch34 ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("AudioChannelToggle"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(32) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = to_clay_color(ch_bg),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->settings.audio_monitor_ch34 ? CLAY_STRING("CH3/4") : CLAY_STRING("CH1/2"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // 4 channel horizontal audio meters (compact for toolbar)
        CLAY(CLAY_ID("AudioLevelBars"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(240), CLAY_SIZING_FIXED(32) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 4, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .padding = { 4, 4, 4, 4 } },
            .backgroundColor = to_clay_color((Color){25,25,30,255}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            // 4 horizontal meters in a row with labels
            for (int i = 0; i < 4; i++) {
                uint32_t p = atomic_load(&app->audio_peak[i]);
                float frac = (p > 0) ? (float)p / 8388607.0f : 0.0f;
                if (frac > 1.0f) frac = 1.0f;

                const int meter_w = 50;
                int fill_w = (int)(frac * (float)meter_w);
                if (fill_w < 0) fill_w = 0;
                if (fill_w > meter_w) fill_w = meter_w;

                // Color thresholds
                Color bar_col = (frac > 0.95f) ? COLOR_CLIP_RED : (frac > 0.75f) ? COLOR_METER_YELLOW : COLOR_SYNC_GREEN;

                // Column: channel label above meter
                CLAY(CLAY_IDI("AudioMeterCol", i), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(54), CLAY_SIZING_FIXED(24) }, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 1, .childAlignment = { .x = CLAY_ALIGN_X_CENTER } }
                }) {
                    // Channel label (CH1-CH4)
                    snprintf(audio_ch_label[i], sizeof(audio_ch_label[i]), "CH%d", i + 1);
                    CLAY(CLAY_IDI("AudioChLabel", i), { .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) } } }) {
                        CLAY_TEXT(make_string(audio_ch_label[i]),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    }

                    // Horizontal meter bar container
                    CLAY(CLAY_IDI("AudioMeter", i), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(meter_w), CLAY_SIZING_FIXED(8) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 0 },
                        .backgroundColor = to_clay_color((Color){40,40,48,255}),
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        // Fill bar (left side)
                        if (fill_w > 0) {
                            CLAY(CLAY_IDI("AudioMeterFill", i), {
                                .layout = { .sizing = { CLAY_SIZING_FIXED(fill_w), CLAY_SIZING_GROW(0) } },
                                .backgroundColor = to_clay_color(bar_col),
                                .cornerRadius = CLAY_CORNER_RADIUS(2)
                            }) { }
                        }
                    }
                }
            }
        }

        // Record button
        Color record_color = app->is_recording ? COLOR_CLIP_RED : COLOR_BUTTON;
        if (!app->is_capturing) record_color = (Color){ 50, 50, 55, 255 };
        CLAY(CLAY_ID("RecordButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(record_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            Color text_color = app->is_capturing ? COLOR_TEXT : COLOR_TEXT_DIM;
            CLAY_TEXT(app->is_recording ? CLAY_STRING("Stop Rec") : CLAY_STRING("Record"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(text_color) }));
        }


        // Settings button
        CLAY(CLAY_ID("SettingsButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(app->settings_panel_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            // Font-independent settings icon (rendered as a custom Clay element)
            CLAY(CLAY_ID("SettingsIcon"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18) } },
                .custom = { .customData = &s_settings_icon_element }
            }) {}
        }

    }
}

// Helper macro for stat row layout
#define STAT_ROW_LAYOUT { \
    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, \
    .layoutDirection = CLAY_LEFT_TO_RIGHT, \
    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, \
    .childGap = 4 \
}

// Fixed width for labels to ensure alignment
#define LABEL_WIDTH 50

// Render per-channel stats panel (trigger controls moved to waveform panel overlay)
static void render_channel_stats(gui_app_t *app, int channel) {
    // Get per-channel stats
    uint32_t clip_pos, clip_neg, errors;
    float peak_pos, peak_neg;
    char *buf_peak_pos, *buf_peak_neg, *buf_clip_pos, *buf_clip_neg, *buf_errors;
    char *buf_rec_raw, *buf_rec_flac, *buf_rec_ratio, *buf_rec_duration;
    Color channel_value_color = (channel == 0) ? COLOR_CHANNEL_A : COLOR_CHANNEL_B;

    if (channel == 0) {
        clip_pos = atomic_load(&app->clip_count_a_pos);
        clip_neg = atomic_load(&app->clip_count_a_neg);
        errors = atomic_load(&app->error_count_a);
        peak_pos = app->vu_a.peak_pos;
        peak_neg = app->vu_a.peak_neg;
        buf_peak_pos = stat_a_peak_pos;
        buf_peak_neg = stat_a_peak_neg;
        buf_clip_pos = stat_a_clip_pos;
        buf_clip_neg = stat_a_clip_neg;
        buf_errors = stat_a_errors;
        buf_rec_raw = stat_rec_raw[0];
        buf_rec_flac = stat_rec_flac[0];
        buf_rec_ratio = stat_rec_ratio[0];
        buf_rec_duration = stat_rec_duration[0];
    } else {
        clip_pos = atomic_load(&app->clip_count_b_pos);
        clip_neg = atomic_load(&app->clip_count_b_neg);
        errors = atomic_load(&app->error_count_b);
        peak_pos = app->vu_b.peak_pos;
        peak_neg = app->vu_b.peak_neg;
        buf_peak_pos = stat_b_peak_pos;
        buf_peak_neg = stat_b_peak_neg;
        buf_clip_pos = stat_b_clip_pos;
        buf_clip_neg = stat_b_clip_neg;
        buf_errors = stat_b_errors;
        buf_rec_raw = stat_rec_raw[1];
        buf_rec_flac = stat_rec_flac[1];
        buf_rec_ratio = stat_rec_ratio[1];
        buf_rec_duration = stat_rec_duration[1];
    }

    // Format stats (peak/clip/errors)
    snprintf(buf_peak_pos, 16, "+%.0f%%", peak_pos * 100.0f);
    snprintf(buf_peak_neg, 16, "-%.0f%%", peak_neg * 100.0f);
    snprintf(buf_clip_pos, 16, "+%u", clip_pos);
    snprintf(buf_clip_neg, 16, "-%u", clip_neg);
    snprintf(buf_errors, 16, "%u", errors);

    CLAY(CLAY_IDI("StatsPanel", channel), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(185), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 6, 6, 4, 4 },
            .childGap = 2
        },
        .backgroundColor = to_clay_color((Color){ 35, 35, 42, 255 })
    }) {
        // Channel label
        // CLAY_TEXT(channel == 0 ? CLAY_STRING("Channel A") : CLAY_STRING("Channel B"),
        //     CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS_LABEL, .textColor = to_clay_color(channel_color) }));

        // Samples row removed (shown in status bar)

        // Peak row (shows both + and -)
        CLAY(CLAY_IDI("StatPeak", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblPeak", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Peak:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(buf_peak_pos),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_pos > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT) }));
            CLAY_TEXT(make_string(buf_peak_neg),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_neg > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Clip row (shows both + and -)
        CLAY(CLAY_IDI("StatClip", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblClip", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Clip:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(buf_clip_pos),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_pos > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
            CLAY_TEXT(make_string(buf_clip_neg),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_neg > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Reset button row (kept separate so it scales better)
        CLAY(CLAY_IDI("ResetClipRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblClipReset", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING(""),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY(CLAY_IDI("ResetClipBtn", channel), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIXED(18) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(CLAY_STRING("RST"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Errors row
        CLAY(CLAY_IDI("StatErrors", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblErrors", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Errors:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
            CLAY_TEXT(make_string(buf_errors),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        {
            uint64_t raw_bytes = (channel == 0)
                                    ? atomic_load(&app->recording_raw_a)
                                    : atomic_load(&app->recording_raw_b);
            uint64_t comp_bytes = (channel == 0)
                                    ? atomic_load(&app->recording_compressed_a)
                                    : atomic_load(&app->recording_compressed_b);
            bool show_record_stats = app->is_capturing && (app->is_recording || raw_bytes > 0 || comp_bytes > 0 || app->last_recording_duration_s > 0.0);
            if (show_record_stats) {
                double raw_mb = (double)raw_bytes / (1024.0 * 1024.0);
                double shown_duration = app->is_recording ? (GetTime() - app->recording_start_time) : app->last_recording_duration_s;
                int d_hours = (int)(shown_duration / 3600.0);
                int d_mins = ((int)(shown_duration / 60.0)) % 60;
                int d_secs = ((int)(shown_duration)) % 60;

                snprintf(buf_rec_duration, 24, "Dur: %02d:%02d:%02d", d_hours, d_mins, d_secs);
                snprintf(buf_rec_raw, 32, "RAW: %.1f MB", raw_mb);
                if (comp_bytes > 0 || app->settings.use_flac) {
                    double comp_mb = (double)comp_bytes / (1024.0 * 1024.0);
                    double ratio = (comp_bytes > 0) ? ((double)raw_bytes / (double)comp_bytes) : 0.0;
                    snprintf(buf_rec_flac, 32, "FLAC: %.1f MB", comp_mb);
                    snprintf(buf_rec_ratio, 24, "Ratio: %.1fx", ratio);
                } else {
                    buf_rec_flac[0] = '\0';
                    buf_rec_ratio[0] = '\0';
                }

                CLAY(CLAY_IDI("RecDurationRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                    CLAY_TEXT(make_string(buf_rec_duration),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                }
                CLAY(CLAY_IDI("RecRawRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                    CLAY_TEXT(make_string(buf_rec_raw),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                }
                if (buf_rec_flac[0]) {
                    CLAY(CLAY_IDI("RecFlacRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                        CLAY_TEXT(make_string(buf_rec_flac),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                    }
                }
                if (buf_rec_ratio[0]) {
                    CLAY(CLAY_IDI("RecRatioRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                        CLAY_TEXT(make_string(buf_rec_ratio),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(channel_value_color) }));
                    }
                }
            }
        }

        // Separator line before panel configuration
        // Note: Trigger controls have moved to the waveform panel overlay (per-panel)
        CLAY(CLAY_IDI("StatSep", channel), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
            .backgroundColor = to_clay_color(COLOR_TEXT_DIM)
        }) {}

        // Get panel config for this channel
        bool panel_split = (channel == 0) ? app->panel_config_a.split : app->panel_config_b.split;
        int left_view = (channel == 0) ? app->panel_config_a.left_view : app->panel_config_b.left_view;
        int right_view = (channel == 0) ? app->panel_config_a.right_view : app->panel_config_b.right_view;

        // Layout row (Single/Split toggle)
        CLAY(CLAY_IDI("LayoutRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblLayout", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(CLAY_STRING("Layout:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            const char *layout_name = panel_split ? "Split" : "Single";
            bool layout_dropdown_open = gui_dropdown_is_open(DROPDOWN_LAYOUT, channel);
            CLAY(CLAY_IDI("LayoutBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIXED(18) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(layout_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(make_string(layout_name),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Layout dropdown options
        if (gui_dropdown_is_open(DROPDOWN_LAYOUT, channel)) {
            CLAY(CLAY_IDI("LayoutOpts", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                    .parentId = CLAY_IDI("LayoutBtn", channel).id,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                },
                .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                bool single_hover = Clay_PointerOver(CLAY_IDI("LayoutOptSingle", channel));
                Color single_color = gui_dropdown_option_color(!panel_split, single_hover);
                CLAY(CLAY_IDI("LayoutOptSingle", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(single_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Single"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                }

                bool split_hover = Clay_PointerOver(CLAY_IDI("LayoutOptSplit", channel));
                Color split_color = gui_dropdown_option_color(panel_split, split_hover);
                CLAY(CLAY_IDI("LayoutOptSplit", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(split_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Split"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }

        // Left view row (always shown)
        CLAY(CLAY_IDI("LeftViewRow", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY(CLAY_IDI("LblLeft", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                CLAY_TEXT(panel_split ? CLAY_STRING("Left:") : CLAY_STRING("View:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }

            const char *left_name = panel_view_type_name((panel_view_type_t)left_view);
            bool left_dropdown_open = gui_dropdown_is_open(DROPDOWN_LEFT_VIEW, channel);
            CLAY(CLAY_IDI("LeftViewBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIXED(18) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(left_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(make_string(left_name),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Left view dropdown options
        if (gui_dropdown_is_open(DROPDOWN_LEFT_VIEW, channel)) {
            CLAY(CLAY_IDI("LeftViewOpts", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                    .parentId = CLAY_IDI("LeftViewBtn", channel).id,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                },
                .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
                    if (!panel_view_type_available((panel_view_type_t)vt)) continue;
                    // Use channel * 10 + vt to create unique IDs per channel
                    bool opt_hover = Clay_PointerOver(CLAY_IDI("LeftViewOpt", channel * 10 + vt));
                    Color opt_color = gui_dropdown_option_color(left_view == vt, opt_hover);
                    CLAY(CLAY_IDI("LeftViewOpt", channel * 10 + vt), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = to_clay_color(opt_color)
                    }) {
                        CLAY_TEXT(make_string(panel_view_type_name((panel_view_type_t)vt)),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
            }
        }

        // Right view row (only shown when split)
        if (panel_split) {
            CLAY(CLAY_IDI("RightViewRow", channel), { .layout = STAT_ROW_LAYOUT }) {
                CLAY(CLAY_IDI("LblRight", channel), { .layout = { .sizing = { CLAY_SIZING_FIXED(LABEL_WIDTH), CLAY_SIZING_FIT(0) } } }) {
                    CLAY_TEXT(CLAY_STRING("Right:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                }

                const char *right_name = panel_view_type_name((panel_view_type_t)right_view);
                bool right_dropdown_open = gui_dropdown_is_open(DROPDOWN_RIGHT_VIEW, channel);
                CLAY(CLAY_IDI("RightViewBtn", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIXED(18) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(right_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    CLAY_TEXT(make_string(right_name),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }

            // Right view dropdown options
            if (gui_dropdown_is_open(DROPDOWN_RIGHT_VIEW, channel)) {
                CLAY(CLAY_IDI("RightViewOpts", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(65), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .floating = {
                        .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                        .parentId = CLAY_IDI("RightViewBtn", channel).id,
                        .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                    },
                    .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
                        if (!panel_view_type_available((panel_view_type_t)vt)) continue;
                        // Use channel * 10 + vt to create unique IDs per channel
                        bool opt_hover = Clay_PointerOver(CLAY_IDI("RightViewOpt", channel * 10 + vt));
                        Color opt_color = gui_dropdown_option_color(right_view == vt, opt_hover);
                        CLAY(CLAY_IDI("RightViewOpt", channel * 10 + vt), {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                            },
                            .backgroundColor = to_clay_color(opt_color)
                        }) {
                            CLAY_TEXT(make_string(panel_view_type_name((panel_view_type_t)vt)),
                                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN_OPT, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                }
            }
        }

    }
}

// Render the channels panel - each channel has VU meter + waveform + stats grouped together
static void render_channels_panel(gui_app_t *app) {
    // Setup custom element data for this frame
    s_vu_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_a_element.customData.vu_meter.meter = &app->vu_a;
    s_vu_a_element.customData.vu_meter.label = "CH A";
    s_vu_a_element.customData.vu_meter.is_clipping_pos = atomic_load(&app->clip_count_a_pos) > 0;
    s_vu_a_element.customData.vu_meter.is_clipping_neg = atomic_load(&app->clip_count_a_neg) > 0;
    s_vu_a_element.customData.vu_meter.channel_color = COLOR_CHANNEL_A;

    s_osc_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL;
    s_osc_a_element.customData.channel_panel.app = app;
    s_osc_a_element.customData.channel_panel.channel = 0;

    s_vu_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_b_element.customData.vu_meter.meter = &app->vu_b;
    s_vu_b_element.customData.vu_meter.label = "CH B";
    s_vu_b_element.customData.vu_meter.is_clipping_pos = atomic_load(&app->clip_count_b_pos) > 0;
    s_vu_b_element.customData.vu_meter.is_clipping_neg = atomic_load(&app->clip_count_b_neg) > 0;
    s_vu_b_element.customData.vu_meter.channel_color = COLOR_CHANNEL_B;

    s_osc_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_CHANNEL_PANEL;
    s_osc_b_element.customData.channel_panel.app = app;
    s_osc_b_element.customData.channel_panel.channel = 1;

    s_settings_icon_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_SETTINGS_ICON;

    CLAY(CLAY_ID("ChannelsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 4, 4, 4, 4 },
            .childGap = 8
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG)
    }) {
        // Channel A row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter A - custom element
            CLAY(CLAY_ID("VUMeterA"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_vu_a_element }
            }) {}

            // Oscilloscope canvas A - custom element
            CLAY(CLAY_ID("OscilloscopeCanvasA"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_osc_a_element }
            }) {}

            // Stats panel A
            render_channel_stats(app, 0);
        }

        // Channel B row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelBRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter B - custom element
            CLAY(CLAY_ID("VUMeterB"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_vu_b_element }
            }) {}

            // Oscilloscope canvas B - custom element
            CLAY(CLAY_ID("OscilloscopeCanvasB"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_osc_b_element }
            }) {}

            // Stats panel B
            render_channel_stats(app, 1);
        }
    }
}

// Render status bar
static void render_status_bar(gui_app_t *app) {
    CLAY(CLAY_ID("StatusBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .padding = { 12, 12, 0, 0 },
            .childGap = 20
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        // Left side: Recording indicators / Status message
        CLAY(CLAY_ID("StatusLeft"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 12
            }
        }) {
            if (app->is_recording) {
                CLAY(CLAY_ID("RecIndicator"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(12) } },
                    .backgroundColor = to_clay_color(COLOR_CLIP_RED),
                    .cornerRadius = CLAY_CORNER_RADIUS(6)
                }) {}

                double duration = GetTime() - app->recording_start_time;
                int hours = (int)(duration / 3600);
                int mins = ((int)(duration / 60)) % 60;
                int secs = ((int)duration) % 60;
                snprintf(temp_buf1, sizeof(temp_buf1), "%02d:%02d:%02d", hours, mins, secs);
                CLAY_TEXT(make_string(temp_buf1),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
            } else {
                snprintf(temp_buf1, sizeof(temp_buf1), "%s", app->status_message);
                CLAY_TEXT(make_string(temp_buf1),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
        }

        // Spacer to push right side to the right
        CLAY(CLAY_ID("StatusSpacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

        // Right side: Connection stats
        CLAY(CLAY_ID("StatusRight"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 16
            }
        }) {
            // Sync status indicator
            bool synced = atomic_load(&app->stream_synced);
            Color sync_color = synced ? COLOR_SYNC_GREEN : COLOR_SYNC_RED;
            CLAY(CLAY_ID("SyncStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("Sync:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY_TEXT(synced ? CLAY_STRING("OK") : CLAY_STRING("--"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(sync_color) }));
            }

            // Sample rate (placed next to Sync status)
            {
                uint32_t srate = atomic_load(&app->sample_rate);
                if (srate > 0) {
                    snprintf(status_sample_rate_display, sizeof(status_sample_rate_display), "%u MSPS", srate / 1000000);
                    CLAY(CLAY_ID("SampleRate"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_sample_rate_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    }
                }
            }

            

            // Unified Samples (same for both channels)
            {
                uint64_t samples_status = atomic_load(&app->samples_a);
                if (samples_status >= 1000000000ULL) {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%.2fG", (double)samples_status / 1000000000.0);
                } else if (samples_status >= 1000000ULL) {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%.2fM", (double)samples_status / 1000000.0);
                } else if (samples_status >= 1000ULL) {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%.1fK", (double)samples_status / 1000.0);
                } else {
                    snprintf(status_samples_display, sizeof(status_samples_display), "%llu", (unsigned long long)samples_status);
                }

                CLAY(CLAY_ID("SamplesStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Samples:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("SamplesValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_samples_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }

                // Frames count placed next to Samples
                uint32_t frames = atomic_load(&app->frame_count);
                snprintf(status_frames_display, sizeof(status_frames_display), "%u", frames);
                CLAY(CLAY_ID("FrameStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Frames:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("FrameValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(50), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_frames_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                    }
                }
            }

            // Missed frames count
            uint32_t missed = app->is_capturing ? atomic_load(&app->missed_frame_count) : 0;
            //if (missed > 0) {
                snprintf(status_missed_display, sizeof(status_missed_display), "%u", missed);
                CLAY(CLAY_ID("MissedStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Missed:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("MissedValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_missed_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_CLIP_RED) }));
                    }
                }
            //}

            // Total errors
            uint32_t errors = app->is_capturing ? atomic_load(&app->error_count) : 0;
            //if (errors > 0) {
                snprintf(status_errors_display, sizeof(status_errors_display), "%u", errors);
                CLAY(CLAY_ID("ErrorStatus"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 4
                    }
                }) {
                    CLAY_TEXT(CLAY_STRING("Errors:"),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                    CLAY(CLAY_ID("ErrorValue"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_FIT(0) } }
                    }) {
                        CLAY_TEXT(make_string(status_errors_display),
                            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(COLOR_CLIP_RED) }));
                    }
                }
            //}

            // RF Buffer usage
            size_t rf_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].head);
            size_t rf_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_RF].tail);
            size_t rf_size = app->buffers.buffers[BUF_CAPTURE_RF].buffer_size;
            size_t rf_used = rf_tail - rf_head;  // Simple subtraction - no wrap handling
            int rf_pct = rf_size > 0 ? (int)((rf_used * 100) / rf_size) : 0;
            snprintf(status_rf_buf_display, sizeof(status_rf_buf_display), "%d%%", rf_pct);
            CLAY(CLAY_ID("RFBufStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("RF Buffer:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY(CLAY_ID("RFBufValue"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(35), CLAY_SIZING_FIT(0) } }
                }) {
                    Color rf_color = (rf_pct > 90) ? COLOR_CLIP_RED : (rf_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                    CLAY_TEXT(make_string(status_rf_buf_display),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(rf_color) }));
                }
            }

            // Audio Buffer usage
            size_t aud_head = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].head);
            size_t aud_tail = atomic_load(&app->buffers.buffers[BUF_CAPTURE_AUDIO].tail);
            size_t aud_size = app->buffers.buffers[BUF_CAPTURE_AUDIO].buffer_size;
            size_t aud_used = aud_tail - aud_head;  // Simple subtraction - no wrap handling  
            int aud_pct = aud_size > 0 ? (int)((aud_used * 100) / aud_size) : 0;
            snprintf(status_aud_buf_display, sizeof(status_aud_buf_display), "%d%%", aud_pct);
            CLAY(CLAY_ID("AudBufStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("Audio Buffer:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY(CLAY_ID("AudBufValue"), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(35), CLAY_SIZING_FIT(0) } }
                }) {
                    Color aud_color = (aud_pct > 90) ? COLOR_CLIP_RED : (aud_pct > 75) ? COLOR_METER_YELLOW : COLOR_TEXT;
                    CLAY_TEXT(make_string(status_aud_buf_display),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .fontId = 1, .textColor = to_clay_color(aud_color) }));
                }
            }
        }
    }
}

// Main layout function
void gui_render_layout(gui_app_t *app) {
    // Root container
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        // Apply cached hsdaoh status/errors at a low rate (2s)
        gui_capture_poll_hsdaoh_status(app);
        
        // Toolbar
        render_toolbar(app);

        // Main content area - channels panel now includes per-channel stats with trigger controls
        render_channels_panel(app);

        // Status bar
        render_status_bar(app);
    }

    // Settings panel overlay (if open)
    render_settings_panel(app);

    // Device dropdown overlay (if open)
    if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0) && app->device_count > 0) {
        CLAY(CLAY_ID("DeviceDropdownOverlay"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(250), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .floating = {
                .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                .parentId = CLAY_ID("DeviceDropdown").id,
                .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
            },
            .backgroundColor = to_clay_color(COLOR_PANEL_BG),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            for (int i = 0; i < app->device_count; i++) {
                bool opt_hover = Clay_PointerOver(CLAY_IDI("DeviceOption", i));
                Color item_color = gui_dropdown_option_color(i == app->selected_device, opt_hover);

                CLAY(CLAY_IDI("DeviceOption", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { 10, 10, 0, 0 }
                    },
                    .backgroundColor = to_clay_color(item_color)
                }) {
                    // Use device name directly - it's already in persistent storage
                    CLAY_TEXT(make_string(app->devices[i].name),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }
    }

    // Popup overlay (renders on top of everything)
    gui_popup_render();
}

// Handle UI interactions
void gui_handle_interactions(gui_app_t *app) {
    // Reset click consumed flag at start of each frame
    s_ui_consumed_click = false;


    // Handle simple text input when editing capture name
    if (app->settings_panel_open && s_edit_output_base_name) {
        // If base name editing is active, don't allow audio label edit at same time.
        s_edit_output_path = false;   // cancel Output-path edit while editing base name
        s_edit_audio_label_idx = -1;
        s_edit_audio_output_tag_idx = -1;
        s_edit_rf_tag_idx = -1;

        // Append printable ASCII chars (avoid locale/undefined behavior with ctype on >255)
        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && ch < 127 && ch != '/' && ch != '\\' && ch != ':' && ch != '*' && ch != '?' && ch != '"' && ch != '<' && ch != '>' && ch != '|') {
                size_t len = strlen(app->settings.output_base_name);
                if (len + 1 < sizeof(app->settings.output_base_name)) {
                    app->settings.output_base_name[len] = (char)ch;
                    app->settings.output_base_name[len + 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
            ch = GetCharPressed();
        }

        // Backspace with repeat
        if (IsKeyPressed(KEY_BACKSPACE)) {
            s_base_name_backspace_repeat_at = GetTime() + 0.25; // initial delay
            size_t len = strlen(app->settings.output_base_name);
            if (len > 0) {
                app->settings.output_base_name[len - 1] = '\0';
                gui_settings_save(&app->settings);
            }
        } else if (IsKeyDown(KEY_BACKSPACE)) {
            double now = GetTime();
            if (now >= s_base_name_backspace_repeat_at) {
                s_base_name_backspace_repeat_at = now + 0.05;
                size_t len = strlen(app->settings.output_base_name);
                if (len > 0) {
                    app->settings.output_base_name[len - 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            s_edit_output_base_name = false;
            gui_settings_save(&app->settings);
        }
    }

        // Handle simple text input when editing output folder path -- MOVED
        if (app->settings_panel_open && s_edit_output_path) {
            // Cancel other edits while editing output path
            s_edit_output_base_name = false;
            s_edit_audio_label_idx = -1;
            s_edit_audio_output_tag_idx = -1;
            s_edit_rf_tag_idx = -1;

            int ch = GetCharPressed();
            while (ch > 0) {
        // Allow common path characters; block only the usual illegal ones.
        if (ch >= 32 && ch < 127 &&
            ch != '*' && ch != '?' && ch != '"' && ch != '<' && ch != '>' && ch != '|') {

            size_t len = strlen(app->settings.output_path);
            if (len + 1 < sizeof(app->settings.output_path)) {
                app->settings.output_path[len] = (char)ch;
                app->settings.output_path[len + 1] = '\0';
                gui_settings_save(&app->settings);
            }
        }
        ch = GetCharPressed();
    }

    if (IsKeyPressed(KEY_BACKSPACE)) {
        s_output_path_backspace_repeat_at = GetTime() + 0.25;
        size_t len = strlen(app->settings.output_path);
        if (len > 0) {
            app->settings.output_path[len - 1] = '\0';
            gui_settings_save(&app->settings);
        }
    } else if (IsKeyDown(KEY_BACKSPACE)) {
        double now = GetTime();
        if (now >= s_output_path_backspace_repeat_at) {
            s_output_path_backspace_repeat_at = now + 0.05;
            size_t len = strlen(app->settings.output_path);
            if (len > 0) {
                app->settings.output_path[len - 1] = '\0';
                gui_settings_save(&app->settings);
            }
        }
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
        s_edit_output_path = false;
        gui_settings_save(&app->settings);
    }
}
    // Handle text input when editing RF tag
    if (app->settings_panel_open && s_edit_rf_tag_idx >= 0 && s_edit_rf_tag_idx < 2 && app->settings.auto_names_enabled) {
        // Cancel other edits
        s_edit_output_base_name = false;
        s_edit_output_path = false;
        s_edit_audio_label_idx = -1;
        s_edit_audio_output_tag_idx = -1;

        int idx = s_edit_rf_tag_idx;
        char *dst = app->settings.rf_channel_tags[idx];
        size_t cap = sizeof(app->settings.rf_channel_tags[idx]);

        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && ch < 127 && ch != '/' && ch != '\\' && ch != ':' &&
                ch != '*' && ch != '?' && ch != 34 && ch != '<' && ch != '>' && ch != '|') {
                size_t len = strlen(dst);
                if (len + 1 < cap) {
                    dst[len] = (char)ch;
                    dst[len + 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
            ch = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            s_rf_tag_backspace_repeat_at = GetTime() + 0.25;
            size_t len = strlen(dst);
            if (len > 0) {
                dst[len - 1] = '\0';
                gui_settings_save(&app->settings);
            }
        } else if (IsKeyDown(KEY_BACKSPACE)) {
            double now = GetTime();
            if (now >= s_rf_tag_backspace_repeat_at) {
                s_rf_tag_backspace_repeat_at = now + 0.05;
                size_t len = strlen(dst);
                if (len > 0) {
                    dst[len - 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            s_edit_rf_tag_idx = -1;
            gui_settings_save(&app->settings);
        }
    }
    // Handle text input when editing non-mono audio output tags (4ch, 2ch12, 2ch34)
    if (app->settings_panel_open && s_edit_audio_output_tag_idx >= 0 && s_edit_audio_output_tag_idx < 3 && app->settings.auto_names_enabled) {
        // Cancel other edits
        s_edit_output_base_name = false;
        s_edit_output_path = false;
        s_edit_audio_label_idx = -1;
        s_edit_rf_tag_idx = -1;

        int idx = s_edit_audio_output_tag_idx;
        char *dst = app->settings.audio_output_tags[idx];
        size_t cap = sizeof(app->settings.audio_output_tags[idx]);

        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && ch < 127 && ch != '/' && ch != '\\' && ch != ':' &&
                ch != '*' && ch != '?' && ch != 34 && ch != '<' && ch != '>' && ch != '|') {
                size_t len = strlen(dst);
                if (len + 1 < cap) {
                    dst[len] = (char)ch;
                    dst[len + 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
            ch = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            s_audio_output_tag_backspace_repeat_at = GetTime() + 0.25;
            size_t len = strlen(dst);
            if (len > 0) {
                dst[len - 1] = '\0';
                gui_settings_save(&app->settings);
            }
        } else if (IsKeyDown(KEY_BACKSPACE)) {
            double now = GetTime();
            if (now >= s_audio_output_tag_backspace_repeat_at) {
                s_audio_output_tag_backspace_repeat_at = now + 0.05;
                size_t len = strlen(dst);
                if (len > 0) {
                    dst[len - 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            s_edit_audio_output_tag_idx = -1;
            gui_settings_save(&app->settings);
        }
    }
    // Handle text input when editing audio label
    if (app->settings_panel_open && s_edit_audio_label_idx >= 0 && s_edit_audio_label_idx < 4 && app->settings.auto_names_enabled) {
        // Cancel base name edit
        s_edit_output_base_name = false;
        s_edit_output_path = false;
        s_edit_audio_output_tag_idx = -1;
        s_edit_rf_tag_idx = -1;

        int idx = s_edit_audio_label_idx;
        char *dst = app->settings.audio_1ch_labels[idx];
        size_t cap = sizeof(app->settings.audio_1ch_labels[idx]);

        int ch = GetCharPressed();
        while (ch > 0) {
            if (ch >= 32 && ch < 127 && ch != '/' && ch != '\\' && ch != ':' && ch != '*' && ch != '?' && ch != '"' && ch != '<' && ch != '>' && ch != '|') {
                size_t len = strlen(dst);
                if (len + 1 < cap) {
                    dst[len] = (char)ch;
                    dst[len + 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
            ch = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE)) {
            s_audio_label_backspace_repeat_at = GetTime() + 0.25;
            size_t len = strlen(dst);
            if (len > 0) {
                dst[len - 1] = '\0';
                gui_settings_save(&app->settings);
            }
        } else if (IsKeyDown(KEY_BACKSPACE)) {
            double now = GetTime();
            if (now >= s_audio_label_backspace_repeat_at) {
                s_audio_label_backspace_repeat_at = now + 0.05;
                size_t len = strlen(dst);
                if (len > 0) {
                    dst[len - 1] = '\0';
                    gui_settings_save(&app->settings);
                }
            }
        }

        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
            s_edit_audio_label_idx = -1;
            gui_settings_save(&app->settings);
        }
    }

    // Handle popup interactions first (modal behavior)
    if (gui_popup_handle_interactions()) {
        s_ui_consumed_click = true;
        return;  // Popup consumed the interaction
    }

    // Handle clicks
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        // Check connect button
        if (Clay_PointerOver(CLAY_ID("ConnectButton"))) {
            if (app->is_capturing) {
                gui_app_stop_capture(app);
                app->reconnect_pending = false;
                app->reconnect_attempts = 0;
            } else {
                if (gui_app_start_capture(app) == 0) {
                    app->reconnect_pending = false;
                    app->reconnect_attempts = 0;
                }
            }
        }
        if (Clay_PointerOver(CLAY_ID("CaptureModeToggle"))) {
            app->settings.misrc_mode = !app->settings.misrc_mode;
            gui_settings_save(&app->settings);
            gui_app_set_status(app, app->settings.misrc_mode
                ? "Capture mode set to MISRC (A/B swapped)"
                : "Capture mode set to HSDAOH (A/B normal)");
        }
        if (Clay_PointerOver(CLAY_ID("StopOnDropoutToggle"))) {
            app->settings.stop_on_dropout = !app->settings.stop_on_dropout;
            gui_settings_save(&app->settings);
            gui_app_set_status(app, app->settings.stop_on_dropout
                ? "Stop on dropout enabled"
                : "Stop on dropout disabled");
        }


        // Audio playback monitoring toggle
        if (Clay_PointerOver(CLAY_ID("AudioPlaybackToggle"))) {
            gui_audio_set_playback_enabled(app, !app->settings.audio_monitor_playback);
        }
        
        // Audio channel select toggle (CH1/2 vs CH3/4)
        if (Clay_PointerOver(CLAY_ID("AudioChannelToggle"))) {
            app->settings.audio_monitor_ch34 = !app->settings.audio_monitor_ch34;
            gui_settings_save(&app->settings);
        }

        // Check record button
        if (Clay_PointerOver(CLAY_ID("RecordButton")) && app->is_capturing) {
            if (app->is_recording) {
                gui_app_stop_recording(app);
            } else {
                gui_app_start_recording(app);
            }
        }

        // Check settings button
        if (Clay_PointerOver(CLAY_ID("SettingsButton"))) {
            app->settings_panel_open = !app->settings_panel_open;
            s_edit_output_base_name = false;
            s_edit_output_path = false;
            s_edit_audio_label_idx = -1;
            s_edit_audio_output_tag_idx = -1;
            s_edit_rf_tag_idx = -1;
            gui_settings_save(&app->settings);
        }


        // Clip reset buttons (per-channel stats)
        if (Clay_PointerOver(CLAY_IDI("ResetClipBtn", 0))) {
            atomic_store(&app->clip_count_a_pos, 0);
            atomic_store(&app->clip_count_a_neg, 0);
        }
        if (Clay_PointerOver(CLAY_IDI("ResetClipBtn", 1))) {
            atomic_store(&app->clip_count_b_pos, 0);
            atomic_store(&app->clip_count_b_neg, 0);
        }

        // Settings panel interactions
        if (app->settings_panel_open) {
            if (Clay_PointerOver(CLAY_ID("SettingsBackdrop")) || Clay_PointerOver(CLAY_ID("SettingsCloseButton"))) {
                app->settings_panel_open = false;
                s_edit_output_base_name = false;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_audio_output_tag_idx = -1;
                s_edit_rf_tag_idx = -1;
                gui_settings_save(&app->settings);
                return;
            }

            if (Clay_PointerOver(CLAY_ID("ToggleCaptureA"))) {
                app->settings.capture_a = !app->settings.capture_a;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleCaptureB"))) {
                app->settings.capture_b = !app->settings.capture_b;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleUseFlac"))) {
                app->settings.use_flac = !app->settings.use_flac;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleOverwrite"))) {
                app->settings.overwrite_files = !app->settings.overwrite_files;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleFlacVerify"))) {
                app->settings.flac_verification = !app->settings.flac_verification;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacLevelMinus"))) {
                if (app->settings.flac_level > 0) app->settings.flac_level--;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacLevelPlus"))) {
                if (app->settings.flac_level < 8) app->settings.flac_level++;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacThreadsMinus"))) {
                if (app->settings.flac_threads > 0) app->settings.flac_threads--;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("FlacThreadsPlus"))) {
                if (app->settings.flac_threads < 64) app->settings.flac_threads++;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleResampleA"))) {
                app->settings.enable_resample_a = !app->settings.enable_resample_a;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ResampleRateABox"))) {
                app->settings.resample_rate_a = cycle_resample_khz(app->settings.resample_rate_a);
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleResampleB"))) {
                app->settings.enable_resample_b = !app->settings.enable_resample_b;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ResampleRateBBox"))) {
                app->settings.resample_rate_b = cycle_resample_khz(app->settings.resample_rate_b);
                gui_settings_save(&app->settings);
            }

            // Auto naming controls
            if (Clay_PointerOver(CLAY_ID("ToggleAutoNames"))) {
                app->settings.auto_names_enabled = !app->settings.auto_names_enabled;
                if (!app->settings.output_base_name[0]) {
                    snprintf(app->settings.output_base_name, sizeof(app->settings.output_base_name), "%s", "capture");
                }
                if (!app->settings.auto_names_enabled) {
                    s_edit_output_base_name = false;
                    s_edit_output_path = false;
                    s_edit_audio_label_idx = -1;
                    s_edit_audio_output_tag_idx = -1;
                    s_edit_rf_tag_idx = -1;
                }
                gui_settings_save(&app->settings);
            }
//            if (Clay_PointerOver(CLAY_ID("OutputBaseNameField"))) {
//                s_edit_output_base_name = true;
//                s_edit_audio_label_idx = -1;
//                s_base_name_backspace_repeat_at = 0.0;
//            }

            // Enter editing Capture/Base name only on click
            if (app->settings_panel_open &&
                Clay_PointerOver(CLAY_ID("OutputBaseNameField")) &&
                // IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && // 130226 - Changed
                !gui_ui_click_consumed())
            {
                s_edit_output_base_name = true;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_audio_output_tag_idx = -1;
                s_edit_rf_tag_idx = -1;

                gui_ui_set_click_consumed();
            }


//            if (Clay_PointerOver(CLAY_ID("OutputPathBox"))) { // 130226 - Enable OUTPUT folder edit
//            s_edit_output_path = true;
//            s_edit_output_base_name = false;
//            s_edit_audio_label_idx = -1;
//            s_output_path_backspace_repeat_at = 0.0;
///}

            if (Clay_PointerOver(CLAY_ID("AppendTimestampToggle")) && app->settings.auto_names_enabled) {
                app->settings.append_timestamp_on_capture_start = !app->settings.append_timestamp_on_capture_start;
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }


            // Enter editing Output folder only on click
            if (app->settings_panel_open &&
                Clay_PointerOver(CLAY_ID("OutputPathBox")) &&
                !gui_ui_click_consumed())
            {
                s_edit_output_path = true;
                s_edit_output_base_name = false;
                s_edit_audio_label_idx = -1;
                s_edit_audio_output_tag_idx = -1;
                s_edit_rf_tag_idx = -1;
                s_output_path_backspace_repeat_at = 0.0;

                gui_ui_set_click_consumed();
            }


             // RF bit depth selection (cycle)
            // If user switches to RAW and a channel was set to 12-bit, treat it as 16-bit.
            if (!app->settings.use_flac) {
                if (app->settings.rf_bits_a == 12) app->settings.rf_bits_a = 16;
                if (app->settings.rf_bits_b == 12) app->settings.rf_bits_b = 16;
            }

            if (Clay_PointerOver(CLAY_ID("RfBitsABox"))) {
                uint8_t b = app->settings.rf_bits_a;
                if (app->settings.use_flac) {
                    // 8 -> 12 -> 16 -> 8
                    b = (b == 8) ? 12 : (b == 12) ? 16 : 8;
                } else {
                    // RAW: 8 <-> 16
                    b = (b == 8) ? 16 : 8;
                }
                app->settings.rf_bits_a = b;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("RfBitsBBox"))) {
                uint8_t b = app->settings.rf_bits_b;
                if (app->settings.use_flac) {
                    b = (b == 8) ? 12 : (b == 12) ? 16 : 8;
                } else {
                    b = (b == 8) ? 16 : 8;
                }
                app->settings.rf_bits_b = b;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("RfTagAField")) && app->settings.auto_names_enabled) {
                s_edit_rf_tag_idx = 0;
                s_edit_output_base_name = false;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_audio_output_tag_idx = -1;
                s_rf_tag_backspace_repeat_at = 0.0;
                gui_ui_set_click_consumed();
            }

            if (Clay_PointerOver(CLAY_ID("AudioTag4chField")) && app->settings.auto_names_enabled) {
                s_edit_audio_output_tag_idx = 0;
                s_edit_output_base_name = false;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_rf_tag_idx = -1;
                s_audio_output_tag_backspace_repeat_at = 0.0;
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("AudioTag2ch12Field")) && app->settings.auto_names_enabled) {
                s_edit_audio_output_tag_idx = 1;
                s_edit_output_base_name = false;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_rf_tag_idx = -1;
                s_audio_output_tag_backspace_repeat_at = 0.0;
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("AudioTag2ch34Field")) && app->settings.auto_names_enabled) {
                s_edit_audio_output_tag_idx = 2;
                s_edit_output_base_name = false;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_rf_tag_idx = -1;
                s_audio_output_tag_backspace_repeat_at = 0.0;
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("RfTagBField")) && app->settings.auto_names_enabled) {
                s_edit_rf_tag_idx = 1;
                s_edit_output_base_name = false;
                s_edit_output_path = false;
                s_edit_audio_label_idx = -1;
                s_edit_audio_output_tag_idx = -1;
                s_rf_tag_backspace_repeat_at = 0.0;
                gui_ui_set_click_consumed();
            }

            if (Clay_PointerOver(CLAY_ID("ToggleAudio2ch12"))) {
                app->settings.enable_audio_2ch_12 = !app->settings.enable_audio_2ch_12;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleAudio4ch"))) {
                app->settings.enable_audio_4ch = !app->settings.enable_audio_4ch;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("ToggleAudio2ch34"))) {
                app->settings.enable_audio_2ch_34 = !app->settings.enable_audio_2ch_34;
                gui_settings_save(&app->settings);
            }

            for (int i = 0; i < 4; i++) {
                if (Clay_PointerOver(CLAY_IDI("ToggleAudio1ch", i))) {
                    app->settings.enable_audio_1ch[i] = !app->settings.enable_audio_1ch[i];
                    gui_settings_save(&app->settings);
                }
                if (Clay_PointerOver(CLAY_IDI("Audio1chLabelField", i)) && app->settings.auto_names_enabled) {
                    s_edit_audio_label_idx = i;
                    s_edit_output_base_name = false;
                    s_edit_output_path = false; //130226 - Added
                    s_edit_audio_output_tag_idx = -1;
                    s_edit_rf_tag_idx = -1;
                    s_audio_label_backspace_repeat_at = 0.0;
                }
            }

            if (Clay_PointerOver(CLAY_ID("ChooseOutputFolderButton"))) {
                // Best-effort folder picker (platform-specific).
                if (gui_settings_choose_output_folder(&app->settings)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No folder selected (or folder picker unavailable)");
                }
            }

        // Playback file selection buttons
            if (Clay_PointerOver(CLAY_ID("PlaybackFileBrowseA"))) {
                if (gui_settings_choose_playback_file(&app->settings, 0)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No file selected (or file picker unavailable)");
                }
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileBrowseB"))) {
                if (gui_settings_choose_playback_file(&app->settings, 1)) {
                    gui_settings_save(&app->settings);
                } else {
                    gui_app_set_status(app, "No file selected (or file picker unavailable)");
                }
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileClearA"))) {
                app->settings.playback_file_a[0] = '\0';
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }
            if (Clay_PointerOver(CLAY_ID("PlaybackFileClearB"))) {
                app->settings.playback_file_b[0] = '\0';
                gui_settings_save(&app->settings);
                gui_ui_set_click_consumed();
            }
        }

        // Note: CVBS enable/disable is now handled automatically when selecting
        // CVBS view via ensure_cvbs_enabled_for_channel() in gui_dropdown.c

        // Handle all dropdown interactions via centralized handler
        if (!s_ui_consumed_click && gui_dropdown_handle_click(app)) {
            s_ui_consumed_click = true;
        }
    }
}
