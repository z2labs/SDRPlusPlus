#include <gui/menus/source.h>
#ifdef __ANDROID__
#include "crashlog.h"
#include <android_backend.h>
#endif
#include <gui/touch.h>
#include <version.h>
#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include <imgui/imgui_internal.h>
#include <cmath>
#include <algorithm>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif
#include <thread>
#include <complex>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <signal_path/iq_frontend.h>
#include <gui/icons.h>
#include <gui/widgets/bandplan.h>
#include <gui/style.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/menus/source.h>
#include <gui/menus/display.h>
#include <gui/menus/bandplan.h>
#include <gui/menus/sink.h>
#include <gui/menus/vfo_color.h>
#include <gui/menus/module_manager.h>
#include <gui/menus/theme.h>
#include <gui/dialogs/credits.h>
#include <filesystem>
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>

void MainWindow::init() {
    LoadingScreen::show("Initializing UI");
    gui::waterfall.init();
    gui::waterfall.setRawFFTSize(fftSize);

    credits::init();

    core::configManager.acquire();
    json menuElements = core::configManager.conf["menuElements"];
    std::string modulesDir = core::configManager.conf["modulesDirectory"];
    std::string resourcesDir = core::configManager.conf["resourcesDirectory"];
    core::configManager.release();

    // Assert that directories are absolute
    modulesDir = std::filesystem::absolute(modulesDir).string();
    resourcesDir = std::filesystem::absolute(resourcesDir).string();

    // Load menu elements
    gui::menu.order.clear();
    for (auto& elem : menuElements) {
        if (!elem.contains("name")) {
            flog::error("Menu element is missing name key");
            continue;
        }
        if (!elem["name"].is_string()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        if (!elem.contains("open")) {
            flog::error("Menu element is missing open key");
            continue;
        }
        if (!elem["open"].is_boolean()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        Menu::MenuOption_t opt;
        opt.name = elem["name"];
        opt.open = elem["open"];
        gui::menu.order.push_back(opt);
    }

    gui::menu.registerEntry("Source", sourcemenu::draw, NULL);
    gui::menu.registerEntry("Sinks", sinkmenu::draw, NULL);
    gui::menu.registerEntry("Band Plan", bandplanmenu::draw, NULL);
    gui::menu.registerEntry("Display", displaymenu::draw, NULL);
    gui::menu.registerEntry("Theme", thememenu::draw, NULL);
    gui::menu.registerEntry("VFO Color", vfo_color_menu::draw, NULL);
    gui::menu.registerEntry("Module Manager", module_manager_menu::draw, NULL);

    gui::freqSelect.init();

    // Set default values for waterfall in case no source init's it
    gui::waterfall.setBandwidth(8000000);
    gui::waterfall.setViewBandwidth(8000000);

    fft_in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fft_out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

    sigpath::iqFrontEnd.init(&dummyStream, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFTBuffer, releaseFFTBuffer, this);
    sigpath::iqFrontEnd.start();

    vfoCreatedHandler.handler = vfoAddedHandler;
    vfoCreatedHandler.ctx = this;
    sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);

    flog::info("Loading modules");

    // Load modules from /module directory
    if (std::filesystem::is_directory(modulesDir)) {
        for (const auto& file : std::filesystem::directory_iterator(modulesDir)) {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != SDRPP_MOD_EXTENTSION) {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            flog::info("Loading {0}", path);
            LoadingScreen::show("Loading " + file.path().filename().string());
            core::moduleManager.loadModule(path);
        }
    }
    else {
        flog::warn("Module directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Read module config
    core::configManager.acquire();
    std::vector<std::string> modules = core::configManager.conf["modules"];
    auto modList = core::configManager.conf["moduleInstances"].items();
    core::configManager.release();

    // Load additional modules specified through config
    for (auto const& path : modules) {
#ifndef __ANDROID__
        std::string apath = std::filesystem::absolute(path).string();
        flog::info("Loading {0}", apath);
        LoadingScreen::show("Loading " + std::filesystem::path(path).filename().string());
        core::moduleManager.loadModule(apath);
#else
        core::moduleManager.loadModule(path);
#endif
    }

    // Create module instances
    for (auto const& [name, _module] : modList) {
        std::string mod = _module["module"];
        bool enabled = _module["enabled"];
        flog::info("Initializing {0} ({1})", name, mod);
        LoadingScreen::show("Initializing " + name + " (" + mod + ")");
        core::moduleManager.createInstance(name, mod);
        if (!enabled) { core::moduleManager.disableInstance(name); }
    }

    // Load color maps
    LoadingScreen::show("Loading color maps");
    flog::info("Loading color maps");
    if (std::filesystem::is_directory(resourcesDir + "/colormaps")) {
        for (const auto& file : std::filesystem::directory_iterator(resourcesDir + "/colormaps")) {
            std::string path = file.path().generic_string();
            LoadingScreen::show("Loading " + file.path().filename().string());
            flog::info("Loading {0}", path);
            if (file.path().extension().generic_string() != ".json") {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            colormaps::loadMap(path);
        }
    }
    else {
        flog::warn("Color map directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    gui::waterfall.updatePalletteFromArray(colormaps::maps["Turbo"].map, colormaps::maps["Turbo"].entryCount);

    sourcemenu::init();
    sinkmenu::init();
    bandplanmenu::init();
    displaymenu::init();
    vfo_color_menu::init();
    module_manager_menu::init();

    // TODO for 0.2.5
    // Fix gain not updated on startup, soapysdr

    // Update UI settings
    LoadingScreen::show("Loading configuration");
    core::configManager.acquire();
    fftMin = core::configManager.conf["min"];
    fftMax = core::configManager.conf["max"];
    autoRange = core::configManager.conf["autoRange"];
    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMax(fftMax);

    double frequency = core::configManager.conf["frequency"];

    showMenu = core::configManager.conf["showMenu"];
#ifdef __ANDROID__
    autoHideMenu = core::configManager.conf.contains("autoHideMenu") && (bool)core::configManager.conf["autoHideMenu"];
#else
    autoHideMenu = !core::configManager.conf.contains("autoHideMenu") || (bool)core::configManager.conf["autoHideMenu"];
#endif
    startedWithMenuClosed = !showMenu;

    gui::freqSelect.setFrequency(frequency);
    gui::freqSelect.frequencyChanged = false;
    sigpath::sourceManager.tune(frequency);
    gui::waterfall.setCenterFrequency(frequency);
    bw = 1.0;
    gui::waterfall.vfoFreqChanged = false;
    gui::waterfall.centerFreqMoved = false;
    gui::waterfall.selectFirstVFO();

    menuWidth = core::configManager.conf["menuWidth"];
    newWidth = menuWidth;

    fftHeight = core::configManager.conf["fftHeight"];
    gui::waterfall.setFFTHeight(fftHeight);

    tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
    gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);

    core::configManager.release();

    // Correct the offset of all VFOs so that they fit on the screen
    float finalBwHalf = gui::waterfall.getBandwidth() / 2.0;
    for (auto& [_name, _vfo] : gui::waterfall.vfos) {
        if (_vfo->lowerOffset < -finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, (_vfo->bandwidth / 2) - finalBwHalf);
            continue;
        }
        if (_vfo->upperOffset > finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, finalBwHalf - (_vfo->bandwidth / 2));
            continue;
        }
    }

    autostart = core::args["autostart"].b();
#ifndef __ANDROID__
    // z2labs: start receiving right away (Android starts on USB plug-in instead)
    {
        core::configManager.acquire();
        bool pos = !core::configManager.conf.contains("playOnStart") || (bool)core::configManager.conf["playOnStart"];
        core::configManager.release();
        if (pos) { autostart = true; }
    }
#endif
    initComplete = true;

    core::moduleManager.doPostInitAll();
}

float* MainWindow::acquireFFTBuffer(void* ctx) {
    return gui::waterfall.getFFTBuffer();
}

void MainWindow::releaseFFTBuffer(void* ctx) {
    gui::waterfall.pushFFT();
}

void MainWindow::vfoAddedHandler(VFOManager::VFO* vfo, void* ctx) {
    MainWindow* _this = (MainWindow*)ctx;
    std::string name = vfo->getName();
    core::configManager.acquire();
    if (!core::configManager.conf["vfoOffsets"].contains(name)) {
        core::configManager.release();
        return;
    }
    double offset = core::configManager.conf["vfoOffsets"][name];
    core::configManager.release();

    double viewBW = gui::waterfall.getViewBandwidth();
    double viewOffset = gui::waterfall.getViewOffset();

    double viewLower = viewOffset - (viewBW / 2.0);
    double viewUpper = viewOffset + (viewBW / 2.0);

    double newOffset = std::clamp<double>(offset, viewLower, viewUpper);

    sigpath::vfoManager.setCenterOffset(name, _this->initComplete ? newOffset : offset);
}

void MainWindow::draw() {
    if (stopRequested.exchange(false) && playing) { setPlayState(false); }
    if (startRequested.exchange(false) && !playing) { setPlayState(true); }
#ifdef __ANDROID__
    pollUsbSdr();
#endif
    ImGui::Begin("Main", NULL, WINDOW_FLAGS);
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    ImGui::WaterfallVFO* vfo = NULL;
    if (gui::waterfall.selectedVFO != "") {
        vfo = gui::waterfall.vfos[gui::waterfall.selectedVFO];
    }

    // Handle VFO movement
    if (vfo != NULL) {
        if (vfo->centerOffsetChanged) {
            if (tuningMode == tuner::TUNER_MODE_CENTER) {
                tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            }
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            gui::freqSelect.frequencyChanged = false;
            core::configManager.acquire();
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            core::configManager.release(true);
        }
    }

    sigpath::vfoManager.updateFromWaterfall(&gui::waterfall);

    // Handle selection of another VFO
    if (gui::waterfall.selectedVFOChanged) {
        gui::freqSelect.setFrequency((vfo != NULL) ? (vfo->generalOffset + gui::waterfall.getCenterFrequency()) : gui::waterfall.getCenterFrequency());
        gui::waterfall.selectedVFOChanged = false;
        gui::freqSelect.frequencyChanged = false;
    }

    // Handle change in selected frequency
    if (gui::freqSelect.frequencyChanged) {
        gui::freqSelect.frequencyChanged = false;
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
        if (vfo != NULL) {
            vfo->centerOffsetChanged = false;
            vfo->lowerOffsetChanged = false;
            vfo->upperOffsetChanged = false;
        }
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        if (vfo != NULL) {
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
        }
        core::configManager.release(true);
    }

    // Handle dragging the frequency scale
    if (gui::waterfall.centerFreqMoved) {
        gui::waterfall.centerFreqMoved = false;
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        if (vfo != NULL) {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
        }
        else {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency());
        }
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        core::configManager.release(true);
    }

    int _fftHeight = gui::waterfall.getFFTHeight();
    if (fftHeight != _fftHeight) {
        fftHeight = _fftHeight;
        core::configManager.acquire();
        core::configManager.conf["fftHeight"] = fftHeight;
        core::configManager.release(true);
    }

    // Touch devices: full-screen waterfall with floating controls (gui/touch_layout.cpp)
    if (touch::enabled) {
#ifdef __ANDROID__
        crashlog::step("drawTouchLayout");
#endif
        drawTouchLayout(vfo);
        if (demoWindow) { ImGui::ShowDemoWindow(); }
        return;
    }

    // To Bar
    // ImGui::BeginChild("TopBarChild", ImVec2(0, 49.0f * style::uiScale), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 winSize = ImGui::GetWindowSize();
    bool narrow = touch::narrow(winSize); // portrait phone: two-row top bar, menu above the waterfall
    // Landscape phone: the desktop top bar does not fit, use a single compact row instead
    bool compactBar = touch::enabled && !narrow && winSize.x < 1250.0f * style::uiScale;
    float btnBase = (touch::enabled ? 40.0f : 30.0f) * style::uiScale;
    ImVec2 btnSize(btnBase, btnBase);
    float ctrlColW = (touch::enabled ? 84.0f : 60.0f) * style::uiScale;
    ImGui::PushID(ImGui::GetID("sdrpp_menu_btn"));
    if (ImGui::ImageButton(icons::MENU, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false)) {
        if (autoHideMenu && !narrow) {
            menuOverlayOpen = !menuOverlayOpen;
            menuOverlayKeepUntil = ImGui::GetTime() + 3.0; // stays a moment even if the mouse never enters
        }
        else {
            showMenu = !showMenu;
            core::configManager.acquire();
            core::configManager.conf["showMenu"] = showMenu;
            core::configManager.release(true);
        }
    }
    ImGui::PopID();

    ImGui::SameLine();

    bool tmpPlaySate = playing;
    if (playButtonLocked && !tmpPlaySate) { style::beginDisabled(); }
    if (playing) {
        ImGui::PushID(ImGui::GetID("sdrpp_stop_btn"));
        if (ImGui::ImageButton(icons::STOP, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            setPlayState(false);
        }
        ImGui::PopID();
    }
    else { // TODO: Might need to check if there even is a device
        ImGui::PushID(ImGui::GetID("sdrpp_play_btn"));
        if (ImGui::ImageButton(icons::PLAY, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            setPlayState(true);
        }
        ImGui::PopID();
    }
    if (playButtonLocked && !tmpPlaySate) { style::endDisabled(); }

    // Handle auto-start
    if (autostart) {
        autostart = false;
        setPlayState(true);
    }

    ImGui::SameLine();
    float origY = ImGui::GetCursorPosY();

    auto drawTuningModeButton = [&]() {
        if (tuningMode == tuner::TUNER_MODE_CENTER) {
            ImGui::PushID(ImGui::GetID("sdrpp_ena_st_btn"));
            if (ImGui::ImageButton(icons::CENTER_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol)) {
                tuningMode = tuner::TUNER_MODE_NORMAL;
                gui::waterfall.VFOMoveSingleClick = false;
                core::configManager.acquire();
                core::configManager.conf["centerTuning"] = false;
                core::configManager.release(true);
            }
            ImGui::PopID();
        }
        else { // TODO: Might need to check if there even is a device
            ImGui::PushID(ImGui::GetID("sdrpp_dis_st_btn"));
            if (ImGui::ImageButton(icons::NORMAL_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol)) {
                tuningMode = tuner::TUNER_MODE_CENTER;
                gui::waterfall.VFOMoveSingleClick = true;
                tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
                core::configManager.acquire();
                core::configManager.conf["centerTuning"] = true;
                core::configManager.release(true);
            }
            ImGui::PopID();
        }
    };

    // Width of the frequency display at the big font, used to shrink it when space is short
    ImGui::PushFont(style::bigFont);
    ImVec2 digitSz = ImGui::CalcTextSize("0");
    float freqNeed = (12.0f * digitSz.x) + (3.0f * ImGui::CalcTextSize(".").x) + 15.0f;
    ImGui::PopFont();
    float padX = ImGui::GetStyle().WindowPadding.x;
    float btnCenterY = origY + (btnSize.y / 2.0f) + 5.0f;

    if (compactBar) {
        // menu, play, tuning mode, volume, frequency (scaled to the rest of the row)
        drawTuningModeButton();
        ImGui::SameLine();
        sigpath::sinkManager.showVolumeSlider(gui::waterfall.selectedVFO, "##_sdrpp_main_volume_", 150 * style::uiScale, btnSize.x, 5, true);
        ImGui::SameLine();
        float fscale = std::min<float>(1.0f, (winSize.x - ImGui::GetCursorPosX() - padX) / freqNeed);
        ImGui::SetWindowFontScale(fscale);
        ImGui::SetCursorPosY(btnCenterY - ceilf(15 * style::uiScale) - 5);
        gui::freqSelect.draw();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SetCursorPosY(std::max<float>(origY + btnSize.y + 10.0f, btnCenterY + (digitSz.y * fscale / 2.0f)) + ImGui::GetStyle().ItemSpacing.y);
    }
    else if (!narrow) {
        sigpath::sinkManager.showVolumeSlider(gui::waterfall.selectedVFO, "##_sdrpp_main_volume_", (touch::enabled ? 160 : 248) * style::uiScale, btnSize.x, 5, true);

        ImGui::SameLine();

        // Keep the digits centred on the (possibly taller) touch buttons
        ImGui::SetCursorPosY(origY + (btnSize.y - 30.0f * style::uiScale) / 2.0f);
        gui::freqSelect.draw();

        ImGui::SameLine();

        ImGui::SetCursorPosY(origY);
        drawTuningModeButton();
    }
    else {
        // Row 1: menu, play, tuning mode, volume (rest of the width)
        drawTuningModeButton();
        ImGui::SameLine();
        float volW = winSize.x - ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x;
        sigpath::sinkManager.showVolumeSlider(gui::waterfall.selectedVFO, "##_sdrpp_main_volume_", volW, btnSize.x, 5, false);

        // Row 2: frequency (shrunk to fit if needed), SNR meter if there is room
        float row2Y = ImGui::GetCursorPosY();
        float avail = winSize.x - ImGui::GetCursorPosX() - padX;
        float fscale = std::min<float>(1.0f, avail / freqNeed);
        float digH = digitSz.y * fscale;

        ImGui::SetWindowFontScale(fscale);
        ImGui::SetCursorPosY(row2Y + (digH / 2.0f) - ceilf(15 * style::uiScale) - 5);
        gui::freqSelect.draw();
        ImGui::SetWindowFontScale(1.0f);

        ImGui::SameLine();
        float snrW = winSize.x - ImGui::GetCursorPosX() - padX;
        if (snrW > 150.0f * style::uiScale) {
            ImGui::SetCursorPosY(row2Y + (digH / 2.0f) - (13.0f * style::uiScale));
            ImGui::SetNextItemWidth(snrW);
            ImGui::SNRMeter((vfo != NULL) ? gui::waterfall.selectedVFOSNR : 0);
        }
        else {
            ImGui::NewLine();
        }
        ImGui::SetCursorPosY(row2Y + digH + ImGui::GetStyle().ItemSpacing.y);
    }

    if (!narrow && !compactBar) {
        ImGui::SameLine();

        int snrOffset = 87.0f * style::uiScale;
        int snrWidth = std::clamp<int>(ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - snrOffset, 100.0f * style::uiScale, 300.0f * style::uiScale);
        int snrPos = std::max<int>(ImGui::GetWindowSize().x - (snrWidth + snrOffset), ImGui::GetCursorPosX());

        ImGui::SetCursorPosX(snrPos);
        ImGui::SetCursorPosY(origY + (5.0f * style::uiScale));
        ImGui::SetNextItemWidth(snrWidth);
        ImGui::SNRMeter((vfo != NULL) ? gui::waterfall.selectedVFOSNR : 0);

        // Note: this is what makes the vertical size correct, needs to be fixed
        ImGui::SameLine();

        // ImGui::EndChild();

        // Logo button
        ImGui::SetCursorPosX(ImGui::GetWindowSize().x - (48 * style::uiScale));
        ImGui::SetCursorPosY(10.0f * style::uiScale);
        if (ImGui::ImageButton(icons::LOGO, ImVec2(32 * style::uiScale, 32 * style::uiScale), ImVec2(0, 0), ImVec2(1, 1), 0)) {
            showCredits = true;
        }
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        showCredits = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        showCredits = false;
    }

    // Reset waterfall lock
    lockWaterfallControls = showCredits;

    // Handle menu resize
    ImVec2 mousePos = ImGui::GetMousePos();
    bool menuDocked = showMenu && (!autoHideMenu || narrow);
    if (!lockWaterfallControls && menuDocked && !narrow) {
        float curY = ImGui::GetCursorPosY();
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (grabbingMenu) {
            newWidth = mousePos.x;
            newWidth = std::clamp<float>(newWidth, 250, winSize.x - 250);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(newWidth, curY), ImVec2(newWidth, winSize.y - 10), ImGui::GetColorU32(ImGuiCol_SeparatorActive));
        }
        float grabW = (touch::enabled ? 10.0f : 2.0f) * style::uiScale;
        if (mousePos.x >= newWidth - grabW && mousePos.x <= newWidth + grabW && mousePos.y > curY) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (click) {
                grabbingMenu = true;
            }
        }
        else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        if (!down && grabbingMenu) {
            grabbingMenu = false;
            menuWidth = newWidth;
            core::configManager.acquire();
            core::configManager.conf["menuWidth"] = menuWidth;
            core::configManager.release(true);
        }
    }

    // Process menu keybinds
    displaymenu::checkKeybinds();

    // Left Column
    float menuTop = ImGui::GetCursorScreenPos().y;
    if (menuDocked && narrow) {
        // Portrait: menu on top (full width), waterfall below
        float menuH = std::max<float>(ImGui::GetContentRegionAvail().y * 0.55f, 200.0f * style::uiScale);
        ImGui::BeginChild("Left Column", ImVec2(0, menuH));
        drawMenu();
        ImGui::EndChild();
    }
    if (menuDocked && !narrow) {
        // The stored default (300 px) is far too narrow for finger-sized widgets
        if (touch::enabled && menuWidth < 200.0f * style::uiScale && !grabbingMenu) {
            menuWidth = newWidth = std::min<float>(200.0f * style::uiScale, winSize.x * 0.5f);
        }
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, menuWidth);
        ImGui::SetColumnWidth(1, std::max<int>(winSize.x - menuWidth - ctrlColW, 100.0f * style::uiScale));
        ImGui::SetColumnWidth(2, ctrlColW);
#ifndef __ANDROID__
        if (!touch::enabled && ImGui::Button("Auto-hide menu##sdrpp_menu_unpin", ImVec2(ImGui::GetContentRegionAvail().x, 0))) { setMenuPinned(false); }
        if (!touch::enabled && ImGui::IsItemHovered()) { ImGui::SetTooltip("Fold the menu away to the left edge when it is not used"); }
#endif
        ImGui::BeginChild("Left Column");
        drawMenu();
        ImGui::EndChild();
    }
    else {
        // When hiding the menu bar
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, 8 * style::uiScale);
        ImGui::SetColumnWidth(1, winSize.x - (8 * style::uiScale) - ctrlColW);
        ImGui::SetColumnWidth(2, ctrlColW);
    }

    if (autoHideMenu && !narrow) { drawMenuOverlay(menuTop, winSize); }
    else { menuOverlayOpen = false; menuOverlayAnim = 0.0f; }

    // Right Column
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::NextColumn();
    ImGui::PopStyleVar();

    ImGui::BeginChild("Waterfall");

    gui::waterfall.draw();

    ImGui::EndChild();

    handlePinchZoom(vfo);

    if (!lockWaterfallControls) {
        // Handle arrow keys
        if (vfo != NULL && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            bool freqChanged = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset - vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (freqChanged) {
                core::configManager.acquire();
                core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
                if (vfo != NULL) {
                    core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
                }
                core::configManager.release(true);
            }
        }

        // Handle scrollwheel
        int wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            double nfreq;
            if (vfo != NULL) {
                // Select factor depending on modifier keys
                double interval;
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                    interval = vfo->snapInterval * 10.0;
                }
                else if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                    interval = vfo->snapInterval * 0.1;
                }
                else {
                    interval = vfo->snapInterval;
                }

                nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + (interval * wheel);
                nfreq = roundl(nfreq / interval) * interval;
            }
            else {
                nfreq = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() * wheel / 20.0);
            }
            tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
            gui::freqSelect.setFrequency(nfreq);
            core::configManager.acquire();
            core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
            if (vfo != NULL) {
                core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            }
            core::configManager.release(true);
        }
    }

    ImGui::NextColumn();
    // Vertical sliders live here: in touch mode this column must never scroll, or a slider drag becomes a scroll
    ImGui::BeginChild("WaterfallControls", ImVec2(0, 0), false, touch::enabled ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : 0);

    // Automatic FFT / waterfall range (on by default)
    {
        bool wasAuto = autoRange;
        if (wasAuto) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab)); }
        if (ImGui::Button("Auto##_sdrpp_auto_range", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            setAutoRange(!autoRange);
        }
        if (wasAuto) { ImGui::PopStyleColor(); }
    }

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Zoom").x / 2.0));
    ImGui::TextUnformatted("Zoom");
    float wfSliderW = (touch::enabled ? 36.0f : 20.0f) * style::uiScale;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - wfSliderW) / 2.0f);
    // Fit the three sliders (and the Auto button) into the column height (landscape phones)
    float colLabelH = ImGui::GetTextLineHeightWithSpacing();
    float autoBtnH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    float fixedH = autoBtnH + 3.0f * (colLabelH + ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f);
    float sliderH = std::clamp<float>((ImGui::GetContentRegionAvail().y - fixedH) / 3.0f, 40.0f * style::uiScale, 150.0f * style::uiScale);
    ImVec2 wfSliderSize(wfSliderW, sliderH);
    if (ImGui::VSliderFloat("##_7_", wfSliderSize, &bw, 1.0, 0.0, "")) {
        double factor = (double)bw * (double)bw;

        // Map 0.0 -> 1.0 to 1000.0 -> bandwidth
        double wfBw = gui::waterfall.getBandwidth();
        double delta = wfBw - 1000.0;
        double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);

        gui::waterfall.setViewBandwidth(finalBw);
        if (vfo != NULL) {
            gui::waterfall.setViewOffset(vfo->centerOffset); // center vfo on screen
        }
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Max").x / 2.0));
    ImGui::TextUnformatted("Max");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - wfSliderW) / 2.0f);
    if (ImGui::VSliderFloat("##_8_", wfSliderSize, &fftMax, 0.0, -160.0f, "")) {
        if (autoRange) { setAutoRange(false); } // manual adjustment takes over
        fftMax = std::max<float>(fftMax, fftMin + 10);
        core::configManager.acquire();
        core::configManager.conf["max"] = fftMax;
        core::configManager.release(true);
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Min").x / 2.0));
    ImGui::TextUnformatted("Min");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - wfSliderW) / 2.0f);
    ImGui::SetItemUsingMouseWheel();
    if (ImGui::VSliderFloat("##_9_", wfSliderSize, &fftMin, 0.0, -160.0f, "")) {
        if (autoRange) { setAutoRange(false); }
        fftMin = std::min<float>(fftMax - 10, fftMin);
        core::configManager.acquire();
        core::configManager.conf["min"] = fftMin;
        core::configManager.release(true);
    }

    ImGui::EndChild();

