// Satellite database for the Cubesat Hunter: SatNOGS DB transmitters + TLEs (CelesTrak as a
// second TLE source), band / service classification and filtering.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <map>
#include "sgp4/SGP4.h"
#include "sgp4/Tle.h"

namespace satdb {
    enum Band {
        BAND_HF,      // < 30 MHz
        BAND_2M,      // 144-146 MHz
        BAND_70CM,    // 430-440 MHz
        BAND_23CM,    // 1240-1300 MHz
        BAND_SBAND,   // 2200-2300 MHz (EESS / space research downlinks)
        BAND_13CM,    // 2300-2450 MHz (amateur 2400-2450)
        BAND_OTHER,
        _BAND_COUNT
    };
    extern const char* BAND_NAMES[_BAND_COUNT];

    Band bandOf(double hz);
    bool inAmateurBand(double hz);

    struct Transmitter {
        std::string uuid;
        std::string description;
        std::string type;       // Transmitter / Transceiver / Transponder
        std::string mode;       // FM, CW, AFSK, GMSK, BPSK, USB, ...
        std::string service;    // Amateur, Unknown, Earth Exploration, ...
        std::string status;     // active / inactive / invalid
        double downLow = 0;     // Hz
        double downHigh = 0;    // Hz, 0 = single frequency
        double baud = 0;
        bool alive = false;
        bool invert = false;
        int norad = 0;

        bool isLinear() const { return downHigh > downLow; }
        double centre() const { return isLinear() ? 0.5 * (downLow + downHigh) : downLow; }
        bool amateur() const;
        Band band() const { return bandOf(centre()); }
    };

    struct Satellite {
        int norad = 0;
        std::string name;
        std::string line1, line2;
        double epochDays = 0;   // TLE epoch, days since 1970 (to keep the newest set)
        std::shared_ptr<libsgp4::Tle> tle;
        std::shared_ptr<libsgp4::SGP4> sgp4;
        std::vector<Transmitter> tx;
        bool hungarian = false;   // SatNOGS "countries" contains HU
        bool alive = true;        // SatNOGS satellite status (alive / dead / re-entered / future)
    };

    struct Filter {
        bool bands[_BAND_COUNT] = { false, true, true, false, true, true, false };
        bool amateurOnly = true;
        bool activeOnly = true;
        bool favoritesOnly = false;
        std::string search;     // name / NORAD / mode substring, case-insensitive
    };

    // Transmitter passes the filter (search is applied per satellite, not here)
    bool txMatches(const Transmitter& t, const Filter& f);
    bool satMatchesSearch(const Satellite& s, const std::string& search);

    class Database {
    public:
        // Cache folder: <root>/sat_tracker
        void setDir(const std::string& dir) { this->dir = dir; }
        const std::string& getDir() const { return dir; }

        // Download all sources into the cache (blocking, call from a worker). Returns "" on success
        // or a short error text; partial downloads keep the previous cache file.
        std::string download(void (*progress)(const char* what, void* ctx), void* ctx);

        // Load the cache. Returns the number of satellites with a TLE and at least one transmitter.
        int load(std::string& err);

        // Age of the cache in hours (-1 = no cache)
        double ageHours() const;

        std::vector<std::shared_ptr<Satellite>> sats;   // only satellites with a usable TLE
        std::string stats;                              // what the last load found, for the UI

    private:
        std::string dir;
    };

    // Platform download (curl on desktop, Java HttpURLConnection on Android). "" = success.
    std::string httpGet(const std::string& url, const std::string& path);
}
