#include "gui_audio.h"

#include "raylib.h"

#include "../../common/extract.h"
#include "../../common/wave.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"
#include "../../common/buffer_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <soxr.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

#define BUFFER_AUDIO_READ_SIZE  (65536 * 3)

typedef struct {
    buffer_manager_t *bufmgr;
    gui_app_t *app;

    FILE *f_4ch;
    FILE *f_2ch[2];
    FILE *f_1ch[4];

    uint64_t total_bytes;

    bool convert_1ch;
    bool convert_2ch;

    uint8_t *buffer_1ch[4];
    uint8_t *buffer_2ch[2];
} audio_ctx_t;

static thrd_t s_audio_thread;
static bool s_audio_running = false;
static atomic_bool s_audio_stop = false;
static audio_ctx_t s_audio_ctx;

// Playback monitoring (audio thread produces into queue; main thread consumes into AudioStream)
static atomic_bool s_playback_enabled = false;
static AudioStream s_play_stream;
static bool s_play_stream_inited = false;
static bool s_audio_device_inited = false;

// Resampler for 78.125kHz -> 48kHz conversion
static soxr_t s_resampler = NULL;

// Simple lock-free SPSC ring for stereo int16 samples (interleaved) at 48kHz
#define PLAY_Q_FRAMES (96000) // ~2 seconds at 48kHz
static int16_t *s_play_q = NULL; // length = PLAY_Q_FRAMES * 2
static atomic_size_t s_play_q_head = 0;
static atomic_size_t s_play_q_tail = 0;

static inline int32_t signext24(uint32_t v)
{
    v &= 0x00FFFFFFu;
    if (v & 0x00800000u) v |= 0xFF000000u;
    return (int32_t)v;
}

static inline size_t play_q_capacity_samples(void) {
    return (size_t)PLAY_Q_FRAMES * 2; // stereo
}

static void play_q_ensure_alloc(void) {
    if (!s_play_q) {
        s_play_q = (int16_t *)calloc(play_q_capacity_samples(), sizeof(int16_t));
        atomic_store(&s_play_q_head, 0);
        atomic_store(&s_play_q_tail, 0);
    }
}

static inline size_t play_q_used_samples(void) {
    size_t h = atomic_load(&s_play_q_head);
    size_t t = atomic_load(&s_play_q_tail);
    return t - h;
}

static inline size_t play_q_free_samples(void) {
    return play_q_capacity_samples() - play_q_used_samples();
}

static void play_q_push_samples(const int16_t *samples, size_t count) {
    if (!s_play_q || count == 0) return;

    // If overflow, drop oldest samples (monitoring only).
    size_t free_s = play_q_free_samples();
    if (count > free_s) {
        size_t need = count - free_s;
        size_t h = atomic_load(&s_play_q_head);
        atomic_store(&s_play_q_head, h + need);
    }

    size_t cap = play_q_capacity_samples();
    size_t t = atomic_load(&s_play_q_tail);
    for (size_t i = 0; i < count; i++) {
        s_play_q[(t + i) % cap] = samples[i];
    }
    atomic_store(&s_play_q_tail, t + count);
}

static size_t play_q_pop_samples(int16_t *out, size_t max_count) {
    if (!s_play_q || max_count == 0) return 0;
    size_t used = play_q_used_samples();
    size_t n = (used < max_count) ? used : max_count;
    if (n == 0) return 0;

    size_t cap = play_q_capacity_samples();
    size_t h = atomic_load(&s_play_q_head);
    for (size_t i = 0; i < n; i++) {
        out[i] = s_play_q[(h + i) % cap];
    }
    atomic_store(&s_play_q_head, h + n);
    return n;
}

static inline int16_t pcm24_to_i16(int32_t s24) {
    // s24 is signed 24-bit in int32; convert to 16-bit by dropping LSBs
    return (int16_t)(s24 >> 8);
}

