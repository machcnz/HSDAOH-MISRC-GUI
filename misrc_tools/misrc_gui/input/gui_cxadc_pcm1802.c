/*
 * MISRC GUI - CX2388x Clockgen PCM1802 Audio Capture
 *
 * Captures L+R audio from the clockgen's PCM1802 ADC, which appears as a
 * standard USB audio device (VID 0x1209 PID 0x0002 on Windows,
 * hw:CARD=CXADCADCClockGe on Linux - confirmed from cxadc-win audio.h).
 *
 * The device delivers 3 interleaved channels per frame (L, R, HSW head-switch
 * signal) at 24-bit depth - confirmed from cxadc-win LocalCapture.ps1:
 * "Without conversion the output file is 3-channel s24le."
 * Only L+R are written to BUF_CAPTURE_AUDIO (channels 1-2 of the existing
 * 4-channel zero-padded format). The 3rd channel (HSW) is discarded, per
 * accepted design ("we only use the audio L and R").
 *
 * Output format matches existing HSDAOH upstream audio convention:
 * - 6 bytes per frame (2ch × 24-bit L+R) copied into first 6 bytes
 * - 12 bytes per padded frame written to BUF_CAPTURE_AUDIO (channels 3-4 = 0)
 * - Same zero-pad pattern as gui_capture.c stream_id==2 handler (confirmed)
 *
 * Adapted from cxadc-win capture-server audio_wasapi.c / audio_alsa.c
 * (GPL-2.0-or-later, compatible with this project's GPL-3.0 license)
 */

#include "gui_cxadc.h"

#ifdef ENABLE_CXADC

#include "../core/gui_app.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// Platform-specific audio state
//-----------------------------------------------------------------------------

#ifdef _WIN32

#define COBJMACROS
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <devicetopology.h>

#pragma comment(lib, "avrt")
#pragma comment(lib, "ksuser")

#define REFTIMES_PER_SEC    10000000
#define PCM1802_DEVICE_NAME L"usb#vid_1209&pid_0002"
#define PCM1802_OLD_NAME    L"usb#vid_1209&pid_0001"

/* GUIDs required for COM audio API - from audio_wasapi.c reference */
static const CLSID s_IID_MMDeviceEnumerator  = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const CLSID s_IID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const CLSID s_IID_IDeviceTopology     = { 0x2A07407E, 0x6497, 0x4A18, { 0x97, 0x87, 0x32, 0xF7, 0x9B, 0xD0, 0xD9, 0x8F } };
static const CLSID s_IID_IAudioClient        = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const CLSID s_IID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48a0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const PROPERTYKEY s_PKEY_AudioEngine_DeviceFormat = { { 0xF19F064D, 0x082C, 0x4E27, { 0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C } }, 0 };

typedef struct {
    IMMDevice           *device;
    IAudioClient2       *client;
    IAudioCaptureClient *capture_client;
    HANDLE               thread_task;
    HANDLE               event_handle;
    LPWSTR               endpoint_id;
    WAVEFORMATEXTENSIBLE format;
    unsigned             frame_count;
    bool                 com_initialized;
} pcm1802_win_state_t;

#else /* Linux */

#include <alsa/asoundlib.h>
#define PCM1802_DEVICE_NAME "hw:CARD=CXADCADCClockGe"

typedef struct {
    snd_pcm_t *handle;
    unsigned    rate;
    unsigned    channels;
} pcm1802_linux_state_t;

#endif

//-----------------------------------------------------------------------------
// Internal shared state
//-----------------------------------------------------------------------------

typedef struct {
    gui_app_t *app;
#ifdef _WIN32
    pcm1802_win_state_t win;
#else
    pcm1802_linux_state_t lin;
#endif
    uint8_t *read_buf;      // raw device frames (3ch × 24-bit = 9 bytes each)
    uint8_t *lr_buf;        // L+R only (6 bytes each), discarding HSW channel
    size_t   read_buf_frames;
} pcm1802_ctx_t;

