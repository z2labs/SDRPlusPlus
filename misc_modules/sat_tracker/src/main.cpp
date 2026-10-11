#include <cctype>
// Cubesat Hunter: satellite list (SatNOGS DB transmitters + TLEs), pass prediction, automatic
// tuning with Doppler correction and a floating radar on the spectrum that, on a phone, turns with
// the compass and guides the antenna to the satellite (Look4Sat style).
#include <imgui.h>
#include <imgui_internal.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <core.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>
#include <radio_interface.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>
#include <set>
#include <filesystem>
#include "satdb.h"
#include "passes.h"
#include "sat_tracker_interface.h"
#include <json.hpp>
#include <fstream>
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

SDRPP_MOD_INFO{
    /* Name:            */ "sat_tracker",
    /* Description:     */ "Cubesat Hunter: satellite passes, Doppler tuning and radar",
    /* Author:          */ "Zoltan Doczi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static constexpr double DEG = 3.14159265358979323846 / 180.0;

static double nowUnix() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Classroom demo: the whole module can run on a shifted clock so the next pass starts now
static std::atomic<double> g_timeShift{ 0.0 };
static double simNow() { return nowUnix() + g_timeShift.load(); }

static double wrap180(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) { a += 360.0; }
    return a - 180.0;
}

static std::string fmtDuration(double s) {
    char b[32];
    s = std::max(0.0, s);
    int h = (int)(s / 3600), m = (int)(std::fmod(s, 3600.0) / 60), sec = (int)std::fmod(s, 60.0);
    if (h > 0) { snprintf(b, sizeof(b), "%dh%02d", h, m); }
    else { snprintf(b, sizeof(b), "%d:%02d", m, sec); }
    return b;
}

static std::string fmtClock(double unix) {
    time_t t = (time_t)unix;
    char b[16];
    strftime(b, sizeof(b), "%H:%M", localtime(&t));
    return b;
}

static std::string fmtFreq(double hz) {
    char b[32];
    snprintf(b, sizeof(b), "%.4f", hz / 1e6);
    return b;
}


// Maidenhead locator (4, 6 or 8 characters) -> centre of the square
static bool locatorToLatLon(std::string loc, double& lat, double& lon) {
    std::string l;
    for (char c : loc) { if (c != ' ') { l += (char)std::toupper((unsigned char)c); } }
    if (l.size() < 4 || l.size() > 8 || (l.size() & 1)) { return false; }
    if (l[0] < 'A' || l[0] > 'R' || l[1] < 'A' || l[1] > 'R' || !isdigit((unsigned char)l[2]) || !isdigit((unsigned char)l[3])) { return false; }
    lon = (l[0] - 'A') * 20.0 - 180.0 + (l[2] - '0') * 2.0;
    lat = (l[1] - 'A') * 10.0 - 90.0 + (l[3] - '0') * 1.0;
    double w = 2.0, h = 1.0;
    if (l.size() >= 6) {
        if (l[4] < 'A' || l[4] > 'X' || l[5] < 'A' || l[5] > 'X') { return false; }
        w /= 24.0; h /= 24.0;
        lon += (l[4] - 'A') * w;
        lat += (l[5] - 'A') * h;
    }
    if (l.size() == 8) {
        if (!isdigit((unsigned char)l[6]) || !isdigit((unsigned char)l[7])) { return false; }
        w /= 10.0; h /= 10.0;
        lon += (l[6] - '0') * w;
        lat += (l[7] - '0') * h;
    }
    lon += w / 2.0;
    lat += h / 2.0;
    return true;
}

static std::string latLonToLocator(double lat, double lon) {
    lon = std::clamp(lon + 180.0, 0.0, 359.9999);
    lat = std::clamp(lat + 90.0, 0.0, 179.9999);
    char b[7];
    b[0] = (char)('A' + (int)(lon / 20));
    b[1] = (char)('A' + (int)(lat / 10));
    b[2] = (char)('0' + (int)(std::fmod(lon, 20.0) / 2));
    b[3] = (char)('0' + (int)std::fmod(lat, 10.0));
    b[4] = (char)('a' + (int)(std::fmod(lon, 2.0) * 12));
    b[5] = (char)('a' + (int)(std::fmod(lat, 1.0) * 24));
    b[6] = 0;
    return b;
}

