/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#include "gui_extract.h"
#include "gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_cvbs.h"
#include "../misrc_common/extract.h"
#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/rb_event.h"
#include "../misrc_common/threading.h"
#include "../misrc_common/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Buffer sizes
#define BUFFER_READ_SIZE 65536
#define BUFFER_RECORD_SIZE (65536 * 1024)  // 64MB per channel

// Extraction buffers (page-aligned for SSE/AVX)
static int16_t *s_buf_a = NULL;
static int16_t *s_buf_b = NULL;
static uint8_t *s_buf_aux = NULL;
static conv_function_t s_extract_fn = NULL;
static bool s_initialized = false;

// Scratch buffers for recording conversions
static int8_t *s_tmp8_a = NULL;
static int8_t *s_tmp8_b = NULL;
static int32_t *s_tmp32_a = NULL;
static int32_t *s_tmp32_b = NULL;

// Recording ringbuffers (extracted samples -> file writers)
static ringbuffer_t s_record_rb_a;
static ringbuffer_t s_record_rb_b;
static bool s_record_rb_initialized = false;

// Extraction thread state
static thrd_t s_extract_thread;
static bool s_extract_thread_running = false;
static ringbuffer_t *s_capture_rb = NULL;
static gui_app_t *s_extract_app = NULL;

// Recording state (atomic for thread-safe access)
static atomic_bool s_recording_enabled = false;
static atomic_bool s_use_flac = false;
static atomic_uchar s_rf_bits_a = 16;
static atomic_uchar s_rf_bits_b = 16;

static conv_16to8_t s_conv_16to8 = NULL;
static conv_16to32_t s_conv_16to8to32 = NULL;

// Event signaling for producer/consumer synchronization
static rb_event_t s_data_event;       // Signaled when new data is available in ringbuffer
static rb_event_t s_space_event;      // Signaled when space becomes available in ringbuffer
static bool s_events_initialized = false;

// Event signaling for record ringbuffers (extraction thread waits, file writers signal)
static rb_event_t s_record_space_event;  // Signaled when record buffer space becomes available
static bool s_record_events_initialized = false;

// Display throttling - update at ~60fps instead of every buffer
#define DISPLAY_UPDATE_INTERVAL_MS 16  // ~60fps

