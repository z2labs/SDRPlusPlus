// Demo source: behaves like an RTL-SDR (24-1766 MHz, RTL sample rates, 8-bit samples, DC spike,
// gain) but synthesises its band: FM broadcast, airband AM, a 2 m APRS burst, a 70 cm CW beacon,
// 433.92 MHz ISM bursts and the satellite selected in the Cubesat Hunter with its real Doppler.
// Lets the app start without hardware (classroom, store screenshots, trying the UI).
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/smgui.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <utils/flog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <sat_tracker_interface.h>

SDRPP_MOD_INFO{
    /* Name:            */ "demo_source",
    /* Description:     */ "Simulated RTL-SDR for demos without hardware",
    /* Author:          */ "Zoltan Doczi",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

#define DEMO_SOURCE_NAME "Demo (RTL-SDR)"

static const double SAMPLE_RATES[] = { 2400000.0, 2048000.0, 1024000.0, 250000.0 };
static const char* SAMPLE_RATES_TXT = "2.4 MHz\0" "2.048 MHz\0" "1.024 MHz\0" "250 kHz\0";
static const float GAINS[] = { 0.0f, 0.9f, 1.4f, 2.7f, 3.7f, 7.7f, 8.7f, 12.5f, 14.4f, 15.7f, 16.6f, 19.7f, 20.7f, 22.9f, 25.4f,
                               28.0f, 29.7f, 32.8f, 33.8f, 36.4f, 37.2f, 38.6f, 40.2f, 42.1f, 43.4f, 43.9f, 44.5f, 48.0f, 49.6f };
static const int GAIN_COUNT = sizeof(GAINS) / sizeof(GAINS[0]);

// Fast oscillator: 32-bit phase accumulator + 4096-entry sine table
struct SinLut {
    float t[4096 + 1024];
    SinLut() { for (int i = 0; i < 4096 + 1024; i++) { t[i] = (float)std::sin(2.0 * 3.14159265358979323846 * i / 4096.0); } }
    inline float sin(uint32_t p) const { return t[p >> 20]; }
    inline float cos(uint32_t p) const { return t[(p >> 20) + 1024]; }
};
static SinLut LUT;

static inline uint32_t phaseInc(double hz, double fs) {
    double r = hz / fs;
    r -= std::floor(r);
    return (uint32_t)(int64_t)std::llround(r * 4294967296.0);
}

// One synthetic transmitter
struct Emitter {
    enum Kind { WFM, NFM_TONES, AM_TONE, CW, AFSK_BURST, OOK_BURST, LINEAR };
    Kind kind = NFM_TONES;
    double freq = 0;        // Hz
    float snrDb = 30;       // above the noise floor (per sample, before gain)
    double width = 0;       // linear transponder passband
    double baud = 1200;
    double period = 10;     // burst repetition, s
    const char* text = "";  // CW message
    // State
    uint32_t carrier = 0, audio = 0, audio2 = 0;
    double t = 0;           // seconds since start
    uint32_t lin[4] = { 0 };
    std::mt19937 rng{ 1 };
};

class DemoSourceModule : public ModuleManager::Instance {
public:
    DemoSourceModule(std::string name) {
        this->name = name;
        config.acquire();
        if (config.conf.contains("srId")) { srId = std::clamp<int>(config.conf["srId"], 0, 3); }
        if (config.conf.contains("gainId")) { gainId = std::clamp<int>(config.conf["gainId"], 0, GAIN_COUNT - 1); }
        config.release();
        core::configManager.acquire();
        if (core::configManager.conf.contains("demoWhenNoDevice")) { autoDemo = core::configManager.conf["demoWhenNoDevice"]; }
        core::configManager.release();
        sampleRate = SAMPLE_RATES[srId];

        // Fixed band plan of the demo world
        addEmitter(Emitter::WFM, 88.6e6, 38);
        addEmitter(Emitter::WFM, 94.8e6, 32);
        addEmitter(Emitter::WFM, 100.0e6, 44);
        addEmitter(Emitter::WFM, 104.3e6, 28);
        addEmitter(Emitter::AM_TONE, 127.5e6, 22);
        { Emitter& e = addEmitter(Emitter::AFSK_BURST, 144.800e6, 26); e.period = 12; }
        { Emitter& e = addEmitter(Emitter::CW, 432.400e6, 18); e.text = "VVV DE HG0DEMO DEMO BEACON   "; }
        { Emitter& e = addEmitter(Emitter::OOK_BURST, 433.920e6, 30); e.period = 7; }
        { Emitter& e = addEmitter(Emitter::NFM_TONES, 439.300e6, 24); e.period = 20; }

        std::mt19937 rng(42);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        noise.resize(1 << 16);
        for (auto& n : noise) { n.re = nd(rng); n.im = nd(rng); }

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource(DEMO_SOURCE_NAME, &handler);
    }

    ~DemoSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource(DEMO_SOURCE_NAME);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    Emitter& addEmitter(Emitter::Kind k, double f, float snr) {
        Emitter e;
        e.kind = k;
        e.freq = f;
        e.snrDb = snr;
        e.rng.seed((uint32_t)(f / 1000.0));
        emitters.push_back(e);
        return emitters.back();
    }

    static void menuSelected(void* ctx) {
        DemoSourceModule* _this = (DemoSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("Demo source: selected");
    }

    static void menuDeselected(void* ctx) {}

    static void start(void* ctx) {
        DemoSourceModule* _this = (DemoSourceModule*)ctx;
        if (_this->running) { return; }
        _this->running = true;
        _this->workerThread = std::thread(&DemoSourceModule::worker, _this);
        flog::info("Demo source: start");
    }

    static void stop(void* ctx) {
        DemoSourceModule* _this = (DemoSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->stream.stopWriter();
        if (_this->workerThread.joinable()) { _this->workerThread.join(); }
        _this->stream.clearWriteStop();
        _this->running = false;
        flog::info("Demo source: stop");
    }

    static void tune(double freq, void* ctx) {
        DemoSourceModule* _this = (DemoSourceModule*)ctx;
        _this->freq = freq;
    }

    static void menuHandler(void* ctx) {
        DemoSourceModule* _this = (DemoSourceModule*)ctx;
        std::string id = "##demo_" + _this->name;
        ImGui::TextWrapped("Simulated RTL-SDR: FM broadcast 88-105 MHz, airband 127.5, APRS 144.800, "
                           "beacon 432.400, ISM 433.920, repeater 439.300, and the satellite selected in "
                           "the Cubesat Hunter (try \"Simulate this pass now\").");
        if (_this->running) { style::beginDisabled(); }
        ImGui::LeftLabel("Sample rate");
        ImGui::FillWidth();
        if (ImGui::Combo(("##sr" + id).c_str(), &_this->srId, SAMPLE_RATES_TXT)) {
            _this->sampleRate = SAMPLE_RATES[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["srId"] = _this->srId;
            config.release(true);
        }
        if (_this->running) { style::endDisabled(); }
        ImGui::LeftLabel("Gain");
        ImGui::FillWidth();
        char gtxt[16];
        snprintf(gtxt, sizeof(gtxt), "%.1f dB", GAINS[_this->gainId]);
        if (ImGui::SliderInt(("##gain" + id).c_str(), &_this->gainId, 0, GAIN_COUNT - 1, gtxt)) {
            config.acquire();
            config.conf["gainId"] = _this->gainId;
            config.release(true);
        }
        if (ImGui::Checkbox(("Start in demo when no SDR is plugged in" + id).c_str(), &_this->autoDemo)) {
            core::configManager.acquire();
            core::configManager.conf["demoWhenNoDevice"] = _this->autoDemo;
            core::configManager.release(true);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Signal synthesis

    void worker() {
        const double fs = sampleRate;
        const int block = std::max(1000, (int)(fs / 100.0));   // 10 ms
        auto next = std::chrono::steady_clock::now();
        std::mt19937 rng(7);
        std::vector<float> acc(block * 2);
        Emitter sat;
        double satFreq = 0;
        while (true) {
            double f0 = freq;
            float gainLin = std::pow(10.0f, (GAINS[gainId] - 30.0f) / 20.0f);
            // Noise floor about -40 dBFS at mid gain; signals are relative to it
            float nAmp = 0.012f * gainLin;
            bool inRange = f0 >= 24e6 && f0 <= 1766e6;

            // Noise from the table at a random offset
            size_t off = rng() & (noise.size() - 1);
            for (int i = 0; i < block; i++) {
                const dsp::complex_t& n = noise[(off + i) & (noise.size() - 1)];
                acc[2 * i] = n.re * nAmp + 0.004f;          // RTL-like DC offset
                acc[2 * i + 1] = n.im * nAmp - 0.003f;
            }

            if (inRange) {
                for (auto& e : emitters) { synth(e, f0, fs, block, acc.data(), nAmp); }
                // The satellite from the Cubesat Hunter
                SatTrackerSignal sg;
                if (core::modComManager.interfaceExists(SAT_TRACKER_IFACE_NAME)) {
                    core::modComManager.callInterface(SAT_TRACKER_IFACE_NAME, SAT_TRACKER_IFACE_CMD_GET_SIGNAL, NULL, &sg);
                }
                if (sg.active && sg.elevation > 0) {
                    Emitter::Kind k = sg.kind == SAT_SIGNAL_CW ? Emitter::CW : sg.kind == SAT_SIGNAL_DATA ? Emitter::AFSK_BURST
                                    : sg.kind == SAT_SIGNAL_LINEAR ? Emitter::LINEAR : Emitter::NFM_TONES;
                    if (sat.kind != k || std::fabs(satFreq - sg.freq) > 100e3) {
                        sat = Emitter();
                        sat.kind = k;
                        sat.text = "CQ CQ DE DEMOSAT   ";
                        sat.period = 4;
                        sat.rng.seed(99);
                    }
                    satFreq = sg.freq;
                    sat.freq = sg.freq;
                    sat.width = sg.width;
                    sat.baud = sg.baud > 0 ? sg.baud : 1200;
                    // Link budget: ~30 dB overhead at 500 km, falling with range; fades near the horizon
                    double r = std::max(300.0, sg.rangeKm);
                    sat.snrDb = (float)(32.0 - 20.0 * std::log10(r / 500.0) - (sg.elevation < 5 ? (5 - sg.elevation) * 2.0 : 0.0));
                    synth(sat, f0, fs, block, acc.data(), nAmp);
                }
            }

            // 8-bit quantisation and clipping like the RTL2832U
            dsp::complex_t* out = stream.writeBuf;
            for (int i = 0; i < block; i++) {
                float re = std::clamp(acc[2 * i], -1.0f, 1.0f), im = std::clamp(acc[2 * i + 1], -1.0f, 1.0f);
                out[i].re = std::round(re * 127.5f) / 127.5f;
                out[i].im = std::round(im * 127.5f) / 127.5f;
            }
            if (!stream.swap(block)) { break; }

            // Real-time pacing
            next += std::chrono::microseconds((int64_t)(1e6 * block / fs));
            auto now = std::chrono::steady_clock::now();
            if (now - next > std::chrono::milliseconds(200)) { next = now; }
            else { std::this_thread::sleep_until(next); }
        }
    }

    // Morse timing: is the key down at time t (dot = 60 ms)?
    static bool morseKey(const char* text, double t) {
        static const char* codes[] = { ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                                       "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.." };
        static const char* digits[] = { "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----." };
        // Build the key pattern in dot units once per call (short texts, cheap enough per block)
        int total = 0;
        for (const char* c = text; *c; c++) {
            const char* code = nullptr;
            if (*c >= 'A' && *c <= 'Z') { code = codes[*c - 'A']; }
            else if (*c >= '0' && *c <= '9') { code = digits[*c - '0']; }
            if (!code) { total += 4; continue; }
            for (const char* d = code; *d; d++) { total += (*d == '.' ? 1 : 3) + 1; }
            total += 2;
        }
        if (total == 0) { return false; }
        int u = (int)std::fmod(t / 0.06, (double)total);
        for (const char* c = text; *c; c++) {
            const char* code = nullptr;
            if (*c >= 'A' && *c <= 'Z') { code = codes[*c - 'A']; }
            else if (*c >= '0' && *c <= '9') { code = digits[*c - '0']; }
            if (!code) { if (u < 4) { return false; } u -= 4; continue; }
            for (const char* d = code; *d; d++) {
                int len = (*d == '.' ? 1 : 3);
                if (u < len) { return true; }
                u -= len;
                if (u < 1) { return false; }
                u -= 1;
            }
            if (u < 2) { return false; }
            u -= 2;
        }
        return false;
    }

    void synth(Emitter& e, double f0, double fs, int n, float* acc, float nAmp) {
        double off = e.freq - f0;
        double bwHalf = e.kind == Emitter::WFM ? 100e3 : e.kind == Emitter::LINEAR ? e.width / 2 + 5e3 : 15e3;
        double dt = 1.0 / fs;
        double tStart = e.t;
        e.t += n * dt;
        if (std::fabs(off) - bwHalf > fs / 2) { return; }
        float amp = nAmp * std::pow(10.0f, e.snrDb / 20.0f);
        uint32_t cInc = phaseInc(off, fs);

        switch (e.kind) {
        case Emitter::WFM: {
            // Mono FM broadcast: a slow melody (75 kHz deviation scaled by the audio level)
            // Music-like mix: melody + fifth + bass, plus the 19 kHz stereo pilot (10 %)
            double note = 440.0 * std::pow(2.0, ((int)(tStart * 2.0) % 8) / 12.0);
            uint32_t aInc = phaseInc(note, fs), a2Inc = phaseInc(note * 1.5, fs), bInc = phaseInc(note / 4.0, fs), pInc = phaseInc(19000.0, fs);
            double dev = 75e3 / fs * 4294967296.0;
            for (int i = 0; i < n; i++) {
                e.audio += aInc;
                e.audio2 += a2Inc;
                e.lin[0] += bInc;
                e.lin[1] += pInc;
                float a = 0.4f * LUT.sin(e.audio) + 0.25f * LUT.sin(e.audio2) + 0.25f * LUT.sin(e.lin[0]) + 0.1f * LUT.sin(e.lin[1]);
                e.carrier += cInc + (uint32_t)(int64_t)(dev * a);
                acc[2 * i] += amp * LUT.cos(e.carrier);
                acc[2 * i + 1] += amp * LUT.sin(e.carrier);
            }
            break;
        }
        case Emitter::NFM_TONES: {
            // Voice-like: syllables of changing pitch, pauses; 3 kHz deviation
            bool on = e.period <= 0 || std::fmod(tStart, e.period) < e.period * 0.6;
            if (!on) { break; }
            static const double notes[] = { 523, 587, 659, 784, 880, 784, 659, 587 };
            double note = notes[(int)(tStart * 3.0) % 8];
            float env = 0.5f + 0.5f * (float)std::sin(tStart * 2 * 3.14159 * 3.0);
            uint32_t aInc = phaseInc(note, fs);
            double dev = 3e3 / fs * 4294967296.0 * env;
            for (int i = 0; i < n; i++) {
                e.audio += aInc;
                e.carrier += cInc + (uint32_t)(int64_t)(dev * LUT.sin(e.audio));
                acc[2 * i] += amp * LUT.cos(e.carrier);
                acc[2 * i + 1] += amp * LUT.sin(e.carrier);
            }
            break;
        }
        case Emitter::AM_TONE: {
            uint32_t aInc = phaseInc(1000.0, fs);
            bool on = std::fmod(tStart, 8.0) < 5.0;
            for (int i = 0; i < n; i++) {
                e.audio += aInc;
                e.carrier += cInc;
                float m = on ? 1.0f + 0.7f * LUT.sin(e.audio) : 1.0f;
                acc[2 * i] += amp * 0.6f * m * LUT.cos(e.carrier);
                acc[2 * i + 1] += amp * 0.6f * m * LUT.sin(e.carrier);
            }
            break;
        }
        case Emitter::CW: {
            for (int i = 0; i < n; i += 64) {
                bool key = morseKey(e.text, tStart + i * dt);
                int m = std::min(n, i + 64);
                for (int k = i; k < m; k++) {
                    e.carrier += cInc;
                    if (key) {
                        acc[2 * k] += amp * LUT.cos(e.carrier);
                        acc[2 * k + 1] += amp * LUT.sin(e.carrier);
                    }
                }
            }
            break;
        }
        case Emitter::AFSK_BURST: {
            // ~0.6 s packet per period: 1200 Bd AFSK (1200 / 2200 Hz) on FM, 3 kHz deviation; for
            // higher baud rates direct FSK with deviation = baud / 2
            double ph = std::fmod(tStart, e.period);
            double burst = 0.6;
            if (ph > burst) { break; }
            bool afsk = e.baud <= 1200.0;
            double bitT = 1.0 / std::max(300.0, e.baud);
            double dev = (afsk ? 3e3 : e.baud / 2.0) / fs * 4294967296.0;
            uint32_t mark = phaseInc(1200.0, fs), space = phaseInc(2200.0, fs);
            double t = tStart;
            int bit = (int)(t / bitT);
            bool val = (e.rng() & 1) != 0;
            for (int i = 0; i < n; i++, t += dt) {
                int b = (int)(t / bitT);
                if (b != bit) { bit = b; val = (e.rng() & 1) != 0; }
                double fdev;
                if (afsk) { e.audio += val ? mark : space; fdev = dev * LUT.sin(e.audio); }
                else { fdev = val ? dev : -dev; }
                e.carrier += cInc + (uint32_t)(int64_t)fdev;
                acc[2 * i] += amp * LUT.cos(e.carrier);
                acc[2 * i + 1] += amp * LUT.sin(e.carrier);
            }
            break;
        }
        case Emitter::OOK_BURST: {
            // Weather-station style: 8 repeats of a 40-bit PWM frame, then silence
            double ph = std::fmod(tStart, e.period);
            if (ph > 0.9) { break; }
            double t = ph;
            for (int i = 0; i < n; i++, t += dt) {
                double bitT = 1e-3;
                int bitIdx = (int)(t / bitT);
                double inBit = std::fmod(t, bitT) / bitT;
                bool one = ((bitIdx * 2654435761u) >> 7) & 1;
                bool key = (bitIdx % 50) < 40 && inBit < (one ? 0.7 : 0.3);
                e.carrier += cInc;
                if (key) {
                    acc[2 * i] += amp * LUT.cos(e.carrier);
                    acc[2 * i + 1] += amp * LUT.sin(e.carrier);
                }
            }
            break;
        }
        case Emitter::LINEAR: {
            // A few SSB / CW users spread across the transponder passband
            double w = std::max(10e3, e.width);
            double users[4] = { -0.3 * w, -0.05 * w, 0.15 * w, 0.35 * w };
            for (int u = 0; u < 4; u++) {
                bool cw = (u == 3);
                bool on = cw ? morseKey("CQ DE DEMOSAT  ", tStart + u) : std::fmod(tStart + u * 1.7, 6.0) < 4.0;
                if (!on) { continue; }
                // USB voice approximated by two tones 600 / 1400 Hz above the user's carrier
                double tone = cw ? 700.0 : (600.0 + 800.0 * (((int)(tStart * 4) + u) % 2));
                uint32_t inc = phaseInc(off + users[u] + tone, fs);
                float a = amp * (cw ? 0.8f : 0.5f);
                for (int i = 0; i < n; i++) {
                    e.lin[u] += inc;
                    acc[2 * i] += a * LUT.cos(e.lin[u]);
                    acc[2 * i + 1] += a * LUT.sin(e.lin[u]);
                }
            }
            break;
        }
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    bool autoDemo = true;
    int srId = 0;
    int gainId = 14;
    double sampleRate = 2.4e6;
    std::atomic<double> freq{ 100e6 };
    std::vector<Emitter> emitters;
    std::vector<dsp::complex_t> noise;
    std::thread workerThread;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["srId"] = 0;
    def["gainId"] = 14;
    config.setPath(core::args["root"].s() + "/demo_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new DemoSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (DemoSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