static void audio_update_peaks(gui_app_t *app, const uint8_t *buf, size_t len)
{
    if (!app || !buf) return;
    if (len < 12) return;

    uint32_t peak[4] = {0, 0, 0, 0};

    // 4ch 24-bit, little-endian, interleaved per sample frame:
    // ch1: b0 b1 b2, ch2: b3 b4 b5, ch3: b6 b7 b8, ch4: b9 b10 b11
    for (size_t i = 0; i + 12 <= len; i += 12) {
        for (int ch = 0; ch < 4; ch++) {
            size_t o = i + (size_t)ch * 3;
            uint32_t raw = (uint32_t)buf[o] | ((uint32_t)buf[o + 1] << 8) | ((uint32_t)buf[o + 2] << 16);
            int32_t s = signext24(raw);
            uint32_t a = (uint32_t)(s < 0 ? -s : s);
            if (a > peak[ch]) peak[ch] = a;
        }
    }

    for (int ch = 0; ch < 4; ch++) {
        atomic_store(&app->audio_peak[ch], peak[ch]);
    }
}

static int audio_thread_main(void *ctx)
{
    audio_ctx_t *a = (audio_ctx_t *)ctx;
    size_t len = BUFFER_AUDIO_READ_SIZE;
    void *buf;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    
    fprintf(stderr, "[AUDIO] Audio thread started\n");

    wave_header_t h;
    memset(&h, 0, sizeof(h));

    a->total_bytes = 0;

    // Write placeholder headers
    if (a->f_4ch && a->f_4ch != stdout) fwrite(&h, 1, sizeof(h), a->f_4ch);
    for (int i = 0; i < 2; i++) {
        if (a->f_2ch[i] && a->f_2ch[i] != stdout) fwrite(&h, 1, sizeof(h), a->f_2ch[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (a->f_1ch[i] && a->f_1ch[i] != stdout) fwrite(&h, 1, sizeof(h), a->f_1ch[i]);
    }

    // Allocate conversion buffers if needed
    if (a->convert_1ch) {
        a->buffer_1ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
        if (!a->buffer_1ch[0]) return -1;
        for (int i = 1; i < 4; i++) a->buffer_1ch[i] = a->buffer_1ch[0] + (BUFFER_AUDIO_READ_SIZE / 4) * i;
    }
    if (a->convert_2ch) {
        a->buffer_2ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
        if (!a->buffer_2ch[0]) return -1;
        a->buffer_2ch[1] = a->buffer_2ch[0] + (BUFFER_AUDIO_READ_SIZE / 2);
    }

    size_t iter_count = 0;
    bool was_recording = false;
    while (1) {
        // Read from buffer manager with timeout
        buf = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 10);
        if (!buf) {
            if (iter_count < 5) {
                fprintf(stderr, "[AUDIO] No data in buffer (iter %zu), fill_level=%zu\n", 
                        iter_count, bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO));
            }
            iter_count++;
            if (atomic_load(&do_exit) || atomic_load(&s_audio_stop) || !a->app || !a->app->is_capturing) {
                // Drain any remaining data before exiting
                size_t remaining = bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO);
                while (remaining > 0) {
                    size_t drain_len = (remaining < len) ? remaining : len;
                    buf = bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, drain_len, 0);
                    if (!buf) break;

                    audio_update_peaks(a->app, (const uint8_t *)buf, drain_len);
                    if (a->f_4ch) fwrite(buf, 1, drain_len, a->f_4ch);
                    if (a->convert_1ch) extract_audio_1ch_C((uint8_t *)buf, drain_len, a->buffer_1ch[0], a->buffer_1ch[1], a->buffer_1ch[2], a->buffer_1ch[3]);
                    if (a->convert_2ch) extract_audio_2ch_C((uint16_t *)buf, drain_len, (uint16_t *)a->buffer_2ch[0], (uint16_t *)a->buffer_2ch[1]);

                    bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, drain_len);

                    for (int i = 0; i < 2; i++) if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, drain_len / 2, a->f_2ch[i]);
                    for (int i = 0; i < 4; i++) if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, drain_len / 4, a->f_1ch[i]);

                    a->total_bytes += drain_len;
                    remaining = bufmgr_fill_level(a->bufmgr, BUF_CAPTURE_AUDIO);
                }
                break;
            }
            continue;
        }

        // Update monitoring peaks
        audio_update_peaks(a->app, (const uint8_t *)buf, len);
        
        if (iter_count < 5) {
            fprintf(stderr, "[AUDIO] Got data: %zu bytes, peaks will be updated\n", len);
        }
        iter_count++;

        // Playback monitoring: downmix 4ch 24-bit to stereo 16-bit, resample 78.125kHz -> 48kHz
        if (a->app && atomic_load(&s_playback_enabled) && a->app->settings.audio_monitor_playback && s_resampler) {
            const uint8_t *b = (const uint8_t *)buf;
            const size_t frames_78khz = len / 12; // 4ch * 3 bytes per frame
            
            // Use static buffers to avoid alloca issues
            static int16_t tmp_78khz[65536];
            static int16_t tmp_48khz[65536];
            
            if (frames_78khz * 2 > 65536) {
                fprintf(stderr, "[AUDIO] Buffer too large for monitoring: %zu frames\n", frames_78khz);
                goto skip_monitoring;
            }
            
            // Downmix 4ch 24-bit to stereo 16-bit at 78.125kHz
            // Select CH1/2 or CH3/4 based on user setting
            const bool use_ch34 = a->app->settings.audio_monitor_ch34;
            const size_t ch_offset = use_ch34 ? 6 : 0; // CH3/4 starts at byte 6, CH1/2 at byte 0
            
            for (size_t i = 0; i < frames_78khz; i++) {
                size_t base = i * 12 + ch_offset;
                uint32_t r1 = (uint32_t)b[base] | ((uint32_t)b[base + 1] << 8) | ((uint32_t)b[base + 2] << 16);
                uint32_t r2 = (uint32_t)b[base + 3] | ((uint32_t)b[base + 4] << 8) | ((uint32_t)b[base + 5] << 16);
                int32_t s1 = signext24(r1);
                int32_t s2 = signext24(r2);
                int32_t mix = (s1 / 2) + (s2 / 2);
                int16_t s16 = pcm24_to_i16(mix);
                tmp_78khz[i * 2 + 0] = s16;
                tmp_78khz[i * 2 + 1] = s16;
            }
            
            // Resample 78.125kHz -> 48kHz
            size_t frames_48khz_max = (size_t)((double)frames_78khz * 48000.0 / 78125.0) + 32;
            if (frames_48khz_max * 2 > 65536) {
                fprintf(stderr, "[AUDIO] Output buffer too large: %zu frames\n", frames_48khz_max);
                goto skip_monitoring;
            }
            
            size_t idone = 0, odone = 0;
            soxr_error_t err = soxr_process(s_resampler,
                tmp_78khz, frames_78khz, &idone,
                tmp_48khz, frames_48khz_max, &odone);
            
            if (!err && odone > 0) {
                play_q_push_samples(tmp_48khz, odone * 2);
            } else if (err) {
                static int err_count = 0;
                if (err_count < 3) {
                    fprintf(stderr, "[AUDIO] Resampler error: %s\n", soxr_strerror(err));
                    err_count++;
                }
            }
        }
        
