# CMakeLists.txt for hsdaoh-rp2350 RP2350 - MISRC GUI (forked from harrypm)
cmake_minimum_required(VERSION 3.15)
project(misrc_gui_rp2350 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow include(CheckCPUArch) from harrypm cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/cmake")

# ========================================
# Platform detection (works in MSYS2/MinGW shells too)
# ========================================
if(WIN32 OR MINGW OR MSYS)
    set(IS_WINDOWS TRUE)
else()
    set(IS_WINDOWS FALSE)
endif()

# Windows 10 1803+ APIs needed for ringbuffer (VirtualAlloc2, MapViewOfFile3)
if(IS_WINDOWS)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000005)
else()
    # Ensure 64-bit file offsets on Linux/POSIX
    add_definitions(-D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE)
endif()

# ========================================
# HSDAOH dependency (user-supplied)
# ========================================
set(HSDAOH_INC "" CACHE PATH "Path to hsdaoh include directory (contains hsdaoh.h or hsdaoh/hsdaoh.h)")
set(HSDAOH_LIB "" CACHE FILEPATH "Path to hsdaoh library file (e.g. libhsdaoh.so or libhsdaoh.dll.a)")

if (NOT EXISTS "${HSDAOH_INC}")
    message(FATAL_ERROR "HSDAOH_INC is not set or does not exist. Set -DHSDAOH_INC=/path/to/include")
endif()

if (NOT EXISTS "${HSDAOH_LIB}")
    message(FATAL_ERROR "HSDAOH_LIB is not set or does not exist. Set -DHSDAOH_LIB=/path/to/libhsdaoh.dll.a")
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

# Generate version.h from template
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.h
    @ONLY
)

# Build options
option(USE_SYSTEM_RAYLIB "Use system raylib instead of downloading" OFF)

# ========================================
# Find Dependencies
# ========================================
if(IS_WINDOWS)
    find_package(PkgConfig QUIET)
else()
    find_package(PkgConfig REQUIRED)
endif()

if(PkgConfig_FOUND OR PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    pkg_check_modules(FLAC REQUIRED flac)
    pkg_check_modules(SOXR REQUIRED soxr)
    pkg_check_modules(FFTW3F QUIET fftw3f)
else()
    # Windows without pkg-config: user must supply paths manually
    set(LIBUSB_INCLUDE_DIRS "" CACHE PATH "libusb-1.0 include directory")
    set(LIBUSB_LIBRARIES "" CACHE FILEPATH "libusb-1.0 library file")
    set(FLAC_INCLUDE_DIRS "" CACHE PATH "FLAC include directory")
    set(FLAC_LIBRARIES "" CACHE FILEPATH "FLAC library file")
    set(SOXR_INCLUDE_DIRS "" CACHE PATH "soxr include directory")
    set(SOXR_LIBRARIES "" CACHE FILEPATH "soxr library file")
    set(FFTW3F_INCLUDE_DIRS "" CACHE PATH "fftw3f include directory (optional)")
    set(FFTW3F_LIBRARIES "" CACHE FILEPATH "fftw3f library file (optional)")
    if(EXISTS "${FFTW3F_LIBRARIES}")
        set(FFTW3F_FOUND TRUE)
    else()
        set(FFTW3F_FOUND FALSE)
    endif()
endif()

# Test if FLAC library actually has threading support... we need v1.5+
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
)

# Platform-specific capture implementation
if(NOT IS_WINDOWS)
    list(APPEND COMMON_SOURCES
        misrc_tools/misrc_capture/simple_capture/simple_capture_v4l2.c
    )
else()
    list(APPEND COMMON_SOURCES
        misrc_tools/misrc_capture/simple_capture/simple_capture_mediafoundation.c
    )
endif()

# Extract ASM (x86_64 optimizations)
if(EXISTS "${CMAKE_SOURCE_DIR}/misrc_tools/common/extract.asm")
    enable_language(ASM_NASM)

    if(IS_WINDOWS)
        set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
        set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> -f win64 -o <OBJECT> <SOURCE>")
    else()
        set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
        set(CMAKE_ASM_NASM_COMPILE_OBJECT "<CMAKE_ASM_NASM_COMPILER> -f elf64 -o <OBJECT> <SOURCE>")
    endif()

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

