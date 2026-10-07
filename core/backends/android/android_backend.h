#pragma once
#include <vector>
#include <stdint.h>

namespace backend {
    struct DevVIDPID {
        uint16_t vid;
        uint16_t pid;
    };

    extern const std::vector<DevVIDPID> AIRSPY_VIDPIDS;
    extern const std::vector<DevVIDPID> AIRSPYHF_VIDPIDS;
    extern const std::vector<DevVIDPID> HACKRF_VIDPIDS;
    extern const std::vector<DevVIDPID> HYDRASDR_VIDPIDS;
    extern const std::vector<DevVIDPID> RTL_SDR_VIDPIDS;

    // Default UI scale derived from the display density
    float getDisplayScale();

    int getDeviceFD(int& vid, int& pid, const std::vector<DevVIDPID>& allowedVidPids);
    // A USB-UART bridge (CH343, CP210x, FTDI...) is plugged in: (vid << 16) | pid, else 0.
    // On an ESP32-S3 DevKit that is the UART port, not the native USB the SDR needs.
    int getUartBridgeHint();
    // Keep the display on while the app is in front (config "keepScreenOn", default on)
    void setKeepScreenOn(bool on);
}