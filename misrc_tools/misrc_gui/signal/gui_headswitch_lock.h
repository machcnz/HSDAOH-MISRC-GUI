#ifndef GUI_HEADSWITCH_LOCK_H
#define GUI_HEADSWITCH_LOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool locked;
    uint64_t edge_count;
    uint32_t audio_sample_rate_hz;
    float period_samples_audio;
    uint64_t last_edge_time_us;
} gui_headswitch_lock_status_t;

// Reset all lock state.
void gui_headswitch_lock_reset(void);

// Ingest packed 4ch 24-bit interleaved audio frames (12 bytes/frame).
// CH3 is used as headswitch input. bytes must be a multiple of 12.
void gui_headswitch_lock_ingest_s24le_interleaved(const uint8_t *data,
                                                  size_t bytes,
                                                  uint32_t audio_sample_rate_hz);

// Predict a trigger position in an RF sample window using headswitch phase.
// rf_window_samples is the number of RF samples in the current trigger search window.
// rf_sample_rate_hz is the RF sampling rate.
// Returns true and writes out_trigger_index when lock is valid.
bool gui_headswitch_lock_predict_trigger(size_t rf_window_samples,
                                         uint32_t rf_sample_rate_hz,
                                         size_t min_index,
                                         size_t *out_trigger_index);

// Read current lock status (best-effort snapshot).
void gui_headswitch_lock_get_status(gui_headswitch_lock_status_t *out_status);

#endif // GUI_HEADSWITCH_LOCK_H
