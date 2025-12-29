/*
 * MISRC GUI - Capture Integration
 *
 * Uses the same ringbuffer and extraction pattern as misrc_capture.c
 */

#include "gui_capture.h"
#include "../core/gui_app.h"
#include "../visualization/gui_text.h"
#include "../output/gui_record.h"
#include "../processing/gui_extract.h"
#include "../visualization/gui_oscilloscope.h"
#include "../visualization/gui_phosphor_rt.h"
#include "../visualization/gui_fft.h"
#include "gui_simulated.h"
#include "gui_playback.h"
#include "../visualization/gui_panel.h"
#include "../visualization/panel_interface.h"
#include "../visualization/gui_histogram_panel.h"
#include "../signal/gui_cvbs.h"
#include "../processing/gui_display_thread.h"

#include <hsdaoh.h>
#include <hsdaoh_raw.h>
#include "../../common/extract.h"
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/threading.h"
#include "../../common/frame_parser.h"
#include "../../common/device_enum.h"
#include "../../common/buffer_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Buffer sizes - match reference implementation
#define BUFFER_READ_SIZE 65536
#define BUFFER_TOTAL_SIZE (65536 * 1024)  // Same as reference: 64MB
#define BUFFER_AUDIO_TOTAL_SIZE (65536 * 256)

// Note: All capture buffers now managed by buffer manager (BUF_CAPTURE_RF, BUF_CAPTURE_AUDIO)

// Capture handler context (includes frame parser state)
static capture_handler_ctx_t s_capture_handler;

// Message callback for hsdaoh
static void gui_message_callback(void *ctx, enum hsdaoh_msg_level level, const char *format, ...) {
    gui_app_t *app = (gui_app_t *)ctx;

    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Update status message
    if (level == HSDAOH_ERROR || level == HSDAOH_CRITICAL) {
        gui_app_set_status(app, buffer);
        atomic_fetch_add(&app->error_count, 1);
    }

    // Print to console for debugging
    const char *level_str = "INFO";
    if (level == HSDAOH_WARNING) level_str = "WARN";
    else if (level == HSDAOH_ERROR) level_str = "ERROR";
    else if (level == HSDAOH_CRITICAL) level_str = "CRITICAL";

    fprintf(stderr, "[%s] %s", level_str, buffer);
}

// Debug counter
static int s_callback_count = 0;

/*-----------------------------------------------------------------------------
 * GUI-Specific Capture Handler Callbacks
 *-----------------------------------------------------------------------------*/

static void gui_sync_event_cb(void *user_ctx, frame_sync_result_t result,
                               const metadata_t *meta, bool was_synced)
{
    (void)meta;
    gui_app_t *app = (gui_app_t *)user_ctx;

    switch (result) {
        case FRAME_SYNC_LOST:
            if (was_synced) {
                fprintf(stderr, "[CB] Lost sync to HDMI input stream\n");
            }
            atomic_store(&app->stream_synced, false);
            break;
        case FRAME_SYNC_MISSED:
            fprintf(stderr, "[CB] Missed frame(s)\n");
            atomic_fetch_add(&app->missed_frame_count, 1);
            break;
        case FRAME_SYNC_ACQUIRED:
            fprintf(stderr, "[CB] Synchronized to HDMI input stream\n");
            atomic_store(&app->stream_synced, true);
            break;
        case FRAME_SYNC_DUPLICATE:
        case FRAME_SYNC_OK:
            break;
    }
}

/*-----------------------------------------------------------------------------
 * Main Capture Callback
 *-----------------------------------------------------------------------------*/

