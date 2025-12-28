/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#include "gui_record.h"
#include "gui_app.h"
#include "gui_extract.h"
#include "gui_popup.h"
#include "gui_audio.h"
#include "gui_capture.h"

#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/rb_event.h"
#include "../misrc_common/flac_writer.h"
#include "../misrc_common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#define access _access
#define F_OK 0
#endif

#include <sys/types.h>
#include <sys/stat.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

// Buffer sizes
#define BUFFER_READ_SIZE 65536

// Format file size into human-readable string
static void format_file_size(off_t size, char *buf, size_t buf_size) {
    if (size >= 1073741824) {  // >= 1 GB
        snprintf(buf, buf_size, "%.2f GB", (double)size / 1073741824.0);
    } else if (size >= 1048576) {  // >= 1 MB
        snprintf(buf, buf_size, "%.2f MB", (double)size / 1048576.0);
    } else if (size >= 1024) {  // >= 1 KB
        snprintf(buf, buf_size, "%.2f KB", (double)size / 1024.0);
    } else {
        snprintf(buf, buf_size, "%lld bytes", (long long)size);
    }
}

// do_exit is declared in gui_capture.h (defined in misrc_gui.c)

// Writer threads
static thrd_t s_writer_thread_a;
static thrd_t s_writer_thread_b;
static bool s_writer_threads_running = false;
static FILE *s_file_a = NULL;
static FILE *s_file_b = NULL;

// Global app pointer for threads
static gui_app_t *s_recording_app = NULL;

// Overwrite confirmation pending state
static bool s_overwrite_pending = false;
static gui_app_t *s_pending_app = NULL;

// Backpressure stats at recording start (to compute delta)
static uint32_t s_start_wait_count = 0;
static uint32_t s_start_drop_count = 0;

// File writer context
typedef struct {
    ringbuffer_t *rb;
    FILE *file;
    int channel;  // 0 = A, 1 = B

    // For RAW writer: bytes per sample (1=8-bit, 2=16-bit)
    size_t raw_bytes_per_sample;

#if LIBFLAC_ENABLED == 1
    flac_writer_t *writer;
    atomic_uint_fast64_t *compressed_bytes;
    uint8_t flac_bits_per_sample;
#endif
    gui_app_t *app;  // For error reporting
} writer_ctx_t;

static writer_ctx_t s_ctx_a;
static writer_ctx_t s_ctx_b;

#if LIBFLAC_ENABLED == 1
// FLAC writers (managed by shared library)
static flac_writer_t *s_flac_writer_a = NULL;
static flac_writer_t *s_flac_writer_b = NULL;

// Error callback for GUI FLAC writer
static void gui_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
    (void)error;
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->app) {
        gui_app_set_status(wctx->app, message);
    }
    fprintf(stderr, "FLAC ERROR: %s\n", message);
}

// Bytes written callback for compression ratio tracking
static void gui_flac_bytes_callback(void *user_data, size_t bytes_written) {
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->compressed_bytes) {
        atomic_fetch_add(wctx->compressed_bytes, bytes_written);
    }
}