// Extraction thread - runs continuously from capture start to stop
// Always updates display/stats, conditionally writes to record ringbuffers
static int extraction_thread(void *ctx) {
    (void)ctx;
    size_t read_size = BUFFER_READ_SIZE * 4;  // 4 bytes per sample pair
    size_t clip[2] = {0, 0};
    uint16_t peak[2] = {0, 0};

    // Display throttling state
    uint64_t last_display_update_ms = 0;

    fprintf(stderr, "[EXTRACT] Continuous extraction thread started\n");

    while (1) {
        // Check for exit
        if (atomic_load(&do_exit)) {
            break;
        }

        // Try to read from capture ringbuffer
        void *buf = rb_read_ptr(s_capture_rb, read_size);
        if (!buf) {
            // No data available yet - check if we should exit
            if (!s_extract_app || !s_extract_app->is_capturing) {
                break;
            }
            // Wait on event instead of polling (with timeout for exit check)
            if (s_events_initialized) {
                rb_event_wait_timeout(&s_data_event, 20);  // Reduced from 100ms for faster exit
            } else {
                thrd_sleep_ms(1);
            }
            continue;
        }

        // Extract samples (always - this is the core work)
        s_extract_fn((uint32_t*)buf, BUFFER_READ_SIZE, clip, s_buf_aux, s_buf_a, s_buf_b, peak);

        // Mark capture buffer as consumed
        rb_read_finished(s_capture_rb, read_size);

        // Signal that space is now available (for callback waiting on full buffer)
        if (s_events_initialized) {
            rb_event_signal(&s_space_event);
        }

        // CVBS decode must process every buffer to maintain video timing
        // (cannot be throttled - decoder tracks line/frame sync)
        cvbs_decoder_t *cvbs_a = atomic_load(&s_extract_app->cvbs_a);
        if (cvbs_a) {
            atomic_fetch_add(&s_extract_app->cvbs_busy_a, 1);
            int sys = atomic_load(&s_extract_app->cvbs_system_a);
            gui_cvbs_set_format(cvbs_a, sys);
            gui_cvbs_process_buffer(cvbs_a, s_buf_a, BUFFER_READ_SIZE);
            atomic_fetch_sub(&s_extract_app->cvbs_busy_a, 1);
        }
        cvbs_decoder_t *cvbs_b = atomic_load(&s_extract_app->cvbs_b);
        if (cvbs_b) {
            atomic_fetch_add(&s_extract_app->cvbs_busy_b, 1);
            int sys = atomic_load(&s_extract_app->cvbs_system_b);
            gui_cvbs_set_format(cvbs_b, sys);
            gui_cvbs_process_buffer(cvbs_b, s_buf_b, BUFFER_READ_SIZE);
            atomic_fetch_sub(&s_extract_app->cvbs_busy_b, 1);
        }

        // Throttle display/stats updates to ~60fps to reduce CPU usage
        // (oscilloscope and stats don't need every sample)
        uint64_t now_ms = get_time_ms();
        if (now_ms - last_display_update_ms >= DISPLAY_UPDATE_INTERVAL_MS) {
            last_display_update_ms = now_ms;

            // Update stats and display
            gui_extract_update_stats(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);
            gui_oscilloscope_update_display(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);
        }

        // Sample counters always updated (cheap atomic ops)
        atomic_fetch_add(&s_extract_app->total_samples, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_a, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_b, BUFFER_READ_SIZE);

        // Note: FFT is now processed from display samples in the render thread
        // (see gui_oscilloscope.c render_oscilloscope_channel split mode)

        // Conditionally write to record ringbuffers
        if (atomic_load(&s_recording_enabled)) {
            bool use_flac = atomic_load(&s_use_flac);
            uint8_t bits_a = (uint8_t)atomic_load(&s_rf_bits_a);
            uint8_t bits_b = (uint8_t)atomic_load(&s_rf_bits_b);

            if (use_flac) {
                // FLAC: write int32 samples (BUFFER_READ_SIZE samples)
                size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int32_t);

                int32_t *write_a;
                int32_t *write_b;
                while ((write_a = (int32_t *)rb_write_ptr(&s_record_rb_a, sample_bytes)) == NULL ||
                       (write_b = (int32_t *)rb_write_ptr(&s_record_rb_b, sample_bytes)) == NULL) {
                    if (atomic_load(&do_exit)) {
                        goto exit_thread;
                    }
                    // Wait on event instead of polling (file writers signal when space available)
                    if (s_record_events_initialized) {
                        rb_event_wait_timeout(&s_record_space_event, 10);
                    } else {
                        thrd_sleep_ms(1);
                    }
                }

                // Channel A
                if (bits_a == 8) {
                    // clamp to int8 range, widen to int32
                    if (s_conv_16to8to32) s_conv_16to8to32(s_buf_a, write_a, BUFFER_READ_SIZE);
                    else {
                        for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                            int16_t v = s_buf_a[i];
                            if (v > 127) v = 127;
                            if (v < -128) v = -128;
                            write_a[i] = (int32_t)v;
                        }
                    }
                } else if (bits_a == 12) {
                    // already 12-bit range; keep as-is
                    for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                        write_a[i] = (int32_t)s_buf_a[i];
                    }
                } else {
                    // 16-bit: expand 12-bit to 16-bit by shifting
                    for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                        write_a[i] = (int32_t)s_buf_a[i] << 4;
                    }
                }

                // Channel B
                if (bits_b == 8) {
                    if (s_conv_16to8to32) s_conv_16to8to32(s_buf_b, write_b, BUFFER_READ_SIZE);
                    else {
                        for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                            int16_t v = s_buf_b[i];
                            if (v > 127) v = 127;
                            if (v < -128) v = -128;
                            write_b[i] = (int32_t)v;
                        }
                    }
                } else if (bits_b == 12) {
                    for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                        write_b[i] = (int32_t)s_buf_b[i];
                    }
                } else {
                    for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                        write_b[i] = (int32_t)s_buf_b[i] << 4;
                    }
                }

                rb_write_finished(&s_record_rb_a, sample_bytes);
                rb_write_finished(&s_record_rb_b, sample_bytes);
            } else {
                // RAW: write either int16 or int8 bytes
                size_t bytes_a = BUFFER_READ_SIZE * ((bits_a == 8) ? sizeof(int8_t) : sizeof(int16_t));
                size_t bytes_b = BUFFER_READ_SIZE * ((bits_b == 8) ? sizeof(int8_t) : sizeof(int16_t));

                void *write_a;
                void *write_b;
                while ((write_a = rb_write_ptr(&s_record_rb_a, bytes_a)) == NULL ||
                       (write_b = rb_write_ptr(&s_record_rb_b, bytes_b)) == NULL) {
                    if (atomic_load(&do_exit)) {
                        goto exit_thread;
                    }
                    // Wait on event instead of polling (file writers signal when space available)
                    if (s_record_events_initialized) {
                        rb_event_wait_timeout(&s_record_space_event, 10);
                    } else {
                        thrd_sleep_ms(1);
                    }
                }

                if (bits_a == 8) {
                    if (s_conv_16to8) s_conv_16to8(s_buf_a, (int8_t *)write_a, BUFFER_READ_SIZE);
                    else {
                        int8_t *dst = (int8_t *)write_a;
                        for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                            int16_t v = s_buf_a[i];
                            if (v > 127) v = 127;
                            if (v < -128) v = -128;
                            dst[i] = (int8_t)v;
                        }
                    }
                } else {
                    memcpy(write_a, s_buf_a, bytes_a);
                }

                if (bits_b == 8) {
                    if (s_conv_16to8) s_conv_16to8(s_buf_b, (int8_t *)write_b, BUFFER_READ_SIZE);
                    else {
                        int8_t *dst = (int8_t *)write_b;
                        for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                            int16_t v = s_buf_b[i];
                            if (v > 127) v = 127;
                            if (v < -128) v = -128;
                            dst[i] = (int8_t)v;
                        }
                    }
                } else {
                    memcpy(write_b, s_buf_b, bytes_b);
                }

                rb_write_finished(&s_record_rb_a, bytes_a);
                rb_write_finished(&s_record_rb_b, bytes_b);
            }
        }
    }

