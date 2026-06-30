#include "gui_cxadc.h"

#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>

#ifndef LIBASOUND_ENABLED
#define LIBASOUND_ENABLED 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#if LIBASOUND_ENABLED
#include <alsa/asoundlib.h>
#endif
#endif

extern volatile atomic_int do_exit;

#define CXADC_MAX_CARDS 2
#define CXADC_SAMPLE_RATE_HZ 40000000U
#define CXADC_READ_CHUNK_BYTES 65536
#define CXADC_AUDIO_SAMPLE_RATE_HZ 46875U
#define CXADC_AUDIO_CHANNEL_COUNT 3
#define CXADC_AUDIO_READ_FRAMES 1024
#define CXADC_AUDIO_PACKED_FRAME_BYTES 12

typedef enum {
    CXADC_AUDIO_FMT_NONE = 0,
    CXADC_AUDIO_FMT_S24_3LE,
    CXADC_AUDIO_FMT_S32_LE,
    CXADC_AUDIO_FMT_S16_LE
} cxadc_audio_format_t;

typedef struct {
    gui_app_t *app;
    atomic_bool running;
    thrd_t rf_thread;
    bool rf_thread_started;
    thrd_t audio_thread;
    bool audio_thread_started;
    int card_count;
#if defined(_WIN32)
    HANDLE card_handles[CXADC_MAX_CARDS];
#else
    int card_fds[CXADC_MAX_CARDS];
#if LIBASOUND_ENABLED
    snd_pcm_t *audio_pcm;
    cxadc_audio_format_t audio_format;
    size_t audio_sample_bytes;
    uint32_t audio_sample_rate_hz;
    char audio_device_name[64];
#endif
#endif
} cxadc_ctx_t;

static cxadc_ctx_t s_cxadc = {0};

static inline uint32_t cxadc_encode_raw_sample(int16_t sample_a, int16_t sample_b)
{
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    return ch_a | (ch_b << 20);
}

static void cxadc_reset_stats(gui_app_t *app)
{
    if (!app) return;
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, CXADC_SAMPLE_RATE_HZ);
    atomic_store(&app->audio_sample_rate, CXADC_AUDIO_SAMPLE_RATE_HZ);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    for (int i = 0; i < 4; i++) {
        atomic_store(&app->audio_peak[i], 0);
    }

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;
}

#if defined(_WIN32)
static HANDLE cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\cxadc%d", card_idx);
    return CreateFileA(path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

static int cxadc_read_card(HANDLE handle, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    DWORD read_bytes = 0;
    BOOL ok = ReadFile(handle, buf, (DWORD)len, &read_bytes, NULL);
    if (!ok) {
        return -1;
    }
    return (int)read_bytes;
}

static void cxadc_close_handle(HANDLE *handle)
{
    if (!handle) return;
    if (*handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}
#else
static int cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/cxadc%d", card_idx);
    return open(path, O_RDONLY);
}

static int cxadc_read_card(int fd, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || fd < 0) {
        return -1;
    }

    ssize_t r = read(fd, buf, len);
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    return (int)r;
}

static void cxadc_close_fd(int *fd)
{
    if (!fd) return;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
#endif

static void cxadc_close_cards(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        cxadc_close_handle(&ctx->card_handles[i]);
#else
        cxadc_close_fd(&ctx->card_fds[i]);
#endif
    }
}

static int cxadc_open_cards(cxadc_ctx_t *ctx, int card_count)
{
    if (!ctx) return -1;
    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    for (int i = 0; i < card_count; i++) {
#if defined(_WIN32)
        ctx->card_handles[i] = cxadc_open_card(i);
        if (ctx->card_handles[i] == INVALID_HANDLE_VALUE) {
            cxadc_close_cards(ctx);
            return -1;
        }
#else
        ctx->card_fds[i] = cxadc_open_card(i);
        if (ctx->card_fds[i] < 0) {
            cxadc_close_cards(ctx);
            return -1;
        }
#endif
    }
    return 0;
}

int gui_cxadc_detect_cards(void)
{
    int count = 0;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        HANDLE h = cxadc_open_card(i);
        if (h != INVALID_HANDLE_VALUE) {
            count++;
            CloseHandle(h);
        }
#else
        char path[32];
        snprintf(path, sizeof(path), "/dev/cxadc%d", i);
        if (access(path, R_OK) == 0 || access(path, W_OK) == 0) {
            count++;
        }
#endif
    }
    return count;
}