skip_monitoring:
        ;

        // Check if recording state changed
        bool is_recording = a->app && a->app->is_recording;
        
        // RECORDING JUST STARTED - open files dynamically
        if (!was_recording && is_recording) {
            fprintf(stderr, "[AUDIO] Recording started, opening files\n");
            
            char path[512];
            wave_header_t hdr;
            memset(&hdr, 0, sizeof(hdr));
            
            if (a->app->settings.enable_audio_4ch) {
                snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_4ch_filename);
                a->f_4ch = fopen(path, "wb");
                if (a->f_4ch && a->f_4ch != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_4ch);
            }
            
            if (a->app->settings.enable_audio_2ch_12) {
                snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_2ch_12_filename);
                a->f_2ch[0] = fopen(path, "wb");
                if (a->f_2ch[0] && a->f_2ch[0] != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_2ch[0]);
                a->convert_2ch = true;
            }
            
            if (a->app->settings.enable_audio_2ch_34) {
                snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_2ch_34_filename);
                a->f_2ch[1] = fopen(path, "wb");
                if (a->f_2ch[1] && a->f_2ch[1] != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_2ch[1]);
                a->convert_2ch = true;
            }
            
            for (int i = 0; i < 4; i++) {
                if (a->app->settings.enable_audio_1ch[i]) {
                    snprintf(path, sizeof(path), "%s/%s", a->app->settings.output_path, a->app->settings.audio_1ch_filenames[i]);
                    a->f_1ch[i] = fopen(path, "wb");
                    if (a->f_1ch[i] && a->f_1ch[i] != stdout) fwrite(&hdr, 1, sizeof(hdr), a->f_1ch[i]);
                    a->convert_1ch = true;
                }
            }
            
