// Look angles, Doppler and pass prediction on top of libsgp4
#pragma once
#include "satdb.h"
#include "sgp4/Observer.h"
#include "sgp4/CoordTopocentric.h"
#include <cmath>
#include <utility>
#include <vector>

namespace satpass {
    static constexpr double C_KMS = 299792.458;

    struct Station {
        double lat = 47.4979;   // Budapest by default
        double lon = 19.0402;
        double altM = 120.0;
    };

    struct Look {
        double az = 0, el = -90;   // degrees
        double rangeKm = 0;
        double rangeRateKms = 0;   // + = moving away
        bool ok = false;
    };

    struct Pass {
        bool valid = false;        // found within the search window
        bool always = false;       // above the horizon for the whole window (GEO / HEO apogee)
        double aos = 0, los = 0;   // unix seconds
        double tmax = 0, maxEl = 0;
        double aosAz = 0, losAz = 0;
    };

    inline libsgp4::DateTime toDateTime(double unix) {
        return libsgp4::DateTime(libsgp4::kUNIX_EPOCH + (int64_t)std::llround(unix * 1e6));
    }

    inline Look look(const satdb::Satellite& s, libsgp4::Observer& obs, double unix) {
        Look l;
        if (!s.sgp4) { return l; }
        try {
            libsgp4::Eci eci = s.sgp4->FindPosition(toDateTime(unix));
            libsgp4::CoordTopocentric t = obs.GetLookAngle(eci);
            l.az = t.azimuth * 180.0 / 3.14159265358979323846;
            l.el = t.elevation * 180.0 / 3.14159265358979323846;
            l.rangeKm = t.range;
            l.rangeRateKms = t.rangeRate;
            l.ok = true;
        }
        catch (...) {}
        return l;
    }

    // Received frequency for a transmitted one (first order is plenty for LEO: |v/c| < 3e-5)
    inline double doppler(double fTx, double rangeRateKms) { return fTx * (1.0 - rangeRateKms / C_KMS); }

    // Next pass whose maximum elevation is at least minEl, starting at 'now' (an ongoing pass is
    // returned with aos in the past). Coarse search with an elevation-dependent step, then
    // bisection to 1 s for AOS / LOS.
    inline Pass nextPass(const satdb::Satellite& s, libsgp4::Observer& obs, double now, double hours = 24.0, double minEl = 0.0) {
        Pass p;
        const double end = now + hours * 3600.0;
        auto el = [&](double t) { Look l = look(s, obs, t); return l.ok ? l.el : -90.0; };
        auto stepFor = [](double e) { return std::min(300.0, std::max(20.0, -e * 8.0)); };
        auto edge = [&](double a, double b, bool rising) {   // el(a) and el(b) on opposite sides of 0
            for (int i = 0; i < 20 && b - a > 1.0; i++) {
                double m = 0.5 * (a + b);
                if ((el(m) > 0) == rising) { b = m; } else { a = m; }
            }
            return 0.5 * (a + b);
        };

        double t = now;
        double e = el(t);
        while (t < end) {
            double aos = now;
            if (e > 0) {
                // Above the horizon: walk back to the AOS of this pass (at most 3 h)
                double b = t, a = t;
                while (a > t - 3 * 3600.0 && el(a) > 0) { a -= 60.0; }
                aos = (el(a) > 0) ? a : edge(a, b, true);
            }
            else {
                double t1 = t, e1 = e;
                bool found = false;
                while (t1 < end) {
                    double t2 = t1 + stepFor(e1);
                    double e2 = el(t2);
                    if (e2 > 0) { aos = edge(t1, t2, true); t = t2; found = true; break; }
                    t1 = t2; e1 = e2;
                }
                if (!found) { return p; }
            }
            // Walk the pass: maximum and LOS
            double tm = std::max(aos, now), em = el(tm), tt = tm;
            double last = tt;
            bool set = false;
            while (tt < end + 12 * 3600.0) {
                double t2 = tt + 15.0;
                double e2 = el(t2);
                if (e2 > em) { em = e2; tm = t2; }
                if (e2 <= 0) { last = edge(tt, t2, false); set = true; break; }
                tt = t2;
                if (tt > aos + 12 * 3600.0) { break; }
            }
            // Refine the maximum around tm (golden-section-like shrink)
            double lo = tm - 15.0, hi = tm + 15.0;
            for (int i = 0; i < 12; i++) {
                double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
                if (el(m1) < el(m2)) { lo = m1; } else { hi = m2; }
            }
            double tpk = 0.5 * (lo + hi), epk = el(tpk);
            if (epk > em) { em = epk; tm = tpk; }

            if (!set) {
                p.valid = true; p.always = true;
                p.aos = aos; p.los = end; p.tmax = tm; p.maxEl = em;
                p.aosAz = look(s, obs, aos).az; p.losAz = p.aosAz;
                return p;
            }
            if (em >= minEl) {
                p.valid = true;
                p.aos = aos; p.los = last; p.tmax = tm; p.maxEl = em;
                p.aosAz = look(s, obs, aos).az;
                p.losAz = look(s, obs, last).az;
                return p;
            }
            // Too low: continue after this pass
            t = last + 60.0;
            e = el(t);
        }
        return p;
    }

    // (az, el) samples along [t0, t1] for the radar
    inline std::vector<std::pair<float, float>> track(const satdb::Satellite& s, libsgp4::Observer& obs, double t0, double t1, double step) {
        std::vector<std::pair<float, float>> v;
        if (t1 <= t0) { return v; }
        int n = std::min(2000, (int)((t1 - t0) / step) + 2);
        for (int i = 0; i < n; i++) {
            double t = std::min(t1, t0 + i * step);
            Look l = look(s, obs, t);
            if (l.ok) { v.push_back({ (float)l.az, (float)l.el }); }
        }
        return v;
    }
}