static inline void cxadc_store_s24le(uint8_t *dst, int32_t sample)
{
    if (sample > 8388607) sample = 8388607;
    if (sample < -8388608) sample = -8388608;
    uint32_t raw = (uint32_t)(sample & 0x00FFFFFF);
    dst[0] = (uint8_t)(raw & 0xFF);
    dst[1] = (uint8_t)((raw >> 8) & 0xFF);
    dst[2] = (uint8_t)((raw >> 16) & 0xFF);
}

#if !defined(_WIN32) && LIBASOUND_ENABLED
typedef struct {
    snd_pcm_format_t alsa_format;
    cxadc_audio_format_t cxadc_format;
    size_t sample_bytes;
} cxadc_audio_format_desc_t;

static int cxadc_configure_audio_pcm(snd_pcm_t *pcm,
                                     snd_pcm_format_t format,
                                     unsigned int *out_rate_hz)
{
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_alloca(&params);

    if (snd_pcm_hw_params_any(pcm, params) < 0) return -1;
    if (snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return -1;
    if (snd_pcm_hw_params_set_format(pcm, params, format) < 0) return -1;
    if (snd_pcm_hw_params_set_channels(pcm, params, CXADC_AUDIO_CHANNEL_COUNT) < 0) return -1;

    unsigned int rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
    int dir = 0;
    if (snd_pcm_hw_params_set_rate_near(pcm, params, &rate_hz, &dir) < 0) return -1;
    if (rate_hz < 46000 || rate_hz > 48000) return -1;

    snd_pcm_uframes_t period_frames = CXADC_AUDIO_READ_FRAMES;
    snd_pcm_uframes_t buffer_frames = CXADC_AUDIO_READ_FRAMES * 8;
    (void)snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, &dir);
    (void)snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames);

    if (snd_pcm_hw_params(pcm, params) < 0) return -1;
    if (snd_pcm_prepare(pcm) < 0) return -1;
    if (snd_pcm_nonblock(pcm, 1) < 0) return -1;

    if (out_rate_hz) {
        *out_rate_hz = rate_hz;
    }
    return 0;
}

static int cxadc_try_open_audio_device(cxadc_ctx_t *ctx, const char *device_name)
{
    if (!ctx || !device_name || !device_name[0]) return -1;

    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        return -1;
    }

    static const cxadc_audio_format_desc_t formats[] = {
        { SND_PCM_FORMAT_S24_3LE, CXADC_AUDIO_FMT_S24_3LE, 3 },
        { SND_PCM_FORMAT_S32_LE,  CXADC_AUDIO_FMT_S32_LE,  4 },
        { SND_PCM_FORMAT_S16_LE,  CXADC_AUDIO_FMT_S16_LE,  2 },
    };

    for (size_t i = 0; i < (sizeof(formats) / sizeof(formats[0])); i++) {
        unsigned int configured_rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
        if (cxadc_configure_audio_pcm(pcm, formats[i].alsa_format, &configured_rate_hz) == 0) {
            ctx->audio_pcm = pcm;
            ctx->audio_format = formats[i].cxadc_format;
            ctx->audio_sample_bytes = formats[i].sample_bytes;
            ctx->audio_sample_rate_hz = configured_rate_hz;
            snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name), "%s", device_name);
            return 0;
        }
    }

    snd_pcm_close(pcm);
    return -1;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    const char *env_device = getenv("MISRC_CXADC_ALSA_DEVICE");
    const char *candidates[] = {
        env_device,
        "hw:CARD=CXADCADCClockGe",
        "hw:CARD=CXADCADCClockGen",
        "plughw:CARD=CXADCADCClockGe",
        "plughw:CARD=CXADCADCClockGen",
        "default",
        NULL
    };

    for (size_t i = 0; candidates[i]; i++) {
        if (cxadc_try_open_audio_device(ctx, candidates[i]) == 0) {
            return 0;
        }
    }
    return -1;
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
    snd_pcm_close(ctx->audio_pcm);
    ctx->audio_pcm = NULL;
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_device_name[0] = '\0';
}
#else
static int cxadc_open_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; return -1; }
static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
static void cxadc_close_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
#endif

static int cxadc_audio_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;
    gui_app_t *app = ctx->app;

#if defined(_WIN32) || !LIBASOUND_ENABLED
    (void)app;
    return 0;
