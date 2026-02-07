# CMakeLists.txt for hsdaoh-rp2350 RP2350 - MISRC GUI (forked from harrypm)
cmake_minimum_required(VERSION 3.15)
project(misrc_gui_rp2350 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow include(CheckCPUArch) from harrypm cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/cmake")

# ========================================
# HSDAOH dependency (user-supplied)
# ========================================
set(HSDAOH_INC "" CACHE PATH "Path to hsdaoh include directory (contains hsdaoh.h or hsdaoh/hsdaoh.h)")
set(HSDAOH_LIB "" CACHE FILEPATH "Path to hsdaoh library file (e.g. libhsdaoh.so or libhsdaoh.so.0)")

if (NOT EXISTS "${HSDAOH_INC}")
    message(FATAL_ERROR "HSDAOH_INC is not set or does not exist. Set -DHSDAOH_INC=/path/to/include")
endif()

if (NOT EXISTS "${HSDAOH_LIB}")
    message(FATAL_ERROR "HSDAOH_LIB is not set or does not exist. Set -DHSDAOH_LIB=/path/to/libhsdaoh.so")
endif()

add_library(hsdaoh::hsdaoh UNKNOWN IMPORTED)
set_target_properties(hsdaoh::hsdaoh PROPERTIES
    IMPORTED_LOCATION "${HSDAOH_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${HSDAOH_INC}"
)

# For runtime safety: ensure we load the same hsdaoh we linked against
get_filename_component(HSDAOH_LIBDIR "${HSDAOH_LIB}" DIRECTORY)

# Enable upstream mode for RP2350
add_definitions(-DHSDAOH_UPSTREAM=1)

# Build options
option(USE_SYSTEM_RAYLIB "Use system raylib instead of downloading" OFF)

# ========================================
# Find Dependencies
# ========================================
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(FLAC REQUIRED flac)
pkg_check_modules(SOXR REQUIRED soxr)
pkg_check_modules(FFTW3F QUIET fftw3f)

# Test if FLAC library actually has threading support... we need v1.5+
# (Version doesn't guarantee the feature - depends on build flags)
include(CheckLibraryExists)
set(CMAKE_REQUIRED_LIBRARIES ${FLAC_LIBRARIES})
check_library_exists(FLAC FLAC__stream_encoder_set_num_threads "" HAVE_FLAC_THREADING)

if(HAVE_FLAC_THREADING)
    message(STATUS "FLAC threading support: YES")
else()
    message(STATUS "FLAC threading support: NO (library built without pthread)")
endif()

# ========================================
# Raylib
# ========================================
if(USE_SYSTEM_RAYLIB)
    find_package(raylib 5.0 REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.0
        GIT_PROGRESS TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

# ========================================
# Common Sources
# ========================================
set(COMMON_SOURCES
    misrc_tools/common/ringbuffer.c
    misrc_tools/common/flac_writer.c
    misrc_tools/common/buffer_manager.c
    misrc_tools/common/extract.c
    misrc_tools/common/rb_event.c
    misrc_tools/common/file_utils.c
    misrc_tools/common/device_enum.c
    misrc_tools/common/ringbuffer_writer.c

    # Fix unresolved sc_get_* symbols (Linux implementation)
    misrc_tools/misrc_capture/simple_capture/simple_capture_v4l2.c
)

# Extract ASM (x86_64 optimizations) must be before add_executable
if(EXISTS "${CMAKE_SOURCE_DIR}/misrc_tools/common/extract.asm")
    enable_language(ASM_NASM)

    # CRITICAL: NASM needs its own compile command, not C compiler flags
    set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
    set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> -f elf64 -o <OBJECT> <SOURCE>")

    list(APPEND COMMON_SOURCES misrc_tools/common/extract.asm)
endif()

# ========================================
# GUI Sources
# ========================================
file(GLOB GUI_CORE_SOURCES
    misrc_tools/misrc_gui/core/*.c
)

set(GUI_INPUT_SOURCES
    misrc_tools/misrc_gui/input/gui_capture.c
    misrc_tools/misrc_gui/input/gui_simulated.c
    misrc_tools/misrc_gui/input/gui_playback.c
)

file(GLOB GUI_OUTPUT_SOURCES
    misrc_tools/misrc_gui/output/*.c
)

file(GLOB GUI_PROCESSING_SOURCES
    misrc_tools/misrc_gui/processing/*.c
)

file(GLOB GUI_SIGNAL_SOURCES
    misrc_tools/misrc_gui/signal/*.c
)

file(GLOB GUI_UI_SOURCES
    misrc_tools/misrc_gui/ui/*.c
)

file(GLOB GUI_VIZ_SOURCES
    misrc_tools/misrc_gui/visualization/*.c
)

set(GUI_SOURCES
    ${GUI_CORE_SOURCES}
    ${GUI_INPUT_SOURCES}
    ${GUI_OUTPUT_SOURCES}
    ${GUI_PROCESSING_SOURCES}
    ${GUI_SIGNAL_SOURCES}
    ${GUI_UI_SOURCES}
    ${GUI_VIZ_SOURCES}
)

# =======