static pcm1802_ctx_t s_pcm1802_ctx;

//-----------------------------------------------------------------------------
// Windows implementation
//-----------------------------------------------------------------------------

#ifdef _WIN32

static bool pcm1802_win_is_device_match(IMMDevice *device, PCWSTR id_str) {
    /* Direct port of audio_is_device_match from audio_wasapi.c */
    HRESULT hr;
    IDeviceTopology *topology = NULL;
    bool is_match = false;

    if (FAILED(IMMDevice_Activate(device, &s_IID_IDeviceTopology,
                                   CLSCTX_ALL, NULL, (void **)&topology)))
        return false;

    IConnector *connector = NULL;
    if (FAILED(IDeviceTopology_GetConnector(topology, 0, &connector)))
        goto done_topo;

    LPWSTR device_id = NULL;
    if (SUCCEEDED(IConnector_GetDeviceIdConnectedTo(connector, &device_id))) {
        is_match = wcsstr(device_id, id_str) != NULL;
        CoTaskMemFree(device_id);
    }
    IConnector_Release(connector);

done_topo:
    IDeviceTopology_Release(topology);
    return is_match;
}

static int pcm1802_win_find_device(IMMDevice **out_device) {
    /* Adapted from audio_find_device in audio_wasapi.c */
    HRESULT hr;
    *out_device = NULL;

    IMMDeviceEnumerator *enumerator = NULL;
    if (FAILED(hr = CoCreateInstance(&s_IID_MMDeviceEnumerator, NULL,
                                      CLSCTX_ALL, &s_IID_IMMDeviceEnumerator,
                                      (void **)&enumerator))) {
        fprintf(stderr, "[PCM1802] CoCreateInstance failed: %ld\n", hr);
        return -1;
    }

    IMMDeviceCollection *collection = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eCapture,
                                                        DEVICE_STATE_ACTIVE,
                                                        &collection))) {
        IMMDeviceEnumerator_Release(enumerator);
        return -1;
    }

    UINT count = 0;
    IMMDeviceCollection_GetCount(collection, &count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice *dev = NULL;
        if (FAILED(IMMDeviceCollection_Item(collection, i, &dev))) continue;

        if (pcm1802_win_is_device_match(dev, PCM1802_DEVICE_NAME) ||
            pcm1802_win_is_device_match(dev, PCM1802_OLD_NAME)) {
            *out_device = dev;
            break;
        }
        IMMDevice_Release(dev);
    }

    IMMDeviceCollection_Release(collection);
    IMMDeviceEnumerator_Release(enumerator);
    return (*out_device != NULL) ? 0 : -1;
}

static int pcm1802_win_open(pcm1802_ctx_t *ctx) {
    pcm1802_win_state_t *s = &ctx->win;

    if (FAILED(CoInitializeEx(NULL, COINIT_DISABLE_OLE1DDE))) {
        fprintf(stderr, "[PCM1802] CoInitializeEx failed\n");
        return -1;
    }
    s->com_initialized = true;

    IMMDevice *device = NULL;
    if (pcm1802_win_find_device(&device) != 0) {
        fprintf(stderr, "[PCM1802] Clockgen audio device not found\n");
        return -1;
    }

    /* Get endpoint ID for later reopen (capture init needs it) */
    if (FAILED(IMMDevice_GetId(device, &s->endpoint_id))) {
        IMMDevice_Release(device);
        return -1;
    }

    /* Get device format (rate/channels reported by device itself) */
    IAudioClient *tmp_client = NULL;
    IMMDevice_Activate(device, &s_IID_IAudioClient, CLSCTX_ALL, 0,
                        (void **)&tmp_client);

    IPropertyStore *prop_store = NULL;
    IMMDevice_OpenPropertyStore(device, STGM_READ, &prop_store);

    PROPVARIANT var;
    PropVariantInit(&var);
    IPropertyStore_GetValue(prop_store, &s_PKEY_AudioEngine_DeviceFormat, &var);

    PWAVEFORMATEXTENSIBLE fmt = (PWAVEFORMATEXTENSIBLE)var.blob.pBlobData;
    s->format = *fmt;
    PropVariantClear(&var);
    IPropertyStore_Release(prop_store);
    IAudioClient_Release(tmp_client);
    IMMDevice_Release(device);

    /* Log the format Windows reported for this device */
    fprintf(stderr, "[PCM1802] Device format: %uch, %luHz, %u-bit, blockAlign=%u\n",
            s->format.Format.nChannels,
            s->format.Format.nSamplesPerSec,
            s->format.Format.wBitsPerSample,
            s->format.Format.nBlockAlign);

    /* Set audio sample rate so the audio monitor resampler uses the correct ratio */
    atomic_store(&ctx->app->audio_sample_rate, s->format.Format.nSamplesPerSec);

    /* Calculate read buffer size based on actual device format */
    ctx->read_buf_frames = s->format.Format.nSamplesPerSec / 10; /* 100ms */
    ctx->read_buf = malloc(ctx->read_buf_frames * s->format.Format.nBlockAlign);
    ctx->lr_buf   = malloc(ctx->read_buf_frames * 6); /* L+R only, 24-bit */
    if (!ctx->read_buf || !ctx->lr_buf) return -1;

    return 0;
}

