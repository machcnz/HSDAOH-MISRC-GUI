#include "gui_headswitch_lock.h"

#include "../../common/threading.h"

#include <math.h>
#include <stdatomic.h>

typedef struct {
    atomic_flag lock;
    bool initialized;
    int8_t schmitt_state; // -1 low, 0 mid, +1 high
    int32_t prev_sample;
    float env_abs_ema;
    uint64_t sample_counter;
    uint64_t last_edge_sample;
    uint64_t edge_count;
    float period_samples_audio;
    uint32_t audio_sample_rate_hz;
    uint64_t last_edge_time_us;
} headswitch_lock_state_t;

static headswitch_lock_state_t s_lock_state = {
    .lock = ATOMIC_FLAG_INIT,
};

static inline void lock_state(void)
{
    while (atomic_flag_test_and_set(&s_lock_state.lock)) {
        thrd_sleep_ms(1);
    }
}

static inline void unlock_state(void)
{
    atomic_flag_clear(&s_lock_state.lock);
}

static inline int32_t signext24(uint32_t raw)
{
    if (raw & 0x00800000U) {
        raw |= 0xFF000000U;
    }
    return (int32_t)raw;
}

void gui_headswitch_lock_reset(void)
{
    lock_state();
    s_lock_state.initialized = true;
    s_lock_state.schmitt_state = 0;
    s_lock_state.prev_sample = 0;
    s_lock_state.env_abs_ema = 0.0f;
    s_lock_state.sample_counter = 0;
    s_lock_state.last_edge_sample = 0;
    s_lock_state.edge_count = 0;
    s_lock_state.period_samples_audio = 0.0f;
    s_lock_state.audio_sample_rate_hz = 0;
    s_lock_state.last_edge_time_us = 0;
    unlock_state();
}

void gui_headswitch_lock_ingest_s24le_interleaved(const uint8_t *data,
                                                  size_t bytes,
                                                  uint32_t audio_sample_rate_hz)
{
    if (!data || bytes < 12 || audio_sample_rate_hz == 0) return;

    size_t frames = bytes / 12;
    if (frames == 0) return;

    lock_state();
    s_lock_state.audio_sample_rate_hz = audio_sample_rate_hz;
    s_lock_state.initialized = true;

    for (size_t i = 0; i < frames; i++) {
        const uint8_t *frame = data + (i * 12);
        uint32_t raw = ((uint32_t)frame[6]) |
                       ((uint32_t)frame[7] << 8) |
                       ((uint32_t)frame[8] << 16);
        int32_t sample = signext24(raw);
        float abs_sample = (float)(sample < 0 ? -sample : sample);

        // Slow envelope follower for adaptive Schmitt thresholds.
        s_lock_state.env_abs_ema = (s_lock_state.env_abs_ema * 0.998f) + (abs_sample * 0.002f);
        float hi_thresh = s_lock_state.env_abs_ema * 0.35f;
        if (hi_thresh < 2500.0f) hi_thresh = 2500.0f;
        float lo_thresh = hi_thresh * 0.45f;

        bool edge_cross = false;
        if (s_lock_state.schmitt_state <= 0 &&
            s_lock_state.prev_sample < (int32_t)hi_thresh &&
            sample >= (int32_t)hi_thresh) {
            s_lock_state.schmitt_state = 1;
            edge_cross = true;
        } else if (s_lock_state.schmitt_state >= 0 &&
                   s_lock_state.prev_sample > -(int32_t)hi_thresh &&
                   sample <= -(int32_t)hi_thresh) {
            s_lock_state.schmitt_state = -1;
            edge_cross = true;
        } else if (s_lock_state.schmitt_state == 1 && sample <= (int32_t)lo_thresh) {
            s_lock_state.schmitt_state = 0;
        } else if (s_lock_state.schmitt_state == -1 && sample >= -(int32_t)lo_thresh) {
            s_lock_state.schmitt_state = 0;
        }

        if (edge_cross) {
            uint64_t edge_sample = s_lock_state.sample_counter;
            if (s_lock_state.last_edge_sample > 0 && edge_sample > s_lock_state.last_edge_sample) {
                float delta = (float)(edge_sample - s_lock_state.last_edge_sample);
                // Reject implausible edge intervals (<0.25ms or >2s at audio rate).
                float min_period = (float)audio_sample_rate_hz * 0.00025f;
                float max_period = (float)audio_sample_rate_hz * 2.0f;
                if (delta >= min_period && delta <= max_period) {
                    if (s_lock_state.period_samples_audio <= 1.0f) {
                        s_lock_state.period_samples_audio = delta;
                    } else {
                        s_lock_state.period_samples_audio =
                            (s_lock_state.period_samples_audio * 0.85f) + (delta * 0.15f);
                    }
                }
            }
            s_lock_state.last_edge_sample = edge_sample;
            s_lock_state.last_edge_time_us = get_time_us();
            s_lock_state.edge_count++;
        }

        s_lock_state.prev_sample = sample;
        s_lock_state.sample_counter++;
    }

    unlock_state();
}