// Main capture callback - writes raw data to ringbuffer (like reference implementation)
void gui_capture_callback(void *data_info_ptr) {
    hsdaoh_data_info_t *data_info = (hsdaoh_data_info_t *)data_info_ptr;
    gui_app_t *app = (gui_app_t *)data_info->ctx;

    s_callback_count++;

    if (atomic_load(&do_exit)) return;
    if (!app) return;
    if (!data_info->buf) return;
    if (data_info->width == 0 || data_info->height == 0) return;

    if (data_info->device_error) {
        atomic_store(&app->stream_synced, false);
        s_capture_handler.frame_state.sync.stream_synced = false;
        return;
    }

    // Extract metadata from frame
    metadata_t meta;
    hsdaoh_extract_metadata(data_info->buf, &meta, data_info->width);

    bool was_synced = s_capture_handler.frame_state.sync.stream_synced;

    // Process frame using shared parser
    frame_process_result_t result = frame_process(&s_capture_handler.frame_state,
                                                   data_info->buf,
                                                   data_info->width,
                                                   data_info->height,
                                                   &meta, 4);

    // Handle sync state changes using shared handler
    if (!capture_handler_process_sync_event(&s_capture_handler, result.sync_result,
                                             &meta, was_synced)) {
        return;  // LOST or DUPLICATE - stop processing
    }

    // Don't process until synced
    if (!s_capture_handler.frame_state.sync.stream_synced) {
        return;
    }

    // Update last callback time for disconnect detection
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    atomic_fetch_add(&app->frame_count, 1);

    // Update sample rate from metadata
    if (meta.stream_info[0].srate > 0) {
        atomic_store(&app->sample_rate, meta.stream_info[0].srate);
    }

    // Handle errors
    if (result.error_count > 0) {
        if (result.report_errors) {
            fprintf(stderr, "[CB] %d frame errors\n", result.error_count);
            atomic_fetch_add(&app->error_count, result.error_count);
        }
        return;  // Discard frame with errors
    }

    // Don't process if no payload
    if (!result.valid || result.stream0_bytes == 0) {
        return;
    }

    uint8_t *buf_out = NULL;
    uint8_t *buf_out_audio = NULL;

    // Write to capture ringbuffer via buffer manager
    // Buffer manager handles backpressure according to default policy for BUF_CAPTURE_RF
    buf_out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, result.stream0_bytes, NULL);
    if (!buf_out) {
        // Buffer full after waiting - drop frame (policy allows this)
        atomic_fetch_add(&app->rb_drop_count, 1);
        if (atomic_load(&app->rb_drop_count) <= 5) {
            fprintf(stderr, "[CB] Dropped frame due to ringbuffer backpressure\n");
        }
        return;
    }

    // If audio capture enabled, reserve space in audio buffer via buffer manager
    if (s_capture_handler.capture_audio && result.stream1_bytes > 0) {
        buf_out_audio = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, result.stream1_bytes, NULL);
        // buf_out_audio may be NULL if buffer full - best-effort audio
    }

    // Copy payloads (RF + optional audio) with shared audio sync filtering
    frame_copy_payloads_cb(data_info->buf, data_info->width, data_info->height,
                           &meta, buf_out, buf_out_audio,
                           capture_handler_audio_filter, &s_capture_handler);

    bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, result.stream0_bytes);
    if (buf_out_audio) {
        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, result.stream1_bytes);
    }

    // Signal that new data is available (data events are managed by buffer manager)
    bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

    if (s_callback_count <= 3) {
        fprintf(stderr, "[CB] Wrote %zu bytes to ringbuffer\n", result.stream0_bytes);
    }
}

