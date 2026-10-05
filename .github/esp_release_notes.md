**SDR++ ESP**: SDR++ with the ESP-SDR (ESP32-S3) source module and the on-chip 16 / 40 / 80 MHz spectrum mode. This is a fork build, not an official SDR++ release.

| Package | Platform |
| --- | --- |
| `sdrpp-esp_windows_x64.zip` | Windows 10/11 x64. Unzip and run `sdrpp.exe`. |
| `sdrpp-esp_macos_arm.zip` | macOS, Apple Silicon (`SDR++.app`). |
| `sdrpp-esp_debian_bookworm_amd64.deb` | Debian 12 x64. |
| `sdrpp-esp_ubuntu_noble_amd64.deb` | Ubuntu 24.04 x64. |
| `sdrpp-esp.apk` | Android. Use USB OTG to the board's native USB port. Installs next to the official SDR++. |

Notes:
- **Firmware:** the ESP32-S3 needs ESP-SDR firmware with the IQS stream. See the driver repository: https://github.com/z2labs/sdrpp-esp-sdr-source
- **SDRplay on macOS and Linux:** these packages have no SDRplay module, because the SDRplay 3.15 installers are no longer downloadable. The Windows package includes it.
- **The .deb packages** use the same package name as the official SDR++, so installing one replaces an official SDR++ installation.
- **Measurements** (phase noise, NBFM SINAD and sensitivity, stability, comparison with HackRF and BB60C) are in the driver repository's README and `docs/MEASUREMENTS.md`.

Built on SDR++ by Alexandre Rouma and ESP-SDR by Florian Euchner (ESPARGOS). Turbo Mode developed by Zoltan Doczi from https://www.z2labs.io
