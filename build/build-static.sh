#!/bin/bash

# Get the directory where this script lives (the build directory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR"
REPO_ROOT="$(cd "$BUILD_DIR/.." && pwd)"
WORKSPACE="$BUILD_DIR/workspace"

# Change to build directory so all relative operations work
cd "$BUILD_DIR"

CFLAGS="-I$WORKSPACE/include"
LDFLAGS="-L$WORKSPACE/lib"
export PATH="${WORKSPACE}/bin:$PATH"
PKG_CONFIG_PATH="$WORKSPACE/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig"
export PKG_CONFIG_PATH

echo "Build directory: $BUILD_DIR"
echo "Repository root: $REPO_ROOT"
echo "Workspace: $WORKSPACE"

if [[ ("$OSTYPE" == "darwin"*) ]]; then
  export MACOSX_DEPLOYMENT_TARGET=10.15
  export MACOS_DEPLOYMENT_TARGET=10.15
  if [[ ("$(uname -m)" == "arm64") ]]; then
    export MACOSX_DEPLOYMENT_TARGET=11.0
    export MACOS_DEPLOYMENT_TARGET=11.0
  fi
fi

mkdir -p "$WORKSPACE"

# Build pkg-config (non-Windows only)
if [[ ("$OSTYPE" != "cygwin"*) && ("$OSTYPE" != "msys"*) ]]; then
  if [[ -f "${WORKSPACE}/bin/pkg-config" ]]; then
    echo "pkg-config already installed, skipping..."
  else
    echo "Building pkg-config..."
    if [[ ! -f "pkg-config-0.29.2.tar.gz" ]]; then
      curl -L --silent -o pkg-config-0.29.2.tar.gz "https://pkgconfig.freedesktop.org/releases/pkg-config-0.29.2.tar.gz"
    fi
    tar xzf pkg-config-0.29.2.tar.gz
    cd pkg-config-0.29.2
    if [[ "$OSTYPE" == "darwin"* ]]; then
      export CFLAGS="-Wno-error=int-conversion"
    fi
    ./configure --silent --prefix="${WORKSPACE}" --with-pc-path="${WORKSPACE}"/lib/pkgconfig --with-internal-glib
    make -j$(nproc)
    make install
    cd ../
  fi
fi

# Build libusb (macOS and Windows only)
if [[ ("$OSTYPE" == "darwin"*) || ("$OSTYPE" == "cygwin"*) || ("$OSTYPE" == "msys"*) ]]; then
  if [[ -f "${WORKSPACE}/lib/libusb-1.0.a" ]]; then
    echo "libusb already installed, skipping..."
  else
    echo "Building libusb..."
    if [[ ! -f "libusb-1.0.29.tar.bz2" ]]; then
      curl -L --silent -o "libusb-1.0.29.tar.bz2" "https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.tar.bz2"
    fi
    tar xjf libusb-1.0.29.tar.bz2
    cd libusb-1.0.29
    ./configure --prefix="${WORKSPACE}" --disable-shared --enable-static
    make -j$(nproc)
    make install
    cd ../
  fi
fi

# Build libuvc
if [[ -f "${WORKSPACE}/lib/libuvc.a" ]]; then
  echo "libuvc already installed, skipping..."
else
  echo "Building libuvc..."
  if [[ ("$OSTYPE" == "cygwin"*) || ("$OSTYPE" == "msys"*) ]]; then
    if [[ ! -f "libuvc-41d0e0403abc5356e6aaeda690329467ef8f3a31.tar.gz" ]]; then
      curl -L --silent -o "libuvc-41d0e0403abc5356e6aaeda690329467ef8f3a31.tar.gz" "https://github.com/steve-m/libuvc/archive/41d0e0403abc5356e6aaeda690329467ef8f3a31.tar.gz"
    fi
    tar xzf libuvc-41d0e0403abc5356e6aaeda690329467ef8f3a31.tar.gz
    cd libuvc-41d0e0403abc5356e6aaeda690329467ef8f3a31
  else
    if [[ ! -f "libuvc-0.0.7.tar.gz" ]]; then
      curl -L --silent -o "libuvc-0.0.7.tar.gz" "https://github.com/libuvc/libuvc/archive/refs/tags/v0.0.7.tar.gz"
    fi
    tar xzf libuvc-0.0.7.tar.gz
    cd libuvc-0.0.7
  fi
  # now we have to edit the cmake file...
  sed "s/BUILD_UVC_SHARED TRUE/BUILD_UVC_SHARED FALSE/g" CMakeLists.txt >CMakeLists.patched
  rm CMakeLists.txt
  sed "s/find_package(JpegPkg QUIET)//g" CMakeLists.patched >CMakeLists.txt
  mkdir -p build
  cd build
  cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DJPEG_FOUND=False -DBUILD_EXAMPLE=False -DBUILD_TEST=False -DCMAKE_INSTALL_PREFIX="${WORKSPACE}" ../
  if [[ ("$OSTYPE" == "cygwin"*) || ("$OSTYPE" == "msys"*) ]]; then
    cmake --build .
    cmake --install .
  else
    make -j$(nproc)
    make install
  fi
  cd ../../
