// Touch layout for SDR++ (Android): full-screen waterfall with floating controls.
//
//  - The waterfall fills the screen.
//  - A fixed strip on top holds the frequency (right) and the SNR meter (left). It never
//    covers the spectrum or its frequency axis.
//  - The controls live in a floating rail that slides in from the left. When it is
//    hidden only a thin glowing handle shows. Tap the handle (or drag it right) to open,
//    tap outside, or leave it alone for a few seconds, to close.
//  - The module menu is a drawer over a dimmed scrim, opened from the rail.
//
// Motion: critically damped exponential approach (no overshoot), ~180 ms to settle.

#include <gui/main_window.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/icons.h>
#include <gui/touch.h>
#include <gui/widgets/snr_meter.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <version.h>
#include <utils/flog.h>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace {
    const ImGuiWindowFlags OVERLAY_FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
    // (no NoBringToFrontOnFocus: ImGui creates such windows at the back, behind "Main")

    void approach(float& v, float target, float rate, float dt) {
        v += (target - v) * (1.0f - expf(-rate * dt));
        if (fabsf(target - v) < 0.002f) { v = target; }
    }

    // Ease for the slide position: the exponential approach already decelerates,
    // a smoothstep on top gives a softer start.
    float ease(float t) {
        return t * t * (3.0f - 2.0f * t);
    }

    bool inRect(ImVec2 p, const ImRect& r) {
        return r.Contains(p);
    }
}