static int pcm1802_win_start(pcm1802_ctx_t *ctx) {
    pcm1802_win_state_t *s = &ctx->win;
    HRESULT hr;

    /* Get device from stored endpoint ID */
    IMMDeviceEnumerator *enumerator = NULL;
    CoCreateInstance(&s_IID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                      &s_IID_IMMDeviceEnumerator, (void **)&enumerator);
    IMMDeviceEnumerator_GetDevice(enumerator, s->endpoint_id, &s->device);
    IMMDeviceEnumerator_Release(enumerator);

    DWORD taskIndex = 0;
    s->thread_task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    IMMDevice_Activate(s->device, &s_IID_IAudioClient, CLSCTX_ALL, 0,
                        (void **)&s->client);

    s->event_handle = CreateEventW(NULL, FALSE, FALSE, NULL);

    AudioClientProperties props = {
        .cbSize = sizeof(AudioClientProperties),
        .bIsOffload = FALSE,
        .eCategory = AudioCategory_Communications,
        .Options = AUDCLNT_STREAMOPTIONS_RAW
    };
    hr = IAudioClient2_SetClientProperties(s->client, &props);
    if (FAILED(hr)) {
        fprintf(stderr, "[PCM1802] SetClientProperties(RAW) failed: 0x%lx (non-fatal, continuing without RAW)\n", hr);
    }

    REFERENCE_TIME default_period = 0, min_period = 0;
    IAudioClient2_GetDevicePeriod(s->client, &default_period, &min_period);

    REFERENCE_TIME duration = default_period;
retry:
    hr = IAudioClient2_Initialize(s->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   duration, duration,
                                   (WAVEFORMATEX *)&s->format, 0);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        unsigned fc = 0;
        IAudioClient2_GetBufferSize(s->client, &fc);
        duration = (REFERENCE_TIME)(
            (REFTIMES_PER_SEC / s->format.Format.nSamplesPerSec * fc) + 0.5);
        goto retry;
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[PCM1802] IAudioClient2::Initialize failed: %ld\n", hr);
        return -1;
    }

    IAudioClient2_GetBufferSize(s->client, &s->frame_count);
    IAudioClient2_SetEventHandle(s->client, s->event_handle);
    IAudioClient2_GetService(s->client, &s_IID_IAudioCaptureClient,
                              (void **)&s->capture_client);
    IAudioClient2_Start(s->client);

    // Discard first buffer - not always full (reference: baseband_writer_thread)
    if (WaitForSingleObject(s->event_handle, 2000) == WAIT_OBJECT_0) {
        BYTE *discard_buf = NULL;
        DWORD discard_flags = 0;
        UINT32 discard_frames = 0;
        if (SUCCEEDED(IAudioCaptureClient_GetBuffer(s->capture_client,
                        &discard_buf, &discard_frames, &discard_flags, NULL, NULL))) {
            IAudioCaptureClient_ReleaseBuffer(s->capture_client, discard_frames);
        }
    }

    return 0;
}

