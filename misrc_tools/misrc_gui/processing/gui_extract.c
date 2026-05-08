/*
 * MISRC hsdaoh-rp2350 GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#include "gui_extract.h"
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#include "../visualization/gui_oscilloscope.h"
#include "../signal/gui_cvbs.h"
#include "../../common/extract.h"
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"
#include "../../common/misrc_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Buffer sizes
#define BUFFER_READ_SIZE 65536
#define BUFFER_RECORD_SIZE ((size_t)256 * 1024 * 1024)  // 256MB per channel

// Extraction buffers (page-aligned for SSE/AVX)
static int16_t *s_buf_a = NULL;
static int16_t *s_buf_b = NULL;
static uint8_t *s_buf_aux = NULL;
static conv_function_t s_extract_fn = NULL;
static bool s_initialized = false;

static bool s_b_present = true; // Assume B channel present by default; adjust based on mode (frame vs upstream) in gui_extract_init

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

// Record path must never block extraction long enough to starve RF ingestion.
// If writer throughput falls behind, drop record writes instead of stalling.
static const backpressure_policy_t s_record_write_policy = {
    .max_wait_attempts = 0,
    .wait_timeout_ms = 0,
    .log_first_wait = false,
    .log_drops = false,
};

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
    uint32_t last_record_drop_a = 0;
    uint32_t last_record_drop_b = 0;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[EXTRACT] Continuous extraction thread started\n");

    if (s_extract_app) {
        last_record_drop_a = atomic_load(&s_extract_app->buffers.stats[BUF_RECORD_A].write_drops);
        last_record_drop_b = atomic_load(&s_extract_app->buffers.stats[BUF_RECORD_B].write_drops);
    }

    while (1) {
        // Check for exit
        if (atomic_load(&do_exit)) {
            break;
        }
        const int16_t *mapped_a = NULL;
        const int16_t *mapped_b = NULL;

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
        if (!s_b_present) {
            // Prevent stale/uninitialized data from driving CH-B meters/display
            memset(s_buf_b, 0, BUFFER_READ_SIZE * sizeof(int16_t));
        }
        bool swap_channels = (s_extract_app && s_extract_app->settings.misrc_mode);
        if (s_b_present && swap_channels) {
            mapped_a = s_buf_b;
            mapped_b = s_buf_a;
        } else {
            mapped_a = s_buf_a;
            mapped_b = s_b_present ? s_buf_b : NULL;
        }

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
                    memcpy(display_buf, mapped_a, BUFFER_READ_SIZE * sizeof(int16_t));

                if (mapped_b) {
                    memcpy(display_buf + BUFFER_READ_SIZE * sizeof(int16_t),
                    mapped_b, BUFFER_READ_SIZE * sizeof(int16_t));
                } else {
                    memset(display_buf + BUFFER_READ_SIZE * sizeof(int16_t),
                    0,
                    BUFFER_READ_SIZE * sizeof(int16_t));
                }

                bufmgr_write_end(&s_extract_app->buffers, BUF_DISPLAY, display_frame_size);
            }
            // If display buffer full, frame is silently dropped (lossy buffer policy)
        }


        // Stats are cheap - always update them
        //gui_extract_update_stats(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);
        gui_extract_update_stats(s_extract_app, mapped_a, mapped_b, BUFFER_READ_SIZE);


        // Sample counters always updated (cheap atomic ops)
        atomic_fetch_add(&s_extract_app->total_samples, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_a, BUFFER_READ_SIZE);
        if (s_b_present) {
        atomic_fetch_add(&s_extract_app->samples_b, BUFFER_READ_SIZE);
        }

        // Periodic buffer stats logging
        frame_count++;
        if (frame_count % BUFMGR_LOG_INTERVAL == 0) {
            bufmgr_log_periodic(&s_extract_app->buffers);
        }

        // Note: FFT is now processed from display samples in the render thread
        // (see gui_oscilloscope.c render_oscilloscope_channel split mode)

        // Conditionally write to record buffers via buffer manager
        // Note: we always write raw int16 blocks to BUF_RECORD_A/B.
        // The writer threads (FLAC/RAW) handle RF bit depth conversion and optional soxr resampling.
        bool recording_enabled = atomic_load(&s_recording_enabled);
        if (!recording_enabled) {
            last_record_drop_a = atomic_load(&s_extract_app->buffers.stats[BUF_RECORD_A].write_drops);
            last_record_drop_b = atomic_load(&s_extract_app->buffers.stats[BUF_RECORD_B].write_drops);
        }

        if (recording_enabled) {
            size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int16_t);
            bool want_a = s_extract_app->settings.capture_a;
            bool want_b = s_b_present && s_extract_app->settings.capture_b;
            int16_t *write_a = NULL;
            int16_t *write_b = NULL;

            if (want_a) {
                write_a = (int16_t *)bufmgr_write_begin(&s_extract_app->buffers,
                                                        BUF_RECORD_A,
                                                        sample_bytes,
                                                        &s_record_write_policy);
            }

            if (want_b) {
                write_b = (int16_t *)bufmgr_write_begin(&s_extract_app->buffers,
                                                        BUF_RECORD_B,
                                                        sample_bytes,
                                                        &s_record_write_policy);
            }

            if ((want_a && !write_a) || (want_b && !write_b)) {
                uint32_t total_drop_a = atomic_load(&s_extract_app->buffers.stats[BUF_RECORD_A].write_drops);
                uint32_t total_drop_b = atomic_load(&s_extract_app->buffers.stats[BUF_RECORD_B].write_drops);
                uint32_t delta_drop_a = (total_drop_a > last_record_drop_a)
                                          ? (total_drop_a - last_record_drop_a)
                                          : 0;
                uint32_t delta_drop_b = (total_drop_b > last_record_drop_b)
                                          ? (total_drop_b - last_record_drop_b)
                                          : 0;
                uint32_t delta_total = delta_drop_a + delta_drop_b;
                if (delta_total == 0) {
                    delta_total = 1;
                }
                last_record_drop_a = total_drop_a;
                last_record_drop_b = total_drop_b;

                char drop_msg[256];
                snprintf(drop_msg, sizeof(drop_msg),
                         "Record backpressure drop at extract_frame=%u: +%u block(s) (A:+%u B:+%u, totals A=%u B=%u)",
                         frame_count, delta_total, delta_drop_a, delta_drop_b, total_drop_a, total_drop_b);
                gui_record_log_capture_event(s_extract_app, "ERROR", drop_msg, GUI_ERROR_CLASS_SYSTEM, delta_total);
                if (misrc_debug_enabled()) {
                    fprintf(stderr, "[EXTRACT] %s\n", drop_msg);
                }

                if (s_extract_app->settings.stop_on_dropout &&
                    !atomic_load(&s_extract_app->dropout_stop_requested)) {
                    atomic_store(&s_extract_app->dropout_stop_reason, GUI_DROPOUT_BACKPRESSURE);
                    atomic_store(&s_extract_app->dropout_stop_requested, true);
                }
                if (write_a) bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_A, 0);
                if (write_b) bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_B, 0);
                if (atomic_load(&do_exit)) goto exit_thread;
                continue;
            }

            if (want_a) {
                memcpy(write_a, mapped_a, sample_bytes);
                bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_A, sample_bytes);
            }

            if (want_b) {
                memcpy(write_b, mapped_b, sample_bytes);
                bufmgr_write_end(&s_extract_app->buffers, BUF_RECORD_B, sample_bytes);
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

//    // Get extraction function (AB mode)
//#ifdef HSDAOH_UPSTREAM
//    // Upstream mode: A-only extraction (B channel is not present, so pass NULL to get_conv_function)
//    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, NULL);
//    fprintf(stderr, "[EXTRACT] Using A-only extraction for upstream mode\n");
//#else
//    // Frame mode: AB dual channel extraction 
//    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);
// #endif

// Get extraction function
#ifdef HSDAOH_UPSTREAM
    // Upstream mode: currently A-only extraction
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, NULL);
    s_b_present = false;
    fprintf(stderr, "[EXTRACT] Using A-only extraction for upstream mode\n");
#else
    // Frame mode: AB dual channel extraction
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);
    s_b_present = true;
#endif



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
    if (thrd_create_with_priority(&s_extract_thread,
                                  extraction_thread,
                                  NULL,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
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
     bool have_b = (buf_b != NULL); // buf_b may be NULL in upstream mode, so check before processing B channel stats                            
    // Peak detection uses first 1000 samples
    size_t peak_samples = (num_samples < 1000) ? num_samples : 1000;

     for (size_t i = 0; i < num_samples; i++) {
        int16_t sa = buf_a[i];

        // Clipping detection (12-bit ADC: +2047 is positive clip, -2048 is negative clip)
        if (sa >= 2047) clip_a_pos++;
        else if (sa <= -2048) clip_a_neg++;

        // Peak detection (first N samples only) - A
        if (i < peak_samples) {
            if (sa > 0 && (uint16_t)sa > peak_a_pos) peak_a_pos = (uint16_t)sa;
            if (sa < 0 && (uint16_t)(-sa) > peak_a_neg) peak_a_neg = (uint16_t)(-sa);
        }

        // B only if present
        if (have_b) {
            int16_t sb = buf_b[i];

            if (sb >= 2047) clip_b_pos++;
            else if (sb <= -2048) clip_b_neg++;

            if (i < peak_samples) {
                if (sb > 0 && (uint16_t)sb > peak_b_pos) peak_b_pos = (uint16_t)sb;
                if (sb < 0 && (uint16_t)(-sb) > peak_b_neg) peak_b_neg = (uint16_t)(-sb);
            }
        }
    }


    // Update atomic counters
    atomic_fetch_add(&app->clip_count_a_pos, clip_a_pos);
    atomic_fetch_add(&app->clip_count_a_neg, clip_a_neg);
    atomic_store(&app->peak_a_pos, peak_a_pos);
    atomic_store(&app->peak_a_neg, peak_a_neg);

    if (have_b) {
        atomic_fetch_add(&app->clip_count_b_pos, clip_b_pos);
        atomic_fetch_add(&app->clip_count_b_neg, clip_b_neg);
        atomic_store(&app->peak_b_pos, peak_b_pos);
        atomic_store(&app->peak_b_neg, peak_b_neg);
    } else {
        atomic_store(&app->peak_b_pos, 0);
        atomic_store(&app->peak_b_neg, 0);
        atomic_store(&app->clip_count_b_pos, 0);
        atomic_store(&app->clip_count_b_neg, 0);
    }

}

rb_event_t *gui_extract_get_data_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_data_event;
}

rb_event_t *gui_extract_get_space_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_space_event;
}