fi

# Build FLAC
if [[ -f "${WORKSPACE}/lib/libFLAC.a" ]]; then
  echo "FLAC already installed, skipping..."
else
  echo "Building FLAC..."
  if [[ ! -f "flac-1.5.0.tar.xz" ]]; then
    curl -L --silent -o "flac-1.5.0.tar.xz" "https://github.com/xiph/flac/releases/download/1.5.0/flac-1.5.0.tar.xz"
  fi
  tar xf flac-1.5.0.tar.xz
  cd flac-1.5.0
  ./configure --disable-shared --enable-static --disable-ogg --disable-programs --disable-examples --prefix="${WORKSPACE}"
  make -j$(nproc)
  make install
  cd ../
fi

# Build soxr
if [[ -f "${WORKSPACE}/lib/libsoxr.a" ]]; then
  echo "soxr already installed, skipping..."
else
  echo "Building soxr..."
  if [[ ! -f "soxr-437e06c739eb825f229b58fa50a565f33f82cbd3.tar.gz" ]]; then
    curl -L --silent -o "soxr-437e06c739eb825f229b58fa50a565f33f82cbd3.tar.gz" "https://github.com/Stefan-Olt/soxr/archive/437e06c739eb825f229b58fa50a565f33f82cbd3.tar.gz"
  fi
  tar xzf soxr-437e06c739eb825f229b58fa50a565f33f82cbd3.tar.gz
  cd soxr-437e06c739eb825f229b58fa50a565f33f82cbd3
  mkdir -p build
  cd build
  cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DWITH_OPENMP=OFF -DCMAKE_INSTALL_PREFIX="${WORKSPACE}" ../
  if [[ ("$OSTYPE" == "cygwin"*) || ("$OSTYPE" == "msys"*) ]]; then
    cmake --build .
    cmake --install .
  else
    make -j$(nproc)
    make install
  fi
  cd ../../
fi

# Build hsdaoh
if [[ -f "${WORKSPACE}/lib/libhsdaoh.a" ]]; then
  echo "hsdaoh already installed, skipping..."
else
  echo "Building hsdaoh..."
  if [[ ! -f "hsdaoh-ecd5f835ffad911e7b0b73d905e70cddc898c1ab.tar.gz" ]]; then
    curl -L --silent -o "hsdaoh-ecd5f835ffad911e7b0b73d905e70cddc898c1ab.tar.gz" "https://github.com/Stefan-Olt/hsdaoh/archive/ecd5f835ffad911e7b0b73d905e70cddc898c1ab.tar.gz"
  fi
  tar xzf hsdaoh-ecd5f835ffad911e7b0b73d905e70cddc898c1ab.tar.gz
  cd hsdaoh-ecd5f835ffad911e7b0b73d905e70cddc898c1ab
  # I cannot get cmake to not build the shared library
  sed "s/SHARED/STATIC/g" ./src/CMakeLists.txt >./src/CMakeLists.txt.patched
  rm ./src/CMakeLists.txt
  cat ./src/CMakeLists.txt.patched | tr '\n' '\r' | sed -e 's/executables.*\r# Install/\r# Install/' | sed -e 's/install(TARGETS hsdaoh_file.*)//' | tr '\r' '\n' > ./src/CMakeLists.txt
  mkdir -p build
  cd build
  cmake -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="${WORKSPACE}" -DINSTALL_UDEV_RULES=False ../
  if [[ ("$OSTYPE" == "cygwin"*) || ("$OSTYPE" == "msys"*) ]]; then
    cmake --build .
    cmake --install .
  else
    make -j$(nproc)
    make install
  fi
  cd ../../
fi

# Build raylib (static)
if [[ -f "${WORKSPACE}/lib/libraylib.a" ]]; then
  echo "raylib already installed, skipping..."
