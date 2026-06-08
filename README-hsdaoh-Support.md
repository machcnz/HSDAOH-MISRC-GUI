# MISRC - Multi Input Simultaneous Raw RF Capture - Fork

**What is new in this fork:**
This fork adds hsdaoh support to misrc_gui for use with Steve-M 12-bit 40 MSPS capture. Changes are scoped to my fork for integration/testing.

- 
- 13/02/26  - Support hsdaoh/rp2350 status/warning/error messages in GUI
            - enables GUI-side error counting / status display (in addition to stderr logging)
            - ** Requires my hsdoah fork which adds a message callback path to enable 
                 GUI-side error counting / status display (in addition to stderr logging)
                [https://github.com/machcnz/hsdaoh]

- 01/02/26  - Edit capture path fix
            - Initial support targets the **single AD9226/PCM1802 variant** 
            (Sev5000 Pico2_12bitADC_PCMAudio): https://github.com/Sev5000/Pico2_12bitADC_PCMAudio

- [hsdaoh rp2350 version hardware:] (https://github.com/steve-m/hsdaoh-rp2350)

## Building hsdaoh-rp2350 MISRC GUI
### Prerequisites
Requires a 'modified-to-support-GUI hsdaoh' build & installed, which is also found in my repo - refer below instructions.  

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


**1) Build and install hsdaoh for MISRC_GUI_Support**
Refer Steve's instructions for building on other platforms, per the main readme

### Build on Windows
#### Install dependencies
- Install MSYS2 (https://www.msys2.org/)
- Start MSYS2 MINGW64 from the application menu

```console
# Update all packages
pacman -Suy

# Install the required dependencies:
pacman -S git zip mingw-w64-x86_64-libusb mingw-w64-x86_64-libwinpthread mingw-w64-x86_64-cc \
mingw-w64-x86_64-gcc-libs mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-flac
```

#### Build libuvc
```console
# Clone the repository:
git clone https://github.com/steve-m/libuvc.git
mkdir libuvc/build && cd libuvc/build
cmake ../ -DCMAKE_POLICY_VERSION_MINIMUM=3.10 -DCMAKE_INSTALL_PREFIX:PATH=/mingw64
cmake --build .
cmake --install .
```

#### Build hsdaoh/libhsdaoh
cd 'to your desired working top level directory'

git clone https://github.com/machcnz/hsdaoh.git
cd 
mkdir hsdaoh/build && cd hsdaoh/build
cmake ../
cmake --build .

**Validate successful build with these directories**
- Required for the gui build - note paths for step 2.
- i. HSDOAH-for-MISRC/hsdaoh/include   <--- this is the [-DHSDAOH_INC=/path/to/hsdaoh/include]
- ii.HSDOAH-for-MISRC/hsdaoh/build/src   <--- this is the [-DHSDAOH_INC=/path/to/hsdaoh/include]

  
**2) Build this GUI**
- cd [to your desired working top level directory]
  git clone https://github.com/machcnz/HSDAOH-MISRC-GUI.git
  cd
  mkdir HSDAOH-MISRC-GUI/build && cd HSDAOH-MISRC-GUI/build
  cd ..

**Example build**
cmake -S . -B build \
-DHSDAOH_INC="/c/temp/HSDOAH-for-MISRC/hsdaoh/include"   
-DHSDAOH_LIB="/c/temp/HSDOAH-for-MISRC/hsdaoh/build/src/libhsdaoh.dll.a"   
-DCMAKE_POLICY_VERSION_MINIMUM=3.5

**Build**
cmake -S . -B build \
  -DHSDAOH_INC=/path/to/hsdaoh/include \
  -DHSDAOH_LIB=/path/to/hsdaoh/lib/libhsdaoh.dll.a
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

**On successful build:**
cmake --build build -j

**Complete:** the misrc_gui.exe should now be found in the **/build** folder

**Note:**
- HSDAOH_INC must point to the directory that contains hsdaoh.h or hsdaoh/hsdaoh.h. (you build in step 1)
- HSDAOH_LIB must point to the actual library file libhsdaoh.dll.a (On linux this is libhsdaoh.xx)

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