// Initialize application
void gui_app_init(gui_app_t *app) {
    // Initialize panel registry and register all panel types
    // This must happen before any panel state is created
    panel_registry_init();
    gui_waveform_line_panel_register();
    gui_waveform_phosphor_panel_register();
    gui_fft_panel_register();
    gui_cvbs_panel_register();
    gui_histogram_panel_register();

    // Initialize per-channel display buffers
    memset(app->display_samples_a, 0, sizeof(app->display_samples_a));
    memset(app->display_samples_b, 0, sizeof(app->display_samples_b));
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize simulated device state
    app->sim_thread = NULL;
    atomic_store(&app->sim_running, false);

    // Initialize playback device state
    atomic_store(&app->playback_running, false);

    app->vu_a.level_pos = 0;
    app->vu_a.level_neg = 0;
    app->vu_a.peak_pos = 0;
    app->vu_a.peak_neg = 0;
    app->vu_a.peak_hold_time_pos = 0;
    app->vu_a.peak_hold_time_neg = 0;

    app->vu_b.level_pos = 0;
    app->vu_b.level_neg = 0;
    app->vu_b.peak_pos = 0;
    app->vu_b.peak_neg = 0;
    app->vu_b.peak_hold_time_pos = 0;
    app->vu_b.peak_hold_time_neg = 0;

    strcpy(app->status_message, "Initializing...");

    for (int i = 0; i < 4; i++) {
        atomic_store(&app->audio_peak[i], 0);
    }

    // Initialize sample rate to default (will be updated when device connects)
    atomic_store(&app->sample_rate, DEFAULT_SAMPLE_RATE);
    TraceLog(LOG_INFO, "APP INIT: sample_rate set to %u", DEFAULT_SAMPLE_RATE);

    // Initialize trigger state for channel A
    app->trigger_a.enabled = false;
    app->trigger_a.level = 0;
    app->trigger_a.zoom_scale = ZOOM_SCALE_DEFAULT;
    app->trigger_a.trigger_display_pos = -1;
    atomic_store(&app->trigger_a.display_width, DISPLAY_BUFFER_SIZE);  // Will be updated by renderer
    app->trigger_a.scope_mode = SCOPE_MODE_PHOSPHOR;  // Phosphor mode by default
    app->trigger_a.trigger_mode = TRIGGER_MODE_RISING;  // Rising edge by default
    app->trigger_a.phosphor_color = PHOSPHOR_COLOR_HEATMAP;  // Opacity mode by default

    // Initialize trigger state for channel B
    app->trigger_b.enabled = false;
    app->trigger_b.level = 0;
    app->trigger_b.zoom_scale = ZOOM_SCALE_DEFAULT;
    app->trigger_b.trigger_display_pos = -1;
    atomic_store(&app->trigger_b.display_width, DISPLAY_BUFFER_SIZE);  // Will be updated by renderer
    app->trigger_b.scope_mode = SCOPE_MODE_PHOSPHOR;  // Phosphor mode by default
    app->trigger_b.trigger_mode = TRIGGER_MODE_RISING;  // Rising edge by default
    app->trigger_b.phosphor_color = PHOSPHOR_COLOR_HEATMAP;  // Opacity mode by default

    // Initialize phosphor display state
    app->phosphor_a = (phosphor_rt_t *)calloc(1, sizeof(phosphor_rt_t));
    app->phosphor_b = (phosphor_rt_t *)calloc(1, sizeof(phosphor_rt_t));

    // Set phosphor config (render textures are created lazily on first use)
    if (app->phosphor_a) {
        app->phosphor_a->config.decay_rate = SCOPE_DECAY_RATE;
        app->phosphor_a->config.hit_increment = SCOPE_HIT_INCREMENT;
        app->phosphor_a->config.bloom_intensity = SCOPE_BLOOM;
        memcpy(app->phosphor_a->config.channel_color, PHOSPHOR_CHANNEL_COLOR_A, sizeof(float) * 3);
    }
    if (app->phosphor_b) {
        app->phosphor_b->config.decay_rate = SCOPE_DECAY_RATE;
        app->phosphor_b->config.hit_increment = SCOPE_HIT_INCREMENT;
        app->phosphor_b->config.bloom_intensity = SCOPE_BLOOM;
        memcpy(app->phosphor_b->config.channel_color, PHOSPHOR_CHANNEL_COLOR_B, sizeof(float) * 3);
    }

    // Initialize panel configuration (new panel abstraction system)
    app->panel_config_a.split = true;
    app->panel_config_a.left_view = PANEL_VIEW_WAVEFORM_PHOSPHOR;
    app->panel_config_a.right_view = PANEL_VIEW_FFT;
    app->panel_config_a.left_state = NULL;
    app->panel_config_a.right_state = panel_create_view_state(PANEL_VIEW_FFT);

    app->panel_config_b.split = true;
    app->panel_config_b.left_view = PANEL_VIEW_WAVEFORM_PHOSPHOR;
    app->panel_config_b.right_view = PANEL_VIEW_FFT;
    app->panel_config_b.left_state = NULL;
    app->panel_config_b.right_state = panel_create_view_state(PANEL_VIEW_FFT);

    // Note: All buffers (BUF_CAPTURE_RF, BUF_CAPTURE_AUDIO, etc.) are initialized
    // by buffer manager automatically on first use

    // Initialize capture handler (includes frame parser state)
    capture_handler_init(&s_capture_handler);
    s_capture_handler.rb_rf = NULL;    // RF uses buffer manager
    s_capture_handler.rb_audio = NULL; // Audio uses buffer manager
    s_capture_handler.capture_rf = true;

    // Enable audio capture if any audio outputs are enabled (mirrors CLI audio options)
    bool want_audio = app->settings.enable_audio_4ch || app->settings.enable_audio_2ch_12 || app->settings.enable_audio_2ch_34;
    for (int i = 0; i < 4; i++) {
        if (app->settings.enable_audio_1ch[i]) want_audio = true;
    }
    s_capture_handler.capture_audio = want_audio;
    s_capture_handler.sync_event_cb = gui_sync_event_cb;
    s_capture_handler.user_ctx = app;

    // Initialize centralized buffer manager
    if (bufmgr_init(&app->buffers) != 0) {
        fprintf(stderr, "Failed to initialize buffer manager\n");
    }

    // Initialize display thread (allocated, started on capture start)
    app->display_thread = (display_thread_t *)calloc(1, sizeof(display_thread_t));
    if (app->display_thread) {
        if (gui_display_thread_init(app->display_thread) != 0) {
            fprintf(stderr, "Failed to initialize display thread state\n");
            free(app->display_thread);
            app->display_thread = NULL;
        }
    }

    // Set app for text rendering
    gui_text_set_app(app);
}