// FLAC file writer thread
static int flac_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = BUFFER_READ_SIZE * sizeof(int32_t);
    size_t raw_bytes_per_block = BUFFER_READ_SIZE * sizeof(int16_t);
    void *buf;

    // Get record space event for signaling
    rb_event_t *record_space_event = gui_extract_get_record_space_event();

    fprintf(stderr, "[FLAC] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (1) {
        buf = rb_read_ptr(wctx->rb, len);
        if (!buf) {
            // No data available - check if we should exit
            if (atomic_load(&do_exit) || !s_recording_app || !s_recording_app->is_recording) {
                // Drain any remaining partial data before exiting
                size_t remaining = wctx->rb->tail - wctx->rb->head;
                if (remaining > 0 && remaining < len) {
                    size_t remaining_samples = remaining / sizeof(int32_t);
                    buf = rb_read_ptr(wctx->rb, remaining);
                    if (buf && remaining_samples > 0) {
                        flac_writer_process(wctx->writer, (const int32_t *)buf, remaining_samples);
                        rb_read_finished(wctx->rb, remaining);
                    }
                }
                break;
            }
            thrd_sleep_ms(1);
            continue;
        }

        int result = flac_writer_process(wctx->writer, (const int32_t *)buf, BUFFER_READ_SIZE);
        if (result < 0) {
            fprintf(stderr, "FLAC encoder error on channel %c\n", wctx->channel == 0 ? 'A' : 'B');
        }

        rb_read_finished(wctx->rb, len);

        // Signal that space is now available (for extraction thread waiting on full buffer)
        if (record_space_event) {
            rb_event_signal(record_space_event);
        }

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, raw_bytes_per_block);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, raw_bytes_per_block);
            }
        }
    }

    fprintf(stderr, "[FLAC] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}
#endif

// RAW file writer thread
static int raw_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t bps = (wctx->raw_bytes_per_sample == 1) ? 1 : 2;
    size_t len = BUFFER_READ_SIZE * bps;

    // Get record space event for signaling
    rb_event_t *record_space_event = gui_extract_get_record_space_event();

    fprintf(stderr, "[RAW] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (1) {
        void *buf = rb_read_ptr(wctx->rb, len);
        if (!buf) {
            // No data available - check if we should exit
            if (atomic_load(&do_exit) || !s_recording_app || !s_recording_app->is_recording) {
                // Drain any remaining partial data before exiting
                size_t remaining = wctx->rb->tail - wctx->rb->head;
                if (remaining > 0 && remaining < len) {
                    buf = rb_read_ptr(wctx->rb, remaining);
                    if (buf) {
                        fwrite(buf, 1, remaining, wctx->file);
                        rb_read_finished(wctx->rb, remaining);
                    }
                }
                break;
            }
            thrd_sleep_ms(1);
            continue;
        }

        size_t written = fwrite(buf, 1, len, wctx->file);
        rb_read_finished(wctx->rb, len);

        // Signal that space is now available (for extraction thread waiting on full buffer)
        if (record_space_event) {
            rb_event_signal(record_space_event);
        }

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, written);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, written);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, written);
            }
        }
    }

    fprintf(stderr, "[RAW] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}

// Initialize recording subsystem
void gui_record_init(void) {
    // Nothing to initialize here anymore - ringbuffers are in gui_extract
}

// Cleanup recording subsystem
void gui_record_cleanup(void) {
    // Nothing to cleanup here anymore - ringbuffers are in gui_extract
}

// Check if recording is active
bool gui_record_is_active(void) {
    return s_recording_app != NULL && s_recording_app->is_recording;
}

// Check if waiting for popup confirmation
bool gui_record_is_pending(void) {
    return s_overwrite_pending;
}

// Forward declaration of actual recording start (after confirmation)
static int gui_record_start_confirmed(gui_app_t *app);

// Start recording - checks for file existence first
int gui_record_start(gui_app_t *app) {

    if (!app->is_capturing) {
        gui_app_set_status(app, "Start capture first");
        return RECORD_ERROR;
    }

    if (app->is_recording) {
        return RECORD_OK;
    }

    // If already pending confirmation, don't show another popup
    if (s_overwrite_pending) {
        return RECORD_PENDING;
    }

    // Build full output paths (output_path + filenames)
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/%s", app->settings.output_path, app->settings.output_filename_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", app->settings.output_path, app->settings.output_filename_b);

    // Check if output files already exist
    struct stat stat_a, stat_b;
    bool file_a_exists = app->settings.capture_a && (stat(path_a, &stat_a) == 0);
    bool file_b_exists = app->settings.capture_b && (stat(path_b, &stat_b) == 0);

    if (file_a_exists || file_b_exists) {
        // Build detailed message with file info
        char message[512];
        char size_buf[32];
        int offset = 0;

        offset += snprintf(message + offset, sizeof(message) - offset,
            "The following files will be overwritten:\n\n");

        if (file_a_exists) {
            format_file_size(stat_a.st_size, size_buf, sizeof(size_buf));
            offset += snprintf(message + offset, sizeof(message) - offset,
                "CH A: %s (%s)\n", path_a, size_buf);
        }

        if (file_b_exists) {
            format_file_size(stat_b.st_size, size_buf, sizeof(size_buf));
            offset += snprintf(message + offset, sizeof(message) - offset,
                "CH B: %s (%s)\n", path_b, size_buf);
        }

        // Show confirmation popup with detailed info
        gui_popup_confirm("Overwrite Files?", message, "Overwrite", "Cancel", app);
        s_overwrite_pending = true;
        s_pending_app = app;
        return RECORD_PENDING;
    }

    // No files exist, start recording directly
    return gui_record_start_confirmed(app);
}

// Check popup result and continue recording if confirmed
void gui_record_check_popup(gui_app_t *app) {
    if (!s_overwrite_pending) {
        return;
    }

    popup_result_t result = gui_popup_get_result();

    if (result == POPUP_RESULT_NONE) {
        // Popup still open, wait
        return;
    }

    // Popup closed, clear pending state
    s_overwrite_pending = false;

    if (result == POPUP_RESULT_YES) {
        // User confirmed, start recording
        gui_record_start_confirmed(app);
    } else {
        // User cancelled
        gui_app_set_status(app, "Recording cancelled");
    }

    s_pending_app = NULL;
}

