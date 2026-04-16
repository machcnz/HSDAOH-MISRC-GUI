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