#ifdef _WIN32
// Windows Location Services through a hidden Windows PowerShell (System.Device GeoCoordinateWatcher).
// Uses Wi-Fi positioning on PCs without GPS. Returns "" on success.
static std::string windowsLocate(double& lat, double& lon, double& acc) {
    const char* script =
        "Add-Type -AssemblyName System.Device;"
        "$w=New-Object System.Device.Location.GeoCoordinateWatcher;$w.Start();"
        "for($i=0;$i -lt 75 -and ($w.Status -ne 'Ready' -or $w.Position.Location.IsUnknown);$i++){"
        "if($w.Permission -eq 'Denied'){break};Start-Sleep -Milliseconds 200};"
        "$l=$w.Position.Location;"
        "if($w.Permission -eq 'Denied'){'DENIED'}elseif($l.IsUnknown){'UNKNOWN'}"
        "else{[string]::Format([Globalization.CultureInfo]::InvariantCulture,'{0} {1} {2}',$l.Latitude,$l.Longitude,$l.HorizontalAccuracy)}";
    std::string cmd = std::string("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"") + script + "\"";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { return "pipe failed"; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;
    PROCESS_INFORMATION pi = {};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return "PowerShell not found"; }
    std::string out;
    char tmp[256];
    DWORD n;
    while (ReadFile(rd, tmp, sizeof(tmp), &n, NULL) && n > 0) { out.append(tmp, n); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (out.find("DENIED") != std::string::npos) { return "Location is off or denied (Windows Settings > Privacy > Location)"; }
    if (out.find("UNKNOWN") != std::string::npos) { return "Windows has no position yet"; }
    if (sscanf(out.c_str(), "%lf %lf %lf", &lat, &lon, &acc) != 3) { return "no reply from Windows location"; }
    return "";
}
#endif

class SatTrackerModule : public ModuleManager::Instance {
public:
    SatTrackerModule(std::string name) {
        this->name = name;
        db.setDir(core::args["root"].s() + "/sat_tracker");
        loadConfig();

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);
        core::modComManager.registerInterface("sat_tracker", SAT_TRACKER_IFACE_NAME, ifaceHandler, this);

        worker = std::thread(&SatTrackerModule::run, this);
    }

    ~SatTrackerModule() {
        {
            std::lock_guard<std::mutex> lck(wMtx);
            quit = true;
        }
        wCv.notify_all();
        if (worker.joinable()) { worker.join(); }
        if (locThread.joinable()) { locThread.join(); }
        core::modComManager.unregisterInterface(SAT_TRACKER_IFACE_NAME);
        g_timeShift = 0;
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
#ifdef __ANDROID__
        backend::setOrientationSensor(false);
        backend::setLocationUpdates(false);
#endif
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() {
        enabled = false;
        tracking = false;
    }
    bool isEnabled() { return enabled; }

private:
    // ------------------------------------------------------------------------------------------
    // Shared state between the worker (download, pass prediction) and the UI thread

    struct Row {
        std::shared_ptr<satdb::Satellite> sat;
        satpass::Pass pass;
        std::string bands;      // e.g. "70 cm, S-band 2.2G"
        std::string modes;      // e.g. "FM, CW"
        bool fav = false;
        double freq = 0;        // first downlink passing the filter
        std::string mode;
    };

    struct Track {
        int norad = 0;
        double t0 = 0, t1 = 0;
        std::vector<std::pair<float, float>> pts;
    };

    void loadConfig() {
        config.acquire();
        auto& c = config.conf[name];
        if (c.contains("lat")) { station.lat = c["lat"]; }
        if (c.contains("lon")) { station.lon = c["lon"]; }
        if (c.contains("alt")) { station.altM = c["alt"]; }
        if (c.contains("bands") && c["bands"].is_array()) {
            for (int i = 0; i < satdb::_BAND_COUNT && i < (int)c["bands"].size(); i++) { filter.bands[i] = c["bands"][i]; }
        }
        if (c.contains("amateurOnly")) { filter.amateurOnly = c["amateurOnly"]; }
        if (c.contains("activeOnly")) { filter.activeOnly = c["activeOnly"]; }
        if (c.contains("minEl")) { minEl = c["minEl"]; }
        if (c.contains("autoMode")) { autoMode = c["autoMode"]; }
        if (c.contains("stepHz")) { stepHz = c["stepHz"]; }
        if (c.contains("radar")) { radarOn = c["radar"]; }
        if (c.contains("radarLarge")) { radarLarge = c["radarLarge"]; }
        if (c.contains("compass")) { compassOn = c["compass"]; }
        if (c.contains("followGps")) { followGps = c["followGps"]; }
        if (c.contains("selNorad")) { selNorad = c["selNorad"]; }
        if (c.contains("selTx")) { selTx = c["selTx"]; }
        if (c.contains("updateDays")) { updateDays = c["updateDays"]; }
        if (c.contains("favorites") && c["favorites"].is_array()) {
            for (auto& v : c["favorites"]) { if (v.is_number_integer()) { favorites.insert(v.get<int>()); } }
        }
        if (c.contains("favoritesOnly")) { filter.favoritesOnly = c["favoritesOnly"]; }
        config.release();
        minEl = std::clamp(minEl, 0.0f, 60.0f);
        stepHz = std::clamp(stepHz, 1, 1000);
        updateDays = std::clamp(updateDays, 0, 30);
#ifdef __ANDROID__
        if (compassOn) { backend::setOrientationSensor(true); }
        if (followGps) { backend::setLocationUpdates(true); }
#else
        compassOn = false;
        followGps = false;
#endif
    }

    void saveConfig() {
        config.acquire();
        auto& c = config.conf[name];
        c["lat"] = station.lat;
        c["lon"] = station.lon;
        c["alt"] = station.altM;
        c["bands"] = json::array();
        for (int i = 0; i < satdb::_BAND_COUNT; i++) { c["bands"][i] = filter.bands[i]; }
        c["amateurOnly"] = filter.amateurOnly;
        c["activeOnly"] = filter.activeOnly;
        c["minEl"] = minEl;
        c["autoMode"] = autoMode;
        c["stepHz"] = stepHz;
        c["radar"] = radarOn;
        c["radarLarge"] = radarLarge;
        c["compass"] = compassOn;
        c["followGps"] = followGps;
        c["selNorad"] = selNorad;
        c["selTx"] = selTx;
        c["updateDays"] = updateDays;
        c["favoritesOnly"] = filter.favoritesOnly;
        c["favorites"] = json::array();
        {
            std::lock_guard<std::mutex> lck(paramMtx);
            for (int n : favorites) { c["favorites"].push_back(n); }
        }
        config.release(true);
    }

    // ------------------------------------------------------------------------------------------
    // Worker

    void request(bool download) {
        {
            std::lock_guard<std::mutex> lck(wMtx);
            if (download) { wantDownload = true; }
            wantRecalc = true;
        }
        wCv.notify_all();
    }

    static void progressCb(const char* what, void* ctx) {
        SatTrackerModule* _this = (SatTrackerModule*)ctx;
        _this->setStatus(std::string("Downloading ") + what + "...");
    }

    void setDbStats(const std::string& s) {
        std::lock_guard<std::mutex> lck(statusMtx);
        dbStats = s;
    }

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> lck(statusMtx);
        status = s;
    }

    void run() {
        // First load from the cache, then refresh it if it is old
        {
            std::string err;
            int n = db.load(err);
            setStatus(n ? "" : err);
            setDbStats(db.stats);
            double age = db.ageHours();
            if (age < 0 || (updateDays > 0 && age > updateDays * 24.0)) { wantDownload = true; }
            wantRecalc = true;
        }
        double lastCalc = 0;
        while (true) {
            bool dl = false, calc = false;
            {
                std::unique_lock<std::mutex> lck(wMtx);
                wCv.wait_for(lck, std::chrono::seconds(5), [&] { return quit || wantDownload || wantRecalc; });
                if (quit) { return; }
                dl = wantDownload;
                calc = wantRecalc || simNow() - lastCalc > 60.0;
                wantDownload = false;
                wantRecalc = false;
            }
            if (dl) {
                busy = true;
                std::string err = db.download(progressCb, this);
                std::string lerr;
                int n = db.load(lerr);
                setDbStats(db.stats);
                setStatus(!err.empty() ? ("Update failed: " + err + (n ? " (using the old data)" : "")) : (n ? "" : lerr));
                busy = false;
                calc = true;
            }
            if (calc) {
                recalc();
                lastCalc = simNow();
            }
        }
    }

    // Sort key: favourites, then Hungarian satellites, then the rest; in each group the ones
    // overhead now first, then by AOS
    static bool rowLess(const Row& a, const Row& b, double now) {
        int ga = a.fav ? 0 : a.sat->hungarian ? 1 : 2, gb = b.fav ? 0 : b.sat->hungarian ? 1 : 2;
        if (ga != gb) { return ga < gb; }
        bool va = a.pass.aos <= now, vb = b.pass.aos <= now;
        if (va != vb) { return va; }
        return a.pass.aos < b.pass.aos;
    }

    void publishRows(std::vector<Row> out, double now, const satpass::Station& st) {
        std::sort(out.begin(), out.end(), [now](const Row& a, const Row& b) { return rowLess(a, b, now); });
        std::lock_guard<std::mutex> lck(rowsMtx);
        rows = std::make_shared<std::vector<Row>>(std::move(out));
        rowsStation = st;
    }

    void recalc() {
        satdb::Filter f;
        satpass::Station st;
        float minE;
        std::set<int> favs;
        {
            std::lock_guard<std::mutex> lck(paramMtx);
            f = filter;
            st = station;
            minE = minEl;
            favs = favorites;
        }
        libsgp4::Observer obs(st.lat, st.lon, st.altM / 1000.0);
        double now = simNow();
        auto t0 = std::chrono::steady_clock::now();
        auto sats = db.sats;   // the worker owns db; the UI only reads rows

        // Candidates: a downlink passing the filter; favourites and Hungarian satellites always
        // (Hungarian ones only while alive when "Active only" is set)
        std::vector<Row> cand;
        for (auto& s : sats) {
            Row r;
            r.sat = s;
            r.fav = favs.count(s->norad) > 0;
            bool any = false;
            bool bandSeen[satdb::_BAND_COUNT] = { false };
            for (auto& t : s->tx) {
                bool match = satdb::txMatches(t, f);
                bool special = r.fav || (s->hungarian && (!f.activeOnly || (s->alive && t.alive && t.status == "active")));
                if (!match && !special) { continue; }
                any = true;
                if (r.freq == 0) { r.freq = t.centre(); r.mode = t.mode; }
                if (!bandSeen[t.band()]) {
                    bandSeen[t.band()] = true;
                    r.bands += (r.bands.empty() ? "" : ", ") + std::string(satdb::BAND_NAMES[t.band()]);
                }
                if (!t.mode.empty() && r.modes.find(t.mode) == std::string::npos) {
                    r.modes += (r.modes.empty() ? "" : ", ") + t.mode;
                }
            }
            if (!any && r.fav) { any = true; }
            if (!any) { continue; }
            if (f.favoritesOnly && !r.fav) { continue; }
            cand.push_back(r);
        }
        // Favourites and Hungarian satellites first, so they appear at once on a slow phone
        std::stable_sort(cand.begin(), cand.end(), [](const Row& a, const Row& b) {
            return (a.fav ? 0 : a.sat->hungarian ? 1 : 2) < (b.fav ? 0 : b.sat->hungarian ? 1 : 2);
        });
        calcTotal = (int)cand.size();
        calcDone = 0;
        std::vector<Row> out;
        auto lastPub = std::chrono::steady_clock::now();
        for (auto& r : cand) {
            if (quit) { return; }
            calcDone++;
            if (!satpass::look(*r.sat, obs, now).ok) { continue; }   // decayed / bad elements
            r.pass = satpass::nextPass(*r.sat, obs, now, 24.0, r.fav ? 0.0 : minE);
            if (!r.pass.valid) { continue; }
            out.push_back(r);
            // Show what is ready every half second while the rest is computed
            if (std::chrono::steady_clock::now() - lastPub > std::chrono::milliseconds(500)) {
                publishRows(out, now, st);
                lastPub = std::chrono::steady_clock::now();
            }
        }
        publishRows(out, now, st);
        calcTotal = 0;
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        flog::info("Cubesat Hunter: {} of {} satellites match the filter, {} with a pass in 24 h ({} ms)",
                   (int)cand.size(), (int)sats.size(), (int)out.size(), ms);

        // Radar track for the selected satellite
        int sel = selNorad;
        std::shared_ptr<satdb::Satellite> ss;
        for (auto& s : sats) { if (s->norad == sel) { ss = s; break; } }
        auto tr = std::make_shared<Track>();
        if (ss) {
            satpass::Pass p = satpass::nextPass(*ss, obs, now, 24.0, 0.0);
            if (p.valid) {
                tr->norad = sel;
                tr->t0 = p.aos;
                tr->t1 = p.always ? p.aos + 3600.0 : p.los;
                tr->pts = satpass::track(*ss, obs, tr->t0, tr->t1, 10.0);
            }
            std::lock_guard<std::mutex> lck(rowsMtx);
            selSat = ss;
            selPass = p;
        }
        else {
            std::lock_guard<std::mutex> lck(rowsMtx);
            selSat.reset();
            selPass = satpass::Pass();
        }
        std::lock_guard<std::mutex> lck(rowsMtx);
        track = tr;
    }

    // ------------------------------------------------------------------------------------------
    // Live state (UI thread)

    struct Live {
        std::shared_ptr<satdb::Satellite> sat;
        const satdb::Transmitter* tx = nullptr;
        satpass::Look look;
        satpass::Pass pass;
        double fTx = 0, fRx = 0;
        bool valid = false;
    };

    Live live() {
        Live l;
        std::shared_ptr<satdb::Satellite> s;
        satpass::Station st;
        {
            std::lock_guard<std::mutex> lck(rowsMtx);
            s = selSat;
            l.pass = selPass;
        }
        {
            std::lock_guard<std::mutex> lck(paramMtx);
            st = station;
        }
        if (!s) { return l; }
        l.sat = s;
        for (auto& t : s->tx) { if (t.uuid == selTx) { l.tx = &t; break; } }
        libsgp4::Observer obs(st.lat, st.lon, st.altM / 1000.0);
        double now = simNow();
        l.look = satpass::look(*s, obs, now);
        if (!l.look.ok) { return l; }
        l.valid = true;
        if (l.tx) {
            l.fTx = l.tx->centre() + userOffset;
            // Below the horizon: wait on the AOS frequency (Doppler at rise), ready for the signal
            satpass::Look dl = l.look;
            if (l.look.el < 0 && l.pass.valid && l.pass.aos > now && l.pass.aos - now < 3600.0) {
                satpass::Look a = satpass::look(*s, obs, l.pass.aos);
                if (a.ok) { dl = a; }
            }
            l.fRx = satpass::doppler(l.fTx, dl.rangeRateKms);
        }
        return l;
    }

    double currentTune() {
        std::string vfo = gui::waterfall.selectedVFO;
        double f = gui::waterfall.getCenterFrequency();
        if (!vfo.empty() && gui::waterfall.vfos.find(vfo) != gui::waterfall.vfos.end()) {
            f += gui::waterfall.vfos[vfo]->generalOffset;
        }
        return f;
    }

    void setDemod(const satdb::Transmitter& t) {
        std::string vfo = gui::waterfall.selectedVFO;
        if (vfo.empty() || !core::modComManager.interfaceExists(vfo) || core::modComManager.getModuleName(vfo) != "radio") { return; }
        std::string m = t.mode;
        std::transform(m.begin(), m.end(), m.begin(), ::toupper);
        int mode = RADIO_IFACE_MODE_NFM;
        float bw = 16000.0f;   // FM voice + residual Doppler error
        if (t.isLinear() || m == "USB" || m == "SSB") { mode = RADIO_IFACE_MODE_USB; bw = 2800.0f; }
        else if (m == "LSB") { mode = RADIO_IFACE_MODE_LSB; bw = 2800.0f; }
        else if (m == "CW") { mode = RADIO_IFACE_MODE_CW; bw = 500.0f; }
        else if (m == "AM") { mode = RADIO_IFACE_MODE_AM; bw = 10000.0f; }
        else if (m == "FM" || m == "AFSK" || m == "FMN" || m == "APRS") { mode = RADIO_IFACE_MODE_NFM; bw = 16000.0f; }
        else {
            // Data modes: NFM wide enough for the symbol rate, so the signal is seen and heard
            double bd = t.baud > 0 ? t.baud : 9600.0;
            bw = (float)std::clamp(2.0 * bd + 6000.0, 12500.0, 50000.0);
        }
        core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
        core::modComManager.callInterface(vfo, RADIO_IFACE_CMD_SET_BANDWIDTH, &bw, NULL);
    }

    // Called every frame from the FFT redraw: Doppler tuning
    static void ifaceHandler(int code, void* in, void* out, void* ctx) {
        SatTrackerModule* _this = (SatTrackerModule*)ctx;
        if (code == SAT_TRACKER_IFACE_CMD_GET_SIGNAL && out) {
            std::lock_guard<std::mutex> lck(_this->sigMtx);
            *(SatTrackerSignal*)out = _this->sig;
        }
    }

    // What the selected satellite sends right now, as received here (for the demo source)
    void publishSignal() {
        double now = simNow();
        if (now - lastPublish < 0.1) { return; }
        lastPublish = now;
        SatTrackerSignal sg;
        Live l = live();
        if (enabled && l.valid && l.tx) {
            sg.active = true;
            sg.freq = satpass::doppler(l.tx->centre(), l.look.rangeRateKms);
            sg.width = l.tx->isLinear() ? (l.tx->downHigh - l.tx->downLow) : 0;
            sg.baud = l.tx->baud;
            sg.elevation = l.look.el;
            sg.rangeKm = l.look.rangeKm;
            std::string m = l.tx->mode;
            std::transform(m.begin(), m.end(), m.begin(), ::toupper);
            if (l.tx->isLinear() || m == "USB" || m == "LSB" || m == "SSB") { sg.kind = SAT_SIGNAL_LINEAR; }
            else if (m == "CW") { sg.kind = SAT_SIGNAL_CW; }
            else if (m == "FM" || m == "FMN") { sg.kind = SAT_SIGNAL_FM; }
            else { sg.kind = SAT_SIGNAL_DATA; }
        }
        std::lock_guard<std::mutex> lck(sigMtx);
        sig = sg;
    }

    // Simulated pass: shift the clock so the selected satellite rises in a few seconds
    void simulatePass() {
        Live l = live();
        if (!l.sat || !l.pass.valid || l.pass.always) { return; }
        double now = simNow();
        if (l.look.el > 0) { return; }
        g_timeShift = g_timeShift.load() + (l.pass.aos - 10.0 - now);
        simLos = l.pass.los;
        lastCmd = 0;
        lastPublish = 0;
        lastTick = 0;
        request(false);
    }

    void realTime() {
        g_timeShift = 0;
        simLos = 0;
        lastCmd = 0;
        lastPublish = 0;
        lastTick = 0;
        request(false);
    }

    void tick() {
        double now = simNow();
        if (simLos > 0 && now > simLos + 30.0) { realTime(); now = simNow(); }
        publishSignal();
        if (!enabled || !tracking) { return; }
        if (now - lastTick < 0.2) { return; }
        lastTick = now;
        Live l = live();
        if (!l.valid || !l.tx) { return; }

        // The user tuned by hand (finger on the waterfall, frequency box): keep that as an offset in
        // the satellite's frame, so tracking continues across a linear transponder or off a drifting beacon
        double cur = currentTune();
        if (lastCmd != 0 && std::fabs(cur - lastCmd) > 1.0) {
            double factor = l.fTx > 0 ? l.fRx / l.fTx : 1.0;
            userOffset += (cur - lastCmd) / factor;
            flog::info("Cubesat Hunter: manual retune {:.0f} Hz (cur {:.0f}, last {:.0f}), offset {:.0f} Hz", cur - lastCmd, cur, lastCmd, userOffset);
            l = live();
        }
        if (lastCmd == 0 || std::fabs(l.fRx - lastCmd) >= stepHz) {
            // A jump to another band: centre the receiver a little below the signal (away from the
            // DC spike) so the VFO is on screen; small Doppler steps move the VFO only
            double bw = gui::waterfall.getBandwidth(), vbw = gui::waterfall.getViewBandwidth();
            if (std::fabs(l.fRx - cur) > bw * 0.25) {
                double off = std::min(0.2 * bw, 0.25 * vbw);
                tuner::centerTuning(gui::waterfall.selectedVFO, l.fRx - off);
            }
            tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, l.fRx);
            lastCmd = currentTune();
            if (std::fabs(lastCmd - l.fRx) > 1.0) { lastCmd = l.fRx; }
        }
    }

    void startTracking() {
        Live l = live();
        if (!l.tx) { return; }
        userOffset = 0;
        lastCmd = 0;
        if (autoMode) { setDemod(*l.tx); }
        tracking = true;
        lastTick = 0;
        tick();
    }

    // ------------------------------------------------------------------------------------------
    // Phone orientation (Android): pointing direction of the back of the phone and screen heading

    struct Pose {
        bool valid = false;
        bool flat = true;       // screen up: radar turns with the compass
        double heading = 0;     // azimuth at the top of the radar (deg, true north)
        double az = 0, el = 0;  // where the back of the phone points (pointing mode)
    };

    Pose pose() {
        Pose p;
#ifdef __ANDROID__
        if (!compassOn) { return p; }
        float R[9], decl = 0;
        int rot = 0;
        if (!backend::getOrientation(R, decl, rot)) { return p; }
        if (std::isnan(decl)) {
            // No GPS fix yet: declination from the station position is good enough (it changes slowly)
            decl = (float)declApprox;
        }
        // Device axes in world (E, N, U): columns of R
        double xd[3] = { R[0], R[3], R[6] }, yd[3] = { R[1], R[4], R[7] }, zd[3] = { R[2], R[5], R[8] };
        double pv[3] = { -zd[0], -zd[1], -zd[2] };   // out of the back of the phone
        double pel = std::asin(std::clamp(pv[2], -1.0, 1.0)) / DEG;
        // Top of the screen in device coordinates, by display rotation
        double up[3];
        for (int i = 0; i < 3; i++) {
            up[i] = rot == 1 ? xd[i] : rot == 2 ? -yd[i] : rot == 3 ? -xd[i] : yd[i];
        }
        double headTarget;
        if (pel < -50.0) {
            p.flat = true;
            headTarget = std::atan2(up[0], up[1]) / DEG + decl;
        }
        else {
            p.flat = false;
            p.az = std::fmod(std::atan2(pv[0], pv[1]) / DEG + decl + 720.0, 360.0);
            p.el = pel;
            headTarget = p.az;
        }
        // Smooth the heading (vector average, no jump at north)
        double hx = std::sin(headTarget * DEG), hy = std::cos(headTarget * DEG);
        if (!headInit) { smX = hx; smY = hy; headInit = true; }
        smX += 0.25 * (hx - smX);
        smY += 0.25 * (hy - smY);
        p.heading = std::fmod(std::atan2(smX, smY) / DEG + 360.0, 360.0);
        p.valid = true;
#endif
        return p;
    }

    // ------------------------------------------------------------------------------------------
    // Radar overlay on the spectrum

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        SatTrackerModule* _this = (SatTrackerModule*)ctx;
        _this->tick();
        _this->pollGps();
        if (!_this->enabled || !_this->radarOn || _this->selNorad == 0 || gui::mainWindow.menuCoversSpectrum()) {
            _this->radarRect = ImRect();
            return;
        }
        _this->drawRadar(args);
    }

    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        SatTrackerModule* _this = (SatTrackerModule*)ctx;
        if (_this->radarRect.GetWidth() <= 0) { return; }
        ImVec2 m = ImGui::GetMousePos();
        if (!_this->radarRect.Contains(m)) { return; }
        gui::waterfall.inputHandled = true;   // no tuning or panning under the radar (it has its own tap)
    }

    // The radar is its own small window over the spectrum and the waterfall (the waterfall is
    // drawn after the FFT, so drawing into the FFT would hide most of it), sized from the screen
    void drawRadar(const ImGui::WaterFall::FFTRedrawArgs& args) {
        Live l = live();
        if (!l.sat) { radarRect = ImRect(); return; }
        Pose ps = pose();
        double heading = ps.valid ? ps.heading : 0.0;

        float s = style::uiScale;
        float textH = ImGui::GetTextLineHeight();
        float pad = 6 * s;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float vh = vp->Size.y, vw = vp->Size.x;
        float size = radarLarge ? std::min(vh * 0.78f, vw * 0.55f) : std::min(vh * 0.42f, vw * 0.32f);
        size = std::max(size, 120.0f * s);
        float boxH = size + 2 * textH + 4 * s;
        // Top left of the spectrum (the right edge carries the touch rail and zoom sliders)
        ImVec2 tl(args.min.x + pad, args.min.y + pad);
        ImVec2 boxMax(tl.x + size, tl.y + boxH);
        radarRect = ImRect(tl, boxMax);

        ImGui::SetNextWindowPos(tl);
        ImGui::SetNextWindowSize(ImVec2(size, boxH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        bool open = ImGui::Begin(("##sat_radar_" + name).c_str(), NULL,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        if (!open) { ImGui::End(); return; }
        if (ImGui::InvisibleButton("##radar_tap", ImVec2(size, boxH))) {
            radarLarge = !radarLarge;
            saveConfig();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(tl, boxMax, true);

        dl->AddRectFilled(tl, boxMax, IM_COL32(10, 14, 20, 215), 8 * s);
        ImVec2 c(tl.x + size * 0.5f, tl.y + size * 0.5f);
        float R = size * 0.5f - 12 * s;
        ImU32 grid = IM_COL32(90, 140, 120, 160), txt = IM_COL32(200, 230, 210, 255);
        auto pt = [&](double az, double el) {
            double r = R * (90.0 - std::max(el, 0.0)) / 90.0;
            double a = (az - heading) * DEG;
            return ImVec2(c.x + (float)(r * std::sin(a)), c.y - (float)(r * std::cos(a)));
        };
        for (int e : { 0, 30, 60 }) { dl->AddCircle(c, R * (90 - e) / 90.0f, grid, 48, 1.0f * s); }
        for (int a = 0; a < 360; a += 45) { dl->AddLine(pt(a, 90), pt(a, 0), IM_COL32(90, 140, 120, 70), 1.0f); }
        const char* card[] = { "N", "E", "S", "W" };
        for (int i = 0; i < 4; i++) {
            double a = (i * 90 - heading) * DEG;
            ImVec2 sz = ImGui::CalcTextSize(card[i]);
            ImVec2 p(c.x + (float)((R + 7 * s) * std::sin(a)) - sz.x * 0.5f, c.y - (float)((R + 7 * s) * std::cos(a)) - sz.y * 0.5f);
            dl->AddText(p, i == 0 ? IM_COL32(255, 110, 90, 255) : txt, card[i]);
        }

        // Pass track
        std::shared_ptr<Track> tr;
        {
            std::lock_guard<std::mutex> lck(rowsMtx);
            tr = track;
        }
        if (tr && tr->norad == l.sat->norad && tr->pts.size() > 1) {
            std::vector<ImVec2> poly;
            for (auto& p : tr->pts) { if (p.second >= 0) { poly.push_back(pt(p.first, p.second)); } }
            if (poly.size() > 1) {
                dl->AddPolyline(poly.data(), (int)poly.size(), IM_COL32(80, 170, 255, 220), 0, 2.0f * s);
                dl->AddCircleFilled(poly.front(), 3.5f * s, IM_COL32(80, 220, 120, 255));
                dl->AddCircleFilled(poly.back(), 3.5f * s, IM_COL32(240, 90, 80, 255));
            }
        }

        // Phone pointing (back of the phone) and guidance
        bool onTarget = false;
        if (ps.valid && !ps.flat) {
            ImVec2 pp = pt(ps.az, ps.el);
            float r = 7 * s;
            dl->AddLine(ImVec2(pp.x - r, pp.y), ImVec2(pp.x + r, pp.y), IM_COL32(255, 255, 255, 230), 1.5f * s);
            dl->AddLine(ImVec2(pp.x, pp.y - r), ImVec2(pp.x, pp.y + r), IM_COL32(255, 255, 255, 230), 1.5f * s);
            double dAz = wrap180(l.look.az - ps.az), dEl = l.look.el - ps.el;
            onTarget = std::fabs(dAz) * std::cos(std::max(0.0, l.look.el) * DEG) < 10.0 && std::fabs(dEl) < 10.0;
        }

        // Satellite
        bool up = l.look.el > 0;
        ImVec2 sp = pt(l.look.az, l.look.el);
        if (up) {
            if (onTarget) { dl->AddCircle(sp, 13 * s, IM_COL32(80, 255, 120, 255), 24, 3.0f * s); }
            dl->AddCircleFilled(sp, 6 * s, IM_COL32(255, 215, 60, 255));
        }
        else {
            dl->AddCircle(sp, 5 * s, IM_COL32(170, 170, 170, 200), 16, 1.5f * s);
        }

        // Text: name and status
        double now = simNow();
        char line1[96], line2[96];
        snprintf(line1, sizeof(line1), "%s", l.sat->name.c_str());
        if (ps.valid && !ps.flat && up) {
            if (onTarget) { snprintf(line2, sizeof(line2), "ON TARGET  el %.0f", l.look.el); }
            else {
                double dAz = wrap180(l.look.az - ps.az), dEl = l.look.el - ps.el;
                snprintf(line2, sizeof(line2), "%s %.0f  %s %.0f", dAz >= 0 ? "right" : "left", std::fabs(dAz), dEl >= 0 ? "up" : "down", std::fabs(dEl));
            }
        }
        else if (up) {
            snprintf(line2, sizeof(line2), "az %.0f el %.0f  LOS %s", l.look.az, l.look.el, fmtDuration(l.pass.los - now).c_str());
        }
        else if (l.pass.valid) {
            snprintf(line2, sizeof(line2), "AOS in %s  max %.0f", fmtDuration(l.pass.aos - now).c_str(), l.pass.maxEl);
        }
        else {
            snprintf(line2, sizeof(line2), "no pass in 24 h");
        }
        if (g_timeShift.load() != 0) {
            dl->AddText(ImVec2(tl.x + 6 * s, tl.y + 4 * s), IM_COL32(255, 190, 60, 255), "SIMULATED");
        }
        float ty = tl.y + size;
        dl->AddText(ImVec2(tl.x + 6 * s, ty), txt, line1);
        dl->AddText(ImVec2(tl.x + 6 * s, ty + textH), onTarget ? IM_COL32(80, 255, 120, 255) : txt, line2);
#ifdef __ANDROID__
        if (compassOn && !ps.valid) {
            dl->AddText(ImVec2(tl.x + 6 * s, tl.y + size - textH - 2 * s), IM_COL32(255, 190, 60, 255), "no compass data");
        }
        else if (compassOn) {
            dl->AddText(ImVec2(tl.x + 6 * s, tl.y + size - textH - 2 * s), IM_COL32(120, 200, 255, 255), ps.flat ? "compass: flat" : "compass: aim");
        }
#endif
        dl->PopClipRect();
        ImGui::End();
    }


    // ------------------------------------------------------------------------------------------
    // Desktop position: Windows Location Services, or IP geolocation (on request only)

    void setStation(double lat, double lon, double altM, bool keepAlt) {
        {
            std::lock_guard<std::mutex> lck(paramMtx);
            station.lat = std::clamp(lat, -90.0, 90.0);
            station.lon = std::clamp(lon, -180.0, 180.0);
            if (!keepAlt) { station.altM = altM; }
        }
        updateDeclination();
        saveConfig();
        request(false);
    }

    void locate(bool byIp) {
        if (locBusy) { return; }
        if (locThread.joinable()) { locThread.join(); }
        locBusy = true;
        setLocStatus(byIp ? "Asking an IP location service..." : "Asking Windows for the position...");
        locThread = std::thread([this, byIp]() {
            double lat = 0, lon = 0, acc = 0;
            std::string err = "not available on this system";
            std::string src;
            if (byIp) {
                std::string path = db.getDir() + "/ipgeo.json";
                std::error_code ec;
                std::filesystem::create_directories(db.getDir(), ec);
                err = satdb::httpGet("https://ipapi.co/json/", path);
                if (err.empty()) {
                    try {
                        std::ifstream f(path);
                        json j = json::parse(f);
                        if (j.contains("latitude") && j["latitude"].is_number() && j.contains("longitude") && j["longitude"].is_number()) {
                            lat = j["latitude"]; lon = j["longitude"];
                            acc = 20000;
                            src = "IP (" + (j.contains("city") && j["city"].is_string() ? j["city"].get<std::string>() : std::string("?")) + ")";
                        }
                        else { err = "no position in the reply"; }
                    }
                    catch (std::exception& e) { err = e.what(); }
                    std::filesystem::remove(path, ec);
                }
            }
#ifdef _WIN32
            else {
                err = windowsLocate(lat, lon, acc);
                src = "Windows";
            }
#endif
            if (err.empty()) {
                setStation(lat, lon, 0, true);
                char b[96];
                snprintf(b, sizeof(b), "%s: %.4f %.4f (+-%.0f m), %s", src.c_str(), lat, lon, acc, latLonToLocator(lat, lon).c_str());
                setLocStatus(b);
            }
            else { setLocStatus("Position: " + err); }
            locBusy = false;
        });
    }

    void setLocStatus(const std::string& s) {
        std::lock_guard<std::mutex> lck(statusMtx);
        locStatus = s;
    }

    // ------------------------------------------------------------------------------------------
    // GPS (Android)

    void pollGps() {
#ifdef __ANDROID__
        if (!followGps) { return; }
        double now = simNow();
        if (now - lastGpsPoll < 1.0) { return; }
        lastGpsPoll = now;
        double lat, lon, alt, acc, age;
        if (!backend::getLocation(lat, lon, alt, acc, age)) { gpsStatus = "waiting for a position..."; return; }
        char b[64];
        snprintf(b, sizeof(b), "GPS +-%.0f m, %.0f s old", acc, age);
        gpsStatus = b;
        double dLat = (lat - station.lat) * 111.0, dLon = (lon - station.lon) * 111.0 * std::cos(lat * DEG);
        if (std::sqrt(dLat * dLat + dLon * dLon) > 0.2 || std::fabs(alt - station.altM) > 50.0) {
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                station.lat = lat;
                station.lon = lon;
                station.altM = alt;
            }
            updateDeclination();
            saveConfig();
            request(false);
        }
#endif
    }

    // Rough magnetic declination for Europe (until the phone has a fix and supplies the exact value)
    void updateDeclination() {
        // Linear fit of WMM2025 over Central Europe: ~ +5.5 deg at 19 E, +0.25 deg per degree east
        declApprox = 5.5 + 0.25 * (station.lon - 19.0);
    }

    // ------------------------------------------------------------------------------------------
    // Menu

    static void menuHandler(void* ctx) {
        SatTrackerModule* _this = (SatTrackerModule*)ctx;
        _this->menu();
    }

    void menu() {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        double now = simNow();
        std::string id = "##sat_" + name;

        // Data
        bool b = busy;
        if (b) { style::beginDisabled(); }
        if (ImGui::Button(("Update data" + id).c_str())) { request(true); }
        if (b) { style::endDisabled(); }
        ImGui::SameLine();
        {
            double age = db.ageHours();
            std::shared_ptr<std::vector<Row>> r;
            {
                std::lock_guard<std::mutex> lck(rowsMtx);
                r = rows;
            }
            if (age < 0) { ImGui::TextUnformatted("no data yet"); }
            else if (age < 48) { ImGui::Text("%d passes, %.0f h old", r ? (int)r->size() : 0, age); }
            else { ImGui::Text("%d passes, %.0f days old", r ? (int)r->size() : 0, age / 24.0); }
        }
        {
            std::lock_guard<std::mutex> lck(statusMtx);
            if (!dbStats.empty()) { ImGui::TextDisabled("%s", dbStats.c_str()); }
            if (!status.empty()) { ImGui::TextWrapped("%s", status.c_str()); }
        }
        if (calcTotal > 0) {
            ImGui::ProgressBar((float)calcDone / (float)std::max(1, (int)calcTotal), ImVec2(-1, 0),
                               ("Computing passes " + std::to_string((int)calcDone) + " / " + std::to_string((int)calcTotal)).c_str());
        }

        // Station
        bool stChanged = false;
        if (ImGui::CollapsingHeader(("Ground station" + id).c_str())) {
            satpass::Station st;
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                st = station;
            }
            ImGui::LeftLabel("Latitude");
            ImGui::FillWidth();
            stChanged |= ImGui::InputDouble(("##lat" + id).c_str(), &st.lat, 0, 0, "%.4f");
            ImGui::LeftLabel("Longitude");
            ImGui::FillWidth();
            stChanged |= ImGui::InputDouble(("##lon" + id).c_str(), &st.lon, 0, 0, "%.4f");
            ImGui::LeftLabel("Altitude (m)");
            ImGui::FillWidth();
            stChanged |= ImGui::InputDouble(("##alt" + id).c_str(), &st.altM, 0, 0, "%.0f");
            ImGui::LeftLabel("Locator");
            ImGui::FillWidth();
            if (!locEditing) {
                std::string loc = latLonToLocator(st.lat, st.lon);
                snprintf(locBuf, sizeof(locBuf), "%s", loc.c_str());
            }
            if (ImGui::InputText(("##loc" + id).c_str(), locBuf, sizeof(locBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                double la, lo;
                if (locatorToLatLon(locBuf, la, lo)) { st.lat = la; st.lon = lo; stChanged = true; }
            }
            locEditing = ImGui::IsItemActive();
#ifndef __ANDROID__
            bool lb = locBusy;
            if (lb) { style::beginDisabled(); }
#ifdef _WIN32
            if (ImGui::Button(("Locate (Windows)" + id).c_str())) { locate(false); }
            ImGui::SameLine();
#endif
            if (ImGui::Button(("Locate by IP" + id).c_str())) { locate(true); }
            if (lb) { style::endDisabled(); }
            {
                std::lock_guard<std::mutex> lck(statusMtx);
                if (!locStatus.empty()) { ImGui::TextWrapped("%s", locStatus.c_str()); }
            }
#endif
            if (stChanged) {
                st.lat = std::clamp(st.lat, -90.0, 90.0);
                st.lon = std::clamp(st.lon, -180.0, 180.0);
                std::lock_guard<std::mutex> lck(paramMtx);
                station = st;
            }
#ifdef __ANDROID__
            if (ImGui::Checkbox(("Follow phone GPS" + id).c_str(), &followGps)) {
                backend::setLocationUpdates(followGps);
                lastGpsPoll = 0;
                saveConfig();
            }
            if (followGps && !gpsStatus.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", gpsStatus.c_str());
            }
#endif
            ImGui::LeftLabel("Auto update (days, 0 = off)");
            ImGui::FillWidth();
            if (ImGui::InputInt(("##upd" + id).c_str(), &updateDays)) {
                updateDays = std::clamp(updateDays, 0, 30);
                saveConfig();
            }
        }
        if (stChanged) {
            updateDeclination();
            saveConfig();
            request(false);
        }

        // Filter
        bool fChanged = false;
        if (ImGui::CollapsingHeader(("Filter" + id).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            satdb::Filter f;
            float me;
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                f = filter;
                me = minEl;
            }
            static const int order[] = { satdb::BAND_2M, satdb::BAND_70CM, satdb::BAND_23CM, satdb::BAND_SBAND, satdb::BAND_13CM, satdb::BAND_HF, satdb::BAND_OTHER };
            int col = 0;
            for (int i : order) {
                float w = ImGui::CalcTextSize(satdb::BAND_NAMES[i]).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 3;
                if (col > 0) {
                    ImGui::SameLine();
                    if (ImGui::GetCursorPosX() + w > menuWidth) { ImGui::NewLine(); col = 0; }
                }
                fChanged |= ImGui::Checkbox((std::string(satdb::BAND_NAMES[i]) + id + "b" + std::to_string(i)).c_str(), &f.bands[i]);
                col++;
            }
            fChanged |= ImGui::Checkbox(("Amateur only" + id).c_str(), &f.amateurOnly);
            ImGui::SameLine();
            fChanged |= ImGui::Checkbox(("Active only" + id).c_str(), &f.activeOnly);
            fChanged |= ImGui::Checkbox(("Favourites only" + id).c_str(), &f.favoritesOnly);
            ImGui::LeftLabel("Min. max elevation");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(("##minel" + id).c_str(), &me, 0.0f, 60.0f, "%.0f deg")) { fChanged = true; }
            ImGui::LeftLabel("Search");
            ImGui::FillWidth();
            ImGui::InputText(("##search" + id).c_str(), searchBuf, sizeof(searchBuf));
            if (fChanged) {
                std::lock_guard<std::mutex> lck(paramMtx);
                filter = f;
                minEl = me;
            }
        }
        if (fChanged && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            saveConfig();
            request(false);
        }
        else if (fChanged) { filterDirty = true; }
        if (filterDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            filterDirty = false;
            saveConfig();
            request(false);
        }

        // Pass list
        std::shared_ptr<std::vector<Row>> r;
        {
            std::lock_guard<std::mutex> lck(rowsMtx);
            r = rows;
        }
        float tableH = std::max(120.0f * style::uiScale, ImGui::GetTextLineHeightWithSpacing() * 9.5f);
        if (ImGui::BeginTable(("sat_table" + id).c_str(), 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, tableH))) {
            ImGui::TableSetupColumn("##fav", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 1.3f);
            ImGui::TableSetupColumn("Satellite", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("XXXXXXXXXXXX [HU]").x);
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("in 00:00 ").x);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("90 ").x);
            ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("2250.0000 ").x);
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("GMSK, BPSK ").x);
            ImGui::TableSetupColumn("AOS -> LOS az", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("AOS -> LOS az ").x);
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();
            if (r) {
                for (auto& row : *r) {
                    if (!satdb::satMatchesSearch(*row.sat, searchBuf)) { continue; }
                    if (row.pass.los < now && !row.pass.always) { continue; }   // over, the next recalc refreshes it
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool fav = isFavorite(row.sat->norad);
                    if (starButton(("##star" + std::to_string(row.sat->norad)).c_str(), fav)) { setFavorite(row.sat->norad, !fav); }
                    ImGui::TableSetColumnIndex(1);
                    bool visible = row.pass.aos <= now;
                    bool sel = row.sat->norad == selNorad;
                    if (visible) { ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(110, 230, 130, 255)); }
                    std::string label = row.sat->name + (row.sat->hungarian ? "  [HU]" : "");
                    if (ImGui::Selectable((label + "##" + std::to_string(row.sat->norad)).c_str(), sel)) {
                        select(row.sat);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("NORAD %d\n%s\n%s", row.sat->norad, row.bands.c_str(), row.modes.c_str());
                    }
                    ImGui::TableSetColumnIndex(2);
                    if (row.pass.always) { ImGui::TextUnformatted("always"); }
                    else if (visible) { ImGui::Text("up %s", fmtDuration(row.pass.los - now).c_str()); }
                    else if (row.pass.aos - now < 3600.0) { ImGui::Text("in %s", fmtDuration(row.pass.aos - now).c_str()); }
                    else { ImGui::TextUnformatted(fmtClock(row.pass.aos).c_str()); }
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.0f", row.pass.maxEl);
                    ImGui::TableSetColumnIndex(4);
                    if (row.freq > 0) { ImGui::TextUnformatted(fmtFreq(row.freq).c_str()); }
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(row.modes.c_str());
                    ImGui::TableSetColumnIndex(6);
                    if (!row.pass.always) { ImGui::Text("%.0f -> %.0f", row.pass.aosAz, row.pass.losAz); }
                    if (visible) { ImGui::PopStyleColor(); }
                }
            }
            ImGui::EndTable();
        }
        if (r && r->empty() && calcTotal == 0 && db.ageHours() >= 0) {
            ImGui::TextWrapped("No pass in the next 24 h for this filter. Try more bands, untick \"Active only\" or lower the minimum elevation.");
        }

        // Selected satellite
        Live l = live();
        if (!l.sat) {
            ImGui::TextDisabled("Tap a satellite to tune to it.");
            return;
        }
        ImGui::Separator();
        {
            bool fav = isFavorite(l.sat->norad);
            if (starButton("##selstar", fav)) { setFavorite(l.sat->norad, !fav); }
            ImGui::SameLine();
            ImGui::Text("%s  (NORAD %d)%s", l.sat->name.c_str(), l.sat->norad, l.sat->hungarian ? "  Hungarian" : "");
        }

        // Transmitter list (only those passing the filter, plus the selected one)
        satdb::Filter f;
        {
            std::lock_guard<std::mutex> lck(paramMtx);
            f = filter;
        }
        std::string prev = l.tx ? txLabel(*l.tx) : std::string("Select a downlink");
        ImGui::FillWidth();
        if (ImGui::BeginCombo(("##tx" + id).c_str(), prev.c_str())) {
            for (auto& t : l.sat->tx) {
                bool match = satdb::txMatches(t, f);
                if (!match && t.uuid != selTx && !showAllTx) { continue; }
                if (ImGui::Selectable((txLabel(t) + "##" + t.uuid).c_str(), t.uuid == selTx)) {
                    selTx = t.uuid;
                    saveConfig();
                    startTracking();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox(("Show all downlinks" + id).c_str(), &showAllTx);

        if (l.valid) {
            ImGui::Text("Az %.1f  El %.1f  %.0f km", l.look.az, l.look.el, l.look.rangeKm);
            if (l.look.el > 0) { ImGui::Text("Overhead now, LOS in %s", fmtDuration(l.pass.los - now).c_str()); }
            else if (l.pass.valid) {
                ImGui::Text("AOS %s (in %s), max %.0f deg, az %.0f -> %.0f", fmtClock(l.pass.aos).c_str(),
                            fmtDuration(l.pass.aos - now).c_str(), l.pass.maxEl, l.pass.aosAz, l.pass.losAz);
            }
            if (l.tx) {
                double dop = l.fRx - l.fTx;
                ImGui::Text("Doppler %+.2f kHz -> %s MHz", dop / 1e3, fmtFreq(l.fRx).c_str());
                if (l.look.el <= 0) { ImGui::TextDisabled("(waiting on the AOS frequency)"); }
            }
        }

        if (g_timeShift.load() != 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 190, 60, 255));
            ImGui::Text("Simulated time (%+.1f h)", g_timeShift.load() / 3600.0);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton(("Real time" + id).c_str())) { realTime(); }
        }
        else if (l.valid && l.look.el <= 0 && l.pass.valid && !l.pass.always) {
            if (ImGui::Button(("Simulate this pass now" + id).c_str())) { simulatePass(); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Classroom demo: the clock jumps to just before AOS.\nWith the Demo source the satellite's signal appears with its real Doppler."); }
        }
        if (l.tx) {
            bool t = tracking;
            if (ImGui::Checkbox(("Doppler tracking" + id).c_str(), &t)) {
                if (t) { startTracking(); } else { tracking = false; }
            }
            if (tracking && std::fabs(userOffset) >= 1.0) {
                ImGui::SameLine();
                ImGui::Text("offset %+.2f kHz", userOffset / 1e3);
                ImGui::SameLine();
                if (ImGui::SmallButton(("Reset" + id).c_str())) {
                    userOffset = 0;
                    lastCmd = 0;
                }
            }
        }
        if (ImGui::Checkbox(("Set demodulator" + id).c_str(), &autoMode)) {
            saveConfig();
            if (autoMode && l.tx) { setDemod(*l.tx); }
        }
        ImGui::LeftLabel("Retune step (Hz)");
        ImGui::FillWidth();
        if (ImGui::InputInt(("##step" + id).c_str(), &stepHz)) {
            stepHz = std::clamp(stepHz, 1, 1000);
            saveConfig();
        }
        if (ImGui::Checkbox(("Radar on spectrum" + id).c_str(), &radarOn)) { saveConfig(); }
#ifdef __ANDROID__
        ImGui::SameLine();
        if (ImGui::Checkbox(("Compass" + id).c_str(), &compassOn)) {
            backend::setOrientationSensor(compassOn);
            headInit = false;
            saveConfig();
        }
        if (compassOn) {
            ImGui::TextDisabled("Flat: radar turns with the phone. Raised: aim the back of the phone at the satellite.");
        }
#endif
    }

    bool isFavorite(int norad) {
        std::lock_guard<std::mutex> lck(paramMtx);
        return favorites.count(norad) > 0;
    }

    void setFavorite(int norad, bool on) {
        {
            std::lock_guard<std::mutex> lck(paramMtx);
            if (on) { favorites.insert(norad); } else { favorites.erase(norad); }
        }
        saveConfig();
        request(false);   // re-sort the list
    }

    // Tap target with a drawn star (the UI font has no star glyph)
    static bool starButton(const char* id, bool on) {
        float h = ImGui::GetTextLineHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(id, ImVec2(h * 1.2f, h));
        bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(p.x + h * 0.6f, p.y + h * 0.5f);
        float ro = h * 0.48f, ri = ro * 0.42f;
        ImVec2 pts[10];
        for (int i = 0; i < 10; i++) {
            float a = (float)(i * 3.14159265358979323846 / 5.0 - 3.14159265358979323846 / 2.0);
            float r = (i & 1) ? ri : ro;
            pts[i] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a));
        }
        if (on) {
            for (int i = 0; i < 10; i++) { dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], IM_COL32(255, 205, 50, 255)); }
        }
        else {
            dl->AddPolyline(pts, 10, hov ? IM_COL32(220, 220, 220, 255) : IM_COL32(140, 140, 140, 255), ImDrawFlags_Closed, 1.2f);
        }
        return clicked;
    }

    static std::string txLabel(const satdb::Transmitter& t) {
        std::string s = fmtFreq(t.centre()) + " " + (t.mode.empty() ? "?" : t.mode);
        if (t.isLinear()) { s += " (" + fmtFreq(t.downLow) + "-" + fmtFreq(t.downHigh) + ")"; }
        if (t.baud > 0) { s += " " + std::to_string((int)t.baud) + " Bd"; }
        if (!t.description.empty()) { s += " - " + t.description; }
        if (!t.alive || t.status != "active") { s += " [inactive]"; }
        return s;
    }

    void select(const std::shared_ptr<satdb::Satellite>& s) {
        if (s->norad != selNorad) {
            selNorad = s->norad;
            // First downlink that passes the filter
            satdb::Filter f;
            {
                std::lock_guard<std::mutex> lck(paramMtx);
                f = filter;
            }
            selTx.clear();
            for (auto& t : s->tx) { if (satdb::txMatches(t, f)) { selTx = t.uuid; break; } }
            if (selTx.empty() && !s->tx.empty()) { selTx = s->tx.front().uuid; }
            {
                std::lock_guard<std::mutex> lck(rowsMtx);
                selSat = s;
                selPass = satpass::Pass();
                track.reset();
            }
            saveConfig();
            request(false);   // pass + radar track for the new selection
        }
        startTracking();
    }

    std::string name;
    bool enabled = true;

    satdb::Database db;

    // Parameters (UI writes, worker reads)
    std::mutex paramMtx;
    satpass::Station station;
    satdb::Filter filter;
    float minEl = 5.0f;
    std::set<int> favorites;
    char searchBuf[64] = { 0 };
    bool filterDirty = false;

    // Results (worker writes, UI reads)
    std::mutex rowsMtx;
    std::shared_ptr<std::vector<Row>> rows;
    satpass::Station rowsStation;
    std::shared_ptr<satdb::Satellite> selSat;
    satpass::Pass selPass;
    std::shared_ptr<Track> track;

    // Selection and tracking (UI thread)
    int selNorad = 0;
    std::string selTx;
    bool tracking = false;
    bool autoMode = true;
    bool showAllTx = false;
    int stepHz = 10;
    double userOffset = 0;   // Hz, satellite frame
    double lastCmd = 0;
    double lastTick = 0;

    // Radar / sensors
#ifdef __ANDROID__
    bool radarOn = true, radarLarge = false, compassOn = true, followGps = true;
#else
    bool radarOn = true, radarLarge = false, compassOn = false, followGps = false;
#endif
    ImRect radarRect;
    bool headInit = false;
    double smX = 0, smY = 1;
    double declApprox = 5.5;
    double lastGpsPoll = 0;
    std::string gpsStatus;

    int updateDays = 3;

    // Demo: published signal and simulated pass
    std::mutex sigMtx;
    SatTrackerSignal sig;
    double lastPublish = 0;
    double simLos = 0;

    // Desktop location
    std::thread locThread;
    std::atomic<bool> locBusy{ false };
    std::string locStatus;
    char locBuf[12] = { 0 };
    bool locEditing = false;

    // Worker
    std::thread worker;
    std::mutex wMtx;
    std::condition_variable wCv;
    bool quit = false, wantDownload = false, wantRecalc = false;
    std::atomic<bool> busy{ false };
    std::atomic<int> calcDone{ 0 }, calcTotal{ 0 };
    std::string dbStats;
    std::mutex statusMtx;
    std::string status;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/sat_tracker_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SatTrackerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SatTrackerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
