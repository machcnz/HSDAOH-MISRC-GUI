/*
 * MISRC GUI - CX2388x (CXADC) Device Support
 *
 * Provides capture support using Conexant CX2388x/CX25800 PCI capture cards
 * forced into raw ADC mode via a modified driver.
 *
 * Linux: character device /dev/cxadcN + sysfs parameters (cxadc-linux3 driver)
 * Windows: device path \\.\cxadcN + DeviceIoControl (cxadc-win driver)
 *
 * Features:
 * - Single-channel raw ADC capture (no RF-B equivalent)
 * - 8-bit unsigned native samples (10-bit mode out of scope for now)
 *
 * CONFIGURATION IS USER-MANAGED, NOT GUI-MANAGED:
 * vmux, sample rate (clockgen + driver rate match), level, sixdb, and
 * center_offset are all set by the user externally (sysfs on Linux,
 * PowerShell module on Windows) before launching the GUI and starting
 * capture. This code does not write any of these values to the device.
 * It only opens the device, reads raw samples, and optionally reads back
 * current values for informational display.
 *
 * LOSS DETECTION IS ASYMMETRIC BETWEEN PLATFORMS - confirmed directly from
 * both drivers' actual source code (not README inference):
 * - Linux (cxadc-linux3, cxadc.c): no overflow/underflow check anywhere in
 *   the driver. cxadc_char_read() copies available DMA pages and blocks on
 *   the next interrupt; it never reads the CX2388x CX_VIDEO_DEVICE_STATUS
 *   register's overflow/underflow ("lof") bit. Sysfs only exposes config
 *   parameters: latency, audsel, vmux, level, tenbit, tenxfsc, sixdb,
 *   crystal, center_offset. No loss counter exists at the driver level.
 * - Windows (cxadc-win, driver/ioctl.c CxEvtIoRead): on every read(), after
 *   copying available pages, the driver calls cx_get_ouflow_state(mmio)
 *   (driver/cx2388x.c) which reads CX_VIDEO_DEVICE_STATUS_ADDR and checks
 *   the .lof bit. If set, dev_ctx->state.ouflow_count is incremented
 *   (InterlockedIncrement) and the hardware flag is cleared via
 *   cx_reset_ouflow_state(). The count is exposed to userspace via
 *   IOCTL CX_IOCTL_STATE_OUFLOW_GET (struct DEVICE_STATE.ouflow_count,
 *   shared/cx_config.h) and resettable via CX_IOCTL_STATE_OUFLOW_RESET.
 * - IMPORTANT: this is NOT a hardware difference. The CX_VIDEO_DEVICE_STATUS
 *   register and its .lof bit exist on the chip regardless of OS - the
 *   Linux driver simply never reads it. The asymmetry is a software gap in
 *   cxadc-linux3, not a platform limitation. On Windows this code reads the
 *   real driver-level counter via this header's get_capture_status(); on
 *   Linux no equivalent exists, and driver_overflow_available will be false.
 *   On both platforms, separately, the existing app->rb_drop_count mechanism
 *   (shared by every backend, see gui_capture.c) already covers GUI
 *   ringbuffer-full conditions - no new mechanism is needed for that case.
 * - Also confirmed from shared/cx_ctl_codes.h: vmux range is 0-3
 *   (CX_CTRL_CONFIG_VMUX_MIN/MAX), not 0-2 as stated in the cxadc-win
 *   README and the Linux driver's sysfs description. The driver's own
 *   validation in control.c (cx_ctrl_set_vmux) enforces value <= 3, so this
 *   is authoritative over the documentation.
 * TWO DISTINCT "OVERFLOW" CONCEPTS EXIST IN THE REFERENCE SOURCE - confirmed
 * from both the driver and the capture-server, these must not be conflated:
 *
 * 1. DRIVER-LEVEL HARDWARE OVERFLOW (Windows only, see above) - the CX2388x
 *    chip's own DMA FIFO over/underflow flag, read via IOCTL.
 *
 * 2. APPLICATION-LEVEL RINGBUFFER OVERFLOW (both platforms, in
 *    capture-server/files.c cxadc_writer_thread and audio writer thread) -
 *    when the capture-server's own userspace ring buffer (between the
 *    read()/ReadFile() call and the disk-writing thread) is full because the
 *    disk write is falling behind, g_state.overflow_counter is incremented,
 *    a 1ms sleep happens, and the read is retried. This has nothing to do
 *    with driver IOCTLs or hardware registers - it is pure userspace ring
 *    buffer accounting, identical in structure on both platforms, and
 *    reported via the /stats and /stop HTTP endpoints and the
 *    "Encountered N overflows during capture" console message.
 *
 * This GUI's existing ringbuffer architecture (bufmgr_write_begin returning
 * NULL when BUF_CAPTURE_RF is full, already used by every other backend -
 * see gui_capture.c) is functionally equivalent to capture-server's
 * cxadc_writer_thread pattern. The CXADC capture thread should detect and
 * count this the same way: when bufmgr_write_begin() returns NULL, increment
 * a counter (already covered by the existing rb_drop_count mechanism shared
 * across all backends - no new GUI-side mechanism is needed for this case).
 *
 * What remains genuinely CXADC-specific and not covered by the existing
 * ringbuffer-full detection is the Windows hardware overflow counter
 * (concept 1 above), which has no equivalent on Linux and no equivalent in
 * any other existing backend in this codebase.
 */

