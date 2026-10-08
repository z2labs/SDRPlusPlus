**Z2 SDR** is a touch-friendly fork of SDR++ for Android and desktop, with the ESP-SDR (ESP32-S3) source module: real IQ and the on-chip 16 / 40 / 80 MHz spectrum mode over the plain USB cable. It is not an official SDR++ release.

### New in 0.3.6
- **4096 FFT bins** in spectrum mode (16 / 40 / 80 MHz), on ESP32-S3 modules with PSRAM (e.g. N16R8). Update the board with **Install firmware**, then pick "4096 (PSRAM boards)" under FFT bins. Boards without PSRAM keep 2048; the module falls back automatically.
- Bundled firmware `4128c24`: SPEC buffers rearranged (heap instead of fixed RAM); both cores in use as before.

### 0.3.5
- **Menu:** after you use it, it stays out for a few seconds after the mouse leaves. **Pin menu** keeps it docked next to the spectrum; **Auto-hide menu** at its top folds it away again.
- Packages are named `z2sdr_*`. The Linux package is now `z2sdr` (it replaces an installed `sdrpp` package, because both install the same files). macOS: `Z2SDR.app`.

### 0.3.4
- **Desktop: the menu hides itself.** It folds away to the left edge, so the spectrum and waterfall use the full width. Move the mouse to the left edge or click the menu button to slide it in over the spectrum; it folds away again when the mouse leaves. Switch it off under Display, "Auto-hide menu".
- A fresh install starts on the ESP-SDR source (it used to start on Airspy).

### 0.3.3
- **IQ tuning no longer stops the stream** (bundled firmware `c9fbc1d`). Each retune costs about 11 ms of samples. Update the board with **Install firmware** in the source menu.
- **The board is found without pressing reset:** the serial port opens with DTR/RTS deasserted.
- New name (Z2 SDR) and icon. The Android package is now `io.z2labs.z2sdr`, so it installs as a separate app next to the old "SDR++ ESP".

### Earlier (0.3.x)
- Firmware update over USB from the app (Android OTG and desktop), with automatic restart.
- Desktop: playback starts on launch, hot-plug detection of the board, Auto range button, log file and **Export debug log**.
- **Spectrum Export** module: FFT snapshots (average / max hold) and continuous logs as CSV, also in spectrum mode.
- Android touch layout: full-screen waterfall, floating controls, pinch zoom, automatic FFT / waterfall range.

| Package | Platform |
| --- | --- |
| `z2sdr_windows_x64.zip` | Windows 10/11 x64. Unzip and run `sdrpp.exe`. |
| `z2sdr_macos_arm.zip` | macOS, Apple Silicon. |
| `z2sdr_debian_bookworm_amd64.deb` | Debian 12 x64. |
| `z2sdr_ubuntu_noble_amd64.deb` | Ubuntu 24.04 x64. |
| `z2sdr.apk` | Android (arm64). Use USB OTG to the board's native USB port. |

Notes:
- **Firmware:** the bundled firmware is installed from the source menu. Firmware source: https://github.com/zodoczi/esp-sdr (based on ESPARGOS/esp-sdr). Module and documentation: https://github.com/z2labs/sdrpp-esp-sdr-source
- **SDRplay on macOS and Linux:** these packages have no SDRplay module (the SDRplay 3.15 installers are no longer downloadable). The Windows package includes it.
- **The .deb packages** (package `z2sdr`) install the same files as the official SDR++, so installing one removes an installed `sdrpp` package.

Built on SDR++ by Alexandre Rouma and ESP-SDR by Florian Euchner (ESPARGOS); GPL-3.0. Z2 SDR by Zoltan Doczi, https://www.z2labs.io