// Cleanup application
void gui_app_cleanup(gui_app_t *app) {
    if (app->is_capturing) {
        gui_app_stop_capture(app);
    }

    // Note: All buffers now managed by buffer manager (cleanup via bufmgr_cleanup)

    // Cleanup extraction subsystem
    gui_extract_cleanup();

    // Cleanup phosphor buffers and textures
    if (app->phosphor_a) {
        phosphor_rt_cleanup(app->phosphor_a);
        free(app->phosphor_a);
        app->phosphor_a = NULL;
    }
    if (app->phosphor_b) {
        phosphor_rt_cleanup(app->phosphor_b);
        free(app->phosphor_b);
        app->phosphor_b = NULL;
    }

    // Note: CVBS decoders are now owned by panel state (left_state/right_state)
    // and cleaned up via panel_config_cleanup() below

    // Cleanup panel configurations (includes FFT, CVBS, histogram state)
    panel_config_cleanup(&app->panel_config_a);
    panel_config_cleanup(&app->panel_config_b);

    // Cleanup oscilloscope resources (static state and resamplers)
    gui_oscilloscope_cleanup_resamplers(app);
    gui_oscilloscope_cleanup();

    // Cleanup display thread
    if (app->display_thread) {
        gui_display_thread_cleanup(app->display_thread);
        free(app->display_thread);
        app->display_thread = NULL;
    }

    // Cleanup buffer manager
    bufmgr_cleanup(&app->buffers);
}

