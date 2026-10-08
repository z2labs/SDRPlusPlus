// Spectrum export: the displayed FFT (any source, including on-chip spectrum modes that have no IQ
// to record) saved as CSV for measurements. One snapshot, an average or a max hold over N FFTs,
// or a continuous log (one row per interval) for drift / occupancy studies.
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/folder_select.h>
#include <core.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:            */ "spectrum_export",
    /* Description:     */ "Export the FFT to CSV for measurements",
    /* Author:          */ "Zoltan Doczi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static const char* MODES_TXT = "Snapshot\0Average\0Max hold\0";
static const char* SPANS_TXT = "Full span\0Visible span\0";

class SpectrumExportModule : public ModuleManager::Instance {
public:
    SpectrumExportModule(std::string name) : folderSelect(defaultFolder()) {
        this->name = name;
        config.acquire();
        if (config.conf[name].contains("folder")) { folderSelect.setPath(config.conf[name]["folder"]); }
        if (config.conf[name].contains("mode")) { mode = std::clamp<int>(config.conf[name]["mode"], 0, 2); }
        if (config.conf[name].contains("frames")) { frames = std::clamp<int>(config.conf[name]["frames"], 1, 10000); }
        if (config.conf[name].contains("span")) { span = std::clamp<int>(config.conf[name]["span"], 0, 1); }
        if (config.conf[name].contains("interval")) { interval = std::clamp<double>(config.conf[name]["interval"], 0.1, 3600.0); }
        config.release();
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        worker = std::thread(&SpectrumExportModule::run, this);
    }