a->total_bytes = 0;
            
            // Allocate conversion buffers if needed and not already allocated
            if (a->convert_1ch && !a->buffer_1ch[0]) {
                a->buffer_1ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
                if (!a->buffer_1ch[0]) {
                    fprintf(stderr, "[AUDIO] Failed to allocate 1ch buffer\n");
                    // Close 1ch files and reset state
                    for (int i = 0; i < 4; i++) {
                        if (a->f_1ch[i]) { fclose(a->f_1ch[i]); a->f_1ch[i] = NULL; }
                    }
                    a->convert_1ch = false;
                    goto skip_file_ops;
                }
                for (int i = 1; i < 4; i++) a->buffer_1ch[i] = a->buffer_1ch[0] + (BUFFER_AUDIO_READ_SIZE / 4) * i;
            }
            if (a->convert_2ch && !a->buffer_2ch[0]) {
                a->buffer_2ch[0] = aligned_alloc(32, BUFFER_AUDIO_READ_SIZE);
                if (!a->buffer_2ch[0]) {
                    fprintf(stderr, "[AUDIO] Failed to allocate 2ch buffer\n");
                    // Close 2ch files and reset state
                    if (a->f_2ch[0]) { fclose(a->f_2ch[0]); a->f_2ch[0] = NULL; }
                    if (a->f_2ch[1]) { fclose(a->f_2ch[1]); a->f_2ch[1] = NULL; }
                    a->convert_2ch = false;
                    goto skip_file_ops;
                }
                a->buffer_2ch[1] = a->buffer_2ch[0] + (BUFFER_AUDIO_READ_SIZE / 2);
            }
        }
        
skip_file_ops:
        ;
        
        // RECORDING JUST STOPPED - close files and write headers
        if (was_recording && !is_recording) {
            wave_header_t h;
            if (a->f_4ch && a->f_4ch != stdout) {
                fseek(a->f_4ch, 0, SEEK_SET);
                create_wave_header(&h, a->total_bytes / 12, 78125, 4, 24);
                fwrite(&h, 1, sizeof(h), a->f_4ch);
                fclose(a->f_4ch);
                a->f_4ch = NULL;
            }
            for (int i = 0; i < 2; i++) {
                if (a->f_2ch[i] && a->f_2ch[i] != stdout) {
                    fseek(a->f_2ch[i], 0, SEEK_SET);
                    create_wave_header(&h, a->total_bytes / 12, 78125, 2, 24);
                    fwrite(&h, 1, sizeof(h), a->f_2ch[i]);
                    fclose(a->f_2ch[i]);
                    a->f_2ch[i] = NULL;
                }
            }
            for (int i = 0; i < 4; i++) {
                if (a->f_1ch[i] && a->f_1ch[i] != stdout) {
                    fseek(a->f_1ch[i], 0, SEEK_SET);
                    create_wave_header(&h, a->total_bytes / 12, 78125, 1, 24);
                    fwrite(&h, 1, sizeof(h), a->f_1ch[i]);
                    fclose(a->f_1ch[i]);
                    a->f_1ch[i] = NULL;
                }
            }
            fprintf(stderr, "[AUDIO] Recording stopped, files closed\n");
            // DO NOT reset a->total_bytes here
        }
        
        was_recording = is_recording;

        // Write files and extract only if recording active
        if (is_recording) {
            if (a->f_4ch) fwrite(buf, 1, len, a->f_4ch);
            if (a->convert_1ch) extract_audio_1ch_C((uint8_t *)buf, len, a->buffer_1ch[0], a->buffer_1ch[1], a->buffer_1ch[2], a->buffer_1ch[3]);
            if (a->convert_2ch) extract_audio_2ch_C((uint16_t *)buf, len, (uint16_t *)a->buffer_2ch[0], (uint16_t *)a->buffer_2ch[1]);
        }
        
        bufmgr_read_end(a->bufmgr, BUF_CAPTURE_AUDIO, len);

        if (is_recording) {
            for (int i = 0; i < 2; i++) if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, len / 2, a->f_2ch[i]);
            for (int i = 0; i < 4; i++) if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, len / 4, a->f_1ch[i]);
            a->total_bytes += len;
        }
    }

    // Cleanup at thread exit
    if (a->f_4ch && a->f_4ch != stdout) {
        fseek(a->f_4ch, 0, SEEK_SET);
        create_wave_header(&h, a->total_bytes / 12, 78125, 4, 24);
        fwrite(&h, 1, sizeof(h), a->f_4ch);
        fclose(a->f_4ch);
    }
    for (int i = 0; i < 2; i++) {
        if (a->f_2ch[i] && a->f_2ch[i] != stdout) {
            fseek(a->f_2ch[i], 0, SEEK_SET);
            create_wave_header(&h, a->total_bytes / 12, 78125, 2, 24);
            fwrite(&h, 1, sizeof(h), a->f_2ch[i]);
            fclose(a->f_2ch[i]);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (a->f_1ch[i] && a->f_1ch[i] != stdout) {
            fseek(a->f_1ch[i], 0, SEEK_SET);
            create_wave_header(&h, a->total_bytes / 12, 78125, 1, 24);
            fwrite(&h, 1, sizeof(h), a->f_1ch[i]);
            fclose(a->f_1ch[i]);
        }
    }

    if (a->convert_1ch && a->buffer_1ch[0]) aligned_free(a->buffer_1ch[0]);
    if (a->convert_2ch && a->buffer_2ch[0]) aligned_free(a->buffer_2ch[0]);

    return 0;
}

