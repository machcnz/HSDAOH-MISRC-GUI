/*
 * MISRC GUI - Graphical Capture Interface
 *
 * Real-time waveform display and capture control using raylib + Clay UI
 *
 * Copyright (C) 2024-2025 vrunk11, stefan_o
 * Licensed under GNU GPL v3 or later
 */

// Clay UI library (header-only, implementation here)
#define CLAY_IMPLEMENTATION
#include <clay.h>

#include "raylib.h"
#include "../assets/inter_font_data.h"
#include "../assets/space_mono_font_data.h"

#include "gui_app.h"
#include "../ui/gui_ui.h"
#include "../visualization/gui_text.h"
#include "../input/gui_capture.h"
#include "../processing/gui_extract.h"
#include "../visualization/gui_panel.h"
#include "../ui/gui_dropdown.h"
#include "../ui/gui_popup.h"
#include "../output/gui_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global exit flag (shared with capture thread)
volatile atomic_int do_exit = 0;

// Font array for Clay
// Index 0: Inter (general UI), Index 1: Space Mono (monospace sections)
#define FONT_COUNT 2
static Font fonts[FONT_COUNT];

// Clay error handler
void clay_error_handler(Clay_ErrorData error) {
    fprintf(stderr, "Clay Error: %s\n", error.errorText.chars);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Initialize application state
    gui_app_t app = {0};
    app.fonts = fonts;

    // Initialize sample rate early (before any capture/rendering can occur)
    atomic_store(&app.sample_rate, DEFAULT_SAMPLE_RATE);

    // Load persistent settings (includes desktop path defaults)
    gui_settings_load(&app.settings);

    // Initialize raylib window
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "MISRC Capture");
    SetTraceLogLevel(LOG_INFO);  // Enable debug logging (phosphor perf traces)
    SetTargetFPS(60);
    SetExitKey(0);  // Disable escape key auto-close

    // Load embedded Inter font directly from memory (Apache 2.0 licensed)
    // Font data is ~342KB and embedded as a C array for complete portability
    fonts[0] = LoadFontFromMemory(".ttf", inter_font_data, inter_font_data_size, 32, NULL, 256);
    if (fonts[0].texture.id == 0) {
        fprintf(stderr, "Error: Failed to load embedded Inter font data\n");
        CloseWindow();
        return 1;
    }
    SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);

    // Load embedded Space Mono font directly from memory (SIL Open Font License)
    // Font data is embedded as a C array for complete portability
    fonts[1] = LoadFontFromMemory(".ttf", space_mono_font_data, space_mono_font_data_size, 32, NULL, 256);
    if (fonts[1].texture.id == 0) {
        fprintf(stderr, "Error: Failed to load embedded Space Mono font data\n");
        CloseWindow();
        return 1;
    }
    SetTextureFilter(fonts[1].texture, TEXTURE_FILTER_BILINEAR);

    // Initialize Clay
    uint64_t clay_memory_size = Clay_MinMemorySize();
    void *clay_memory = malloc(clay_memory_size);
    if (!clay_memory) {
        fprintf(stderr, "Failed to allocate Clay memory\n");
        CloseWindow();
        return 1;
    }

    Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_memory_size, clay_memory);
    Clay_Initialize(clay_arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_error_handler });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);

    // Initialize application
    gui_app_init(&app);

    // Set app for text rendering font access
    gui_text_set_app(&app);

    // Enumerate available devices
    gui_app_enumerate_devices(&app);

    // Enable auto-reconnect by default
    app.auto_reconnect_enabled = true;

    // Autoconnect if a device is found
    if (app.device_count > 0) {
        gui_app_set_status(&app, "Connecting...");
        if (gui_app_start_capture(&app) == 0) {
            gui_app_set_status(&app, "Connected");
        } else {
            gui_app_set_status(&app, "Failed to connect. Click Connect to retry.");
            app.reconnect_pending = true;
            app.reconnect_attempt_time = GetTime();
        }
    } else {
        gui_app_set_status(&app, "No devices found. Connect a device and restart.");
    }

    // Main loop
    while (!WindowShouldClose() && !atomic_load(&do_exit)) {
        float dt = GetFrameTime();

        // Handle window resize
        if (IsWindowResized()) {
            Clay_SetLayoutDimensions((Clay_Dimensions){
                (float)GetScreenWidth(), (float)GetScreenHeight()
            });
        }

        // Check for pending popup result (for async confirmations like file overwrite)
        gui_record_check_popup(&app);

        // Handle keyboard shortcuts
        // Popup gets priority for keyboard input
        if (gui_popup_is_open()) {
            if (IsKeyPressed(KEY_ESCAPE)) {
                gui_popup_dismiss();
            } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                // Enter confirms (equivalent to clicking Yes/OK)
                // This is handled by simulating a click - we'll let the popup handle it
                // For now, ESC dismisses and the popup buttons handle confirmation
            }
        } else {
            if (IsKeyPressed(KEY_ESCAPE)) {
                if (app.settings_panel_open) {
                    app.settings_panel_open = false;
                } else {
                    gui_dropdown_close_all();
                }
            }

            if (IsKeyPressed(KEY_SPACE) && !app.settings_panel_open) {
                if (app.is_capturing) {
                    gui_app_stop_capture(&app);
                } else {
                    gui_app_start_capture(&app);
                }
            }

            if (IsKeyPressed(KEY_R) && app.is_capturing && !app.settings_panel_open) {
                if (app.is_recording) {
                    gui_app_stop_recording(&app);
                } else {
                    gui_app_start_recording(&app);
                }
            }
        }

        // Update Clay mouse state
        Vector2 mouse_pos = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mouse_pos.x, mouse_pos.y },
                             IsMouseButtonDown(MOUSE_LEFT_BUTTON));
        Clay_UpdateScrollContainers(true, (Clay_Vector2){
            GetMouseWheelMoveV().x * 20.0f,
            GetMouseWheelMoveV().y * 20.0f
        }, dt);

        // Auto-reconnect logic
        if (app.auto_reconnect_enabled) {
            double now = GetTime();

            // Detect connection loss via callback timeout (no data for 2+ seconds)
            if (app.is_capturing && gui_capture_device_timeout(&app, 2000)) {
                // Device was disconnected unexpectedly - clean up properly
                fprintf(stderr, "[GUI] Device timeout detected, disconnecting...\n");
                gui_app_stop_capture(&app);
                gui_app_clear_display(&app);
                app.reconnect_pending = true;
                app.reconnect_attempt_time = now;
                app.reconnect_attempts = 0;
                gui_app_set_status(&app, "Connection lost. Reconnecting...");
            }

            // Attempt reconnection if pending
            if (app.reconnect_pending && !app.is_capturing) {
                double retry_delay = (app.reconnect_attempts < 3) ? 1.0 : 3.0;  // 1s for first 3, then 3s
                if (now - app.reconnect_attempt_time >= retry_delay) {
                    app.reconnect_attempt_time = now;
                    app.reconnect_attempts++;

                    // Re-enumerate devices in case device was reconnected
                    gui_app_enumerate_devices(&app);

                    if (app.device_count > 0) {
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "Reconnecting (attempt %d)...", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf);

                        if (gui_app_start_capture(&app) == 0) {
                            app.reconnect_pending = false;
                            app.reconnect_attempts = 0;
                            gui_app_set_status(&app, "Reconnected");
                        }
                    } else {
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "No device found (attempt %d)", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf);
                    }
                }
            }
        }

        // Update VU meters
        gui_app_update_vu_meters(&app, dt);

        // Note: Display processing now handled by display thread via panel_process_all()
        // Each panel type (waveform, histogram, FFT) receives raw samples via vtable->process()

        // Build UI layout
        Clay_BeginLayout();
        gui_render_layout(&app);
        Clay_RenderCommandArray render_commands = Clay_EndLayout();

        // Handle Clay interactions
        gui_handle_interactions(&app);

        // Handle panel scroll events (e.g., waveform/FFT zoom)
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            panel_handle_all_scrolls(&app, wheel);
        }

        // Render
        BeginDrawing();

        // Reset cursor to default at start of frame
        // (Panels will set crosshair cursor when hovering/dragging via their render functions)
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
        ClearBackground(COLOR_BG);

        // Render Clay UI (custom elements are handled via CLAY_RENDER_COMMAND_TYPE_CUSTOM)
        Clay_Raylib_Render(render_commands, fonts);

        // Draw FPS in debug mode
        #ifdef DEBUG
        DrawFPS(10, 10);
        #endif

        EndDrawing();
    }

    // Cleanup
    if (app.is_recording) {
        gui_app_stop_recording(&app);
    }
    if (app.is_capturing) {
        gui_app_stop_capture(&app);
    }

    // Save settings before cleanup
    gui_settings_save(&app.settings);
    
    gui_app_cleanup(&app);
    free(clay_memory);

    // Unload fonts if we loaded TTFs (not the default font)
    if (fonts[0].texture.id != 0 && fonts[0].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[0]);
    }
    if (fonts[1].texture.id != 0 && fonts[1].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[1]);
    }

    CloseWindow();

    return 0;
}
