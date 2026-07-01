/*
 * MISRC GUI - CX2388x (CXADC) Device Support
 *
 * See gui_cxadc.h for the full design rationale, confirmed driver facts,
 * and the overflow-detection distinction (driver-level vs ringbuffer-level).
 *
 * Conversion: CX hardware delivers 8-bit unsigned samples. The rest of
 * this GUI's pipeline (gui_extract.c / common/extract.c) only understands
 * the MISRC 32-bit packed wire format (12-bit A in bits 0-11, 12-bit B in
 * bits 20-31, 8-bit AUX in bits 12-19 - see common/extract.c MASK_1/MASK_2/
 * MASK_AUX). CX is single-channel, so B and AUX are written as zero.
 *
 * extract_A_C unpacks channel A as: outA[i] = 2047 - (int16_t)(in[i] & MASK_1)
 * i.e. it expects an UNSIGNED 12-bit value in bits 0-11, then inverts around
 * 2047. To present an 8-bit CX sample correctly in that same convention:
 *   1. CX byte is unsigned 0-255, midpoint 128.
 *   2. Scale to 12-bit range: shift left 4 (0-255 -> 0-4080, close to 12-bit
 *      full scale 0-4095) preserving the unsigned representation expected
 *      by extract_A_C.
 *   3. Pack into bits 0-11 of the 32-bit word, B and AUX bits left at 0.
 */

#include "gui_cxadc.h"

#ifdef ENABLE_CXADC

#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#include "cxadc-win/cx_ctl_codes.h"  // vendored from cxadc-win shared/cx_ctl_codes.h
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

// NOTE: cxadc-win/cx_ctl_codes.h is vendored verbatim from
// https://github.com/JuniorIsAJitterbug/cxadc-win/blob/master/src/shared/cx_ctl_codes.h
// Licensed GPL-2.0-or-later (per its own SPDX header); this project is
// GPL-3.0 (LICENSE.GPL) - confirmed compatible, "or-later" permits use
// under GPL-3.0 terms. It is plain Win32 macros (CTL_CODE/FILE_DEVICE_UNKNOWN
// from <winioctl.h>, already pulled in by <windows.h>) and is safe for
// user-mode use - confirmed from leveladj.c in that same repo, which
// includes it directly the same way. Still needs adding to
// meson.build/CMakeLists.txt include paths for the build to find it.

//-----------------------------------------------------------------------------
// Internal state
//-----------------------------------------------------------------------------

#define CXADC_READ_BUF_SAMPLES  CXADC_BUFFER_SIZE   // 1 byte per sample (8-bit)

typedef struct {
    gui_app_t *app;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
    uint8_t *read_buf;       // raw bytes from device
    uint32_t *pack_buf;      // converted MISRC-format words
} cxadc_capture_ctx_t;

static cxadc_capture_ctx_t s_ctx;

//-----------------------------------------------------------------------------
// Platform-specific device I/O
//-----------------------------------------------------------------------------

#ifdef _WIN32

static int cxadc_platform_open(const char *path, HANDLE *out_handle) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    *out_handle = h;
    return 0;
}

static void cxadc_platform_close(HANDLE h) {
    if (h != INVALID_HANDLE_VALUE && h != NULL) {
        CloseHandle(h);
    }
}

// Returns bytes read, 0 on EOF/no-data, -1 on error
static ssize_t cxadc_platform_read(HANDLE h, void *buf, size_t len) {
    DWORD bytes_read = 0;
    if (!ReadFile(h, buf, (DWORD)len, &bytes_read, NULL)) {
        return -1;
    }
    return (ssize_t)bytes_read;
}

// Enumerate \\.\cxadc0 .. \\.\cxadc(N-1) by attempting to open each
int gui_cxadc_enumerate(cxadc_device_info_t *devices, int max_devices) {
    int found = 0;
    for (int i = 0; i < CXADC_GUI_MAX_DEVICES && found < max_devices; i++) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\cxadc%d", i);

        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            devices[found].index = i;
            snprintf(devices[found].path, sizeof(devices[found].path), "%s", path);
            snprintf(devices[found].name, sizeof(devices[found].name), "CXADC %d", i);
            found++;
        }
    }
    return found;
}