bool gui_audio_is_running(void)
{
    return s_audio_running;
}

int gui_audio_start(gui_app_t *app, buffer_manager_t *bufmgr)
{
    if (!app || !bufmgr) return -1;
    if (s_audio_running) return 0;

    play_q_ensure_alloc();
    atomic_store(&s_playback_enabled, app->settings.audio_monitor_playback);

    // Ensure audio buffer is initialized
    if (bufmgr_ensure_init(bufmgr, BUF_CAPTURE_AUDIO) < 0) {
        gui_app_set_status(app, "Failed to initialize audio buffer");
        return -1;
    }

    // Decide if audio outputs should be written to files.
    // Audio capture is always-on during capture for monitoring/draining.
    const bool write_files = app->is_recording;

    bool want_4ch = write_files && app->settings.enable_audio_4ch;
    bool want_2ch_12 = write_files && app->settings.enable_audio_2ch_12;
    bool want_2ch_34 = write_files && app->settings.enable_audio_2ch_34;

    memset(&s_audio_ctx, 0, sizeof(s_audio_ctx));
    s_audio_ctx.bufmgr = bufmgr;
    s_audio_ctx.app = app;

    // Open files under output_path
    char path[512];

    if (want_4ch) {
        snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_4ch_filename);
        s_audio_ctx.f_4ch = fopen(path, "wb");
        if (!s_audio_ctx.f_4ch) {
            gui_app_set_status(app, "Failed to open audio_4ch output");
            return -1;
        }
    }

    if (want_2ch_12) {
        snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_2ch_12_filename);
        s_audio_ctx.f_2ch[0] = fopen(path, "wb");
        if (!s_audio_ctx.f_2ch[0]) {
            gui_app_set_status(app, "Failed to open audio_2ch_12 output");
            return -1;
        }
        s_audio_ctx.convert_2ch = true;
    }

    if (want_2ch_34) {
        snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_2ch_34_filename);
        s_audio_ctx.f_2ch[1] = fopen(path, "wb");
        if (!s_audio_ctx.f_2ch[1]) {
            gui_app_set_status(app, "Failed to open audio_2ch_34 output");
            return -1;
        }
        s_audio_ctx.convert_2ch = true;
    }

    for (int i = 0; i < 4; i++) {
        if (write_files && app->settings.enable_audio_1ch[i]) {
            snprintf(path, sizeof(path), "%s/%s", app->settings.output_path, app->settings.audio_1ch_filenames[i]);
            s_audio_ctx.f_1ch[i] = fopen(path, "wb");
            if (!s_audio_ctx.f_1ch[i]) {
                gui_app_set_status(app, "Failed to open audio_1ch output");
                return -1;
            }
            s_audio_ctx.convert_1ch = true;
        }
    }

    // Reset peaks
    for (int i = 0; i < 4; i++) atomic_store(&app->audio_peak[i], 0);

    atomic_store(&s_audio_stop, false);

    if (thrd_create(&s_audio_thread, audio_thread_main, &s_audio_ctx) != thrd_success) {
        gui_app_set_status(app, "Failed to start audio thread");
        return -1;
    }

    s_audio_running = true;
    return 0;
}