else
  echo "Building raylib..."
  if [[ ! -f "raylib-5.5.tar.gz" ]]; then
    curl -L --silent -o "raylib-5.5.tar.gz" "https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz"
  fi
  tar xzf raylib-5.5.tar.gz
  cd raylib-5.5
  mkdir -p build
  cd build
  cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="${WORKSPACE}" -DCMAKE_BUILD_TYPE=Release ../
  if [[ ("$OSTYPE" == "cygwin"*) || ("$OSTYPE" == "msys"*) ]]; then
    cmake --build . --config Release
    cmake --install .
  else
    make -j$(nproc)
    make install
  fi
  cd ../../
fi

# Build FFTW3 (single precision, static)
if [[ -f "${WORKSPACE}/lib/libfftw3f.a" ]]; then
  echo "FFTW3 already installed, skipping..."
else
  echo "Building FFTW3..."
  if [[ ! -f "fftw-3.3.10.tar.gz" ]]; then
    curl -L --silent -o "fftw-3.3.10.tar.gz" "https://www.fftw.org/fftw-3.3.10.tar.gz"
  fi
  tar xzf fftw-3.3.10.tar.gz
  cd fftw-3.3.10
  ./configure --prefix="${WORKSPACE}" --disable-shared --enable-static --enable-float
  make -j$(nproc)
  make install
  cd ../
fi

# Download Clay (header-only library)
if [[ -f "${WORKSPACE}/include/clay.h" ]]; then
  echo "Clay already installed, skipping..."
else
  echo "Downloading Clay..."
  #https://raw.githubusercontent.com/nicbarker/clay/refs/heads/main/clay.h
  #https://raw.githubusercontent.com/nicbarker/clay/refs/tags/v0.14/clay.h
  curl -L --silent -o "clay.h" "https://raw.githubusercontent.com/nicbarker/clay/refs/heads/main/clay.h"
  mkdir -p "${WORKSPACE}/include"
  cp clay.h "${WORKSPACE}/include/"
fi

# ----------------------------------------------------------------------------
# Embed fonts: unzip assets and generate C headers for standalone GUI
# Inter font for general UI, Space Mono for monospace sections
# ----------------------------------------------------------------------------
ASSETS_DIR="$REPO_ROOT/assets/fonts"
INTER_ZIP="$ASSETS_DIR/Inter.zip"
SPACE_MONO_ZIP="$ASSETS_DIR/Space_Mono.zip"
GEN_SCRIPT="$ASSETS_DIR/generate_font_header.py"

# Choose python executable for header generation
if command -v python3 >/dev/null 2>&1; then
  PYTHON=python3
elif command -v python >/dev/null 2>&1; then
  PYTHON=python
else
  PYTHON=""
fi

# Extract Inter font if present
if [[ -f "$INTER_ZIP" ]]; then
  mkdir -p "$ASSETS_DIR"
  # Extract using unzip only (no Python fallback)
  if command -v unzip >/dev/null 2>&1; then
    if unzip -o "$INTER_ZIP" -d "$ASSETS_DIR"; then
      echo "Extracted Inter font"
    fi
  else
    echo "Warning: 'unzip' not found; skipping Inter font extraction"
  fi
fi

# Extract Space Mono font if present
if [[ -f "$SPACE_MONO_ZIP" ]]; then
  mkdir -p "$ASSETS_DIR"
  # Extract using unzip only (no Python fallback)
  if command -v unzip >/dev/null 2>&1; then
    if unzip -o "$SPACE_MONO_ZIP" -d "$ASSETS_DIR"; then
      echo "Extracted Space Mono font"
    fi
  else
    echo "Warning: 'unzip' not found; skipping Space Mono font extraction"
  fi
fi

# Generate both embedded font headers if script and Python exist
if [[ -n "$PYTHON" && -f "$GEN_SCRIPT" ]]; then
  echo "Generating embedded font headers via $GEN_SCRIPT"
  if (cd "$REPO_ROOT" && "$PYTHON" "$GEN_SCRIPT"); then
    echo "Font headers generated successfully"
    # Cleanup extracted files after successful header generation
    echo "Cleaning up extracted font files..."
    rm -rf "$ASSETS_DIR/static" 2>/dev/null || true
    rm -f "$ASSETS_DIR"/*.ttf "$ASSETS_DIR"/OFL.txt "$ASSETS_DIR"/README.txt 2>/dev/null || true
  else
    echo "Warning: Font header generation failed"
  fi
else
  echo "Warning: Python or generate_font_header.py not found; skipping font header generation"
fi

cd "$REPO_ROOT/misrc_tools"
meson setup "$BUILD_DIR/misrc" --prefix="${WORKSPACE}" --buildtype=release --default-library=static --libdir="${WORKSPACE}/lib"
ninja -C "$BUILD_DIR/misrc"
ninja -C "$BUILD_DIR/misrc" install