exit_thread:
    fprintf(stderr, "[EXTRACT] Continuous extraction thread exiting\n");
    return 0;
}

void gui_extract_init(void) {
    if (s_initialized) return;

    // Allocate 32-byte aligned buffers for SSE/AVX
    s_buf_a = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_b = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_aux = aligned_alloc(16, BUFFER_READ_SIZE);

    // Scratch buffers for conversions
    s_tmp8_a = (int8_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int8_t));
    s_tmp8_b = (int8_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int8_t));
    s_tmp32_a = (int32_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int32_t));
    s_tmp32_b = (int32_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int32_t));

    s_conv_16to8 = get_16to8_function();
    s_conv_16to8to32 = get_16to8to32_function();

    // Get extraction function (AB mode)
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);

    // Initialize synchronization events
    if (!s_events_initialized) {
        if (rb_event_init(&s_data_event) == 0 && rb_event_init(&s_space_event) == 0) {
            s_events_initialized = true;
            fprintf(stderr, "[EXTRACT] Event signaling initialized\n");
        } else {
            fprintf(stderr, "[EXTRACT] Warning: Failed to initialize events, falling back to polling\n");
        }
    }

    // Initialize record buffer space event (for event-based backpressure during recording)
    if (!s_record_events_initialized) {
        if (rb_event_init(&s_record_space_event) == 0) {
            s_record_events_initialized = true;
            fprintf(stderr, "[EXTRACT] Record buffer event initialized\n");
        } else {
            fprintf(stderr, "[EXTRACT] Warning: Failed to initialize record event, falling back to polling\n");
        }
    }

    s_initialized = true;
}

