#pragma once
#include <string>
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
    // Collect the logs, settings and device details and open the Android share sheet
    void shareDebugReport(const std::string& header);

    // Cubesat Hunter (sat_tracker module)
    // Blocking HTTP(S) GET into a file (call from a worker thread). "" = success, else the error.
    std::string httpDownload(const std::string& url, const std::string& path);
    // Rotation-vector sensor on/off (released automatically while the app is in the background)
    void setOrientationSensor(bool on);
    // Device -> world (east, north, up) rotation, row-major, magnetic north. declination: degrees
    // east (NaN when no location is known); displayRotation: 0..3 (Surface.ROTATION_*). false = no data.
    bool getOrientation(float R[9], float& declination, int& displayRotation);
    // GPS / network location updates on/off (asks for the permission the first time)
    void setLocationUpdates(bool on);
    // Last known position. false = none yet.
    bool getLocation(double& lat, double& lon, double& altM, double& accuracyM, double& ageS);
}