# CXADC support (CX2388x raw ADC capture card)
# Linux: gated on ALSA (for PCM1802 audio). Windows: always enabled (WASAPI built-in).
if(IS_WINDOWS)
    list(APPEND GUI_INPUT_SOURCES
        misrc_tools/misrc_gui/input/gui_cxadc.c
        misrc_tools/misrc_gui/input/gui_cxadc_pcm1802.c
    )
    set(ENABLE_CXADC TRUE)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND OR PKG_CONFIG_FOUND)
        pkg_check_modules(ALSA QUIET alsa)
    endif()
    if(ALSA_FOUND)
        list(APPEND GUI_INPUT_SOURCES
            misrc_tools/misrc_gui/input/gui_cxadc.c
            misrc_tools/misrc_gui/input/gui_cxadc_pcm1802.c
        )
        set(ENABLE_CXADC TRUE)
    else()
        set(ENABLE_CXADC FALSE)
    endif()
endif()

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

# ========================================
# Build the GUI executable
# ========================================
add_executable(misrc_gui
    ${COMMON_SOURCES}
    ${GUI_SOURCES}
)

target_include_directories(misrc_gui PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools
    ${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/common
    ${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/misrc_gui
    ${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/misrc_capture
    ${CMAKE_CURRENT_SOURCE_DIR}/misrc_tools/misrc_capture/simple_capture
    ${LIBUSB_INCLUDE_DIRS}
    ${FLAC_INCLUDE_DIRS}
    ${SOXR_INCLUDE_DIRS}
)

target_link_libraries(misrc_gui PRIVATE
    raylib
    hsdaoh::hsdaoh
    ${LIBUSB_LIBRARIES}
    ${FLAC_LIBRARIES}
    ${SOXR_LIBRARIES}
)

# ========================================
# Optional feature compile definitions
# ========================================
# FLAC is a required dependency - always enable runtime support
target_compile_definitions(misrc_gui PRIVATE LIBFLAC_ENABLED=1)

if(HAVE_FLAC_THREADING)
    target_compile_definitions(misrc_gui PRIVATE HAVE_FLAC_THREADING=1)
endif()

if(FFTW3F_FOUND)
    target_include_directories(misrc_gui PRIVATE ${FFTW3F_INCLUDE_DIRS})
    target_link_libraries(misrc_gui PRIVATE ${FFTW3F_LIBRARIES})
    target_compile_definitions(misrc_gui PRIVATE HAVE_FFTW3F=1 LIBFFTW_ENABLED=1)
endif()

if(ENABLE_CXADC)
    target_compile_definitions(misrc_gui PRIVATE ENABLE_CXADC=1)
    if(NOT IS_WINDOWS AND ALSA_FOUND)
        target_include_directories(misrc_gui PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(misrc_gui PRIVATE ${ALSA_LIBRARIES})
    endif()
    if(IS_WINDOWS)
        # avrt needed for AvSetMmThreadCharacteristics in PCM1802 WASAPI audio
        target_link_libraries(misrc_gui PRIVATE avrt)
    endif()
endif()

# ========================================
# Platform-specific libraries
# ========================================
if(IS_WINDOWS)
    target_link_libraries(misrc_gui PRIVATE winmm gdi32 opengl32 onecore mf mfplat mfreadwrite mfuuid ole32 strmiids)
else()
    target_link_libraries(misrc_gui PRIVATE m pthread)
endif()

install(TARGETS misrc_gui RUNTIME DESTINATION bin)

# ========================================
# Copy required DLLs to the build output directory
# ========================================
if(IS_WINDOWS)
    # Copy hsdaoh DLL
    find_file(HSDAOH_DLL
        NAMES hsdaoh.dll libhsdaoh.dll
        PATHS "${HSDAOH_LIBDIR}" "${HSDAOH_LIBDIR}/../bin"
        NO_DEFAULT_PATH
    )
    if(HSDAOH_DLL)
        add_custom_command(TARGET misrc_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HSDAOH_DLL}" "$<TARGET_FILE_DIR:misrc_gui>"
        )
    endif()

    # Copy MSYS2/MinGW runtime DLLs
    set(MINGW_DLLS
        libusb-1.0.dll
        libFLAC.dll
        libsoxr.dll
        libfftw3f-3.dll
        libogg-0.dll
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
        libgomp-1.dll
        libuvc.dll
    )

    foreach(dll ${MINGW_DLLS})
        find_file(${dll}_PATH
            NAMES ${dll}
            PATHS "C:/msys64/mingw64/bin"
            NO_DEFAULT_PATH
        )
        if(${dll}_PATH)
            add_custom_command(TARGET misrc_gui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${${dll}_PATH}" "$<TARGET_FILE_DIR:misrc_gui>"
            )
        endif()
    endforeach()
endif()