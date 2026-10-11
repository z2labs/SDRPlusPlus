#include "satdb.h"
#include <json.hpp>
#include <utils/flog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef __ANDROID__
#include <android_backend.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using nlohmann::json;

namespace satdb {
    const char* BAND_NAMES[_BAND_COUNT] = { "HF", "2 m", "70 cm", "23 cm", "S-band 2.2G", "13 cm 2.4G", "Other" };

    // SatNOGS DB (transmitters, TLEs, names) and CelesTrak (amateur group TLEs, usually fresher)
    static const char* URL_TX = "https://db.satnogs.org/api/transmitters/?format=json";
    static const char* URL_TLE = "https://db.satnogs.org/api/tle/?format=json";
    static const char* URL_SATS = "https://db.satnogs.org/api/satellites/?format=json";
    static const char* URL_CELESTRAK = "https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle";

    static const char* F_TX = "transmitters.json";
    static const char* F_TLE = "tle_satnogs.json";
    static const char* F_SATS = "satellites.json";
    static const char* F_CELESTRAK = "tle_celestrak.txt";

    Band bandOf(double hz) {
        double m = hz / 1e6;
        if (m < 30.0) { return BAND_HF; }
        if (m >= 144.0 && m <= 146.0) { return BAND_2M; }
        if (m >= 430.0 && m <= 440.0) { return BAND_70CM; }
        if (m >= 1240.0 && m <= 1300.0) { return BAND_23CM; }
        if (m >= 2200.0 && m < 2300.0) { return BAND_SBAND; }
        if (m >= 2300.0 && m <= 2450.0) { return BAND_13CM; }
        return BAND_OTHER;
    }

    bool inAmateurBand(double hz) {
        static const double b[][2] = {
            { 21.0, 21.45 }, { 28.0, 29.7 }, { 144.0, 146.0 }, { 430.0, 440.0 }, { 1240.0, 1300.0 },
            { 2400.0, 2450.0 }, { 3400.0, 3410.0 }, { 5650.0, 5670.0 }, { 5830.0, 5850.0 },
            { 10450.0, 10500.0 }, { 24000.0, 24050.0 }
        };
        double m = hz / 1e6;
        for (auto& r : b) { if (m >= r[0] && m <= r[1]) { return true; } }
        return false;
    }

