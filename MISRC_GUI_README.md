# MISRC GUI README
This document contains expanded GUI/tool package and source-build notes moved out of `README.md`.

The packages contain two command-line applications, `misrc_capture` and `misrc_extract`. For detailed usage information see the [misrc_tools readme](misrc_tools/README.md) and the [usage example](README.md#capture--usage-example).

If you want to build the tools yourself, see the instructions in the [misrc_tools readme](misrc_tools/README.md).

## Source build dependency note (FFT)
`misrc_gui` requires FFT support via FFTW single-precision (`fftw3f`). Source builds now fail at configure time if FFTW is missing.

Install FFTW development packages before running Meson:

- Debian/Ubuntu/Linux Mint: `libfftw3-dev`
- macOS (Homebrew): `fftw`
- MSYS2 MinGW x86_64: `mingw-w64-x86_64-fftw`
- MSYS2 MinGW arm64: `mingw-w64-clang-aarch64-fftw`

If you already configured a Meson build directory before installing FFTW, wipe and reconfigure so stale dependency paths are removed:

    meson setup --wipe misrc_tools/build misrc_tools

## Local AppImage test build (Ubuntu 22.04 baseline)
For a reproducible local AppImage build that targets a `glibc 2.35` baseline, run:

    ./scripts/build-appimage-local.sh

The script uses `docker` or `podman` (prefers docker if both are installed), installs build dependencies in an `ubuntu:22.04` container, and writes the output AppImage to `.ci-artifacts/linux-appimage/`.
If you want to run directly on your host (with all dependencies already installed), use:

    ./scripts/build-appimage-local.sh --native