#ifndef GUI_CXADC_H
#define GUI_CXADC_H

#ifdef ENABLE_CXADC

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// CXADC Device Configuration
//-----------------------------------------------------------------------------

#define CXADC_GUI_MAX_DEVICES  4        // GUI enumeration cap (driver itself supports up to 256, see CXCOUNT_MAX in cxadc-linux3)
#define CXADC_BUFFER_SIZE     65536    // Bytes per read() / ReadFile() call
#define CXADC_SAMPLE_WIDTH_8BIT 1      // Bytes per sample, 8-bit mode (only mode currently supported)

//-----------------------------------------------------------------------------
// CXADC Capture Status (loss/error detection - see overflow distinction
// note above)
//-----------------------------------------------------------------------------

typedef struct {
    // Windows only - real hardware FIFO over/underflow count from the CX2388x
    // chip's CX_VIDEO_DEVICE_STATUS register, via CX_IOCTL_STATE_OUFLOW_GET.
    // Meaningless/unset on Linux - check driver_overflow_available first.
    // This is distinct from and in addition to the existing rb_drop_count
    // mechanism (app->rb_drop_count) shared by all backends, which already
    // covers GUI ringbuffer-full conditions on both platforms.
    bool driver_overflow_available;
    uint32_t driver_overflow_count;
} cxadc_capture_status_t;

//-----------------------------------------------------------------------------
// CXADC Device Info (for enumeration)
//-----------------------------------------------------------------------------

typedef struct {
    int index;                            // Device index (0-3, matches cxadcN)
    char path[128];                       // Linux: /dev/cxadc0  Windows: \\.\cxadc0
    char name[128];                       // Display name for device list
} cxadc_device_info_t;

//-----------------------------------------------------------------------------
// CXADC Current Configuration (READ-ONLY - informational display only;
// these values are set by the user outside the GUI before capture)
//-----------------------------------------------------------------------------

typedef struct {
    int vmux;
    int level;
    bool sixdb;
    int center_offset;
} cxadc_current_config_t;

// Note: sample_rate is not part of this struct either. It is purely
// informational, user-entered into the GUI to match what was already
// configured externally - stored directly on app->sample_rate, the same
// field used by other backends. This code never reads or writes a sample
// rate value on the device itself.

//-----------------------------------------------------------------------------
// CXADC Device API
//-----------------------------------------------------------------------------

// Enumerate CXADC devices
// Returns number of devices found, fills devices array up to max_devices
int gui_cxadc_enumerate(cxadc_device_info_t *devices, int max_devices);

// Open CXADC device by index
// Returns 0 on success, -1 on error
int gui_cxadc_open(gui_app_t *app, int device_index);

// Close CXADC device
void gui_cxadc_close(gui_app_t *app);

// Start CXADC capture (device must already be open)
// Returns 0 on success, -1 on error
int gui_cxadc_start(gui_app_t *app);

// Stop CXADC capture
void gui_cxadc_stop(gui_app_t *app);

// Check if CXADC capture is running
bool gui_cxadc_is_running(gui_app_t *app);

// Read current configuration directly from the device for display purposes
// only. Returns 0 on success, -1 on error (e.g. device not open).
int gui_cxadc_get_current_config(gui_app_t *app, cxadc_current_config_t *out_config);

// Read current capture loss/error status. See cxadc_capture_status_t comments
// for which fields are meaningful on which platform.
// Returns 0 on success, -1 on error (e.g. device not open).
int gui_cxadc_get_capture_status(gui_app_t *app, cxadc_capture_status_t *out_status);

//-----------------------------------------------------------------------------
// PCM1802 Clockgen Audio API
// Paired with the CX card - auto-opened when a CX device is selected.
// Failure is non-fatal: video capture continues, audio stays silent.
// L+R channels only - 3rd channel (HSW head-switch) is discarded.
//-----------------------------------------------------------------------------

// Open clockgen audio device (matched by known VID/PID on Windows,
// fixed ALSA card name on Linux). Returns 0 on success, -1 if not found.
int gui_cxadc_pcm1802_open(gui_app_t *app);

// Close clockgen audio device.
void gui_cxadc_pcm1802_close(gui_app_t *app);

// Start audio capture. Returns 0 on success, -1 on error.
int gui_cxadc_pcm1802_start(gui_app_t *app);

// Stop audio capture.
void gui_cxadc_pcm1802_stop(gui_app_t *app);

#endif // ENABLE_CXADC

#endif // GUI_CXADC_H