#else // Linux

static int cxadc_platform_open(const char *path, int *out_fd) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    *out_fd = fd;
    return 0;
}

static void cxadc_platform_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

// Returns bytes read, 0 on EOF, -1 on error
static ssize_t cxadc_platform_read(int fd, void *buf, size_t len) {
    return read(fd, buf, len);
}

// Enumerate /dev/cxadc0 .. /dev/cxadc(N-1) by attempting to open each
int gui_cxadc_enumerate(cxadc_device_info_t *devices, int max_devices) {
    int found = 0;
    for (int i = 0; i < CXADC_GUI_MAX_DEVICES && found < max_devices; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/cxadc%d", i);

        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            devices[found].index = i;
            snprintf(devices[found].path, sizeof(devices[found].path), "%s", path);
            snprintf(devices[found].name, sizeof(devices[found].name), "CXADC %d", i);
            found++;
        }
    }
    return found;
}

#endif

//-----------------------------------------------------------------------------
// Sample conversion: 8-bit unsigned -> MISRC 32-bit packed word
//-----------------------------------------------------------------------------

static inline void cxadc_convert_block(const uint8_t *in, uint32_t *out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t b = in[i];

        // 8-bit unsigned (0-255) scaled into the 12-bit field (bits 0-11)
        // that extract_A_C expects.
        uint32_t scaled = ((uint32_t)b) << 4;  // 0-255 -> 0-4080
        uint32_t word = scaled & 0xFFFu;        // confirmed MASK_1 in extract.c

        // Software clip flag: CX has no hardware clip bit in the 8-bit
        // stream (unlike real MISRC ADC data, where bit 12 is a genuine
        // hardware-asserted flag - confirmed bit 12 sits in MASK_AUX, a
        // separate field from the 12-bit sample value, common/extract.c).
        // Threshold confirmed from leveladj.c (cxadc-win reference tool):
        // it flags "over" for byte < 0x08 or > 0xf8 (within 8 counts of
        // either rail), with true 0/255 treated as an immediate hard fail.
        // This mirrors that convention.
        if (b < 0x08 || b > 0xf8) {
            word |= (1u << 12);
        }

        out[i] = word;
    }
}

//-----------------------------------------------------------------------------
// Capture thread
//-----------------------------------------------------------------------------

static int cxadc_capture_thread(void *arg) {
    gui_app_t *app = (gui_app_t *)arg;

#ifdef _WIN32
    // Read current driver overflow counter as baseline so we only report
    // NEW overflows that occur during this capture session.
    uint32_t last_known_overflow = 0;
    {
        cxadc_capture_status_t init_status;
        if (gui_cxadc_get_capture_status(app, &init_status) == 0 &&
            init_status.driver_overflow_available) {
            last_known_overflow = init_status.driver_overflow_count;
        }
    }
    uint64_t last_overflow_poll_ms = get_time_ms();
#define CXADC_OVERFLOW_POLL_INTERVAL_MS 1000
#endif

    while (atomic_load(&app->cxadc_running)) {
#ifdef _WIN32
        ssize_t n = cxadc_platform_read(s_ctx.handle, s_ctx.read_buf, CXADC_READ_BUF_SAMPLES);
#else
        ssize_t n = cxadc_platform_read(s_ctx.fd, s_ctx.read_buf, CXADC_READ_BUF_SAMPLES);
#endif
        if (n < 0) {
            fprintf(stderr, "[CXADC] read error\n");
            break;
        }
        if (n == 0) {
            // No data available yet - device still open, keep trying
            thrd_yield();
            continue;
        }

        size_t sample_count = (size_t)n;
        cxadc_convert_block(s_ctx.read_buf, s_ctx.pack_buf, sample_count);

        size_t packed_bytes = sample_count * sizeof(uint32_t);
        uint8_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, packed_bytes, NULL);
        if (!out) {
            // Ringbuffer full - existing shared mechanism already counts this
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        memcpy(out, s_ctx.pack_buf, packed_bytes);
        bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, packed_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

        atomic_fetch_add(&app->total_samples, sample_count);
        atomic_store(&app->last_callback_time_ms, get_time_ms());

#ifdef _WIN32
        // Periodically poll the real hardware overflow counter and fold
        // any new overflows into the existing app->error_count, per the
        // accepted design ("Windows overflow folds into existing Errors
        // counter, no new UI field"). Delta-based so this is additive to
        // whatever error_count already reflects from other sources.
        uint64_t now_ms = get_time_ms();
        if (now_ms - last_overflow_poll_ms >= CXADC_OVERFLOW_POLL_INTERVAL_MS) {
            cxadc_capture_status_t status;
            if (gui_cxadc_get_capture_status(app, &status) == 0 && status.driver_overflow_available) {
                if (status.driver_overflow_count > last_known_overflow) {
                    uint32_t delta = status.driver_overflow_count - last_known_overflow;
                    atomic_fetch_add(&app->error_count, delta);
                    last_known_overflow = status.driver_overflow_count;
                }
            }
            last_overflow_poll_ms = now_ms;
        }
#endif
    }

    return 0;
}

