/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#ifndef GUI_RECORD_H
#define GUI_RECORD_H

#include <stdbool.h>

// Forward declarations
typedef struct gui_app gui_app_t;

// Initialize recording subsystem
void gui_record_init(void);

// Cleanup recording subsystem
void gui_record_cleanup(void);

// Recording start return codes
#define RECORD_OK       0   // Recording started successfully
#define RECORD_ERROR   -1   // Error occurred
#define RECORD_PENDING  1   // Waiting for user confirmation (popup shown)

// Start recording to files
// Returns RECORD_OK on success, RECORD_ERROR on error, RECORD_PENDING if waiting for confirmation
int gui_record_start(gui_app_t *app);

// Check popup result and continue recording if confirmed
// Call this each frame when gui_record_is_pending() returns true
void gui_record_check_popup(gui_app_t *app);

// Check if waiting for popup confirmation
bool gui_record_is_pending(void);

// Stop recording
void gui_record_stop(gui_app_t *app);

// Check if recording is active
bool gui_record_is_active(void);

#endif // GUI_RECORD_H
