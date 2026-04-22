# MISRC GUI development notes

Recent capture regressions showed that small callback-gating changes can silently break GUI feeds. Keep the following constraints in mind when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when `result.error_count > 0 && result.report_errors`.
  Do not reject tolerated CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null checks), before width/height early returns.
  This prevents false timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start, explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO -> gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`, and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.

## 2026-04-16 capture/runtime snapshot

- Timestamp (UTC): `2026-04-16T03:38:18Z`
- OS: `Linux Mint 21.3`
- System: `Linux 5.15.0-173-generic x86_64 GNU/Linux`
- Branch: `heads/misrc_gui_dev`
- Commit: `48054ea`
- Stability note: current version is running stable for 8+ hours.

## 2026-04-19 local AppImage build note

- Local AppImage builds are now reproducible and passing smoke tests using:
  - `./scripts/build-appimage-local.sh`
- Default mode runs in an `ubuntu:22.04` container (`docker`/`podman`) to keep a portable glibc baseline.
- Script output location:
  - `.ci-artifacts/linux-appimage/`
- Verified locally:
  - AppImage artifact builds successfully.
  - `APPIMAGE_EXTRACT_AND_RUN=1 <AppImage> --smoke-test` passes.
  - Direct run `<AppImage> --smoke-test` passes on host.
## 2026-04-22 macOS Apple Silicon capture scheduling fix

- Symptom: on M-series Macs, capture/recording workloads could remain on efficiency cores, causing immediate drops/errors under GUI load.
- Root cause pattern: process-priority/QoS promotion was happening too late (after stream startup), so transport/ingest workers did not reliably inherit elevated scheduling class.
- Changes made:
  - `misrc_tools/common/threading.h`
    - Apple Silicon maps `THRD_PRIORITY_ABOVE+` to `QOS_CLASS_USER_INTERACTIVE`.
    - Added macOS QoS hinting in `proc_set_priority(...)`.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before `sc_start_capture(...)` / `hsdaoh_start_stream(...)`.
    - Roll back to `PROC_PRIORITY_NORMAL` on startup failure and on capture stop.
  - `misrc_tools/misrc_gui/output/gui_record.c`
    - Move FLAC-recording priority promotion to before encoder/worker creation.
    - Restore normal priority on FLAC init failure paths.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before stream startup.
    - Restore normal priority on shutdown/startup-failure exit paths.
- Validation:
  - GUI soak run (`--debug-view`) for ~331.9s showed:
    - `waits=0`, `drops=0`
    - record buffers: A waits/drops `0/0`, B waits/drops `0/0`
    - no capture/dropout instability lines during soak window.

## 2026-04-22 macOS scheduling regression repair (post-rebase)

- Issue: after rebasing to `main`, a conflict-resolution mistake in `misrc_tools/common/threading.h` weakened macOS QoS escalation for capture-critical threads and reduced the effectiveness of Apple Silicon core placement.
- Corrective changes:
  - `misrc_tools/common/threading.h`
    - restored clean separation between `thrd_set_priority(...)` and `proc_set_priority(...)` QoS logic.
    - strengthened macOS QoS calls by adding non-zero relative priority for `ABOVE/HIGH/CRITICAL` levels.
    - added Mach thread precedence (`THREAD_PRECEDENCE_POLICY`) alongside QoS for capture-critical caller threads, avoiding blanket process-wide escalation of unrelated threads.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - move `proc_set_priority(PROC_PRIORITY_ABOVE)` earlier in HSDAOH startup (before `hsdaoh_open`/`hsdaoh_alloc`/`hsdaoh_open2`) so early transport/open threads inherit elevated scheduling.
    - rollback to `PROC_PRIORITY_NORMAL` on all HSDAOH open/alloc failure exits.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirror earlier process-priority elevation for CLI HSDAOH path before `hsdaoh_alloc/open2`, with rollback on failure exits.
    - removed accidental early option-parse priority side effect so elevation only happens at real capture startup intent.

## 2026-04-22 macOS callback-thread scheduling follow-up

- Issue: callback-priority promotion was previously guarded by a single process-wide one-shot flag, so only the first callback thread was guaranteed to be elevated.
- Corrective changes:
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - changed callback promotion to thread-local one-shot (`MISRC_THREAD_LOCAL`) so each callback worker thread self-promotes once to `THRD_PRIORITY_CRITICAL`.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirrored the same thread-local callback-promotion behavior in CLI capture callback path.
  - `misrc_tools/common/threading.h`
    - in macOS `proc_set_priority(...)`, return early when `pthread_set_qos_class_self_np(...)` succeeds (after setting caller-thread precedence), avoiding unnecessary fallthrough into process `nice` fallback that can fail with EPERM on non-root runs.
- Runtime check (privileged CLI path):
  - successful `hsdaoh` capture runs with `waits=0`, `rf_drops=0`, `audio_drops=0`.
  - sampled `powermetrics --samplers cpu_power` during capture showed sustained higher P-cluster activity than E-cluster activity.