#ifdef _WIN32
#undef CXADC_OVERFLOW_POLL_INTERVAL_MS
#endif

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_cxadc_open(gui_app_t *app, int device_index) {
    if (!app) return -1;

    char path[64];
#ifdef _WIN32
    snprintf(path, sizeof(path), "\\\\.\\cxadc%d", device_index);
    HANDLE h;
    if (cxadc_platform_open(path, &h) != 0) {
        fprintf(stderr, "[CXADC] failed to open %s\n", path);
        return -1;
    }
    app->cxadc_dev = (void *)h;
    app->cxadc_device_index = device_index;
#else
    snprintf(path, sizeof(path), "/dev/cxadc%d", device_index);
    int fd;
    if (cxadc_platform_open(path, &fd) != 0) {
        fprintf(stderr, "[CXADC] failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }
    app->cxadc_dev = (void *)(intptr_t)fd;
    app->cxadc_device_index = device_index;
#endif

    return 0;
}

void gui_cxadc_close(gui_app_t *app) {
    if (!app || !app->cxadc_dev) return;

#ifdef _WIN32
    cxadc_platform_close((HANDLE)app->cxadc_dev);
#else
    cxadc_platform_close((int)(intptr_t)app->cxadc_dev);
#endif
    app->cxadc_dev = NULL;
}

int gui_cxadc_start(gui_app_t *app) {
    if (!app || !app->cxadc_dev) return -1;
    if (atomic_load(&app->cxadc_running)) return 0;

    fprintf(stderr, "[CXADC] Starting CXADC capture\n");

    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    // Reset statistics (mirrors gui_fx3_start - confirmed pattern from
    // gui_fx3.c: total_samples, error counts, clip counts, rb counters,
    // stream_synced, sample_rate, last_callback_time_ms all reset here)
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    // Note: sample_rate is NOT set here - it is the user-entered value
    // already stored on app->sample_rate before capture started (see
    // gui_cxadc.h: CONFIGURATION IS USER-MANAGED). This code does not
    // know the true device rate and must not overwrite it.

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    s_ctx.app = app;
#ifdef _WIN32
    s_ctx.handle = (HANDLE)app->cxadc_dev;
#else
    s_ctx.fd = (int)(intptr_t)app->cxadc_dev;
#endif

    s_ctx.read_buf = (uint8_t *)malloc(CXADC_READ_BUF_SAMPLES);
    s_ctx.pack_buf = (uint32_t *)malloc(CXADC_READ_BUF_SAMPLES * sizeof(uint32_t));
    if (!s_ctx.read_buf || !s_ctx.pack_buf) {
        fprintf(stderr, "[CXADC] failed to allocate capture buffers\n");
        free(s_ctx.read_buf);
        free(s_ctx.pack_buf);
        s_ctx.read_buf = NULL;
        s_ctx.pack_buf = NULL;
        return -1;
    }

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[CXADC] failed to initialize capture ringbuffer\n");
        free(s_ctx.read_buf);
        free(s_ctx.pack_buf);
        return -1;
    }

    atomic_store(&app->cxadc_running, true);
    app->is_capturing = true;

    // Start extraction thread - reads BUF_CAPTURE_RF, writes BUF_DISPLAY.
    // CORRECTION: s_extract_fn is a single function pointer fixed once at
    // gui_extract_init(), selected by the HSDAOH_UPSTREAM build flag, NOT
    // dynamically chosen per active device. This build sets
    // HSDAOH_UPSTREAM=1 (confirmed meson.build), so A-only extraction
    // (extract_A_C) is already in effect regardless of CX - this works for
    // CX by coincidence of that build flag, not because this code selects
    // it. If ever built without HSDAOH_UPSTREAM, CX data would be run
    // through dual-channel extraction reading an always-zero B field
    // (harmless - produces silence on channel B - but not "selected for CX").
    //
    // KNOWN GAPS, NOT YET RESOLVED:
    // - Clip detection: extract_A_C sets clip[0] from bit 12 of the packed
    //   word (part of the AUX field). cxadc_convert_block() never sets bit
    //   12 (masks to 0xFFF, AUX always zero), so clipping will never be
    //   reported for CX data even at full-scale (255). Not crash-causing,
    //   but the on-screen clip indicator will be silently wrong for CX.
    // - Waveform polarity: extract_A_C inverts via (2047 - value). HSDAOH
    //   upstream packs its raw 12-bit ADC value with NO inversion at pack
    //   time (confirmed gui_capture.c: packed[i] = samples[i] & 0x0FFF,
    //   no offset/invert) - the 2047-value inversion lives entirely inside
    //   extract_A_C and is applied identically regardless of source. Both
    //   CX and HSDAOH's ADC produce codes that increase monotonically with
    //   input voltage (standard ADC behaviour), so this conversion's
    //   polarity is correct and consistent with HSDAOH captures.
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[CXADC] failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&app->cxadc_running, false);
        app->is_capturing = false;
        free(s_ctx.read_buf);
        free(s_ctx.pack_buf);
        return -1;
    }

    // Start display thread - oscilloscope/FFT/CVBS processing
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[CXADC] failed to start display thread (non-fatal)\n");
        }
    }

    thrd_t thread;
    if (thrd_create(&thread, cxadc_capture_thread, app) != thrd_success) {
        fprintf(stderr, "[CXADC] failed to create capture thread\n");
        gui_extract_stop();
        if (app->display_thread) {
            gui_display_thread_stop(app->display_thread);
        }
        atomic_store(&app->cxadc_running, false);
        app->is_capturing = false;
        free(s_ctx.read_buf);
        free(s_ctx.pack_buf);
        return -1;
    }
    app->cxadc_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "CXADC capture running");
    return 0;
}

