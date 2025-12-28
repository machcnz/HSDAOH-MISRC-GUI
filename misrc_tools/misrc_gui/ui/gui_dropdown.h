/*
 * MISRC GUI - Generalized Dropdown System
 *
 * Centralized dropdown management for consistent behavior across the UI.
 * Only one dropdown can be open at a time.
 */

#ifndef GUI_DROPDOWN_H
#define GUI_DROPDOWN_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "raylib.h"
#include <clay.h>

// Forward declaration
typedef struct gui_app gui_app_t;

//-----------------------------------------------------------------------------
// Dropdown Identifiers
//-----------------------------------------------------------------------------

#define DROPDOWN_DEVICE       "Device"
#define DROPDOWN_TRIGGER_MODE "TriggerMode"
#define DROPDOWN_LAYOUT       "Layout"
#define DROPDOWN_LEFT_VIEW    "LeftView"
#define DROPDOWN_RIGHT_VIEW   "RightView"
#define DROPDOWN_CVBS_SYSTEM  "CvbsSystem"

//-----------------------------------------------------------------------------
// State Management
//-----------------------------------------------------------------------------

// Open a specific dropdown (closes any other open dropdown)
// id: unique string identifier, index: numeric index for indexed dropdowns (0 if not used)
void gui_dropdown_open(const char *id, uint32_t index);

// Close all dropdowns
void gui_dropdown_close_all(void);

// Check if a specific dropdown is open
bool gui_dropdown_is_open(const char *id, uint32_t index);

// Toggle a dropdown (open if closed, close if open)
// Also closes all other dropdowns
void gui_dropdown_toggle(const char *id, uint32_t index);

//-----------------------------------------------------------------------------
// Rendering Helpers
//-----------------------------------------------------------------------------

// Get color for dropdown option based on selection and hover state
Color gui_dropdown_option_color(bool is_selected, bool is_hovered);

// Convert Color to Clay_Color
static inline Clay_Color gui_dropdown_to_clay(Color c) {
    return (Clay_Color){ c.r, c.g, c.b, c.a };
}

// Create Clay_String from C string
static inline Clay_String gui_dropdown_string(const char *str) {
    return (Clay_String){ .isStaticallyAllocated = false,
                          .length = (int32_t)strlen(str), .chars = str };
}

//-----------------------------------------------------------------------------
// Interaction Handling
//-----------------------------------------------------------------------------

// Handle all dropdown clicks for current frame
// Call once per frame after rendering, when mouse button is pressed
// Returns true if a dropdown consumed the click
bool gui_dropdown_handle_click(gui_app_t *app);

#endif // GUI_DROPDOWN_H