#else
    if (!ctx->audio_pcm || ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);

    size_t input_frame_bytes = (size_t)CXADC_AUDIO_CHANNEL_COUNT * ctx->audio_sample_bytes;
    size_t input_buffer_bytes = (size_t)CXADC_AUDIO_READ_FRAMES * input_frame_bytes;
    uint8_t *input_buf = (uint8_t *)malloc(input_buffer_bytes);
    if (!input_buf) {
        gui_app_set_status(app, "CXADC: failed to allocate ALSA audio buffer");
        return -1;
    }

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        snd_pcm_sframes_t frames = snd_pcm_readi(ctx->audio_pcm, input_buf, CXADC_AUDIO_READ_FRAMES);
        if (frames == -EAGAIN || frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }
        if (frames == -EPIPE) {
            (void)snd_pcm_prepare(ctx->audio_pcm);
            continue;
        }
        if (frames < 0) {
            int rec = snd_pcm_recover(ctx->audio_pcm, (int)frames, 1);
            if (rec >= 0) {
                continue;
            }
            gui_app_set_status(app, "CXADC ALSA audio read error");
            break;
        }

        size_t output_bytes = (size_t)frames * CXADC_AUDIO_PACKED_FRAME_BYTES;
        uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes, NULL);
        if (!out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            const uint8_t *src_frame = input_buf + ((size_t)i * input_frame_bytes);
            uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
            for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                uint8_t *dst = dst_frame + ((size_t)ch * 3);
                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                    int32_t sample32 = (int32_t)((uint32_t)src[0] |
                                                 ((uint32_t)src[1] << 8) |
                                                 ((uint32_t)src[2] << 16) |
                                                 ((uint32_t)src[3] << 24));
                    cxadc_store_s24le(dst, sample32 >> 8);
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                    int16_t sample16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                    cxadc_store_s24le(dst, ((int32_t)sample16) << 8);
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                }
            }
            dst_frame[9] = 0;
            dst_frame[10] = 0;
            dst_frame[11] = 0;
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
        atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
    }

    free(input_buf);
    return 0;
#endif
}