    bool Transmitter::amateur() const {
        // SatNOGS marks many amateur payloads (ISS included) as "Unknown": go by the band then
        if (service == "Amateur") { return true; }
        if (service.empty() || service == "Unknown") { return inAmateurBand(centre()); }
        return false;
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    bool txMatches(const Transmitter& t, const Filter& f) {
        if (!f.bands[t.band()]) { return false; }
        if (f.amateurOnly && !t.amateur()) { return false; }
        if (f.activeOnly && (!t.alive || t.status != "active")) { return false; }
        return true;
    }

    bool satMatchesSearch(const Satellite& s, const std::string& search) {
        if (search.empty()) { return true; }
        std::string q = lower(search);
        if (lower(s.name).find(q) != std::string::npos) { return true; }
        if (std::to_string(s.norad) == q) { return true; }
        for (auto& t : s.tx) {
            if (lower(t.mode).find(q) != std::string::npos || lower(t.description).find(q) != std::string::npos) { return true; }
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------
    // Download

    std::string httpGet(const std::string& url, const std::string& path) {
        std::string tmp = path + ".part";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
#if defined(__ANDROID__)
        std::string err = backend::httpDownload(url, tmp);
        if (!err.empty()) { std::filesystem::remove(tmp, ec); return err; }
#elif defined(_WIN32)
        // curl.exe ships with Windows 10 1803+; CREATE_NO_WINDOW keeps the console from flashing
        std::string cmd = "curl.exe -fsSL --retry 2 --max-time 120 -o \"" + tmp + "\" \"" + url + "\"";
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (!CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            return "curl.exe not found";
        }
        WaitForSingleObject(pi.hProcess, 180000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code != 0) { std::filesystem::remove(tmp, ec); return "download failed (curl " + std::to_string(code) + ")"; }
#else
        auto q = [](const std::string& s) {
            std::string r = "'";
            for (char c : s) { if (c == '\'') { r += "'\\''"; } else { r += c; } }
            return r + "'";
        };
        std::string cmd = "curl -fsSL --retry 2 --max-time 120 -o " + q(tmp) + " " + q(url) + " >/dev/null 2>&1";
        int rc = std::system(cmd.c_str());
        if (rc != 0) { std::filesystem::remove(tmp, ec); return "download failed (curl " + std::to_string(rc) + ")"; }
#endif
        if (std::filesystem::file_size(tmp, ec) < 16 || ec) { std::filesystem::remove(tmp, ec); return "empty reply"; }
        std::filesystem::rename(tmp, path, ec);
        if (ec) { return "cannot write " + path; }
        return "";
    }

    std::string Database::download(void (*progress)(const char*, void*), void* ctx) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        struct Item { const char* what; const char* url; const char* file; bool required; };
        const Item items[] = {
            { "transmitters (SatNOGS)", URL_TX, F_TX, true },
            { "orbits (SatNOGS)", URL_TLE, F_TLE, false },
            { "orbits (CelesTrak)", URL_CELESTRAK, F_CELESTRAK, false },
            { "names (SatNOGS)", URL_SATS, F_SATS, false },
        };
        std::string firstErr;
        int tleOk = 0;
        for (auto& it : items) {
            if (progress) { progress(it.what, ctx); }
            std::string err = httpGet(it.url, dir + "/" + it.file);
            if (!err.empty()) {
                flog::warn("Cubesat Hunter: {} failed: {}", it.what, err);
                if (firstErr.empty()) { firstErr = std::string(it.what) + ": " + err; }
                if (it.required) { return firstErr; }
            }
            else {
                std::error_code ec2;
                flog::info("Cubesat Hunter: {} downloaded, {} bytes", it.what, (int64_t)std::filesystem::file_size(dir + "/" + it.file, ec2));
                if (it.file == F_TLE || it.file == F_CELESTRAK) { tleOk++; }
            }
        }
        if (tleOk == 0) { return firstErr.empty() ? "no orbit data" : firstErr; }
        return "";
    }

    double Database::ageHours() const {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(dir + "/" + F_TX, ec);
        if (ec) { return -1; }
        auto now = std::filesystem::file_time_type::clock::now();
        return std::chrono::duration<double>(now - t).count() / 3600.0;
    }

    // ---------------------------------------------------------------------------------------------
    // Load

    static bool readFile(const std::string& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { return false; }
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    static std::string trim(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) { s.pop_back(); }
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') { i++; }
        return s.substr(i);
    }

    // SatNOGS sometimes wraps results in {"results": [...]} (paginated API); accept both
    static const json* listOf(const json& j) {
        if (j.is_array()) { return &j; }
        if (j.is_object() && j.contains("results") && j["results"].is_array()) { return &j["results"]; }
        return nullptr;
    }

    static double num(const json& o, const char* k) {
        if (!o.contains(k) || !o[k].is_number()) { return 0; }
        return o[k].get<double>();
    }
    static std::string str(const json& o, const char* k) {
        if (!o.contains(k) || !o[k].is_string()) { return ""; }
        return o[k].get<std::string>();
    }

    struct TleSet { std::string name, l1, l2; };

    // SatNOGS satellite status: "in orbit" (older data: "alive"), "re-entered", "future", "dead"
    static bool satAlive(const std::string& s) {
        std::string l = lower(s);
        return !(l == "re-entered" || l == "dead" || l == "decayed" || l == "future");
    }

    static void addTle(std::map<int, TleSet>& m, std::map<int, double>& epochs, const std::string& name,
                       const std::string& l1, const std::string& l2) {
        try {
            libsgp4::Tle t(name, l1, l2);
            int norad = (int)t.NoradNumber();
            double ep = (double)(t.Epoch().Ticks() - libsgp4::kUNIX_EPOCH) / (double)libsgp4::kTICKS_PER_DAY;
            auto it = epochs.find(norad);
            if (it != epochs.end() && it->second >= ep) { return; }   // keep the newest element set
            epochs[norad] = ep;
            m[norad] = { name, l1, l2 };
        }
        catch (...) {}
    }

    int Database::load(std::string& err) {
        err.clear();
        std::map<int, TleSet> tles;
        std::map<int, double> epochs;
        std::string text;

        if (readFile(dir + "/" + F_TLE, text)) {
            try {
                json j = json::parse(text);
                if (auto l = listOf(j)) {
                    for (auto& o : *l) {
                        std::string n = str(o, "tle0");
                        if (n.rfind("0 ", 0) == 0) { n = n.substr(2); }
                        addTle(tles, epochs, trim(n), trim(str(o, "tle1")), trim(str(o, "tle2")));
                    }
                }
            }
            catch (std::exception& e) { flog::warn("Cubesat Hunter: SatNOGS TLE parse: {}", e.what()); }
        }
        if (readFile(dir + "/" + F_CELESTRAK, text)) {
            std::istringstream ss(text);
            std::string a, b, c;
            while (std::getline(ss, a)) {
                a = trim(a);
                if (a.empty()) { continue; }
                if (a[0] == '1' && a.size() > 60) { b = a; a = ""; }   // 2-line set without a name
                else if (!std::getline(ss, b)) { break; }
                if (!std::getline(ss, c)) { break; }
                addTle(tles, epochs, a, trim(b), trim(c));
            }
        }
        if (tles.empty()) { err = "no orbit data, tap Update"; sats.clear(); return 0; }

        std::map<int, std::string> names;
        std::map<int, std::string> satStatus;
        std::map<int, bool> hungarian;
        if (readFile(dir + "/" + F_SATS, text)) {
            try {
                json j = json::parse(text);
                if (auto l = listOf(j)) {
                    for (auto& o : *l) {
                        int norad = (int)num(o, "norad_cat_id");
                        if (norad <= 0) { continue; }
                        std::string n = str(o, "name");
                        if (!n.empty()) { names[norad] = n; }
                        satStatus[norad] = str(o, "status");
                        // "countries" is a comma separated list of ISO codes, e.g. "HU" or "HU,SK"
                        std::string c = str(o, "countries");
                        for (size_t p = 0; p < c.size();) {
                            size_t q = c.find(',', p);
                            if (q == std::string::npos) { q = c.size(); }
                            if (trim(c.substr(p, q - p)) == "HU") { hungarian[norad] = true; }
                            p = q + 1;
                        }
                    }
                }
            }
            catch (std::exception& e) { flog::warn("Cubesat Hunter: SatNOGS satellites parse: {}", e.what()); }
        }

        std::map<int, std::vector<Transmitter>> txs;
        if (!readFile(dir + "/" + F_TX, text)) { err = "no transmitter list, tap Update"; sats.clear(); return 0; }
        try {
            json j = json::parse(text);
            if (auto l = listOf(j)) {
                for (auto& o : *l) {
                    Transmitter t;
                    t.norad = (int)num(o, "norad_cat_id");
                    if (t.norad <= 0) { continue; }
                    // "downlink_drifted" is the observed frequency when SatNOGS knows a drift
                    t.downLow = num(o, "downlink_drifted");
                    if (t.downLow <= 0) { t.downLow = num(o, "downlink_low"); }
                    if (t.downLow <= 0) { continue; }
                    t.downHigh = num(o, "downlink_high");
                    if (t.downHigh > 0 && t.downHigh < t.downLow) { std::swap(t.downHigh, t.downLow); }
                    t.uuid = str(o, "uuid");
                    t.description = str(o, "description");
                    t.type = str(o, "type");
                    t.mode = str(o, "mode");
                    t.service = str(o, "service");
                    t.status = str(o, "status");
                    t.baud = num(o, "baud");
                    t.alive = o.contains("alive") && o["alive"].is_boolean() && o["alive"].get<bool>();
                    t.invert = o.contains("invert") && o["invert"].is_boolean() && o["invert"].get<bool>();
                    auto st = satStatus.find(t.norad);
                    if (st != satStatus.end() && !satAlive(st->second)) { t.alive = false; }
                    txs[t.norad].push_back(t);
                }
            }
        }
        catch (std::exception& e) { err = std::string("transmitter list: ") + e.what(); sats.clear(); return 0; }

        std::vector<std::shared_ptr<Satellite>> out;
        for (auto& [norad, list] : txs) {
            auto tl = tles.find(norad);
            if (tl == tles.end()) { continue; }
            auto s = std::make_shared<Satellite>();
            s->norad = norad;
            auto nm = names.find(norad);
            s->name = (nm != names.end()) ? nm->second : tl->second.name;
            if (s->name.empty()) { s->name = "NORAD " + std::to_string(norad); }
            s->line1 = tl->second.l1;
            s->line2 = tl->second.l2;
            s->epochDays = epochs[norad];
            s->hungarian = hungarian.count(norad) > 0;
            auto st = satStatus.find(norad);
            s->alive = st == satStatus.end() || satAlive(st->second);
            try {
                s->tle = std::make_shared<libsgp4::Tle>(s->name, s->line1, s->line2);
                s->sgp4 = std::make_shared<libsgp4::SGP4>(*s->tle);
            }
            catch (...) { continue; }
            std::sort(list.begin(), list.end(), [](const Transmitter& a, const Transmitter& b) { return a.centre() < b.centre(); });
            s->tx = list;
            out.push_back(s);
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a->name < b->name; });
        sats = out;
        size_t ntx = 0;
        for (auto& [n, l] : txs) { ntx += l.size(); }
        flog::info("Cubesat Hunter: {} satellites with orbit + downlink ({} TLEs, {} transmitters on {} satellites, {} names, {} HU)",
                   (int)sats.size(), (int)tles.size(), (int)ntx, (int)txs.size(), (int)names.size(), (int)hungarian.size());
        stats = std::to_string(sats.size()) + " satellites (" + std::to_string(tles.size()) + " orbits, " + std::to_string(ntx) + " downlinks)";
        return (int)sats.size();
    }
}