// Enumerate available capture devices
void gui_app_enumerate_devices(gui_app_t *app) {
    app->device_count = 0;

    // Use shared device enumeration (hsdaoh + simple_capture)
    misrc_device_list_t devices;
    misrc_device_list_init(&devices);
    int count = misrc_device_enumerate(&devices, true, true);

    if (count < 0) {
        gui_app_set_status(app, "Device enumeration failed");
        misrc_device_list_free(&devices);
        return;
    }

    // Copy devices to GUI format
    for (size_t i = 0; i < devices.count && app->device_count < MAX_DEVICES; i++) {
        misrc_device_info_t *src = &devices.devices[i];
        device_info_t *dst = &app->devices[app->device_count];

        // Format name with type prefix for simple_capture devices
        if (src->type == MISRC_DEVICE_TYPE_SIMPLE_CAPTURE) {
            snprintf(dst->name, sizeof(dst->name), "[%s] %s",
                     device_get_simple_capture_short_name(), src->name);
            dst->type = DEVICE_TYPE_SIMPLE_CAPTURE;
            dst->index = -1;
            // Store device_id in serial field for simple_capture
            snprintf(dst->serial, sizeof(dst->serial), "%s", src->device_id);
        } else {
            snprintf(dst->name, sizeof(dst->name), "%s", src->name);
            dst->type = DEVICE_TYPE_HSDAOH;
            dst->index = src->index;
            dst->serial[0] = '\0';
        }

        app->device_count++;
    }

    misrc_device_list_free(&devices);

    // Always add simulated device at the end
    if (app->device_count < MAX_DEVICES) {
        device_info_t *dst = &app->devices[app->device_count];
        snprintf(dst->name, sizeof(dst->name), "[Simulated] Test Signal");
        snprintf(dst->serial, sizeof(dst->serial), "SIM001");
        dst->type = DEVICE_TYPE_SIMULATED;
        dst->index = -1;
        app->device_count++;
    }

    // Add playback device option
    if (app->device_count < MAX_DEVICES) {
        device_info_t *dst = &app->devices[app->device_count];
        snprintf(dst->name, sizeof(dst->name), "[Playback] FLAC Files");
        snprintf(dst->serial, sizeof(dst->serial), "PLAYBACK");
        dst->type = DEVICE_TYPE_PLAYBACK;
        dst->index = -1;
        app->device_count++;
    }

    if (app->device_count == 0) {
        gui_app_set_status(app, "No capture devices found");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Found %d device(s)", app->device_count);
        gui_app_set_status(app, msg);
    }
}

// Start capture
int gui_app_start_capture(gui_app_t *app) {
    fprintf(stderr, "[GUI] gui_app_start_capture called\n");

    if (app->is_capturing) {
        fprintf(stderr, "[GUI] Already capturing\n");
        return 0;
    }

    if (app->device_count == 0) {
        fprintf(stderr, "[GUI] No devices available\n");
        gui_app_set_status(app, "No devices available");
        return -1;
    }

    device_info_t *dev = &app->devices[app->selected_device];
    fprintf(stderr, "[GUI] Selected device: %s (type %d, index %d)\n", dev->name, dev->type, dev->index);

    // Handle simulated device separately
    if (dev->type == DEVICE_TYPE_SIMULATED) {
        return gui_simulated_start(app);
    }

    // Handle playback device
    if (dev->type == DEVICE_TYPE_PLAYBACK) {
        const char *file_a = app->settings.playback_file_a[0] ? app->settings.playback_file_a : NULL;
        const char *file_b = app->settings.playback_file_b[0] ? app->settings.playback_file_b : NULL;
        if (!file_a && !file_b) {
            gui_app_set_status(app, "No playback files selected");
            return -1;
        }
        return gui_playback_start(app, file_a, file_b);
    }

    // Ensure capture buffer is initialized via buffer manager
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[GUI] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, DEFAULT_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers (per-channel)
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset callback counter and capture handler state
    s_callback_count = 0;
    capture_handler_init(&s_capture_handler);
    // Note: All buffers now use buffer manager directly
    s_capture_handler.rb_rf = NULL;
    s_capture_handler.rb_audio = NULL;
    s_capture_handler.capture_rf = true;

    bool want_audio = app->settings.enable_audio_4ch || app->settings.enable_audio_2ch_12 || app->settings.enable_audio_2ch_34;
    for (int i = 0; i < 4; i++) {
        if (app->settings.enable_audio_1ch[i]) want_audio = true;
    }
    s_capture_handler.capture_audio = want_audio;

    s_capture_handler.sync_event_cb = gui_sync_event_cb;
    s_capture_handler.user_ctx = app;

    // Open device
    fprintf(stderr, "[GUI] Allocating device...\n");
    int r = hsdaoh_alloc(&app->hs_dev);
    if (r < 0) {
        fprintf(stderr, "[GUI] hsdaoh_alloc failed: %d\n", r);
        gui_app_set_status(app, "Failed to allocate device");
        return -1;
    }

    hsdaoh_set_msg_callback(app->hs_dev, gui_message_callback, app);
    hsdaoh_raw_callback(app->hs_dev, true);

    fprintf(stderr, "[GUI] Opening device index %d...\n", dev->index);
    r = hsdaoh_open2(app->hs_dev, dev->index);
    if (r < 0) {
        fprintf(stderr, "[GUI] hsdaoh_open2 failed: %d\n", r);
        gui_app_set_status(app, "Failed to open device");
        // Note: hsdaoh_open2 frees dev on failure, so DON'T call hsdaoh_close
        app->hs_dev = NULL;
        return -1;
    }

    fprintf(stderr, "[GUI] Starting stream...\n");
    r = hsdaoh_start_stream(app->hs_dev, (hsdaoh_read_cb_t)gui_capture_callback, app);
    if (r < 0) {
        fprintf(stderr, "[GUI] hsdaoh_start_stream failed: %d\n", r);
        gui_app_set_status(app, "Failed to start stream");
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
        return -1;
    }

    app->is_capturing = true;

    // Start the extraction thread - runs continuously from capture start
    r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[GUI] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        hsdaoh_stop_stream(app->hs_dev);
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
        app->is_capturing = false;
        return -1;
    }

    // Start the display thread - processes BUF_DISPLAY for oscilloscope/CVBS
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[GUI] Failed to start display thread (non-fatal)\n");
            // Non-fatal - display will use legacy path
        }
    }

    gui_app_set_status(app, "Capturing...");

    return 0;
}

