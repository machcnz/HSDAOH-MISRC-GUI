/*
 * MISRC GUI - Cypress FX3 Device Support
 *
 * Provides capture support using Cypress FX3 USB 3.0 devices.
 * This was the original capture interface before hsdaoh/HDMI support.
 *
 * Features:
 * - Direct USB 3.0 bulk transfers via libcyusb
 * - 40 MSPS dual-channel ADC capture
 * - Same data format as hsdaoh (12-bit ADC A + 12-bit ADC B + 8-bit AUX)
 */

#ifndef GUI_FX3_H
#define GUI_FX3_H

#ifdef ENABLE_FX3

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// FX3 Device Configuration
//-----------------------------------------------------------------------------

#define FX3_SAMPLE_RATE      40000000    // 40 MSPS - matches HDMI capture
#define FX3_BUFFER_SIZE      65536       // Samples per USB transfer
#define FX3_NUM_TRANSFERS    8           // Number of async USB transfers

// FX3 USB endpoints
#define FX3_EP_BULK_IN       0x82        // Bulk IN endpoint for ADC data

// FX3 vendor commands (from sigrok cypress-fx3 driver)
#define FX3_CMD_FW_UPLOAD        0xA0    // Upload firmware to RAM (bootloader command)
#define FX3_CMD_GET_FW_VERSION   0xb0    // Get firmware version
#define FX3_CMD_START            0xb1    // Start acquisition
#define FX3_CMD_GET_REVID        0xb2    // Get revision ID

// FX3 PIB clock for sample rate calculation (from sigrok)
#define FX3_PIB_CLOCK            400000000  // 400 MHz

// FX3 firmware upload parameters
#define FX3_FW_CHUNK_SIZE        4096    // Max bytes per control transfer

//-----------------------------------------------------------------------------
// FX3 Device Info (for enumeration)
//-----------------------------------------------------------------------------

typedef struct {
    int bus;                              // USB bus number
    int address;                          // USB device address
    char serial[64];                      // Serial number (if available)
    char name[128];                       // Device name
} fx3_device_info_t;

//-----------------------------------------------------------------------------
// FX3 Device API
//-----------------------------------------------------------------------------

// Enumerate FX3 devices
// Returns number of devices found, fills devices array up to max_devices
int gui_fx3_enumerate(fx3_device_info_t *devices, int max_devices);

// Start FX3 capture
// Returns 0 on success, -1 on error
int gui_fx3_start(gui_app_t *app);

// Stop FX3 capture
void gui_fx3_stop(gui_app_t *app);

// Check if FX3 capture is running
bool gui_fx3_is_running(gui_app_t *app);

// Open FX3 device by index
// Returns 0 on success, -1 on error
int gui_fx3_open(gui_app_t *app, int device_index);

// Close FX3 device
void gui_fx3_close(gui_app_t *app);

#endif // ENABLE_FX3

#endif // GUI_FX3_H
