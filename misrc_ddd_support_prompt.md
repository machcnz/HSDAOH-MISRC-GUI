# MISRC DdD Support — Prompt Log

Per user rule: record all prompt input, reference context, file map, and commands run.

## Prompt input
- User: "I want to add DdD support, there is already FX3 support so support for the DdD should not be a hard mode to add. https://github.com/simoninns/DomesdayDuplicator"
- User follow-up: "It's native app has 16-bit signed support output, that would be the defacto alongside 8-bit downsampling."
- Decision: bit-match native DdD .raw 16-bit signed format (polarity-compensated pack) for ld-decode compatibility.

## DdD protocol reference (verified from local DomesDayDuplicator source)
- VID 0x1D50, PID 0x603B; USB 3.0 only; bulk IN endpoint 0x81 (CY_FX_EP_CONSUMER).
- No firmware upload: FX3 firmware + FPGA bitstream in onboard SPI flash.
- No explicit start command: native app sends only 0xB6 (config flags, bit 0 = test mode).
  Data flows automatically when bulk transfers submitted (GPIF/DMA started on SET_CONF).
- Raw 16-bit LE word: bits 0-9 = 10-bit unsigned ADC sample, bits 10-15 = 6-bit rolling seq number (wraps at 63).
- Native Signed16Bit conversion: (int16_t)(sample10 - 0x0200) << 6.
- Sample rate: 40 MSPS.

## Data mapping (bit-matches native DdD .raw)
- sample10 = word & 0x3FF (strips seq)
- Polarity-compensated 12-bit pack: sample12 = 4095 - (sample10 << 2)
- packed32 = sample12 (ch A low 12 bits, AUX=0, ch B=0)
- MISRC extract-pad: (2047 - sample12) << 4 == native (sample10 - 0x200) << 6
  - sample10=512 -> 0, sample10=0 -> -32768, sample10=1023 -> +32704 (all verified equal)

## File map
- NEW misrc_gui/input/gui_ddd.h — DdD device API header (ENABLE_DDD)
- NEW misrc_gui/input/gui_ddd.c — DdD device implementation (libusb, all platforms)
- EDIT common/device_enum.h — MISRC_DEVICE_TYPE_DDD + misrc_device_enumerate_ddd decl
- EDIT common/device_enum.c — misrc_device_enumerate_ddd impl
- EDIT misrc_gui/core/gui_app.h — DEVICE_TYPE_DDD + ddd_dev/thread/running fields
- EDIT misrc_gui/input/gui_capture.c — include, init, enumerate, start/stop blocks
- EDIT misrc_gui/output/gui_record.c — device type name "ddd"
- EDIT misrc_gui/ui/gui_ui.c — device type display name "DdD"
- EDIT meson.build — libusb-1.0 dep, ENABLE_DDD, gui_ddd.c in sources_gui

## Commands run
- `pkg-config --exists libusb-1.0` -> FOUND (1.0.25); `pkg-config --exists libcyusb` -> NOT FOUND
  (So on this host: ENABLE_DDD=1, ENABLE_FX3=0 — DdD compiles independently of FX3)
- `meson setup --reconfigure build` -> "libusb-1.0 found, building with DdD support"; FX3 off (no cyusb)
- `ninja -C build misrc_gui` -> exit 0, binary built (3.1 MiB)
- Forced recompile of gui_ddd.c/device_enum.c/gui_capture.c/gui_app.h: no -Wall/-Wextra/-Wshadow warnings
- `nm`/`strings` on build/misrc_gui: gui_ddd.c.o linked; DdD strings present ("[DdD] %s", "Domesday Duplicator (Bus %d, Addr %d)", etc.)

## Enum syntax fix applied
Initial build failed: leading-comma enum pattern (`,\n DEVICE_TYPE_DDD`) broke when ENABLE_FX3=0 because DEVICE_TYPE_PLAYBACK already had a trailing comma (`,,` error). Fixed by giving every conditional enum member a trailing comma instead (valid in C11 before `}`). Applied to both device_enum.h and gui_app.h.

## Status
Code compiles clean with ENABLE_DDD=1 on Linux Mint (libusb-1.0 1.0.25, no cyusb).

Hardware validation DONE on real DdD (Bus 2 Addr 3, SuperSpeed 5 Gbps, VID 1D50 PID 603B):
- Device enumerates as `[DdD] Domesday Duplicator` in dropdown (name trimmed, no Bus/Addr clutter)
- libusb_open succeeds as user harry (no root/udev issue)
- Connect starts capture; `[DdD] First data received: 1048576 bytes`; 288+ batches received
- User confirmed: "Working fine for initial implementation"

## Fixes applied during validation
1. Enum trailing-comma syntax (initial build broke when ENABLE_FX3=0 + ENABLE_DDD=1)
2. Reverted over-reach that forced DdD into HSDAOH mode (broke the feed by moving ch-A signal off the watched panel) — MISRC/HSDAOH user choice restored
3. Sequence-number validation: DdD seq is constant for 65536 samples then +1 (mod 64), NOT per-sample. Original per-sample check spammed missed/error counters on every sample. Fixed to only flag real skips/backward jumps.
4. Channel B row hidden on main preview for DdD (single-channel device) — channel A expands to fill preview height.
5. RF B toggle/bits/tag grayed out in settings + capture_b forced off (no empty B file on record).

## Restore point
Zip: /home/harry/ddd_initial_impl_restore_point.zip (DdD source files + prompt log, captured at confirmed-working state)