// Stop capture
void gui_app_stop_capture(gui_app_t *app) {
    if (!app->is_capturing) {
        return;
    }

    if (app->is_recording) {
        gui_app_stop_recording(app);
    }

    // Check if this is a simulated or playback capture
    device_info_t *dev = &app->devices[app->selected_device];
    if (dev->type == DEVICE_TYPE_SIMULATED) {
        gui_simulated_stop(app);
        gui_app_clear_display(app);
        return;
    }
    if (dev->type == DEVICE_TYPE_PLAYBACK) {
        gui_playback_stop(app);
        gui_app_clear_display(app);
        return;
    }

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;

    // Stop display thread first (it reads from BUF_DISPLAY written by extraction)
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    // Stop extraction thread before closing device
    gui_extract_stop();

    if (app->hs_dev) {
        hsdaoh_stop_stream(app->hs_dev);
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
    }

    atomic_store(&app->stream_synced, false);

    // Print capture summary with backpressure stats
    uint32_t frames = atomic_load(&app->frame_count);
    uint32_t missed = atomic_load(&app->missed_frame_count);
    uint32_t errors = atomic_load(&app->error_count);
    uint32_t waits = atomic_load(&app->rb_wait_count);
    uint32_t drops = atomic_load(&app->rb_drop_count);
    fprintf(stderr, "[GUI] Capture stopped: %u frames, %u missed, %u errors, %u waits, %u drops\n",
            frames, missed, errors, waits, drops);

    // Clear display to show "No Signal"
    gui_app_clear_display(app);

    gui_app_set_status(app, "Capture stopped");
}

// Note: Audio buffer now accessed via app->buffers (buffer_manager)
// Use BUF_CAPTURE_AUDIO with bufmgr_read_begin/bufmgr_read_end

// Recording wrappers - delegate to gui_record module
int gui_app_start_recording(gui_app_t *app) {
    return gui_record_start(app);
}

void gui_app_stop_recording(gui_app_t *app) {
    gui_record_stop(app);
}

// Helper to update one direction of VU meter (pos or neg)
static void update_vu_direction(float *level, float *peak, float *peak_hold_time,
                                 float target, float dt) {
    // Fast attack, slow release
    if (target > *level) {
        *level = target;
    } else {
        *level += (target - *level) * dt / VU_RELEASE_TIME;
    }

    // Peak hold
    if (target > *peak) {
        *peak = target;
        *peak_hold_time = 0;
    } else {
        *peak_hold_time += dt;
        if (*peak_hold_time > PEAK_HOLD_DURATION) {
            *peak -= dt * PEAK_DECAY_RATE;
            if (*peak < 0) *peak = 0;
        }
    }
}

