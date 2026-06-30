#ifndef GUI_CXADC_H
#define GUI_CXADC_H

#include <stdbool.h>

typedef struct gui_app gui_app_t;

// Detect available CXADC RF cards.
// Returns number of cards detected (0..2).
int gui_cxadc_detect_cards(void);

// Start CXADC capture mode (card_count: 1 or 2).
// Returns 0 on success, -1 on error.
int gui_cxadc_start(gui_app_t *app, int card_count);

// Stop CXADC capture mode.
void gui_cxadc_stop(gui_app_t *app);

// Check whether CXADC capture mode is currently running.
bool gui_cxadc_is_running(void);

#endif // GUI_CXADC_H
