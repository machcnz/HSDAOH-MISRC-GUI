/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#ifndef GUI_EXTRACT_H
#define GUI_EXTRACT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"

// Forward declarations
typedef struct gui_app gui_app_t;

// Initialize extraction subsystem (allocates buffers, gets extraction function)
void gui_extract_init(void);

// Cleanup extraction subsystem
void gui_extract_cleanup(void);

// Start the extraction thread (call after capture starts)
// Uses app->buffers (buffer_manager) for reading BUF_CAPTURE_RF
// Returns 0 on success, -1 on error
int gui_extract_start(gui_app_t *app);

// Stop the extraction thread (call before capture stops)
void gui_extract_stop(void);

// Check if extraction thread is running
bool gui_extract_is_running(void);

// Get record ringbuffers (for writer threads)
ringbuffer_t *gui_extract_get_record_rb_a(void);
ringbuffer_t *gui_extract_get_record_rb_b(void);

// Enable/disable recording mode
// When enabled, extraction thread writes to record ringbuffers
// rf_bits_* semantics:
// - If use_flac=true: bits per sample for FLAC stream (8/12/16)
// - If use_flac=false: bits per sample for RAW output (8/16)
void gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b);

// Reset record ringbuffers (call before starting writer threads)
void gui_extract_reset_record_rbs(void);

// Initialize record ringbuffers (for simulated capture that doesn't use extraction thread)
void gui_extract_init_record_rbs(void);

// Check if recording is enabled and get FLAC mode
bool gui_extract_is_recording(bool *use_flac);

// Get the extraction function pointer (for direct use)
typedef void (*extract_fn_t)(uint32_t *buf, size_t num_samples, size_t *clip,
                             uint8_t *aux_buf, int16_t *buf_a, int16_t *buf_b,
                             uint16_t *peak);
extract_fn_t gui_extract_get_function(void);

// Get extraction output buffers (for direct use)
int16_t *gui_extract_get_buf_a(void);
int16_t *gui_extract_get_buf_b(void);
uint8_t *gui_extract_get_buf_aux(void);

// Update clip counts and peak values from extracted samples
// Processes sample buffers and updates app's atomic counters
void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples);

// Called by the render thread to update oscilloscope display when ready to draw a frame
// Uses atomic flag to check if new samples are available from extraction thread
// Returns true if new samples were processed, false if no new samples available
bool gui_extract_update_display(void);

// Get the "data available" event (callback signals this after writing to ringbuffer)
// Returns NULL if extraction is not initialized
rb_event_t *gui_extract_get_data_event(void);

// Get the "space available" event (extraction signals this after consuming from ringbuffer)
// Returns NULL if extraction is not initialized
rb_event_t *gui_extract_get_space_event(void);

// Get the "record space available" event (file writers signal this after consuming from record ringbuffer)
// Returns NULL if extraction is not initialized
rb_event_t *gui_extract_get_record_space_event(void);

#endif // GUI_EXTRACT_H
