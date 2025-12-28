/*
 * MISRC GUI - Simulated Device
 *
 * Provides a simulated capture device for development and testing without hardware.
 * Generates NTSC CVBS video on Channel A and VHS RF head signal on Channel B.
 *
 * Features:
 * - Proper 2-field (525-line) interlaced NTSC video timing
 * - SMPTE color bars test pattern with chroma
 * - VHS RF signal with FM luminance and 629 kHz chroma-under
 * - Head switching noise simulation at field boundaries
 * - Recording support (RAW and FLAC)
 */

#ifndef GUI_SIMULATED_H
#define GUI_SIMULATED_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// Simulated Device Configuration
//-----------------------------------------------------------------------------

#define SIM_SAMPLE_RATE      40000000    // 40 MSPS - matches real hardware
#define SIM_BUFFER_SIZE      65536       // Samples per batch
#define SIM_UPDATE_INTERVAL_MS  2        // Time between batches

//-----------------------------------------------------------------------------
// Simulated Device API
//-----------------------------------------------------------------------------

// Start simulated capture
// Returns 0 on success, -1 on error
int gui_simulated_start(gui_app_t *app);

// Stop simulated capture
void gui_simulated_stop(gui_app_t *app);

// Check if simulated capture is running
bool gui_simulated_is_running(gui_app_t *app);

#endif // GUI_SIMULATED_H