void gui_extract_cleanup(void) {
    // Stop extraction thread if running
    gui_extract_stop();

    // Close record ringbuffers
    if (s_record_rb_initialized) {
        rb_close(&s_record_rb_a);
        rb_close(&s_record_rb_b);
        s_record_rb_initialized = false;
    }

    // Destroy synchronization events
    if (s_events_initialized) {
        rb_event_destroy(&s_data_event);
        rb_event_destroy(&s_space_event);
        s_events_initialized = false;
    }

    // Destroy record buffer event
    if (s_record_events_initialized) {
        rb_event_destroy(&s_record_space_event);
        s_record_events_initialized = false;
    }

    // Free extraction buffers
    if (s_initialized) {
        if (s_buf_a) {
            aligned_free(s_buf_a);
            s_buf_a = NULL;
        }
        if (s_buf_b) {
            aligned_free(s_buf_b);
            s_buf_b = NULL;
        }
        if (s_buf_aux) {
            aligned_free(s_buf_aux);
            s_buf_aux = NULL;
        }
        if (s_tmp8_a) {
            aligned_free(s_tmp8_a);
            s_tmp8_a = NULL;
        }
        if (s_tmp8_b) {
            aligned_free(s_tmp8_b);
            s_tmp8_b = NULL;
        }
        if (s_tmp32_a) {
            aligned_free(s_tmp32_a);
            s_tmp32_a = NULL;
        }
        if (s_tmp32_b) {
            aligned_free(s_tmp32_b);
            s_tmp32_b = NULL;
        }
        s_initialized = false;
    }
}

int gui_extract_start(gui_app_t *app, ringbuffer_t *capture_rb) {
    if (s_extract_thread_running) {
        return 0;  // Already running
    }

    // Initialize extraction if needed
    gui_extract_init();

    // Initialize record ringbuffers if needed
    if (!s_record_rb_initialized) {
        rb_init(&s_record_rb_a, "record_a_rb", BUFFER_RECORD_SIZE);
        rb_init(&s_record_rb_b, "record_b_rb", BUFFER_RECORD_SIZE);
        s_record_rb_initialized = true;
    }

    // Store context
    s_capture_rb = capture_rb;
    s_extract_app = app;
    atomic_store(&s_recording_enabled, false);
    atomic_store(&s_use_flac, false);

    // Start extraction thread
    if (thrd_create(&s_extract_thread, extraction_thread, NULL) != thrd_success) {
        fprintf(stderr, "[EXTRACT] Failed to create extraction thread\n");
        return -1;
    }

    s_extract_thread_running = true;
    fprintf(stderr, "[EXTRACT] Started continuous extraction thread\n");
    return 0;
}

void gui_extract_stop(void) {
    if (!s_extract_thread_running) {
        return;
    }

    // Disable recording first
    atomic_store(&s_recording_enabled, false);

    // Wait for thread to exit
    thrd_join(s_extract_thread, NULL);
    s_extract_thread_running = false;
    s_extract_app = NULL;
    s_capture_rb = NULL;

    fprintf(stderr, "[EXTRACT] Stopped continuous extraction thread\n");
}

bool gui_extract_is_running(void) {
    return s_extract_thread_running;
}

ringbuffer_t *gui_extract_get_record_rb_a(void) {
    return &s_record_rb_a;
}

ringbuffer_t *gui_extract_get_record_rb_b(void) {
    return &s_record_rb_b;
}

void gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b) {
    atomic_store(&s_use_flac, use_flac);
    atomic_store(&s_rf_bits_a, rf_bits_a);
    atomic_store(&s_rf_bits_b, rf_bits_b);
    atomic_store(&s_recording_enabled, enabled);
    fprintf(stderr, "[EXTRACT] Recording %s (FLAC: %s, bits A:%u B:%u)\n",
            enabled ? "enabled" : "disabled",
            use_flac ? "yes" : "no",
            (unsigned)rf_bits_a, (unsigned)rf_bits_b);
}