static int cxadc_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;

    gui_app_t *app = ctx->app;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *card_buf_a = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    uint8_t *card_buf_b = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    if (!card_buf_a || !card_buf_b) {
        free(card_buf_a);
        free(card_buf_b);
        gui_app_set_status(app, "CXADC: failed to allocate capture buffers");
        return -1;
    }

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, CXADC_SAMPLE_RATE_HZ);

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
#if defined(_WIN32)
        int read_a = cxadc_read_card(ctx->card_handles[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#else
        int read_a = cxadc_read_card(ctx->card_fds[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#endif
        if (read_a < 0) {
            gui_app_set_status(app, "CXADC read error on card 0");
            break;
        }
        if (read_a == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        int output_samples = read_a;
        if (ctx->card_count > 1) {
#if defined(_WIN32)
            int read_b = cxadc_read_card(ctx->card_handles[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#else
            int read_b = cxadc_read_card(ctx->card_fds[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#endif
            if (read_b < 0) {
                gui_app_set_status(app, "CXADC read error on card 1");
                break;
            }
            if (read_b == 0) {
                thrd_sleep_ms(1);
                continue;
            }
            if (read_b < output_samples) {
                output_samples = read_b;
            }
        }

        if (output_samples <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

        size_t output_bytes = (size_t)output_samples * sizeof(uint32_t);
        uint32_t *raw_out = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, output_bytes, NULL);
        if (!raw_out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (int i = 0; i < output_samples; i++) {
            int16_t sample_a = (int16_t)(((int)card_buf_a[i] - 128) << 4);
            int16_t sample_b = 0;
            if (ctx->card_count > 1) {
                sample_b = (int16_t)(((int)card_buf_b[i] - 128) << 4);
            }
            raw_out[i] = cxadc_encode_raw_sample(sample_a, sample_b);
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

        atomic_store(&app->last_callback_time_ms, get_time_ms());
        atomic_fetch_add(&app->frame_count, 1);
    }

    free(card_buf_a);
    free(card_buf_b);
    return 0;
}

int gui_cxadc_start(gui_app_t *app, int card_count)
{
    if (!app) return -1;
    if (atomic_load(&s_cxadc.running)) return 0;

    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    memset(&s_cxadc, 0, sizeof(s_cxadc));
    s_cxadc.app = app;
    s_cxadc.card_count = card_count;
#if defined(_WIN32)
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_handles[i] = INVALID_HANDLE_VALUE;
    }
#else
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_fds[i] = -1;
    }
#if LIBASOUND_ENABLED
    s_cxadc.audio_pcm = NULL;
    s_cxadc.audio_format = CXADC_AUDIO_FMT_NONE;
    s_cxadc.audio_sample_bytes = 0;
    s_cxadc.audio_sample_rate_hz = 0;
    s_cxadc.audio_device_name[0] = '\0';
#endif
#endif

    if (cxadc_open_cards(&s_cxadc, card_count) != 0) {
        gui_app_set_status(app, "CXADC: failed to open card device(s)");
        return -1;
    }

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        gui_app_set_status(app, "CXADC: failed to initialize RF buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        gui_app_set_status(app, "CXADC: failed to initialize audio buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    cxadc_reset_stats(app);

    bool audio_capture_available = false;
    if (cxadc_open_audio_capture(&s_cxadc) == 0) {
        audio_capture_available = true;
        if (s_cxadc.audio_sample_rate_hz > 0) {
            atomic_store(&app->audio_sample_rate, s_cxadc.audio_sample_rate_hz);
        }
#if !defined(_WIN32) && LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] ALSA audio capture device: %s (%u Hz)\n",
                s_cxadc.audio_device_name,
                s_cxadc.audio_sample_rate_hz);
#endif
    } else {
#if !defined(_WIN32) && LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] ALSA clockgen audio device not available; continuing RF-only\n");
#endif
    }

    app->is_capturing = true;
    int r = gui_extract_start(app);
    if (r < 0) {
        gui_app_set_status(app, "CXADC: failed to start extraction thread");
        app->is_capturing = false;
        cxadc_close_audio_capture(&s_cxadc);
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[CXADC] Failed to start display thread (non-fatal)\n");
        }
    }

    (void)gui_audio_start(app, &app->buffers);

    atomic_store(&s_cxadc.running, true);
    if (thrd_create_with_priority(&s_cxadc.rf_thread,
                                  cxadc_capture_thread,
                                  &s_cxadc,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        gui_app_set_status(app, "CXADC: failed to start RF capture thread");
        atomic_store(&s_cxadc.running, false);
        gui_audio_stop(app);
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        gui_extract_stop();
        app->is_capturing = false;
        cxadc_close_audio_capture(&s_cxadc);
        cxadc_close_cards(&s_cxadc);
        return -1;
    }
    s_cxadc.rf_thread_started = true;

    if (audio_capture_available) {
        if (thrd_create_with_priority(&s_cxadc.audio_thread,
                                      cxadc_audio_capture_thread,
                                      &s_cxadc,
                                      THRD_PRIORITY_ABOVE) != thrd_success) {
#if !defined(_WIN32) && LIBASOUND_ENABLED
            fprintf(stderr, "[CXADC] Failed to start ALSA audio capture thread; continuing RF-only\n");
#endif
            cxadc_close_audio_capture(&s_cxadc);
        } else {
            s_cxadc.audio_thread_started = true;
        }
    }

    gui_app_set_status(app, (card_count > 1) ? "CXADC Clockgen capture running" : "CXADC capture running");
    return 0;
}

void gui_cxadc_stop(gui_app_t *app)
{
    if (!atomic_load(&s_cxadc.running) && !s_cxadc.rf_thread_started && !s_cxadc.audio_thread_started) {
        return;
    }

    if (!app) {
        app = s_cxadc.app;
    }

    if (app) {
        app->is_capturing = false;
    }
    atomic_store(&s_cxadc.running, false);
    cxadc_abort_audio_capture(&s_cxadc);

    if (s_cxadc.rf_thread_started) {
        thrd_join(s_cxadc.rf_thread, NULL);
        s_cxadc.rf_thread_started = false;
    }
    if (s_cxadc.audio_thread_started) {
        thrd_join(s_cxadc.audio_thread, NULL);
        s_cxadc.audio_thread_started = false;
    }

    if (app && app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }
    gui_audio_stop(app);
    gui_extract_stop();
    cxadc_close_audio_capture(&s_cxadc);

    cxadc_close_cards(&s_cxadc);

    if (app) {
        atomic_store(&app->stream_synced, false);
        atomic_store(&app->dropout_stop_requested, false);
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
        gui_app_set_status(app, "CXADC capture stopped");
    }

    memset(&s_cxadc, 0, sizeof(s_cxadc));
}

bool gui_cxadc_is_running(void)
{
    return atomic_load(&s_cxadc.running);
}
