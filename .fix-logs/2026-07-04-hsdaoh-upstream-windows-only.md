# Fix log — HSDAOH_UPSTREAM Windows-only (restore v1.0.9 MISRC + ChB on Linux/macOS)

**Date:** 2026-07-04
**Commit:** 96f6fb7 (on harrypm/main)
**Tag context:** v1.1.0 regression fix, post-PR #12
**Status:** CONFIRMED WORKING on real hardware by user (harrypm)

## Problem
v1.1.0 (PR #12, commit 2eaec01 "Add additional GUI linker flags for Windows") added
`cflags += ['-DHSDAOH_UPSTREAM=1']` as a GLOBAL define across all platforms. The
HSDAOH_UPSTREAM code path in `misrc_tools/misrc_gui/input/gui_capture.c` is an
incomplete WIP reference implementation:
  1. hard-disables MISRC simple-capture mode (~line 1407: returns -1 with
     "Simple-capture unavailable in HSDAOH_UPSTREAM build")
  2. has NO stream_id==1 (ChB) handler in gui_capture_upstream_callback -> ChB dropped
  3. skips gui_capture_configure_handler(app, true) -> frame parser never initialized

Result on Linux/macOS: HSDAOH/MISRC modes failed to connect (0 frames, errors only),
CXADC ChB broken.

## Fix
`misrc_tools/meson.build`: moved `cflags += ['-DHSDAOH_UPSTREAM=1']` out of the global
cflags block (line 20) and into the Windows-only `if host_system == 'windows' or 'cygwin'`
block (line 255). Windows keeps PR #12's connect fix verbatim; Linux/macOS restore the
complete v1.0.9 MISRC raw-callback + frame-parser path (both channels).
The Windows -lavrt/-lksuser GUI linker flags from the same commit were already
Windows-gated and unchanged.

## Also added (AGENTS.MD, lines 29-32)
Four HARD CI MIRROR RULES after discovering a local-build validity gap:
`scripts/build-appimage-local.sh --native` on a host with /usr/local/lib/libhsdaoh.so
bundled a DIFFERENT libhsdaoh (sha 4b1204a0, has hsdaoh_open2) than the v1.0.9 CI
release AppImage (sha f1d3b785, vendored, no hsdaoh_open2), because the 997cf05
vendored-hsdaoh rpath was shadowed by the system libFLAC's /usr/local/lib rpath.
Local builds that bundle system libs are NOT representative of CI/published artifacts;
tests against them are invalid. Must verify bundled-dep sha256 matches CI release
artifact before reporting a local test as fix-confirmation.

## Validation (CI-built, representative)
- CI run: https://github.com/harrypm/MISRC/actions/runs/28713351781 (commit 96f6fb7)
- Artifact: linux_MISRC_dev-96f6fb7_x86.AppImage (linux-zip-x86_64)
- Bundled libhsdaoh sha256: f1d3b785be0500bf04a58a01e4c8ef5dee1638af9cf041250283e9c1501ac389
  (byte-identical to v1.0.9 release AppImage bundled libhsdaoh - REPRESENTATIVE)
- Hardware test log: /tmp/misrc-fix-test.log (read via command precmd-...-82)

### Before (v1.1.0 broken, /tmp/misrc-gui-run.log):
  [GUI] Opening hsdaoh device...
  [GUI] Starting stream...
  attempt to claim already-claimed interface 1
  [GUI] Capture stopped: 0 frames, 0 missed, 131 errors (parser=0, system=131), 0 waits, 0 drops

### After (fix, /tmp/misrc-fix-test.log):
  [GUI] Opening hsdaoh device...
  [GUI] Starting stream...
  attempt to claim already-claimed interface 1   <- benign, hsdaoh internal retry recovers
  Detected processor with SSE4.1, using optimized 8 bit repacking routine
  [GUI] Capture stopped: 257 frames, 0 missed, 0 errors (parser=0, system=0), 0 waits, 0 drops
  ... CXADC ...
  [GUI] Capture stopped: 4483 frames, 0 missed, 0 errors (parser=0, system=0), 0 waits, 0 drops
  ... HSDAOH long run ...
  [GUI] Capture stopped: 24933 frames, 0 missed, 0 errors (parser=0, system=0), 0 waits, 0 drops

Both HSDAOH and CXADC now connect and capture with 0 errors, 0 missed, 0 drops.
