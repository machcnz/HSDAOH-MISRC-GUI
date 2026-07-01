**Windows CXCard & Clockgen support**

Note:
- Windows - untested on other platforms
-- Required https://github.com/JuniorIsAJitterbug/cxadc-win/tree/master Driver and ClockGen Firmware
- PCM audio clock at default of 48000
- Use JuniorIsAJitterbugs' powershell tools to configure clockgen clks
-- Set-CxadcClockGenRate -Output Clock0 -Rate 40mhz
-- Set-CxadcClockGenRate -Output Clock1 -Rate 40mhz
-- Set-CxadcClockGenRate -Output ADC -Rate 48000 (note: **Not 46875**
- Audio levels and monitoring working
- MISRC_GUI FFT & Wave/level and clip % display working & correct
- Cmake build - meson modified, meson needs testing
- Plus: Clean, doesnt require cx server or GNU radio.
- Fixed a couple of bugs:
-- MISRC GUI Start up Disconnected
-- Menu pulldown to select cxcard
MA 01.07.2026
