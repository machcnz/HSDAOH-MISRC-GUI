/*
 * MISRC GUI - VU Meter Rendering
 *
 * Bipolar VU meter for AC-coupled signals with clip indicators.
 */

#ifndef GUI_VU_METER_H
#define GUI_VU_METER_H

#include "../core/gui_app.h"
#include "raylib.h"
#include <stdbool.h>

// Render a bipolar VU meter
// Shows separate positive (upward) and negative (downward) levels from center
// Positive and negative bars can have different heights for asymmetric signals
void render_vu_meter(float x, float y, float width, float height,
                     vu_meter_state_t *meter, const char *label,
                     bool is_clipping_pos, bool is_clipping_neg, Color channel_color);

#endif // GUI_VU_METER_H
