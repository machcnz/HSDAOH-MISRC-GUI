/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#include "gui_extract.h"
#include "../core/gui_app.h"
#include "../visualization/gui_oscilloscope.h"
#include "../signal/gui_cvbs.h"
#include "../../common/extract.h"
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"

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

// Note: Record ringbuffers now managed by buffer manager (BUF_RECORD_A, BUF_RECORD_B)

// Extraction thread state
static thrd_t s_extract_thread;
static bool s_extract_thread_running = false;
static gui_app_t *s_extract_app = NULL;
// Note: s_capture_rb removed - now using app->buffers (buffer_manager)

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

// Note: Record buffer events now managed by buffer manager

// Periodic buffer stats logging interval (in frames, ~1 frame = 65536 samples at 40MHz = 1.64ms)
// Log every ~2 seconds = 1220 frames
#define BUFMGR_LOG_INTERVAL 1220

// Extraction thread - runs continuously from capture start to stop
// Always updates display/stats, conditionally writes to record ringbuffers
static int extraction_thread(void *ctx) {
    (void)ctx;
    size_t read_size = BUFFER_READ_SIZE * 4;  // 4 bytes per sample pair
    size_t clip[2] = {0, 0};
    uint16_t peak[2] = {0, 0};
    uint32_t frame_count = 0;

    fprintf(stderr, "[EXTRACT] Continuous extraction thread started\n");

    while (1) {
        // Check for exit
        if (atomic_load(&do_exit)) {
            break;
        }

        // Try to read from capture ringbuffer via buffer manager
        void *buf = bufmgr_read_begin(&s_extract_app->buffers, BUF_CAPTURE_RF, read_size, 20);
        if (!buf) {
            // No data available yet - check if we should exit
            if (!s_extract_app || !s_extract_app->is_capturing) {
                break;
            }
            // Buffer manager already waits with timeout, just continue loop
            continue;
        }

        // Extract samples (always - this is the core work)
        s_extract_fn((uint32_t*)buf, BUFFER_READ_SIZE, clip, s_buf_aux, s_buf_a, s_buf_b, peak);

        // Mark capture buffer as consumed via buffer manager
        bufmgr_read_end(&s_extract_app->buffers, BUF_CAPTURE_RF, read_size);

        // Signal that space is now available (buffer manager handles events internally)

        // Write extracted samples to display buffer (lossy - OK to drop for display)
        // Display thread handles CVBS decode + oscilloscope processing
        // Frame layout: [samples_a: BUFFER_READ_SIZE * 2 bytes] [samples_b: BUFFER_READ_SIZE * 2 bytes]
        {
            size_t display_frame_size = BUFFER_READ_SIZE * sizeof(int16_t) * 2;
            uint8_t *display_buf = bufmgr_write_begin(&s_extract_app->buffers, BUF_DISPLAY,
                                                       display_frame_size, NULL);
            if (display_buf) {
                // Copy both channels into display buffer
                memcpy(display_buf, s_buf_a, BUFFER_READ_SIZE * sizeof(int16_t));
                memcpy(display_buf + BUFFER_READ_SIZE * sizeof(int16_t),
                       s_buf_b, BUFFER_READ_SIZE * sizeof(int16_t));
                bufmgr_write_end(&s_extract_app->buffers, BUF_DISPLAY, display_frame_size);
            }
            // If display buffer full, frame is silently dropped (lossy buffer policy)
        }

        // Stats are cheap - always update them
        gui_extract_update_stats(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);

        // Sample counters always updated (cheap atomic ops)
        atomic_fetch_add(&s_extract_app->total_samples, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_a, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_b, BUFFER_READ_SIZE);

        // Periodic buffer stats logging
        frame_count++;
        if (frame_count % BUFMGR_LOG_INTERVAL == 0) {
            bufmgr_log_periodic(&s_extract_app->buffers);
        }

        // Note: FFT is now processed from display samples in the render thread
        // (see gui_oscilloscope.c render_oscilloscope_channel split mode)

        // Conditionally write to record buffers via buffer manager
        if (atomic_load(&s_recording_enabled)) {
            bool use_flac = atomic_load(&s_use_flac);
            uint8_t bits_a = (uint8_t)atomic_load(&s_rf_bits_a);
            uint8_t bits_b = (uint8_t)atomic_load(&s_rf_bits_b);

            if (use_flac) {
                // FLAC: write int32 samples (BUFFER_READ_SIZE samples)
                size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int32_t);

                // Use default lossless backpressure policy from buffer_manager.c
                int32_t *write_a = (int32_t *)bufmgr_write_begin(&s_extract_app->buffers,
                                                                  BUF_RECORD_A, sample_bytes, NULL);
                int32_t *write_b = (int32_t *)bufmgr_write_begin(&s_extract_app->buffers,
                                                                  BUF_RECORD_B, sample_bytes, NULL);

                if (!write_a || !write_b) {
                    // Buffer full after waiting - this shouldn't happen with lossless policy
                    // but handle gracefully
                    if (write_a) bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_A, 0);
                    if (write_b) bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_B, 0);
                    if (atomic_load(&do_exit)) goto exit_thread;
                    continue;
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

                bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_A, sample_bytes);
                bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_B, sample_bytes);
            } else {
                // RAW: write either int16 or int8 bytes
                size_t bytes_a = BUFFER_READ_SIZE * ((bits_a == 8) ? sizeof(int8_t) : sizeof(int16_t));
                size_t bytes_b = BUFFER_READ_SIZE * ((bits_b == 8) ? sizeof(int8_t) : sizeof(int16_t));

                // Use default lossless backpressure policy from buffer_manager.c
                void *write_a = bufmgr_write_begin(&s_extract_app->buffers,
                                                    BUF_RECORD_A, bytes_a, NULL);
                void *write_b = bufmgr_write_begin(&s_extract_app->buffers,
                                                    BUF_RECORD_B, bytes_b, NULL);

                if (!write_a || !write_b) {
                    // Buffer full after waiting - handle gracefully
                    if (write_a) bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_A, 0);
                    if (write_b) bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_B, 0);
                    if (atomic_load(&do_exit)) goto exit_thread;
                    continue;
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

                bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_A, bytes_a);
                bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_B, bytes_b);
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

    // Note: Record buffer events now managed by buffer manager

    s_initialized = true;
}