static void pcm1802_win_stop(pcm1802_ctx_t *ctx) {
    pcm1802_win_state_t *s = &ctx->win;
    if (s->client)          IAudioClient2_Stop(s->client);
    if (s->capture_client)  { IAudioCaptureClient_Release(s->capture_client); s->capture_client = NULL; }
    if (s->client)          { IAudioClient2_Release(s->client); s->client = NULL; }
    if (s->device)          { IMMDevice_Release(s->device); s->device = NULL; }
    if (s->event_handle)    { CloseHandle(s->event_handle); s->event_handle = NULL; }
    if (s->thread_task)     { AvRevertMmThreadCharacteristics(s->thread_task); s->thread_task = NULL; }
}

static void pcm1802_win_close(pcm1802_ctx_t *ctx) {
    pcm1802_win_state_t *s = &ctx->win;
    if (s->endpoint_id) { CoTaskMemFree(s->endpoint_id); s->endpoint_id = NULL; }
    if (s->com_initialized) { CoUninitialize(); s->com_initialized = false; }
}

static ssize_t pcm1802_win_read_frames(pcm1802_ctx_t *ctx) {
    pcm1802_win_state_t *s = &ctx->win;
    if (WaitForSingleObject(s->event_handle, 2000) != WAIT_OBJECT_0) return -1;

    BYTE *buf = NULL;
    DWORD flags = 0;
    UINT32 frame_count = 0;
    if (FAILED(IAudioCaptureClient_GetBuffer(s->capture_client, &buf,
                                              &frame_count, &flags, NULL, NULL)))
        return -1;

    // Match reference: on silent, write nothing (return 0). Always release.
    size_t bytes = 0;
    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && buf) {
        bytes = s->format.Format.nBlockAlign * frame_count;
        memcpy(ctx->read_buf, buf, bytes);
    }

    IAudioCaptureClient_ReleaseBuffer(s->capture_client, frame_count);
    return (bytes > 0) ? (ssize_t)frame_count : 0;
}

#else /* Linux */

static int pcm1802_linux_open(pcm1802_ctx_t *ctx) {
    /* Adapted from audio_alsa.c audio_device_init */
    pcm1802_linux_state_t *s = &ctx->lin;
    int ret;

    static const int MODE = SND_PCM_NONBLOCK
                           | SND_PCM_NO_AUTO_RESAMPLE
                           | SND_PCM_NO_AUTO_CHANNELS
                           | SND_PCM_NO_AUTO_FORMAT
                           | SND_PCM_NO_SOFTVOL;

    if ((ret = snd_pcm_open(&s->handle, PCM1802_DEVICE_NAME,
                             SND_PCM_STREAM_CAPTURE, MODE)) < 0) {
        fprintf(stderr, "[PCM1802] cannot open ALSA device: %s\n",
                snd_strerror(ret));
        return -1;
    }

    snd_pcm_hw_params_t *hw_params = NULL;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(s->handle, hw_params);
    snd_pcm_hw_params_set_access(s->handle, hw_params,
                                  SND_PCM_ACCESS_RW_INTERLEAVED);

    /* Query rate and channels from device - don't hardcode */
    snd_pcm_hw_params_get_rate(hw_params, &s->rate, 0);
    snd_pcm_hw_params_set_rate(s->handle, hw_params, s->rate, 0);

    snd_pcm_hw_params_get_channels(hw_params, &s->channels);
    snd_pcm_hw_params_set_channels(s->handle, hw_params, s->channels);

    /* 24-bit little-endian */
    snd_pcm_hw_params_set_format(s->handle, hw_params, SND_PCM_FORMAT_S24_LE);

    if ((ret = snd_pcm_hw_params(s->handle, hw_params)) < 0) {
        fprintf(stderr, "[PCM1802] cannot set hw parameters: %s\n",
                snd_strerror(ret));
        snd_pcm_close(s->handle);
        return -1;
    }

    snd_pcm_sw_params_t *sw_params = NULL;
    snd_pcm_sw_params_alloca(&sw_params);
    snd_pcm_sw_params_current(s->handle, sw_params);
    snd_pcm_sw_params_set_tstamp_mode(s->handle, sw_params, SND_PCM_TSTAMP_ENABLE);
    snd_pcm_sw_params_set_tstamp_type(s->handle, sw_params,
                                       SND_PCM_TSTAMP_TYPE_MONOTONIC_RAW);
    snd_pcm_sw_params(s->handle, sw_params);
    snd_pcm_prepare(s->handle);

    /* 100ms read buffer: 3ch × 24-bit (4 bytes per sample in ALSA S24_LE) */
    size_t bytes_per_frame = s->channels * 4;
    ctx->read_buf_frames = s->rate / 10;
    ctx->read_buf = malloc(ctx->read_buf_frames * bytes_per_frame);
    ctx->lr_buf   = malloc(ctx->read_buf_frames * 6);
    if (!ctx->read_buf || !ctx->lr_buf) {
        snd_pcm_close(s->handle);
        return -1;
    }

    return 0;
}