    ~SpectrumExportModule() {
        quit = true;
        if (worker.joinable()) { worker.join(); }
        gui::menu.removeEntry(name);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    static std::string defaultFolder() {
#ifdef __ANDROID__
        return "/storage/emulated/0/Download/sdrpp-spectrum";
#else
        return "%ROOT%/spectrum";
#endif
    }

    // One FFT line plus the frequency axis it belongs to
    struct Frame {
        std::vector<float> db;
        double center = 0, span = 0;
    };

    // Latest FFT if it is newer than lastSeq (UI-independent: the waterfall keeps a copy of every line)
    bool grab(Frame& f) {
        uint64_t seq = gui::waterfall.copyLatestRawFFT(f.db);
        if (seq == 0 || seq == lastSeq || f.db.empty()) { return false; }
        lastSeq = seq;
        f.center = gui::waterfall.getCenterFrequency();
        f.span = gui::waterfall.getBandwidth();
        return true;
    }

    // Bin range [b0, b1) to write: everything, or what is on screen
    void binRange(const Frame& f, int& b0, int& b1) {
        int n = (int)f.db.size();
        b0 = 0; b1 = n;
        if (spanSel == 1 && f.span > 0) {
            double vbw = gui::waterfall.getViewBandwidth(), off = gui::waterfall.getViewOffset();
            b0 = std::clamp<int>((int)std::floor(((off - vbw / 2.0) / f.span + 0.5) * n), 0, n);
            b1 = std::clamp<int>((int)std::ceil(((off + vbw / 2.0) / f.span + 0.5) * n), b0, n);
        }
    }

    static double binFreq(const Frame& f, int i) {
        int n = (int)f.db.size();
        return f.center + (double)(i - n / 2) * f.span / n;
    }

    static std::string timeStamp(bool forFile) {
        auto now = std::chrono::system_clock::now();
        time_t t = std::chrono::system_clock::to_time_t(now);
        int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        char b[64];
        if (forFile) { strftime(b, sizeof(b), "%Y%m%d_%H%M%S", localtime(&t)); return b; }
        strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", gmtime(&t));
        char r[80];
        snprintf(r, sizeof(r), "%s.%03dZ", b, ms);
        return r;
    }

    std::string outDir() {
        std::string d = folderSelect.expandString(folderSelect.path);
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }

    // Accumulate N frames into acc (average in linear power, or max), result in dB
    bool collect(int n, int how, Frame& out) {
        Frame f;
        std::vector<double> acc;
        int got = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10 + n / 5);
        while (got < n && !quit) {
            if (std::chrono::steady_clock::now() > deadline) { break; }
            if (!grab(f)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            if (got == 0 || f.db.size() != acc.size() || f.center != out.center || f.span != out.span) {
                acc.assign(f.db.size(), how == 2 ? -1e30 : 0.0);
                out.center = f.center; out.span = f.span;
                got = 0;
            }
            for (size_t i = 0; i < f.db.size(); i++) {
                if (how == 2) { acc[i] = std::max<double>(acc[i], f.db[i]); }
                else { acc[i] += std::pow(10.0, f.db[i] / 10.0); }
            }
            got++;
            progress = (float)got / n;
        }
        if (got == 0) { return false; }
        out.db.resize(acc.size());
        for (size_t i = 0; i < acc.size(); i++) {
            out.db[i] = how == 2 ? (float)acc[i] : (float)(10.0 * std::log10(acc[i] / got + 1e-30));
        }
        framesUsed = got;
        return true;
    }

    void saveSnapshot() {
        Frame f;
        int how = mode, n = mode == 0 ? 1 : frames;
        if (!collect(n, how, f)) { setStatus("No FFT data: start the source first"); return; }
        int b0, b1;
        binRange(f, b0, b1);
        char fname[160];
        snprintf(fname, sizeof(fname), "spectrum_%s_%.6fMHz.csv", timeStamp(true).c_str(), f.center / 1e6);
        std::string path = outDir() + "/" + fname;
        FILE* fp = fopen(path.c_str(), "w");
        if (!fp) { setStatus("Cannot write " + path); return; }
        writeHeader(fp, f, b0, b1, n);
        fprintf(fp, "frequency_hz,level_db\n");
        for (int i = b0; i < b1; i++) { fprintf(fp, "%.1f,%.2f\n", binFreq(f, i), f.db[i]); }
        fclose(fp);
        flog::info("Spectrum export: {} ({} bins)", path, b1 - b0);
        setStatus("Saved " + path);
    }

    void writeHeader(FILE* fp, const Frame& f, int b0, int b1, int n) {
        const char* modes[] = { "snapshot", "average", "max hold" };
        fprintf(fp, "# SDR++ spectrum export\n");
        fprintf(fp, "# time_utc,%s\n", timeStamp(false).c_str());
        std::string src;
        core::configManager.acquire();
        if (core::configManager.conf.contains("source")) { src = core::configManager.conf["source"]; }
        core::configManager.release();
        fprintf(fp, "# source,%s\n", src.c_str());
        fprintf(fp, "# center_hz,%.1f\n", f.center);
        fprintf(fp, "# span_hz,%.1f\n", f.span);
        fprintf(fp, "# fft_bins,%d\n", (int)f.db.size());
        fprintf(fp, "# bin_width_hz,%.3f\n", f.span / f.db.size());
        fprintf(fp, "# exported_bins,%d..%d\n", b0, b1 - 1);
        fprintf(fp, "# mode,%s,%d frames (%d used)\n", modes[mode], n, framesUsed);
        fprintf(fp, "# level unit,dB as displayed (dBFS, not calibrated to dBm)\n");
    }

    // Continuous log: one row per interval (time, then one column per bin); a new file whenever the
    // frequency axis changes
    void logStep() {
        Frame f;
        int how = mode == 2 ? 2 : 1;
        int n = mode == 0 ? 1 : frames;
        if (!collect(n, how, f)) { return; }
        int b0, b1;
        binRange(f, b0, b1);
        if (!logFile || f.center != logCenter || f.span != logSpan || (int)f.db.size() != logBins || b0 != logB0 || b1 != logB1) {
            if (logFile) { fclose(logFile); logFile = NULL; }
            char fname[160];
            snprintf(fname, sizeof(fname), "spectrum_log_%s_%.6fMHz.csv", timeStamp(true).c_str(), f.center / 1e6);
            logPath = outDir() + "/" + fname;
            logFile = fopen(logPath.c_str(), "w");
            if (!logFile) { setStatus("Cannot write " + logPath); logging = false; return; }
            writeHeader(logFile, f, b0, b1, n);
            fprintf(logFile, "time_utc");
            for (int i = b0; i < b1; i++) { fprintf(logFile, ",%.1f", binFreq(f, i)); }
            fprintf(logFile, "\n");
            logCenter = f.center; logSpan = f.span; logBins = (int)f.db.size(); logB0 = b0; logB1 = b1;
            logRows = 0;
            flog::info("Spectrum log: {}", logPath);
        }
        fprintf(logFile, "%s", timeStamp(false).c_str());
        for (int i = b0; i < b1; i++) { fprintf(logFile, ",%.2f", f.db[i]); }
        fprintf(logFile, "\n");
        fflush(logFile);
        logRows++;
        setStatus("Logging to " + logPath + " (" + std::to_string(logRows) + " rows)");
    }

    void run() {
        auto nextLog = std::chrono::steady_clock::now();
        while (!quit) {
            if (snapshotRequested.exchange(false)) {
                busy = true;
                spanSel = span;
                saveSnapshot();
                busy = false;
                continue;
            }
            if (logging) {
                auto now = std::chrono::steady_clock::now();
                if (now >= nextLog) {
                    nextLog = now + std::chrono::milliseconds((int)(interval * 1000.0));
                    spanSel = span;
                    logStep();
                }
            }
            else if (logFile) {
                fclose(logFile);
                logFile = NULL;
                setStatus("Log saved: " + logPath + " (" + std::to_string(logRows) + " rows)");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (logFile) { fclose(logFile); logFile = NULL; }
    }

    void setStatus(const std::string& s) {
        std::lock_guard<std::mutex> l(statusMtx);
        status = s;
    }

    void saveConfig() {
        config.acquire();
        config.conf[name]["folder"] = folderSelect.path;
        config.conf[name]["mode"] = mode;
        config.conf[name]["frames"] = frames;
        config.conf[name]["span"] = span;
        config.conf[name]["interval"] = interval;
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        SpectrumExportModule* _this = (SpectrumExportModule*)ctx;
        float w = ImGui::GetContentRegionAvail().x;
        bool locked = _this->logging || _this->busy;
        if (locked) { style::beginDisabled(); }

        ImGui::LeftLabel("Folder");
        if (_this->folderSelect.render("##_spectrum_export_folder_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) { _this->saveConfig(); }
        }

        ImGui::LeftLabel("Mode");
        ImGui::SetNextItemWidth(w - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_spectrum_export_mode", &_this->mode, MODES_TXT)) { _this->saveConfig(); }
        if (_this->mode != 0) {
            ImGui::LeftLabel("FFTs");
            ImGui::SetNextItemWidth(w - ImGui::GetCursorPosX());
            if (ImGui::InputInt("##_spectrum_export_frames", &_this->frames, 1, 10)) {
                _this->frames = std::clamp<int>(_this->frames, 1, 10000);
                _this->saveConfig();
            }
        }
        ImGui::LeftLabel("Range");
        ImGui::SetNextItemWidth(w - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_spectrum_export_span", &_this->span, SPANS_TXT)) { _this->saveConfig(); }
        if (locked) { style::endDisabled(); }

        if (_this->busy) {
            ImGui::ProgressBar(_this->progress, ImVec2(-FLT_MIN, 0));
        }
        else {
            if (_this->logging) { style::beginDisabled(); }
            if (ImGui::Button("Save CSV##_spectrum_export_save", ImVec2(-FLT_MIN, 0))) {
                _this->progress = 0;
                _this->snapshotRequested = true;
            }
            if (_this->logging) { style::endDisabled(); }
        }

        ImGui::Separator();
        if (_this->logging) { style::beginDisabled(); }
        ImGui::LeftLabel("Log every (s)");
        ImGui::SetNextItemWidth(w - ImGui::GetCursorPosX());
        if (ImGui::InputDouble("##_spectrum_export_interval", &_this->interval, 1.0, 10.0, "%.1f")) {
            _this->interval = std::clamp<double>(_this->interval, 0.1, 3600.0);
            _this->saveConfig();
        }
        if (_this->logging) { style::endDisabled(); }
        bool lg = _this->logging;
        if (_this->busy) { style::beginDisabled(); }
        if (ImGui::Button(lg ? "Stop logging##_spectrum_export_log" : "Start logging##_spectrum_export_log", ImVec2(-FLT_MIN, 0))) {
            _this->logging = !lg;
        }
        if (_this->busy) { style::endDisabled(); }

        std::string st;
        {
            std::lock_guard<std::mutex> l(_this->statusMtx);
            st = _this->status;
        }
        if (!st.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", st.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    std::string name;
    bool enabled = true;
    FolderSelect folderSelect;
    int mode = 1, frames = 20, span = 0, spanSel = 0;
    double interval = 10.0;
    std::thread worker;
    std::atomic<bool> quit{false}, snapshotRequested{false}, logging{false}, busy{false};
    std::atomic<float> progress{0};
    int framesUsed = 0;
    uint64_t lastSeq = 0;
    FILE* logFile = NULL;
    std::string logPath;
    double logCenter = 0, logSpan = 0;
    int logBins = 0, logB0 = 0, logB1 = 0;
    uint64_t logRows = 0;
    std::mutex statusMtx;
    std::string status;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/spectrum_export_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpectrumExportModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SpectrumExportModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