// Internal: Start recording after confirmation
static int gui_record_start_confirmed(gui_app_t *app) {

    // Build full output paths (output_path + filenames)
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/%s", app->settings.output_path, app->settings.output_filename_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", app->settings.output_path, app->settings.output_filename_b);

    // Check if using simulated device (doesn't use extraction thread)
    bool is_simulated = false;
    if (app->device_count > 0 && app->selected_device < app->device_count) {
        is_simulated = (app->devices[app->selected_device].type == DEVICE_TYPE_SIMULATED);
    }

    // Verify extraction thread is running (or simulated capture)
    if (!gui_extract_is_running() && !is_simulated) {
        gui_app_set_status(app, "Extraction not running");
        return RECORD_ERROR;
    }

    // For simulated capture, ensure record ringbuffers are initialized
    if (is_simulated) {
        gui_extract_init_record_rbs();
    }

    // Get record ringbuffers from gui_extract
    ringbuffer_t *rb_a = gui_extract_get_record_rb_a();
    ringbuffer_t *rb_b = gui_extract_get_record_rb_b();

    if (!rb_a || !rb_b) {
        gui_app_set_status(app, "Record buffers not initialized");
        return RECORD_ERROR;
    }

    s_recording_app = app;
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);

    // Reset record ringbuffers before starting
    gui_extract_reset_record_rbs();

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac) {
        // Open FLAC files (respect per-channel enable)
        s_file_a = app->settings.capture_a ? fopen(path_a, "wb") : NULL;
        s_file_b = app->settings.capture_b ? fopen(path_b, "wb") : NULL;

        if ((app->settings.capture_a && !s_file_a) || (app->settings.capture_b && !s_file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return RECORD_ERROR;
        }

        // Determine per-channel RF bit depth (matches CLI: --8bit-* overrides)
        uint8_t bits_a = app->settings.reduce_8bit_a ? 8 : (app->settings.flac_12bit ? 12 : 16);
        uint8_t bits_b = app->settings.reduce_8bit_b ? 8 : (app->settings.flac_12bit ? 12 : 16);

        // Setup writer contexts
        s_ctx_a.rb = rb_a;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.compressed_bytes = &app->recording_compressed_a;
        s_ctx_a.flac_bits_per_sample = bits_a;
        s_ctx_a.app = app;

        s_ctx_b.rb = rb_b;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;
        s_ctx_b.compressed_bytes = &app->recording_compressed_b;
        s_ctx_b.flac_bits_per_sample = bits_b;
        s_ctx_b.app = app;

        // Configure FLAC writers using shared library
        flac_writer_config_t config = flac_writer_default_config();
        config.sample_rate = 40000;
        // bits_per_sample is set per-channel below
        config.bits_per_sample = 16;
        config.compression_level = app->settings.flac_level;
        config.verify = app->settings.flac_verification;
        config.num_threads = (app->settings.flac_threads > 0) ? (uint32_t)app->settings.flac_threads : 0;  // 0 = auto
        config.enable_seektable = true;

        // Create writer for channel A
        config.error_cb = gui_flac_error_callback;
        config.bytes_cb = gui_flac_bytes_callback;

        if (app->settings.capture_a) {
            config.bits_per_sample = s_ctx_a.flac_bits_per_sample;
            config.callback_user_data = &s_ctx_a;
            s_flac_writer_a = flac_writer_create_stream(s_file_a, &config);
            if (!s_flac_writer_a) {
                gui_app_set_status(app, "Failed to create FLAC encoder A");
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                return RECORD_ERROR;
            }
            s_ctx_a.writer = s_flac_writer_a;
        } else {
            s_flac_writer_a = NULL;
            s_ctx_a.writer = NULL;
        }

        // Create writer for channel B
        if (app->settings.capture_b) {
            config.bits_per_sample = s_ctx_b.flac_bits_per_sample;
            config.callback_user_data = &s_ctx_b;
            s_flac_writer_b = flac_writer_create_stream(s_file_b, &config);
            if (!s_flac_writer_b) {
                gui_app_set_status(app, "Failed to create FLAC encoder B");
                if (s_flac_writer_a) { flac_writer_abort(s_flac_writer_a); s_flac_writer_a = NULL; }
                if (s_file_a) fclose(s_file_a);
                if (s_file_b) fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                return RECORD_ERROR;
            }
            s_ctx_b.writer = s_flac_writer_b;
        } else {
            s_flac_writer_b = NULL;
            s_ctx_b.writer = NULL;
        }

        // Capture backpressure stats at recording start
        s_start_wait_count = atomic_load(&app->rb_wait_count);
        s_start_drop_count = atomic_load(&app->rb_drop_count);

        // Mark as recording and start writer threads
        app->is_recording = true;
        app->recording_start_time = GetTime();

        // Enable recording in extraction thread
        gui_extract_set_recording(true, true, bits_a, bits_b);

        if (app->settings.capture_a) {
            thrd_create(&s_writer_thread_a, flac_writer_thread, &s_ctx_a);
        }
        if (app->settings.capture_b) {
            thrd_create(&s_writer_thread_b, flac_writer_thread, &s_ctx_b);
        }
        s_writer_threads_running = true;

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, gui_capture_get_audio_ringbuffer());

        gui_app_set_status(app, "Recording (FLAC)...");
    } else