void gui_extract_cleanup(void) {
    // Stop extraction thread if running
    gui_extract_stop();

    // Note: Record buffers now managed by buffer manager (cleanup via bufmgr_cleanup)

    // Destroy synchronization events
    if (s_events_initialized) {
        rb_event_destroy(&s_data_event);
        rb_event_destroy(&s_space_event);
        s_events_initialized = false;
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

int gui_extract_start(gui_app_t *app) {
    if (s_extract_thread_running) {
        return 0;  // Already running
    }

    // Initialize extraction if needed
    gui_extract_init();

    // Store context (uses app->buffers for capture ringbuffer via buffer manager)
    s_extract_app = app;
    atomic_store(&s_recording_enabled, false);
    atomic_store(&s_use_flac, false);

    // Ensure buffers are initialized in buffer manager
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) < 0) {
        fprintf(stderr, "[EXTRACT] Failed to initialize capture buffer\n");
        return -1;
    }
    if (bufmgr_ensure_init(&app->buffers, BUF_RECORD_A) < 0) {
        fprintf(stderr, "[EXTRACT] Failed to initialize record buffer A\n");
        return -1;
    }
    if (bufmgr_ensure_init(&app->buffers, BUF_RECORD_B) < 0) {
        fprintf(stderr, "[EXTRACT] Failed to initialize record buffer B\n");
        return -1;
    }

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

    fprintf(stderr, "[EXTRACT] Stopped continuous extraction thread\n");
}

bool gui_extract_is_running(void) {
    return s_extract_thread_running;
}

// Note: Record ringbuffers now accessed via app->buffers (buffer_manager)
// Use BUF_RECORD_A and BUF_RECORD_B with bufmgr_read_begin/bufmgr_read_end

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

void gui_extract_reset_record_rbs(gui_app_t *app) {
    // Reset record buffers via buffer manager
    if (app) {
        bufmgr_reset(&app->buffers, BUF_RECORD_A);
        bufmgr_reset(&app->buffers, BUF_RECORD_B);
    }
}

void gui_extract_init_record_rbs(gui_app_t *app) {
    // Initialize record buffers via buffer manager (for simulated capture that
    // doesn't go through gui_extract_start)
    if (app) {
        bufmgr_ensure_init(&app->buffers, BUF_RECORD_A);
        bufmgr_ensure_init(&app->buffers, BUF_RECORD_B);
        fprintf(stderr, "[EXTRACT] Record buffers initialized (for simulated capture)\n");
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

