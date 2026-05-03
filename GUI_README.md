# MISRC GUI Readout + Stats Breakdown
This file documents what the GUI stats/readouts currently show, where each value comes from, and what the wait/drop counters actually mean.

## Scope
- Focus is the live GUI readout/counter behavior in `misrc_gui`.
- Definitions below are based on current code paths in:
  - `misrc_tools/misrc_gui/ui/gui_ui.c`
  - `misrc_tools/misrc_gui/input/gui_capture.c`
  - `misrc_tools/misrc_gui/processing/gui_extract.c`
  - `misrc_tools/misrc_gui/output/gui_record.c`
  - `misrc_tools/common/buffer_manager.c`

## Data flow (where counters are produced)
- Capture callback writes raw RF into `BUF_CAPTURE_RF`.
- Extraction thread reads `BUF_CAPTURE_RF`, updates sample/clip/peak stats, writes:
  - display frames to `BUF_DISPLAY`
  - recording data to `BUF_RECORD_A` / `BUF_RECORD_B` (only while recording)
- Writer threads drain `BUF_RECORD_A/B` and update recording byte counters.

## Bottom status bar readouts
Rendered in `render_status_bar()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `REC dot + HH:MM:SS`
  - Shown only while recording.
  - Time is `GetTime() - recording_start_time`.
- Status text (when not recording)
  - Shows `app->status_message`.
- `Sync: OK` / `Sync: --`
  - Uses `stream_synced`.
- `XX MSPS`
  - From `sample_rate` displayed as integer `sample_rate / 1000000`.
- `Samples`
  - Uses `samples_a` (channel A sample counter).
  - Formatted as raw count with `K/M/G` suffixes.
- `Frames`
  - Uses `frame_count`.
- `Missed`
  - Uses `missed_frame_count`.
  - This counter is debounced in GUI capture callback logic: isolated single miss events are suppressed, and only persistent/consecutive miss conditions increment it.
- `Errors`
  - Uses total `error_count`.
  - This counter is debounced in GUI capture callback logic and tracks persistent parser-error events rather than summing every per-line parser error value.
- `RF Buffer`
  - Percent fill computed from `BUF_CAPTURE_RF` ringbuffer head/tail.
- `Audio Buffer`
  - Percent fill computed from `BUF_CAPTURE_AUDIO` ringbuffer head/tail.

## Side channel stats panels (CH A / CH B)
Rendered by `render_channel_stats()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `Peak: +X% -Y%`
  - Based on `vu_*.peak_pos/peak_neg` (VU peak-hold values, not instantaneous raw ADC values).
  - Source peaks are derived from extraction stats and then smoothed/held in `gui_app_update_vu_meters()`.
- `Clip: +N -M`
  - Cumulative clip counts from extraction thread:
    - positive clip when sample `>= 2047`
    - negative clip when sample `<= -2048`
- `RST` button
  - Clears that channel’s clip counters only.
- `Errors`
  - Displays `error_count_a` / `error_count_b`.
  - Current code resets these counters at capture start but does not increment them in active processing paths, so they remain `0` unless future wiring is added.
- During recording only:
  - `RAW: X MB`
    - From `recording_raw_a` / `recording_raw_b`.
  - `FLAC: Y MB` (FLAC mode only)
    - From `recording_compressed_a` / `recording_compressed_b`.
  - `Ratio: Zx` (FLAC mode only)
    - `raw_bytes / compressed_bytes`.

## Record counter placement
- Recording duration counter is currently in the bottom bar (left side), next to the red record indicator.
- Side channel panels currently carry per-channel recording size stats (`RAW/FLAC/Ratio`), not the global timer.

## Wait/drop counters: exact meaning
Wait/drop are backpressure metrics tied to ringbuffer write behavior.

### Buffer-manager definition
In `bufmgr_write_begin()` (`misrc_tools/common/buffer_manager.c`):
- `write_waits` increments when producer must wait for space.
- `write_drops` increments when write is dropped (immediate-drop policy or retries exhausted).

So:
- **wait** = write had to pause because buffer was full.
- **drop** = write could not be queued and was discarded.

### Policies by path
Default policies in `misrc_tools/common/buffer_manager.c`:
- `BUF_CAPTURE_RF`: wait up to 10 attempts × 5 ms, then drop.
- `BUF_CAPTURE_AUDIO`: immediate drop (no waiting).
- `BUF_RECORD_A/B`: default 200 × 5 ms, but GUI extract record path overrides this.
- `BUF_DISPLAY`: short wait, then drop (display is intentionally lossy).
GUI capture callback overrides in `misrc_tools/misrc_gui/input/gui_capture.c` (aligned to CLI timing):
- RF callback writes use `8 attempts × 1 ms`.
- Audio callback writes use `8 attempts × 1 ms`.

Record-path override in `misrc_tools/misrc_gui/processing/gui_extract.c`:
- `s_record_write_policy = immediate-drop (0 wait attempts, 0 ms timeout)`.
- This means recording writes never block extraction; when record buffers are full, record frames are dropped.

### Counters used by GUI app state
`gui_app_t` has:
- `rb_wait_count`
- `rb_drop_count`

Current behavior:
- `rb_wait_count` and `rb_drop_count` are updated from `BUF_CAPTURE_RF` buffer-manager write stats deltas each callback.
- in upstream mode, `rb_drop_count` can also be incremented by parsed hsdaoh overrun messages.

### Where wait/drop is visible today
- Capture stop log (`gui_app_stop_capture()`):
  - prints `waits` and `drops` from app-level counters.
- Recording stop logs (`gui_record_stop()`):
  - prints recording-session wait/drop totals computed from `BUF_RECORD_A/B` deltas.
  - also prints per-buffer `A` and `B` wait/drop deltas.
- Periodic debug log from buffer manager:
  - one-line per-buffer fill/wait/drop summary.

## Rawness and interpretation notes
- Most counters are monotonic event counts since capture start.
- `Missed` is an event count (“missed at least one frame” events), not an exact per-frame-loss total.
- `Missed` and `Errors` are intentionally debounced in GUI capture mode to avoid one-off transient spikes from dominating the UI readout.
- GUI capture also applies a callback-gap resync guard: if callback timing stalls for >100 ms (for example due to system/display interruptions), parser sync state is reset before continuing so stale parser state does not generate a long burst of follow-on errors.
- `Peak` in side panels is VU peak-hold representation, not raw unsmoothed instantaneous sample.
- Buffer percentages are instantaneous snapshot values.

## Screenshot section
Add GUI images below as needed.