void gui_extract_reset_record_rbs(void) {
    if (s_record_rb_initialized) {
        atomic_store(&s_record_rb_a.head, 0);
        atomic_store(&s_record_rb_a.tail, 0);
        atomic_store(&s_record_rb_b.head, 0);
        atomic_store(&s_record_rb_b.tail, 0);
    }
}

void gui_extract_init_record_rbs(void) {
    if (!s_record_rb_initialized) {
        rb_init(&s_record_rb_a, "record_a_rb", BUFFER_RECORD_SIZE);
        rb_init(&s_record_rb_b, "record_b_rb", BUFFER_RECORD_SIZE);
        s_record_rb_initialized = true;
        fprintf(stderr, "[EXTRACT] Record ringbuffers initialized (for simulated capture)\n");
    }
}

bool gui_extract_is_recording(bool *use_flac) {
    if (use_flac) {
        *use_flac = atomic_load(&s_use_flac);
    }
    return atomic_load(&s_recording_enabled);
}

extract_fn_t gui_extract_get_function(void) {
    if (!s_initialized) gui_extract_init();
    return (extract_fn_t)s_extract_fn;
}

int16_t *gui_extract_get_buf_a(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_a;
}

int16_t *gui_extract_get_buf_b(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_b;
}

uint8_t *gui_extract_get_buf_aux(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_aux;
}

void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples) {
    size_t clip_a_pos = 0, clip_a_neg = 0;
    size_t clip_b_pos = 0, clip_b_neg = 0;
    uint16_t peak_a_pos = 0, peak_a_neg = 0;
    uint16_t peak_b_pos = 0, peak_b_neg = 0;

    // Peak detection uses first 1000 samples
    size_t peak_samples = (num_samples < 1000) ? num_samples : 1000;

    for (size_t i = 0; i < num_samples; i++) {
        int16_t sa = buf_a[i];
        int16_t sb = buf_b[i];

        // Clipping detection (12-bit ADC: +2047 is positive clip, -2048 is negative clip)
        if (sa >= 2047) clip_a_pos++;
        else if (sa <= -2048) clip_a_neg++;
        if (sb >= 2047) clip_b_pos++;
        else if (sb <= -2048) clip_b_neg++;

        // Peak detection (first N samples only)
        if (i < peak_samples) {
            if (sa > 0 && (uint16_t)sa > peak_a_pos) peak_a_pos = (uint16_t)sa;
            if (sb > 0 && (uint16_t)sb > peak_b_pos) peak_b_pos = (uint16_t)sb;
            if (sa < 0 && (uint16_t)(-sa) > peak_a_neg) peak_a_neg = (uint16_t)(-sa);
            if (sb < 0 && (uint16_t)(-sb) > peak_b_neg) peak_b_neg = (uint16_t)(-sb);
        }
    }

    // Update atomic counters
    atomic_fetch_add(&app->clip_count_a_pos, clip_a_pos);
    atomic_fetch_add(&app->clip_count_a_neg, clip_a_neg);
    atomic_fetch_add(&app->clip_count_b_pos, clip_b_pos);
    atomic_fetch_add(&app->clip_count_b_neg, clip_b_neg);
    atomic_store(&app->peak_a_pos, peak_a_pos);
    atomic_store(&app->peak_a_neg, peak_a_neg);
    atomic_store(&app->peak_b_pos, peak_b_pos);
    atomic_store(&app->peak_b_neg, peak_b_neg);
}

rb_event_t *gui_extract_get_data_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_data_event;
}

rb_event_t *gui_extract_get_space_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_space_event;
}

rb_event_t *gui_extract_get_record_space_event(void) {
    if (!s_record_events_initialized) return NULL;
    return &s_record_space_event;
}