#endif
    {
        // RAW recording (respect per-channel enable)
        s_file_a = app->settings.capture_a ? fopen(path_a, "wb") : NULL;
        s_file_b = app->settings.capture_b ? fopen(path_b, "wb") : NULL;

        if ((app->settings.capture_a && !s_file_a) || (app->settings.capture_b && !s_file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return RECORD_ERROR;
        }

        uint8_t bits_a = app->settings.reduce_8bit_a ? 8 : 16;
        uint8_t bits_b = app->settings.reduce_8bit_b ? 8 : 16;

        s_ctx_a.rb = rb_a;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.raw_bytes_per_sample = (bits_a == 8) ? 1 : 2;

        s_ctx_b.rb = rb_b;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;
        s_ctx_b.raw_bytes_per_sample = (bits_b == 8) ? 1 : 2;

        // Capture backpressure stats at recording start
        s_start_wait_count = atomic_load(&app->rb_wait_count);
        s_start_drop_count = atomic_load(&app->rb_drop_count);

        // Mark as recording and start writer threads
        app->is_recording = true;
        app->recording_start_time = GetTime();

        // Enable recording in extraction thread
        gui_extract_set_recording(true, false, bits_a, bits_b);

        if (app->settings.capture_a) {
            thrd_create(&s_writer_thread_a, raw_writer_thread, &s_ctx_a);
        }
        if (app->settings.capture_b) {
            thrd_create(&s_writer_thread_b, raw_writer_thread, &s_ctx_b);
        }
        s_writer_threads_running = true;

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, gui_capture_get_audio_ringbuffer());

        gui_app_set_status(app, "Recording (RAW)...");
    }

    return RECORD_OK;
}

// Stop recording
void gui_record_stop(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }

    // Disable recording in extraction thread first
    // This stops new data from being written to record ringbuffers
    gui_extract_set_recording(false, false, 16, 16);

    // Stop audio output/monitoring
    gui_audio_stop(app);

    // Signal threads to stop
    app->is_recording = false;

    // Wait for writer threads to drain and exit
    if (s_writer_threads_running) {
        if (app->settings.capture_a) thrd_join(s_writer_thread_a, NULL);
        if (app->settings.capture_b) thrd_join(s_writer_thread_b, NULL);
        s_writer_threads_running = false;
    }

#if LIBFLAC_ENABLED == 1
    // Finalize FLAC writers (this also cleans them up)
    if (s_flac_writer_a) {
        flac_writer_finish(s_flac_writer_a);
        s_flac_writer_a = NULL;
    }
    if (s_flac_writer_b) {
        flac_writer_finish(s_flac_writer_b);
        s_flac_writer_b = NULL;
    }
#endif

    // Close files
    if (s_file_a) {
        fclose(s_file_a);
        s_file_a = NULL;
    }
    if (s_file_b) {
        fclose(s_file_b);
        s_file_b = NULL;
    }

    // Print recording summary with backpressure stats
    double duration = GetTime() - app->recording_start_time;
    uint64_t raw_a = atomic_load(&app->recording_raw_a);
    uint64_t raw_b = atomic_load(&app->recording_raw_b);
    uint32_t end_wait = atomic_load(&app->rb_wait_count);
    uint32_t end_drop = atomic_load(&app->rb_drop_count);
    uint32_t rec_waits = end_wait - s_start_wait_count;
    uint32_t rec_drops = end_drop - s_start_drop_count;

    char size_a[32], size_b[32];
    format_file_size((off_t)raw_a, size_a, sizeof(size_a));
    format_file_size((off_t)raw_b, size_b, sizeof(size_b));

    fprintf(stderr, "[REC] Recording stopped: %.1fs, A=%s, B=%s, waits=%u, drops=%u\n",
            duration, size_a, size_b, rec_waits, rec_drops);

    if (rec_drops > 0) {
        fprintf(stderr, "[REC] WARNING: %u frames were dropped during recording due to backpressure!\n", rec_drops);
    }

    s_recording_app = NULL;
    gui_app_set_status(app, "Recording stopped");
}