void gui_cxadc_stop(gui_app_t *app) {
    if (!app) return;
    if (!atomic_load(&app->cxadc_running)) return;

    atomic_store(&app->cxadc_running, false);

    // Close the device handle BEFORE joining the thread.
    // On Windows, ReadFile() blocks until data arrives; closing the handle
    // forces it to return with an error so the thread can exit.
#ifdef _WIN32
    if (app->cxadc_dev) {
        CloseHandle((HANDLE)app->cxadc_dev);
        app->cxadc_dev = NULL;
    }
#else
    if (app->cxadc_dev) {
        close((int)(intptr_t)app->cxadc_dev);
        app->cxadc_dev = NULL;
    }
#endif

    if (app->cxadc_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->cxadc_thread;
        thrd_join(thread, NULL);
        app->cxadc_thread = NULL;
    }

    gui_extract_stop();
    if (app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }

    app->is_capturing = false;

    free(s_ctx.read_buf);
    free(s_ctx.pack_buf);
    s_ctx.read_buf = NULL;
    s_ctx.pack_buf = NULL;
}

bool gui_cxadc_is_running(gui_app_t *app) {
    if (!app) return false;
    return atomic_load(&app->cxadc_running);
}

int gui_cxadc_get_current_config(gui_app_t *app, cxadc_current_config_t *out_config) {
    if (!app || !app->cxadc_dev || !out_config) return -1;

    memset(out_config, 0, sizeof(*out_config));

#ifdef _WIN32
    typedef struct {
        unsigned long vmux;
        unsigned long level;
        unsigned char tenbit;
        unsigned char sixdb;
        unsigned long center_offset;
    } cx_device_config_raw_t;

    cx_device_config_raw_t raw = {0};
    DWORD bytes_returned = 0;

    BOOL ok = DeviceIoControl((HANDLE)app->cxadc_dev,
                               CX_IOCTL_CONFIG_GET,
                               NULL, 0,
                               &raw, sizeof(raw),
                               &bytes_returned, NULL);
    if (!ok) {
        fprintf(stderr, "[CXADC] CX_IOCTL_CONFIG_GET failed (err=%lu)\n", GetLastError());
        return -1;
    }

    out_config->vmux          = (int)raw.vmux;
    out_config->level         = (int)raw.level;
    out_config->sixdb         = raw.sixdb ? true : false;
    out_config->center_offset = (int)raw.center_offset;
    return 0;

#else
    // Read each parameter from sysfs. Path format:
    // /sys/class/cxadc/cxadcN/device/parameters/{vmux,level,sixdb,center_offset}
    // Confirmed from cxadc-linux3 driver: mycxadc_attrs[] registers these names.
    static const struct {
        const char *name;
        int        *field;
    } params[] = {
        { "vmux",          &out_config->vmux          },
        { "level",         &out_config->level         },
        { "center_offset", &out_config->center_offset },
    };

    for (size_t i = 0; i < sizeof(params)/sizeof(params[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/class/cxadc/cxadc%d/device/parameters/%s",
                 app->cxadc_device_index, params[i].name);

        FILE *f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "[CXADC] cannot read sysfs %s\n", path);
            return -1;
        }
        fscanf(f, "%d", params[i].field);
        fclose(f);
    }

    // sixdb is a boolean - read separately
    {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/class/cxadc/cxadc%d/device/parameters/sixdb",
                 app->cxadc_device_index);
        FILE *f = fopen(path, "r");
        if (f) {
            int v = 0;
            fscanf(f, "%d", &v);
            out_config->sixdb = v ? true : false;
            fclose(f);
        }
    }

    return 0;
