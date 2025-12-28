#include "gui_audio.h"

#include "../../common/extract.h"
#include "../../common/wave.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

#define BUFFER_AUDIO_READ_SIZE  (65536 * 3)

typedef struct {
    ringbuffer_t *rb;
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

static inline int32_t signext24(uint32_t v)
{
    v &= 0x00FFFFFFu;
    if (v & 0x00800000u) v |= 0xFF000000u;
    return (int32_t)v;
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

    while (1) {
        while (((buf = rb_read_ptr(a->rb, len)) == NULL) && !atomic_load(&do_exit) && !atomic_load(&s_audio_stop)) {
            thrd_sleep_ms(10);
        }

        if (atomic_load(&do_exit) || atomic_load(&s_audio_stop) || !a->app || !a->app->is_capturing) {
            len = a->rb->tail - a->rb->head;
            if (len == 0) break;
            buf = rb_read_ptr(a->rb, len);
            if (!buf) break;
        }

        // Update monitoring peaks
        audio_update_peaks(a->app, (const uint8_t *)buf, len);

        // Write files
        if (a->f_4ch) fwrite(buf, 1, len, a->f_4ch);
        if (a->convert_1ch) extract_audio_1ch_C((uint8_t *)buf, len, a->buffer_1ch[0], a->buffer_1ch[1], a->buffer_1ch[2], a->buffer_1ch[3]);
        if (a->convert_2ch) extract_audio_2ch_C((uint16_t *)buf, len, (uint16_t *)a->buffer_2ch[0], (uint16_t *)a->buffer_2ch[1]);

        rb_read_finished(a->rb, len);

        for (int i = 0; i < 2; i++) if (a->f_2ch[i]) fwrite(a->buffer_2ch[i], 1, len / 2, a->f_2ch[i]);
        for (int i = 0; i < 4; i++) if (a->f_1ch[i]) fwrite(a->buffer_1ch[i], 1, len / 4, a->f_1ch[i]);

        a->total_bytes += len;
    }

    // Rewrite headers with correct sizes
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

int gui_audio_start(gui_app_t *app, ringbuffer_t *audio_rb)
{
    if (!app || !audio_rb) return -1;
    if (s_audio_running) return 0;

    // Decide if any audio outputs are enabled
    bool want_4ch = app->settings.enable_audio_4ch;
    bool want_2ch_12 = app->settings.enable_audio_2ch_12;
    bool want_2ch_34 = app->settings.enable_audio_2ch_34;
    bool want_1ch = false;
    for (int i = 0; i < 4; i++) if (app->settings.enable_audio_1ch[i]) want_1ch = true;

    if (!want_4ch && !want_2ch_12 && !want_2ch_34 && !want_1ch) {
        return 0; // no-op
    }

    memset(&s_audio_ctx, 0, sizeof(s_audio_ctx));
    s_audio_ctx.rb = audio_rb;
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
        if (app->settings.enable_audio_1ch[i]) {
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
}
