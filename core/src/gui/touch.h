#pragma once
#include <imgui.h>

// Touch-optimised UI support (enabled by default on Android).
//
// The platform backend feeds raw finger events into this module instead of
// straight into ImGui. A touch that starts over a scrollable window is held
// back until its direction is known: a vertical drag scrolls the window
// (with fling), anything else is replayed to ImGui as a normal click/drag.
// Two fingers produce a pinch gesture that the waterfall uses for zooming.
namespace touch {
    extern bool enabled;

    // Platform backend -> touch (call before ImGui::NewFrame)
    void fingerDown(float x, float y);
    void fingerMove(float x, float y);
    void fingerUp(float x, float y);
    void pinchBegin(ImVec2 p0, ImVec2 p1);
    void pinchMove(ImVec2 p0, ImVec2 p1);
    void pinchEnd();
    void cancel();
    void update();

    // Bigger hit targets on top of ImGuiStyle::ScaleAllSizes()
    void applyStyle(ImGuiStyle& style, float scale);

    // True when the main window is in a portrait-like layout
    bool narrow(ImVec2 winSize);

    struct Pinch {
        bool active = false;
        bool started = false; // true for exactly one frame when a pinch begins
        float factor = 1.0f;  // current finger distance / distance at start
        ImVec2 center;        // current midpoint
        ImVec2 center0;       // midpoint at start
    };
    extern Pinch pinch;
}