#ifdef __ANDROID__
    crashlog::step("updateAutoRange");
#endif
    if (autoRange) { updateAutoRange(); }

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(autoRange ? wfAutoMin : fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

    ImGui::End();

    if (showCredits) {
        credits::show();
    }

    if (demoWindow) {
        ImGui::ShowDemoWindow();
    }
}

void MainWindow::setPlayState(bool _playing) {
    if (_playing == playing) { return; }
    if (_playing) {
        sigpath::iqFrontEnd.flushInputBuffer();
        sigpath::sourceManager.start();
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        playing = true;
        onPlayStateChange.emit(true);
    }
    else {
        playing = false;
        onPlayStateChange.emit(false);
        sigpath::sourceManager.stop();
        sigpath::iqFrontEnd.flushInputBuffer();
    }
}

void MainWindow::setViewBandwidthSlider(float bandwidth) {
    bw = bandwidth;
}

bool MainWindow::sdrIsRunning() {
    return playing;
}

bool MainWindow::isPlaying() {
    return playing;
}

void MainWindow::setFirstMenuRender() {
    firstMenuRender = true;
}
// Desktop auto-hide menu: a thin glowing tab on the left edge. Resting the mouse on the
// edge (or clicking the menu button) slides the menu in over the spectrum, so the
// spectrum and waterfall never change size. It folds away again shortly after the mouse
// leaves it, unless a widget or popup in it is still in use.
void MainWindow::drawMenuOverlay(float top, ImVec2 winSize) {
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    ImVec2 mp = ImGui::GetMousePos();
    float scale = style::uiScale;
    float h = winSize.y - top;
    float w = std::clamp<float>(menuWidth, 250.0f * scale, std::max<float>(250.0f * scale, winSize.x * 0.6f));
    float edgeW = 8.0f * scale;
    bool anyDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool mouseValid = ImGui::IsMousePosValid(&mp);

    // Open by resting on the left edge (not while dragging, e.g. a waterfall drag)
    bool onEdge = mouseValid && mp.x >= 0.0f && mp.x < edgeW && mp.y > top && mp.y < winSize.y;
    if (!menuOverlayOpen && onEdge && !anyDown && !lockWaterfallControls) {
        if (menuEdgeSince < 0.0) { menuEdgeSince = now; }
        if (now - menuEdgeSince > 0.12) { menuOverlayOpen = true; menuOverlayKeepUntil = now + 0.8; }
    }
    else if (!onEdge) {
        menuEdgeSince = -1.0;
    }

    // Keep it open while the mouse is over it or anything in it is in use
    float x = -w * (1.0f - menuOverlayAnim * menuOverlayAnim * (3.0f - 2.0f * menuOverlayAnim));
    bool inside = mouseValid && mp.x < x + w + 4.0f * scale && mp.y > top;
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (menuOverlayOpen) {
        // Used (clicked, dragged, scrolled in it): stays out a while after the mouse leaves
        if (inside && menuOverlayAnim > 0.5f && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                                 io.MouseWheel != 0.0f || ImGui::IsAnyItemActive())) {
            menuOverlayUsedAt = now;
        }
        bool recentlyUsed = now - menuOverlayUsedAt < 6.0;
        if (inside || popup) {
            menuOverlayKeepUntil = std::max<double>(menuOverlayKeepUntil, now + (recentlyUsed ? 6.0 : 0.6));
        }
        // A click outside (below the top bar, whose menu button toggles it) closes it right away
        if (!inside && !popup && !recentlyUsed && mouseValid && mp.y > top && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            menuOverlayOpen = false;
        }
        if (now > menuOverlayKeepUntil) { menuOverlayOpen = false; }
    }

    // Critically damped slide, ~150 ms
    float target = menuOverlayOpen ? 1.0f : 0.0f;
    menuOverlayAnim += (target - menuOverlayAnim) * (1.0f - expf(-16.0f * io.DeltaTime));
    if (fabsf(target - menuOverlayAnim) < 0.002f) { menuOverlayAnim = target; }

    // The tab on the edge, brighter when the mouse is near
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (menuOverlayAnim < 1.0f) {
        ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        c.w = (onEdge ? 0.95f : 0.45f) * (1.0f - menuOverlayAnim);
        float th = std::min<float>(120.0f * scale, h * 0.3f);
        float ty = top + (h - th) * 0.5f;
        dl->AddRectFilled(ImVec2(1.0f * scale, ty), ImVec2(4.0f * scale, ty + th), ImGui::GetColorU32(c), 2.0f * scale);
    }
    if (menuOverlayAnim <= 0.0f) { return; }

    // The waterfall under the menu reads the wheel directly; keep it for the menu only
    if (inside) { io.MouseWheel = 0.0f; io.MouseWheelH = 0.0f; }

    x = -w * (1.0f - menuOverlayAnim * menuOverlayAnim * (3.0f - 2.0f * menuOverlayAnim));
    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##sdrpp_menu_overlay", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (menuOverlayOpen && menuOverlayAnim < 0.5f) { ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); }
    if (ImGui::Button("Pin menu##sdrpp_menu_pin", ImVec2(ImGui::GetContentRegionAvail().x, 0))) { setMenuPinned(true); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Keep the menu open next to the spectrum"); }
    ImGui::BeginChild("Left Column");
    drawMenu();
    ImGui::EndChild();
    // Shadow-like edge on the right
    ImDrawList* odl = ImGui::GetWindowDrawList();
    odl->AddLine(ImVec2(x + w - 1.0f, top), ImVec2(x + w - 1.0f, top + h), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::End();
}

void MainWindow::setMenuPinned(bool pinned) {
    autoHideMenu = !pinned;
    showMenu = true;
    menuOverlayOpen = false;
    menuOverlayAnim = 0.0f;
    core::configManager.acquire();
    core::configManager.conf["autoHideMenu"] = autoHideMenu;
    core::configManager.conf["showMenu"] = true;
    core::configManager.release(true);
}

void MainWindow::drawMenu() {

    if (gui::menu.draw(firstMenuRender)) {
        core::configManager.acquire();
        json arr = json::array();
        for (int i = 0; i < gui::menu.order.size(); i++) {
            arr[i]["name"] = gui::menu.order[i].name;
            arr[i]["open"] = gui::menu.order[i].open;
        }
        core::configManager.conf["menuElements"] = arr;

        // Update enabled and disabled modules
        for (auto [_name, inst] : core::moduleManager.instances) {
            if (!core::configManager.conf["moduleInstances"].contains(_name)) { continue; }
            core::configManager.conf["moduleInstances"][_name]["enabled"] = inst.instance->isEnabled();
        }

        core::configManager.release(true);
    }
    if (startedWithMenuClosed) {
        startedWithMenuClosed = false;
    }
    else {
        firstMenuRender = false;
    }

    if (ImGui::CollapsingHeader("Debug")) {
        ImGui::Text("Frame time: %.3f ms/frame", ImGui::GetIO().DeltaTime * 1000.0f);
        ImGui::Text("Framerate: %.1f FPS", ImGui::GetIO().Framerate);
        ImGui::Text("Center Frequency: %.0f Hz", gui::waterfall.getCenterFrequency());
        ImGui::Text("Source name: %s", sourceName.c_str());
        ImGui::Text("Build: " APP_NAME " " APP_VERSION " (" SDRPP_GIT_REV ", " __DATE__ "), based on SDR++ " VERSION_STR);
        ImGui::Checkbox("Show demo window", &demoWindow);
        ImGui::Text("ImGui version: %s", ImGui::GetVersion());

        // ImGui::Checkbox("Bypass buffering", &sigpath::iqFrontEnd.inputBuffer.bypass);

        // ImGui::Text("Buffering: %d", (sigpath::iqFrontEnd.inputBuffer.writeCur - sigpath::iqFrontEnd.inputBuffer.readCur + 32) % 32);

        if (ImGui::Button("Test Bug")) {
            flog::error("Will this make the software crash?");
        }

        if (ImGui::Button("Testing something")) {
            gui::menu.order[0].open = true;
            firstMenuRender = true;
        }

        ImGui::Checkbox("WF Single Click", &gui::waterfall.VFOMoveSingleClick);
        ImGui::Checkbox("Lock Menu Order", &gui::menu.locked);

        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader("About " APP_NAME "##sdrpp_about")) {
        credits::drawLicenseText();
        ImGui::Spacing();
    }

#ifdef __ANDROID__
    // Bug reports: device, USB devices, app build, the recent logs and the configuration,
    // handed to the Android share sheet (mail, messengers, Drive...)
    ImGui::Spacing();
    if (ImGui::Button("Export debug log##sdrpp_debug_export", ImVec2(-FLT_MIN, 0))) {
        flog::info("Debug log export requested");
        backend::shareDebugReport(APP_NAME " " APP_VERSION " (" SDRPP_GIT_REV ", built " __DATE__ " " __TIME__ "), based on SDR++ " VERSION_STR
                                  ", source: " + sourceName + (playing ? " (running)" : " (stopped)"));
    }
    ImGui::TextDisabled("Sends the log and settings, e.g. by mail, for a bug report");
#else
    ImGui::Spacing();
    if (ImGui::Button("Export debug log##sdrpp_debug_export", ImVec2(-FLT_MIN, 0))) {
        flog::info("Debug log export requested");
        debugReportPath = exportDebugReport(APP_NAME " " APP_VERSION " (" SDRPP_GIT_REV ", built " __DATE__ " " __TIME__ "), based on SDR++ " VERSION_STR
                                            ", source: " + sourceName + (playing ? " (running)" : " (stopped)"));
    }
    if (!debugReportPath.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Saved: %s", debugReportPath.c_str());
        ImGui::PopTextWrapPos();
    }
    else {
        ImGui::TextDisabled("Saves the log and settings to your Downloads folder, for a bug report");
    }
#endif
}

#ifndef __ANDROID__
// Desktop bug report: build line, platform, the last three logs (root/sdrpp-log*.txt) and the
// configuration in one text file in Downloads; the folder is then shown in the file manager.
std::string MainWindow::exportDebugReport(const std::string& header) {
    std::string root = (std::string)core::args["root"];
    const char* home = getenv(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    std::string dir = home ? std::string(home) : root;
    std::error_code ec;
    if (home && std::filesystem::is_directory(dir + "/Downloads", ec)) { dir += "/Downloads"; }
    time_t t = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
    std::string path = dir + "/z2sdr-report-" + stamp + ".txt";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        flog::error("Debug report: cannot write {}", path);
        return "could not write " + path;
    }
    fprintf(f, APP_NAME " debug report %s\n%s\n", stamp, header.c_str());
#if defined(_WIN32)
    fprintf(f, "Platform: Windows\n");
#elif defined(__APPLE__)
    fprintf(f, "Platform: macOS\n");
#else
    fprintf(f, "Platform: Linux\n");
#endif
    fprintf(f, "Root: %s\n", root.c_str());
    auto append = [f](const std::string& p, const char* title) {
        FILE* in = fopen(p.c_str(), "rb");
        if (!in) { return; }
        fprintf(f, "\n===== %s =====\n", title);
        // Keep the end of long logs (3 MB each)
        fseek(in, 0, SEEK_END);
        long len = ftell(in);
        long start = len > 3 * 1024 * 1024 ? len - 3 * 1024 * 1024 : 0;
        if (start) { fprintf(f, "[... %ld bytes left out ...]\n", start); }
        fseek(in, start, SEEK_SET);
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) { fwrite(buf, 1, n, f); }
        fclose(in);
    };
    append(root + "/sdrpp-log.2.txt", "sdrpp-log.2.txt");
    append(root + "/sdrpp-log.1.txt", "sdrpp-log.1.txt");
    append(root + "/sdrpp-log.txt", "sdrpp-log.txt");
    append(root + "/config.json", "config.json");
    fclose(f);
    flog::info("Debug report written to {}", path);
#if defined(_WIN32)
    std::string args = "/select,\"" + path + "\"";
    for (auto& c : args) { if (c == '/' && &c != &args[0]) { c = '\\'; } }
    ShellExecuteA(NULL, "open", "explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open -R \"" + path + "\" &";
    (void)!system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
    (void)!system(cmd.c_str());
#endif
    return path;
}
#endif

void MainWindow::handlePinchZoom(ImGui::WaterfallVFO* vfo) {
    if (!touch::pinch.active) {
        pinchValid = false;
        return;
    }

    ImVec2 a = gui::waterfall.fftAreaMin;
    ImVec2 b = gui::waterfall.wfMax;
    float w = b.x - a.x;
    if (w <= 0.0f) { return; }

    if (touch::pinch.started) {
        ImVec2 c = touch::pinch.center0;
        pinchValid = (c.x >= a.x && c.x <= b.x && c.y >= a.y && c.y <= b.y);
        pinchBw0 = gui::waterfall.getViewBandwidth();
        // Frequency offset under the fingers stays put while zooming (and pans with them)
        pinchAnchor = gui::waterfall.getViewOffset() + (((c.x - a.x) / w) - 0.5) * pinchBw0;
    }
    if (!pinchValid) { return; }

    double wfBw = gui::waterfall.getBandwidth();
    double minBw = std::min<double>(1000.0, wfBw);
    double nbw = std::clamp<double>(pinchBw0 / touch::pinch.factor, minBw, wfBw);
    double frac = ((touch::pinch.center.x - a.x) / w) - 0.5;
    gui::waterfall.setViewBandwidth(nbw);
    gui::waterfall.setViewOffset(pinchAnchor - frac * nbw);

    // Keep the zoom slider in sync (inverse of the slider mapping)
    if (wfBw > 1000.0) {
        bw = sqrt(std::clamp<double>((nbw - 1000.0) / (wfBw - 1000.0), 0.0, 1.0));
    }
}

void MainWindow::setAutoRange(bool enabled) {
    autoRange = enabled;
    autoRangeInit = false;
    core::configManager.acquire();
    core::configManager.conf["autoRange"] = autoRange;
    if (!autoRange) {
        // Keep the last automatic levels as the manual starting point
        core::configManager.conf["min"] = fftMin;
        core::configManager.conf["max"] = fftMax;
    }
    core::configManager.release(true);
}

void MainWindow::updateAutoRange() {
    // ~10 updates per second is plenty and keeps the cost negligible
    double now = ImGui::GetTime();
    if (now - lastAutoRange < 0.1) { return; }
    lastAutoRange = now;

    int width = 0;
    float* fft = gui::waterfall.acquireLatestFFT(width);
    if (!fft) { return; }
    autoRangeBuf.clear();
    float peak = -INFINITY;
    for (int i = 0; i < width; i++) {
        float v = fft[i];
        if (!std::isfinite(v) || v <= -500.0f) { continue; } // hidden / invalid bins
        autoRangeBuf.push_back(v);
        peak = std::max<float>(peak, v);
    }
    gui::waterfall.releaseLatestFFT();
    if (autoRangeBuf.size() < 16) { return; }

    // Noise floor = 25th percentile of the visible bins (robust against carriers)
    size_t k = autoRangeBuf.size() / 4;
    std::nth_element(autoRangeBuf.begin(), autoRangeBuf.begin() + k, autoRangeBuf.end());
    float floor = autoRangeBuf[k];

    // Peak hold over the last 3 s: bursty / pulsed signals keep the top steady
    autoPeakHist.push_back({ now, peak });
    while (!autoPeakHist.empty() && now - autoPeakHist.front().first > 3.0) { autoPeakHist.erase(autoPeakHist.begin()); }
    for (auto const& ph : autoPeakHist) { peak = std::max<float>(peak, ph.second); }

    float tMin = floor - 6.0f;
    float tMax = std::max<float>(peak + 6.0f, floor + 35.0f);
    float tWfMin = floor - 2.0f; // noise sits just above black in the waterfall

    if (!autoRangeInit) {
        fftMin = tMin;
        fftMax = tMax;
        wfAutoMin = tWfMin;
        autoRangeInit = true;
        return;
    }

    // Widen quickly (new strong signal), narrow slowly (avoid pumping)
    // ~0.3 s to widen, ~5 s to narrow back (updates run at 10 Hz)
    auto track = [](float& cur, float target, bool fast) { cur += (target - cur) * (fast ? 0.35f : 0.02f); };
    track(fftMin, tMin, tMin < fftMin);
    track(fftMax, tMax, tMax > fftMax);
    track(wfAutoMin, tWfMin, tWfMin < wfAutoMin);
    fftMin = std::clamp<float>(fftMin, -160.0f, 0.0f);
    fftMax = std::clamp<float>(fftMax, fftMin + 10.0f, 20.0f);
}

void MainWindow::drawUsbNotice() {
    if (usbNotice.empty()) { return; }
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float s = style::uiScale;
    float w = std::min<float>(ds.x - 32.0f * s, 460.0f * s);
    ImGui::SetNextWindowPos(ImVec2(ds.x / 2.0f, ds.y / 3.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * s, 16.0f * s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.16f, 0.12f, 0.06f, 0.97f));
    ImGui::Begin("##sdrpp_usb_notice", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Wrong USB port?");
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(usbNotice.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(-FLT_MIN, 44.0f * s))) { usbNotice.clear(); }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

#ifdef __ANDROID__
// Plug and play: when Android hands us a newly opened SDR (USB permission granted, at
// start-up or when one is plugged in), select the matching source and start it.
void MainWindow::pollUsbSdr() {
    // Give the GUI a moment to lay out (waterfall sized, FFT buffers allocated) before a
    // source starts streaming into it: on a cold start (app launched by plugging in the
    // SDR) the first poll used to start the source before the first frame was drawn.
    if (++usbPollFrames < 30) { return; }
    double now = ImGui::GetTime();
    if (now - lastUsbPoll < 0.5) { return; }
    lastUsbPoll = now;

    // ESP32-S3 DevKit plugged in through its UART port: say which port to use instead
    int uartHint = backend::getUartBridgeHint();
    if (uartHint != lastUartHint) {
        lastUartHint = uartHint;
        if (uartHint) {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "A USB-serial adapter is plugged in (%04x:%04x).\n\n"
                     "On an ESP32-S3 DevKit this is the UART port. The SDR needs the native USB port: "
                     "unplug and use the other USB-C connector (the left one, marked USB / JTAG).",
                     (uartHint >> 16) & 0xFFFF, uartHint & 0xFFFF);
            usbNotice = buf;
            flog::warn("USB: UART bridge {:04x}:{:04x} plugged in, not an SDR", (uartHint >> 16) & 0xFFFF, uartHint & 0xFFFF);
        }
        else {
            usbNotice.clear();
        }
    }

    int vid = 0, pid = 0;
    int fd = backend::getDeviceFD(vid, pid, {});
    if (fd < 0) {
        // Unplugged (or not yet permitted): the next fd is a new device even if Android
        // happens to reuse the same descriptor number.
        lastUsbFd = -1;
        return;
    }
    if (fd == lastUsbFd) { return; }
    if (usbAutoStartPaused) { return; }   // a source is flashing its device: keep lastUsbFd for later
    lastUsbFd = fd;

    core::configManager.acquire();
    bool enabled = core::configManager.conf["usbAutoStart"];
    core::configManager.release();
    if (!enabled) { return; }

    auto matches = [vid, pid](const std::vector<backend::DevVIDPID>& list) {
        for (auto const& vp : list) {
            if (vp.vid == vid && vp.pid == pid) { return true; }
        }
        return false;
    };
    std::string source;
    if (matches(backend::RTL_SDR_VIDPIDS)) { source = "RTL-SDR"; }
    else if (vid == 0x303A && pid == 0x1001) { source = "ESP-SDR (ESP32-S3)"; }
    else if (matches(backend::HACKRF_VIDPIDS)) { source = "HackRF"; }
    else if (matches(backend::AIRSPY_VIDPIDS)) { source = "Airspy"; }
    else if (matches(backend::AIRSPYHF_VIDPIDS)) { source = "Airspy HF+"; }
    else if (matches(backend::HYDRASDR_VIDPIDS)) { source = "HydraSDR"; }
    if (source.empty()) { return; }

    flog::info("USB SDR {:04x}:{:04x} (fd {}): starting source '{}'", vid, pid, fd, source);
    if (playing) {
        crashlog::step("USB auto-start: stopping the running source");
        setPlayState(false);
    }
    crashlog::step("USB auto-start: selecting the source");
    if (!sourcemenu::selectSourceByName(source)) {
        flog::warn("USB auto-start: source '{}' is not loaded", source);
        return;
    }
    flog::info("USB auto-start: '{}' selected, starting", source);
    crashlog::step("USB auto-start: starting the source");
    setPlayState(true);
    flog::info("USB auto-start: started (playing = {})", playing);
    crashlog::step("MainWindow::draw");
}
#endif
