/*
 * MISRC GUI - Text Rendering Utilities
 *
 * Centralized text rendering with custom font support.
 * Uses the application's loaded fonts (Inter for labels, Space Mono for numbers).
 */

#ifndef GUI_TEXT_H
#define GUI_TEXT_H

#include "../core/gui_app.h"
#include "raylib.h"

//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------

// Set the app reference for font access
// Must be called before using any text rendering functions
void gui_text_set_app(gui_app_t *app);

//-----------------------------------------------------------------------------
// Text Drawing
//-----------------------------------------------------------------------------

// Draw text using the app's primary font (Inter - for labels)
// Falls back to raylib default font if app fonts not loaded
void gui_text_draw(const char *text, float x, float y, int fontSize, Color color);

// Draw text using monospace font (Space Mono - for numbers/data)
// Falls back to raylib default font if app fonts not loaded
void gui_text_draw_mono(const char *text, float x, float y, int fontSize, Color color);

//-----------------------------------------------------------------------------
// Text Measurement
//-----------------------------------------------------------------------------

// Measure text width using the app's primary font
int gui_text_measure(const char *text, int fontSize);

// Measure text width using monospace font
int gui_text_measure_mono(const char *text, int fontSize);

#endif // GUI_TEXT_H