void gui_audio_stop(gui_app_t *app)
{
    (void)app;
    if (!s_audio_running) return;

    atomic_store(&s_audio_stop, true);
    thrd_join(s_audio_thread, NULL);
    s_audio_running = false;
    
    // Cleanup audio stream and resampler - no goat. Honestly.
    if (s_play_stream_inited) {
        StopAudioStream(s_play_stream);
        UnloadAudioStream(s_play_stream);
        s_play_stream_inited = false;
    }
    if (s_resampler) {
        soxr_delete(s_resampler);
        s_resampler = NULL;
    }
}

void gui_audio_set_playback_enabled(gui_app_t *app, bool enabled)
{
    if (!app) return;
    
    // Stop and cleanup existing stream when disabling
    if (!enabled && s_play_stream_inited) {
        StopAudioStream(s_play_stream);
        UnloadAudioStream(s_play_stream);
        s_play_stream_inited = false;
    }
    
    app->settings.audio_monitor_playback = enabled;
    atomic_store(&s_playback_enabled, enabled);
    gui_settings_save(&app->settings);

    // Reset playback queue and resampler state when toggling
    if (s_play_q) {
        atomic_store(&s_play_q_head, 0);
        atomic_store(&s_play_q_tail, 0);
    }
    if (s_resampler) {
        soxr_clear(s_resampler);
    }
}

void gui_audio_update_playback(gui_app_t *app)
{
    if (!app) return;
    if (!app->is_capturing) return;
    if (!app->settings.audio_monitor_playback) return;

    play_q_ensure_alloc();

    if (!s_audio_device_inited) {
        InitAudioDevice();
        s_audio_device_inited = true;
    }

    if (!s_play_stream_inited) {
        // Create resampler 78.125kHz -> 48kHz, stereo int16
        if (!s_resampler) {
            soxr_error_t err = NULL;
            soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
            soxr_quality_spec_t qual = soxr_quality_spec(SOXR_HQ, 0);
            s_resampler = soxr_create(78125.0, 48000.0, 2, &err, &io_spec, &qual, NULL);
            if (err) {
                fprintf(stderr, "[AUDIO] Failed to create resampler: %s\n", soxr_strerror(err));
                s_resampler = NULL;
            } else {
                fprintf(stderr, "[AUDIO] Created resampler 78.125kHz -> 48kHz\n");
            }
        }
        
        // Set buffer size BEFORE creating stream
        SetAudioStreamBufferSizeDefault(4096);
        
        // Stream at 48kHz (resampled audio)
        s_play_stream = LoadAudioStream(48000, 16, 2);
        PlayAudioStream(s_play_stream);
        s_play_stream_inited = true;
        fprintf(stderr, "[AUDIO] Initialized playback stream at 48kHz (buffer: 4096 frames)\n");
    }

    if (!IsAudioStreamPlaying(s_play_stream)) {
        PlayAudioStream(s_play_stream);
    }

    // Feed stream when it needs data (stream requests data in chunks)
    if (IsAudioStreamProcessed(s_play_stream)) {
        // Match buffer size to what we configured (4096 frames)
        const int frames_per_chunk = 4096;
        int16_t chunk[4096 * 2]; // stereo
        size_t samples_wanted = frames_per_chunk * 2;
        size_t got = play_q_pop_samples(chunk, samples_wanted);
        
        if (got >= samples_wanted) {
            UpdateAudioStream(s_play_stream, chunk, frames_per_chunk);
        } else if (got > 0) {
            // Partial data - pad with silence to avoid glitches
            memset(chunk + got, 0, (samples_wanted - got) * sizeof(int16_t));
            UpdateAudioStream(s_play_stream, chunk, frames_per_chunk);
        } else {
            // Complete underrun: feed silence
            memset(chunk, 0, sizeof(chunk));
            UpdateAudioStream(s_play_stream, chunk, frames_per_chunk);
        }
    }
}