static ssize_t pcm1802_linux_read_frames(pcm1802_ctx_t *ctx) {
    pcm1802_linux_state_t *s = &ctx->lin;
    long count = snd_pcm_readi(s->handle, ctx->read_buf, ctx->read_buf_frames);
    if (count == -EAGAIN || count == 0) return 0;
    if (count < 0) {
        fprintf(stderr, "[PCM1802] snd_pcm_readi failed: %s\n",
                snd_strerror((int)count));
        return -1;
    }
    return (ssize_t)count;
}

#endif /* platform */

//-----------------------------------------------------------------------------
// Channel strip: 3ch interleaved → L+R only (discard HSW, channel 3)
//
// Device reports blockAlign which tells us the actual bytes per frame.
// Two known layouts:
//   blockAlign=9:  packed 24-bit (3 bytes/sample × 3ch = 9 bytes/frame)
//   blockAlign=12: 32-bit containers (4 bytes/sample × 3ch = 12 bytes/frame)
//
// Output: 2 × 3 = 6 bytes/frame (packed 24-bit L+R) for BUF_CAPTURE_AUDIO,
// matching the existing HSDAOH convention (gui_capture.c stream_id==2 path).
//-----------------------------------------------------------------------------
static void pcm1802_strip_to_lr(const uint8_t *src, uint8_t *dst,
                                  size_t frames, unsigned block_align) {
    unsigned bytes_per_sample = block_align / 3;  // 3 channels

    for (size_t i = 0; i < frames; i++) {
        const uint8_t *frame = src + i * block_align;
        // L = sample 0, R = sample 1, HSW = sample 2 (discarded)
        // Copy first 3 bytes of each sample (24-bit LE payload)
        dst[i*6 + 0] = frame[0];                        // L byte 0
        dst[i*6 + 1] = frame[1];                        // L byte 1
        dst[i*6 + 2] = frame[2];                        // L byte 2
        dst[i*6 + 3] = frame[bytes_per_sample + 0];     // R byte 0
        dst[i*6 + 4] = frame[bytes_per_sample + 1];     // R byte 1
        dst[i*6 + 5] = frame[bytes_per_sample + 2];     // R byte 2
    }
}

//-----------------------------------------------------------------------------
// Audio capture thread
//-----------------------------------------------------------------------------