// Update VU meters - tracks positive and negative separately for AC signals
void gui_app_update_vu_meters(gui_app_t *app, float dt) {
    // Get current peak values (separate pos/neg)
    uint16_t peak_a_pos = atomic_load(&app->peak_a_pos);
    uint16_t peak_a_neg = atomic_load(&app->peak_a_neg);
    uint16_t peak_b_pos = atomic_load(&app->peak_b_pos);
    uint16_t peak_b_neg = atomic_load(&app->peak_b_neg);

    // Convert to normalized levels (0-1), using 2048 as full scale
    float level_a_pos = (float)peak_a_pos / 2048.0f;
    float level_a_neg = (float)peak_a_neg / 2048.0f;
    float level_b_pos = (float)peak_b_pos / 2048.0f;
    float level_b_neg = (float)peak_b_neg / 2048.0f;

    // Clamp to 1.0
    if (level_a_pos > 1.0f) level_a_pos = 1.0f;
    if (level_a_neg > 1.0f) level_a_neg = 1.0f;
    if (level_b_pos > 1.0f) level_b_pos = 1.0f;
    if (level_b_neg > 1.0f) level_b_neg = 1.0f;

    // Update channel A (positive and negative separately)
    update_vu_direction(&app->vu_a.level_pos, &app->vu_a.peak_pos,
                        &app->vu_a.peak_hold_time_pos, level_a_pos, dt);
    update_vu_direction(&app->vu_a.level_neg, &app->vu_a.peak_neg,
                        &app->vu_a.peak_hold_time_neg, level_a_neg, dt);

    // Update channel B (positive and negative separately)
    update_vu_direction(&app->vu_b.level_pos, &app->vu_b.peak_pos,
                        &app->vu_b.peak_hold_time_pos, level_b_pos, dt);
    update_vu_direction(&app->vu_b.level_neg, &app->vu_b.peak_neg,
                        &app->vu_b.peak_hold_time_neg, level_b_neg, dt);
}

// Update display buffer (called from main thread)
// Note: Display is now updated by the continuous extraction thread
void gui_app_update_display_buffer(gui_app_t *app) {
    (void)app;
    // No-op: extraction thread handles display updates continuously
}

// Clear display buffer and reset VU meters (called when device disconnects)
void gui_app_clear_display(gui_app_t *app) {
    // Clear display samples (per-channel)
    memset(app->display_samples_a, 0, sizeof(app->display_samples_a));
    memset(app->display_samples_b, 0, sizeof(app->display_samples_b));
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset VU meters
    app->vu_a.level_pos = 0;
    app->vu_a.level_neg = 0;
    app->vu_a.peak_pos = 0;
    app->vu_a.peak_neg = 0;
    app->vu_a.peak_hold_time_pos = 0;
    app->vu_a.peak_hold_time_neg = 0;

    app->vu_b.level_pos = 0;
    app->vu_b.level_neg = 0;
    app->vu_b.peak_pos = 0;
    app->vu_b.peak_neg = 0;
    app->vu_b.peak_hold_time_pos = 0;
    app->vu_b.peak_hold_time_neg = 0;

    // Reset peak values
    atomic_store(&app->peak_a_pos, 0);
    atomic_store(&app->peak_a_neg, 0);
    atomic_store(&app->peak_b_pos, 0);
    atomic_store(&app->peak_b_neg, 0);

    // Reset stream sync status
    atomic_store(&app->stream_synced, false);
}

// Set status message
void gui_app_set_status(gui_app_t *app, const char *message) {
    strncpy(app->status_message, message, sizeof(app->status_message) - 1);
    app->status_message[sizeof(app->status_message) - 1] = '\0';
    app->status_message_time = GetTime();
}

// Check if device has timed out (no callbacks for too long)
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms) {
    if (!app->is_capturing) return false;

    uint64_t last_cb = atomic_load(&app->last_callback_time_ms);
    uint64_t now = get_time_ms();

    // Handle wrap-around (GetTickCount wraps every ~49 days)
    uint64_t elapsed = now - last_cb;
    if (now < last_cb) {
        // Wrap-around occurred
        elapsed = (UINT64_MAX - last_cb) + now + 1;
    }

    return elapsed > timeout_ms;
}