void MainWindow::drawTouchTuningButton(ImVec2 imageSize, int framePadding) {
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (tuningMode == tuner::TUNER_MODE_CENTER) {
        ImGui::PushID("sdrpp_touch_ena_st_btn");
        if (ImGui::ImageButton(icons::CENTER_TUNING, imageSize, ImVec2(0, 0), ImVec2(1, 1), framePadding, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_NORMAL;
            gui::waterfall.VFOMoveSingleClick = false;
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = false;
            core::configManager.release(true);
        }
        ImGui::PopID();
    }
    else {
        ImGui::PushID("sdrpp_touch_dis_st_btn");
        if (ImGui::ImageButton(icons::NORMAL_TUNING, imageSize, ImVec2(0, 0), ImVec2(1, 1), framePadding, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_CENTER;
            gui::waterfall.VFOMoveSingleClick = true;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = true;
            core::configManager.release(true);
        }
        ImGui::PopID();
    }
}

void MainWindow::applyZoomSlider(ImGui::WaterfallVFO* vfo) {
    double factor = (double)bw * (double)bw;
    double wfBw = gui::waterfall.getBandwidth();
    double delta = wfBw - 1000.0;
    double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);
    gui::waterfall.setViewBandwidth(finalBw);
    if (vfo != NULL) {
        gui::waterfall.setViewOffset(vfo->centerOffset); // center vfo on screen
    }
}

void MainWindow::drawTouchLayout(ImGui::WaterfallVFO* vfo) {
    ImGuiIO& io = ImGui::GetIO();
    const float s = style::uiScale;
    const float dt = std::clamp<float>(io.DeltaTime, 0.0f, 0.1f);
    const double now = ImGui::GetTime();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImVec2 mouse = io.MousePos;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const float pad = 8.0f * s;

    if (autostart) {
        autostart = false;
        setPlayState(true);
    }
    showCredits = false;

    // ---- Animation state ----
    approach(railT, railOpen ? 1.0f : 0.0f, 16.0f, dt);
    approach(drawerT, drawerOpen ? 1.0f : 0.0f, 16.0f, dt);

    // ---- Who owns this touch? (overlay rects are from the previous frame) ----
    bool overOverlay = (railT > 0.01f && inRect(mouse, railRect)) || (railT < 0.99f && inRect(mouse, handleRect));
    if (clicked && drawerT < 0.01f && !overOverlay && railOpen) {
        // Tap outside the rail closes it, and the tap must not also tune
        railOpen = false;
        swallowTouch = true;
    }
    if (swallowTouch && !down && !clicked) { swallowTouch = false; }
    if (railOpen && down && inRect(mouse, railRect)) { railLastTouch = now; }
    if (railOpen && now - railLastTouch > 4.5 && !ImGui::IsAnyItemActive()) { railOpen = false; }

    lockWaterfallControls = overOverlay || swallowTouch || drawerT > 0.01f;

    // ---- Top strip: SNR meter (left) and frequency (right) ----
    const ImVec2 wpad = ImGui::GetStyle().WindowPadding;
    ImGui::PushFont(style::bigFont);
    ImVec2 dsz = ImGui::CalcTextSize("0");
    float need = (12.0f * dsz.x) + (3.0f * ImGui::CalcTextSize(".").x) + 15.0f;
    ImGui::PopFont();
    float availW = winSize.x - 2.0f * wpad.x;
    float fscale = std::min<float>(0.75f, availW / need); // big enough for sunlight, leaves room for the meter
    float digH = dsz.y * fscale;
    float freqW = need * fscale;
    float stripTop = wpad.y;
    float stripH = digH + 4.0f * s;

    float snrW = availW - freqW - 16.0f * s;
    if (snrW > 140.0f * s) {
        ImGui::SetCursorPos(ImVec2(wpad.x, stripTop + (stripH - 26.0f) / 2.0f));
        ImGui::SetNextItemWidth(std::min<float>(snrW, 320.0f * s));
        ImGui::SNRMeter((vfo != NULL) ? gui::waterfall.selectedVFOSNR : 0);
    }
    {
        bool lockSave = lockWaterfallControls;
        lockWaterfallControls = drawerT > 0.01f || (railT > 0.01f && inRect(mouse, railRect));
        ImGui::SetWindowFontScale(fscale);
        // the widget adds the window padding and centres the digits around the cursor itself
        ImGui::SetCursorPos(ImVec2(winSize.x - wpad.x - freqW - wpad.x, stripTop + (digH / 2.0f) - ceilf(15 * s) - 5));
        gui::freqSelect.draw();
        ImGui::SetWindowFontScale(1.0f);
        lockWaterfallControls = lockSave;
    }

    // ---- Waterfall (rest of the screen) ----
    ImGui::SetCursorPos(ImVec2(wpad.x, stripTop + stripH + 4.0f * s));
    ImGui::BeginChild("Waterfall");
    gui::waterfall.draw();
    ImGui::EndChild();

    if (!lockWaterfallControls) { handlePinchZoom(vfo); }
    else { pinchValid = false; }

    if (autoRange) { updateAutoRange(); }
    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(autoRange ? wfAutoMin : fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

    ImGui::End(); // "Main"

    const ImVec4 panelBg(0.10f, 0.10f, 0.11f, 0.90f);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_SliderGrab);

    // ---- Rail ----
    const float btn = 48.0f * s;
    const float gap = 8.0f * s;
    const float railPad = 10.0f * s;
    const int fp = (int)(9.0f * s);
    const ImVec2 img(btn - 2 * fp, btn - 2 * fp);
    const float railW = 2.0f * btn + gap + 2.0f * railPad;
    const float railTop = stripTop + stripH + pad;
    const float railAvail = winSize.y - railTop - pad;
    const float labelH = ImGui::GetTextLineHeight();
    const float fixedH = 2.0f * railPad + 3.0f * btn + 3.0f * gap + gap + labelH;
    const float sliderH = std::clamp<float>(railAvail - fixedH, 60.0f * s, 260.0f * s);
    const float railH = std::min<float>(fixedH + sliderH, railAvail);
    const float railX = pad - (1.0f - ease(railT)) * (railW + 2.0f * pad);

    if (railT > 0.001f) {
        // Spare height goes above the rail: it sits in the thumb zone on tall (portrait) screens
        ImGui::SetNextWindowPos(ImVec2(railX, railTop + (railAvail - railH) * 0.6f));
        ImGui::SetNextWindowSize(ImVec2(railW, railH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(railPad, railPad));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * s);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, panelBg);
        ImGui::Begin("##sdrpp_touch_rail", NULL, OVERLAY_FLAGS | ImGuiWindowFlags_NoScrollbar);
        {
            ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);

            // Row 1: menu, play/stop
            ImGui::PushID("sdrpp_touch_menu_btn");
            if (ImGui::ImageButton(icons::MENU, img, ImVec2(0, 0), ImVec2(1, 1), fp, ImVec4(0, 0, 0, 0), textCol)) {
                drawerOpen = true;
                railOpen = false;
            }
            ImGui::PopID();
            ImGui::SameLine();
            bool tmpPlaying = playing;
            if (playButtonLocked && !tmpPlaying) { style::beginDisabled(); }
            ImGui::PushID("sdrpp_touch_play_btn");
            if (ImGui::ImageButton(playing ? icons::STOP : icons::PLAY, img, ImVec2(0, 0), ImVec2(1, 1), fp, ImVec4(0, 0, 0, 0), textCol)) {
                setPlayState(!playing);
            }
            ImGui::PopID();
            if (playButtonLocked && !tmpPlaying) { style::endDisabled(); }

            // Row 2: tuning mode, mute
            drawTouchTuningButton(img, fp);
            ImGui::SameLine();
            sigpath::sinkManager.showMuteButton(gui::waterfall.selectedVFO, img, fp);

            // Row 3: auto range
            bool wasAuto = autoRange;
            if (wasAuto) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive));
            }
            if (ImGui::Button("Auto##sdrpp_touch_auto", ImVec2(ImGui::GetContentRegionAvail().x, btn))) {
                setAutoRange(!autoRange);
            }
            if (wasAuto) { ImGui::PopStyleColor(2); }

            // Sliders: volume and zoom, plus max/min in manual mode
            int n = autoRange ? 2 : 4;
            float innerW = ImGui::GetContentRegionAvail().x;
            float sg = autoRange ? gap : 4.0f * s;
            float sw = (innerW - (n - 1) * sg) / n;
            float sh = std::max<float>(ImGui::GetContentRegionAvail().y - labelH - gap, 40.0f * s);
            ImVec2 vs(sw, sh);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sg, gap));
            ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, std::min<float>(32.0f * s, sh / 4.0f));

            sigpath::sinkManager.showVolumeSliderV(gui::waterfall.selectedVFO, "##sdrpp_touch_vol", vs);
            ImGui::SameLine();
            if (ImGui::VSliderFloat("##sdrpp_touch_zoom", vs, &bw, 1.0f, 0.0f, "")) {
                applyZoomSlider(vfo);
            }
            if (!autoRange) {
                ImGui::SameLine();
                if (ImGui::VSliderFloat("##sdrpp_touch_max", vs, &fftMax, 0.0f, -160.0f, "")) {
                    fftMax = std::max<float>(fftMax, fftMin + 10);
                    core::configManager.acquire();
                    core::configManager.conf["max"] = fftMax;
                    core::configManager.release(true);
                }
                ImGui::SameLine();
                if (ImGui::VSliderFloat("##sdrpp_touch_min", vs, &fftMin, 0.0f, -160.0f, "")) {
                    fftMin = std::min<float>(fftMax - 10, fftMin);
                    core::configManager.acquire();
                    core::configManager.conf["min"] = fftMin;
                    core::configManager.release(true);
                }
            }
            ImGui::PopStyleVar(2);

            // Labels under the sliders
            const char* labels[4] = { "Vol", "Zoom", "Max", "Min" };
            float lx = ImGui::GetCursorPosX();
            float ly = ImGui::GetCursorPosY();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            for (int i = 0; i < n; i++) {
                float tw = ImGui::CalcTextSize(labels[i]).x;
                ImGui::SetCursorPos(ImVec2(lx + i * (sw + sg) + (sw - tw) / 2.0f, ly));
                ImGui::TextUnformatted(labels[i]);
            }
            ImGui::PopStyleColor();

            railRect = ImRect(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize());
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(5);
    }
    else {
        railRect = ImRect();
    }

    // ---- Handle (rail closed) ----
    if (railT < 0.999f && drawerT < 0.01f) {
        float hw = 36.0f * s, hh = 150.0f * s;
        ImVec2 hPos(0.0f, railTop + (railAvail - hh) * 0.55f);
        ImGui::SetNextWindowPos(hPos);
        ImGui::SetNextWindowSize(ImVec2(hw, hh));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##sdrpp_touch_handle", NULL, OVERLAY_FLAGS | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        {
            float a = 1.0f - railT;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float pillW = 5.0f * s, pillH = 72.0f * s;
            ImVec2 p0(hPos.x + 5.0f * s, hPos.y + (hh - pillH) / 2.0f);
            ImVec4 ac = ImGui::ColorConvertU32ToFloat4(accent);
            // soft glow, then the pill
            dl->AddRectFilled(p0 - ImVec2(3.0f * s, 3.0f * s), p0 + ImVec2(pillW + 3.0f * s, pillH + 3.0f * s),
                              ImGui::GetColorU32(ImVec4(ac.x, ac.y, ac.z, 0.18f * a)), 6.0f * s);
            dl->AddRectFilled(p0, p0 + ImVec2(pillW, pillH), ImGui::GetColorU32(ImVec4(ac.x, ac.y, ac.z, 0.85f * a)), pillW / 2.0f);

            ImGui::InvisibleButton("##sdrpp_touch_handle_btn", ImVec2(hw, hh));
            bool dragOpen = ImGui::IsItemActive() && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x > 16.0f * s;
            if (ImGui::IsItemClicked() || dragOpen) {
                railOpen = true;
                railLastTouch = now;
            }
            handleRect = ImRect(hPos, hPos + ImVec2(hw, hh));
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
    else {
        handleRect = ImRect();
    }

    // ---- Drawer with the module menu ----
    bool drawerOpening = drawerOpen && !drawerWasOpen;
    drawerWasOpen = drawerOpen;
    if (drawerT > 0.001f) {
        // Scrim
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(winSize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f * drawerT));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##sdrpp_touch_scrim", NULL, OVERLAY_FLAGS | ImGuiWindowFlags_NoScrollbar);
        if (drawerOpening) { ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); }
        ImGui::SetCursorPos(ImVec2(0, 0));
        if (ImGui::InvisibleButton("##sdrpp_touch_scrim_btn", winSize) && drawerOpen) {
            drawerOpen = false;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Drawer
        float dw = std::min<float>(winSize.x * 0.85f, 420.0f * s);
        float dx = -(1.0f - ease(drawerT)) * dw;
        ImGui::SetNextWindowPos(ImVec2(dx, 0));
        ImGui::SetNextWindowSize(ImVec2(dw, winSize.y));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.09f, 0.10f, 0.98f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##sdrpp_touch_drawer", NULL, OVERLAY_FLAGS | ImGuiWindowFlags_NoScrollbar);
        if (drawerOpening) { ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); }
        {
            ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushID("sdrpp_touch_drawer_close");
            if (ImGui::ImageButton(icons::MENU, img, ImVec2(0, 0), ImVec2(1, 1), fp, ImVec4(0, 0, 0, 0), textCol)) {
                drawerOpen = false;
            }
            ImGui::PopID();
            ImGui::SameLine();
            float rowY = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(rowY + (btn - ImGui::GetTextLineHeight()) / 2.0f);
            ImGui::TextUnformatted("SDR++");

            // Play / stop here too: the source settings (device, sample rate, mode) can only be
            // changed while stopped, so stopping must not mean closing the drawer
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - btn, rowY));
            // Accent coloured, so it never reads as a greyed-out control
            bool tmpPlaying = playing;
            if (playButtonLocked && !tmpPlaying) { style::beginDisabled(); }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.23f, 0.51f, 0.89f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.58f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.42f, 0.78f, 1.0f));
            ImGui::PushID("sdrpp_touch_drawer_play");
            if (ImGui::ImageButton(playing ? icons::STOP : icons::PLAY, img, ImVec2(0, 0), ImVec2(1, 1), fp, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1))) {
                setPlayState(!playing);
            }
            ImGui::PopID();
            ImGui::PopStyleColor(3);
            if (playButtonLocked && !tmpPlaying) { style::endDisabled(); }
            ImGui::TextDisabled("SDR++ " VERSION_STR " / " SDRPP_ESP_VERSION " (" SDRPP_GIT_REV ", " __DATE__ ")");

            ImGui::BeginChild("##sdrpp_touch_drawer_scroll");
            drawMenu();
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}
