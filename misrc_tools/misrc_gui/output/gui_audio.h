#ifndef GUI_AUDIO_H
#define GUI_AUDIO_H

#include "../core/gui_app.h"
#include "../../common/buffer_manager.h"

// Start audio writer/monitor thread.
// If no audio outputs are enabled in settings, this is a no-op and returns 0.
int gui_audio_start(gui_app_t *app, buffer_manager_t *bufmgr);

// Stop audio writer/monitor thread if running.
void gui_audio_stop(gui_app_t *app);

// True if audio thread is running.
bool gui_audio_is_running(void);

#endif // GUI_AUDIO_H