bool gui_headswitch_lock_predict_trigger(size_t rf_window_samples,
                                         uint32_t rf_sample_rate_hz,
                                         size_t min_index,
                                         size_t *out_trigger_index)
{
    if (!out_trigger_index || rf_window_samples < 2 || rf_sample_rate_hz == 0) {
        return false;
    }

    lock_state();
    bool valid = (s_lock_state.period_samples_audio > 1.0f) &&
                 (s_lock_state.audio_sample_rate_hz > 0) &&
                 (s_lock_state.last_edge_time_us > 0) &&
                 (s_lock_state.edge_count >= 2);
    if (!valid) {
        unlock_state();
        return false;
    }

    double period_sec = (double)s_lock_state.period_samples_audio /
                        (double)s_lock_state.audio_sample_rate_hz;
    double period_rf = period_sec * (double)rf_sample_rate_hz;
    if (period_rf < 1.0) {
        unlock_state();
        return false;
    }

    if (s_lock_state.sample_counter <= s_lock_state.last_edge_sample) {
        unlock_state();
        return false;
    }
    uint64_t audio_age_samples = s_lock_state.sample_counter - s_lock_state.last_edge_sample;
    if (audio_age_samples > ((uint64_t)s_lock_state.audio_sample_rate_hz * 2ULL)) {
        // Stale lock: no headswitch edge for >2s.
        unlock_state();
        return false;
    }

    // Use sample-domain elapsed phase to avoid scheduler/wall-clock jitter.
    double phase_audio = fmod((double)audio_age_samples, (double)s_lock_state.period_samples_audio);
    double phase_rf = phase_audio * ((double)rf_sample_rate_hz / (double)s_lock_state.audio_sample_rate_hz);
    if (phase_rf < 0.0) phase_rf = 0.0;
    if (phase_rf >= period_rf) phase_rf = fmod(phase_rf, period_rf);
    double win_min = (double)min_index;
    double win_max = (double)rf_window_samples - 1.0;
    double win_span = win_max - win_min;
    if (win_span < 1.0) {
        unlock_state();
        return false;
    }

    // Candidate edge location in the current RF window (can be out-of-window).
    double edge_idx = win_max - phase_rf;

    // Choose periodic equivalent nearest the window center.
    double win_center = win_min + (win_span * 0.5);
    double k = nearbyint((win_center - edge_idx) / period_rf);
    double projected = edge_idx + (k * period_rf);

    // If no periodic edge lands in-window (e.g. interval >> window),
    // map the interval phase into the visible window so CH3 still drives trigger position.
    if (projected < win_min || projected > win_max) {
        double phase_frac = phase_rf / period_rf; // [0,1)
        if (phase_frac < 0.0) phase_frac = 0.0;
        if (phase_frac >= 1.0) phase_frac = 0.999999;
        projected = win_min + (phase_frac * win_span);
    }

    size_t idx = (size_t)llround(projected);
    if (idx < min_index) idx = min_index;
    if (idx >= rf_window_samples) idx = rf_window_samples - 1;
    *out_trigger_index = idx;

    unlock_state();
    return true;
}

void gui_headswitch_lock_get_status(gui_headswitch_lock_status_t *out_status)
{
    if (!out_status) return;
    lock_state();
    out_status->edge_count = s_lock_state.edge_count;
    out_status->audio_sample_rate_hz = s_lock_state.audio_sample_rate_hz;
    out_status->period_samples_audio = s_lock_state.period_samples_audio;
    out_status->last_edge_time_us = s_lock_state.last_edge_time_us;
    out_status->locked = (s_lock_state.period_samples_audio > 1.0f &&
                          s_lock_state.edge_count >= 2);
    unlock_state();
}
