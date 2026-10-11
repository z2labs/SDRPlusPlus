// Cubesat Hunter -> other modules (the demo source uses it to put the selected satellite's
// signal on the air, with the real Doppler of the current or simulated pass).
#pragma once

#define SAT_TRACKER_IFACE_NAME "Cubesat Hunter"

enum {
    SAT_TRACKER_IFACE_CMD_GET_SIGNAL   // out: SatTrackerSignal*
};

enum {
    SAT_SIGNAL_FM,       // FM voice / repeater
    SAT_SIGNAL_CW,       // CW beacon
    SAT_SIGNAL_DATA,     // AFSK / FSK / GMSK telemetry bursts
    SAT_SIGNAL_LINEAR    // linear transponder (SSB / CW users across the passband)
};

struct SatTrackerSignal {
    bool active = false;     // a satellite and downlink are selected
    double freq = 0;         // received frequency with Doppler, Hz (centre for a linear transponder)
    double width = 0;        // linear transponder passband, Hz
    double baud = 0;
    double elevation = -90;  // deg
    double rangeKm = 0;
    int kind = SAT_SIGNAL_FM;
};