#endif
}

int gui_cxadc_get_capture_status(gui_app_t *app, cxadc_capture_status_t *out_status) {
    if (!app || !out_status) return -1;

    memset(out_status, 0, sizeof(*out_status));

#ifdef _WIN32
    if (app->cxadc_dev) {
        // CX_IOCTL_STATE_OUFLOW_GET returns a single ULONG (confirmed
        // driver/ioctl.c: cx_evt_set_output(req, out_len,
        // &dev_ctx->state.ouflow_count, sizeof(dev_ctx->state.ouflow_count)))
        unsigned long ouflow_count = 0;
        DWORD bytes_returned = 0;

        BOOL ok = DeviceIoControl((HANDLE)app->cxadc_dev,
                                   CX_IOCTL_STATE_OUFLOW_GET,
                                   NULL, 0,
                                   &ouflow_count, sizeof(ouflow_count),
                                   &bytes_returned, NULL);
        if (ok) {
            out_status->driver_overflow_available = true;
            out_status->driver_overflow_count = (uint32_t)ouflow_count;
        } else {
            fprintf(stderr, "[CXADC] CX_IOCTL_STATE_OUFLOW_GET failed (err=%lu)\n", GetLastError());
            out_status->driver_overflow_available = false;
        }
    }
#else
    out_status->driver_overflow_available = false;
#endif

    return 0;
}

#endif // ENABLE_CXADC
