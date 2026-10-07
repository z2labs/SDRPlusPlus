#pragma once
#include <imgui/imgui.h>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include <fftw3.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <vector>
#include <utils/event.h>
#include <mutex>
#include <gui/tuner.h>
#include <gui/widgets/waterfall.h>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground

class MainWindow {
public:
    void init();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    bool sdrIsRunning();
    void setFirstMenuRender();

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    bool isPlaying();

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;

    Event<bool> onPlayStateChange;

private:
    static void vfoAddedHandler(VFOManager::VFO* vfo, void* ctx);
    void drawMenu();
#ifdef __ANDROID__
    void pollUsbSdr();
    double lastUsbPoll = 0.0;
    int lastUsbFd = -1;
#endif

    // Touch layout (gui/touch_layout.cpp)
    void drawTouchLayout(ImGui::WaterfallVFO* vfo);
    void drawTouchTuningButton(ImVec2 imageSize, int framePadding);
    void applyZoomSlider(ImGui::WaterfallVFO* vfo);
    bool railOpen = false;
    bool drawerOpen = false;
    bool drawerWasOpen = false;
    bool swallowTouch = false;
    float railT = 0.0f;
    float drawerT = 0.0f;
    float freqT = 0.0f;
    double railLastTouch = 0.0;
    double freqLastTouch = -10.0;
    ImRect railRect;
    ImRect handleRect;
    ImRect freqRect;
    std::vector<std::pair<double, float>> autoPeakHist;
    void setAutoRange(bool enabled);
    void updateAutoRange();

    // Automatic FFT / waterfall range
    bool autoRange = false;
    bool autoRangeInit = false;
    float wfAutoMin = -120.0f;
    double lastAutoRange = 0.0;
    std::vector<float> autoRangeBuf;
    void handlePinchZoom(ImGui::WaterfallVFO* vfo);

    // Touch pinch-zoom state
    bool pinchValid = false;
    double pinchBw0 = 0.0;
    double pinchAnchor = 0.0;

    // FFT Variables
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;
    fftwf_complex *fft_in, *fft_out;
    fftwf_plan fftwPlan;

    // GUI Variables
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    float fftMin = -70.0;
    float fftMax = 0.0;
    float bw = 8000000;
    bool playing = false;
    bool showCredits = false;
    std::string audioStreamName = "";
    std::string sourceName = "";
    int menuWidth = 300;
    bool grabbingMenu = false;
    int newWidth = 300;
    int fftHeight = 300;
    bool showMenu = true;
    int tuningMode = tuner::TUNER_MODE_NORMAL;
    dsp::stream<dsp::complex_t> dummyStream;
    bool demoWindow = false;
    int selectedWindow = 0;

    bool initComplete = false;
    bool autostart = false;

    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
};