static int pcm1802_audio_thread(void *arg) {
    gui_app_t *app = (gui_app_t *)arg;
    pcm1802_ctx_t *ctx = &s_pcm1802_ctx;

    thrd_set_priority(THRD_PRIORITY_HIGH);

    while (atomic_load(&app->cxadc_audio_running)) {
#ifdef _WIN32
        ssize_t frames = pcm1802_win_read_frames(ctx);
#else
        ssize_t frames = pcm1802_linux_read_frames(ctx);
#endif
        if (frames < 0) {
            fprintf(stderr, "[PCM1802] Read error, stopping audio\n");
            break;
        }
        if (frames == 0) {
            thrd_yield();
            continue;
        }

        /* Strip HSW channel, keep L+R only */
#ifdef _WIN32
        unsigned block_align = s_pcm1802_ctx.win.format.Format.nBlockAlign;
#else
        unsigned block_align = s_pcm1802_ctx.lin.channels * 4;  // ALSA S24_LE uses 4-byte containers
#endif
        pcm1802_strip_to_lr(ctx->read_buf, ctx->lr_buf, (size_t)frames, block_align);

        /* Write to BUF_CAPTURE_AUDIO using same zero-pad convention as
         * HSDAOH upstream audio (gui_capture.c stream_id==2 handler):
         * 6 bytes L+R copied into first 6 of each 12-byte padded frame */
        size_t padded_len = (size_t)frames * 12;
        uint8_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO,
                                           padded_len, NULL);
        if (!out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        memset(out, 0, padded_len);
        for (size_t i = 0; i < (size_t)frames; i++) {
            memcpy(out + i*12, ctx->lr_buf + i*6, 6);
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, padded_len);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
    }

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_cxadc_pcm1802_open(gui_app_t *app) {
    if (!app) return -1;
    memset(&s_pcm1802_ctx, 0, sizeof(s_pcm1802_ctx));
    s_pcm1802_ctx.app = app;

#ifdef _WIN32
    return pcm1802_win_open(&s_pcm1802_ctx);
#else
    return pcm1802_linux_open(&s_pcm1802_ctx);
#endif
}

void gui_cxadc_pcm1802_close(gui_app_t *app) {
    (void)app;
#ifdef _WIN32
    pcm1802_win_close(&s_pcm1802_ctx);
#endif
    free(s_pcm1802_ctx.read_buf);
    free(s_pcm1802_ctx.lr_buf);
    s_pcm1802_ctx.read_buf = NULL;
    s_pcm1802_ctx.lr_buf = NULL;
}

int gui_cxadc_pcm1802_start(gui_app_t *app) {
    if (!app) return -1;

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        fprintf(stderr, "[PCM1802] failed to initialize audio ringbuffer\n");
        return -1;
    }

#ifdef _WIN32
    if (pcm1802_win_start(&s_pcm1802_ctx) != 0) return -1;
#else
    if (snd_pcm_start(s_pcm1802_ctx.lin.handle) < 0) return -1;
#endif

    atomic_store(&app->cxadc_audio_running, true);

    thrd_t thread;
    if (thrd_create(&thread, pcm1802_audio_thread, app) != thrd_success) {
        fprintf(stderr, "[PCM1802] failed to create audio thread\n");
        atomic_store(&app->cxadc_audio_running, false);
        return -1;
    }
    app->cxadc_audio_thread = (void *)(uintptr_t)thread;
    return 0;
}

void gui_cxadc_pcm1802_stop(gui_app_t *app) {
    if (!app) return;
    if (!atomic_load(&app->cxadc_audio_running)) return;

    atomic_store(&app->cxadc_audio_running, false);

    if (app->cxadc_audio_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->cxadc_audio_thread;
        thrd_join(thread, NULL);
        app->cxadc_audio_thread = NULL;
    }

#ifdef _WIN32
    pcm1802_win_stop(&s_pcm1802_ctx);
#else
    snd_pcm_drop(s_pcm1802_ctx.lin.handle);
    snd_pcm_close(s_pcm1802_ctx.lin.handle);
    s_pcm1802_ctx.lin.handle = NULL;
#endif
}

#endif /* ENABLE_CXADC */
