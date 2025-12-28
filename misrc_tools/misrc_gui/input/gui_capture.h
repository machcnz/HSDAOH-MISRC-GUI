#ifndef GUI_CAPTURE_H
#define GUI_CAPTURE_H

#include "../core/gui_app.h"
#include "../../common/ringbuffer.h"

// Global exit flag (defined in misrc_gui.c)
extern volatile atomic_int do_exit;

// Capture callback function
void gui_capture_callback(void *data_info);

// Get audio ringbuffer (may be NULL if not initialized)
ringbuffer_t *gui_capture_get_audio_ringbuffer(void);

// Check if device has timed out (no callbacks for too long)
// Returns true if device appears disconnected
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms);

#endif // GUI_CAPTURE_H
