# MISRC - Multi Input Simultaneous Raw RF Capture - Fork

**What is new in this fork:**
This fork adds hsdaoh support to misrc_gui for use with Steve-M 12-bit 40 MSPS capture. Changes are scoped to my fork for integration/testing.
- 12/05/26  - v1.0.5 release summary (tag review)
            - Linux: added FLAC core pinning controls and capture-session metadata logging.
            - CVBS: added Active/Full geometry toggle, PAL-default Active mode tuning, and follow-up crop/contrast fixes.
            - Recording and status: added free-space guard, real-time backpressure/drop logging, spill handling updates, and refined status counters.
            - UI: adjusted record-limit/timecode layout, restored toolbar/default launch width behavior, and added record-clock icon rendering.
            - Build/runtime fixes: Unix build fixes plus Windows include guards for Win32 symbols in recording paths.

- 12/05/26  - Windows ARM64 build artifacts
            - GitHub Actions release pipeline now also emits `windows_MISRC_<version>_arm64.zip`.
            - ARM64 builds are currently cross-built/validated structurally in CI and should still be treated as experimental at runtime.
- 07/05/26  - Error counter behavior update
            - Restored status-bar counter to a single combined `Errors:` value for users.
            - Kept internal parser/system event classification for logging and diagnostics.

- 16/02/25  - Fix Windows settings saving and minor path issues. Release portable exe - refer Releases
  
- 13/02/26  - Support hsdaoh/rp2350 status/warning/error messages in GUI
            - enables GUI-side error counting / status display (in addition to stderr logging)
            - ** Requires my hsdoah fork which adds a message callback path to enable 
                 GUI-side error counting / status display (in addition to stderr logging)
                [https://github.com/machcnz/hsdaoh]

- 08/06/26  - Edit capture path fix
            - Initial support targets the **single AD9226/PCM1802 variant** 
            (Sev5000 Pico2_12bitADC_PCMAudio): https://github.com/Sev5000/Pico2_12bitADC_PCMAudio

- [hsdaoh rp2350 version hardware:] (https://github.com/steve-m/hsdaoh-rp2350)

## Building hsdaoh-rp2350 MISRC GUI
### Prerequisites
Steve-M's libhsdaoh must be installed or built locally - refer below for detail.

### Porting notes:
1. Implements Steve Markgraf's hsdaoh API in upstream mode via a compile-time switch
- hsdaoh_start_stream(dev, cb, ctx, buf_num) (4 args)
- callback gets hsdaoh_data_info_t with stream_id, len, buf, srate, et
- hsdaoh firmware/upstream library is aware of CRC and tracks any errors - (metadata struct) generating CRC16 per line, pipelines CRC accumulation, stores saves_crc, & writes into the HDMI line buffer at next_line[RBUF_SLICE_LEN - 2]
- Essentially - Open the device, start a stream, receives callbacks per stream (with stream_id and buffer length), treating the callback byffer as payload for that stream (RF, PCM1802 audio..)

2. Misrc notes:
- Raw frame callback implementation
- Callbacks represent frame buffers
- host code extract metadata and payload out of these frames



**1) Build and install upstream hsdaoh (steve-m)**
Example: build from source in your desired path
https://github.com/steve-m/hsdaoh

git clone https://github.com/steve-m/hsdaoh.git
cd hsdaoh
mkdir -p build
cd build
cmake .. -DINSTALL_UDEV_RULES=ON
make -j 4
sudo make install
sudo ldconfig

If you prefer an isolated install:
git clone https://github.com/steve-m/hsdaoh.git
cmake -S hsdaoh -B hsdaoh/build -DINSTALL_UDEV_RULES=ON -DCMAKE_INSTALL_PREFIX=$HOME/opt/hsdaoh-rp2350
cmake --build hsdaoh/build -j
cmake --install hsdaoh/build

**2) Build this GUI**
Recommended build (from repo root)
cd /path/to/HSDAOH-MISRC-GUI-misrc_gui_dev

cmake -S . -B build \
  -DHSDAOH_INC=/path/to/hsdaoh/include \
  -DHSDAOH_LIB=/path/to/hsdaoh/lib/libhsdaoh.so

cmake --build build -j

**Note:**
- HSDAOH_INC must point to the directory that contains hsdaoh.h or hsdaoh/hsdaoh.h. (you build in step 1)
- HSDAOH_LIB must point to the actual library file you want to link against (e.g. libhsdaoh.so or libhsdaoh.so.0).

----------------------------------------------------------------------------------------------------------------------------------------

## Description
MISRC & HSDAOH are devices that directly capture raw video signals (RF) directly from the Video Heads, including Hi-FI Audio, or other legacy video format hardware such as Laser Disc, Beta Hi8 etc.
- MISRC is purpose build, based on HSDAOH

**MISRC GUI** replaces lengthy command lines and provides users tools and indication of progress or issues with the capture.

## The Decode Projects:

- [VHS-Decode - Start Here](https://github.com/oyvindln/vhs-decode/wiki)
- [Build your own HSDAOH for MISRC GUI - Start here](https://github.com/Sev5000/Pico2_12bitADC_PCMAudio)

-----------

### Media Support
| Media RF Type | MISRC Support |
| ------------- | ------------- |
| Video FM RF   | Yes           |
| HiFi FM RF    | Yes           |
| CVBS RF       | Yes           |
| S-Video RF    | Yes           |

Real world examples:
- Capture 2x CVBS
- Capture 1x S-Video (Y & C)
- Capture Video RF and HiFi RF simultaneously
- Capture Video RF and CVBS simultaneously
- Capture 4ch of 24-bit 48khz audio with AUX pins via integrated or external clock-locked ADCs


## Hardware
- MISRC - Refer to the VHS-Decode project [here](https://github.com/Stefan-Olt/MISRC)
- HSDAOH RP2350 - [Here](https://github.com/steve-m/hsdaoh-rp2350?tab=readme-ov-file)

> [!TIP]  
> You can support the development and production of the MISRC platform [here](https://github.com/Stefan-Olt/MISRC/wiki/Donations).
- PCB: 20-30USD
- Parts 100-150USD
- Single unit total production cost is currently 260-300USD.
- [Order a V1.5 Development MISRC](https://github.com/Stefan-Olt/MISRC/wiki/Fabrication)

------


## License

The hardware, firmware and software is released under different open-source licenses.
You can read the [License here](https://github.com/Stefan-Olt/MISRC/wiki/Licenses)
