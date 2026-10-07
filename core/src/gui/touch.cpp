#include <gui/touch.h>
#include <gui/style.h>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include <cfloat>
#include <cmath>
#include <algorithm>

namespace touch {
#ifdef __ANDROID__
    bool enabled = true;
#else
    bool enabled = false;
#endif
    Pinch pinch;

    // TS_ prefix: Windows headers define IGNORE (and friends) as macros
    enum State {
        TS_IDLE,     // no finger
        TS_PENDING,  // finger down over a scrollable window, direction not known yet
        TS_PRESSED,  // forwarded to ImGui as a held mouse button
        TS_SCROLLING,
        TS_PINCHING,
        TS_IGNORE    // rest of the gesture is swallowed (after a pinch)
    };

    static State state = TS_IDLE;
    static ImVec2 downPos;
    static ImVec2 lastPos;
    static double downTime = 0.0;
    static double lastMoveTime = 0.0;
    static ImGuiWindow* target = NULL;
    static float pendingScroll = 0.0f;
    static float velocity = 0.0f; // px/s, positive = content follows finger downwards
    static ImGuiWindow* flingTarget = NULL;
    static double lastUpdate = 0.0;
    static float pinchDist0 = 1.0f;
    static bool pinchStartPending = false;

    // Hold time after which a still finger is treated as a press (ms)
    static const double HOLD_TIME = 0.15;

    static double now() {
        return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    }

    static float slop() {
        return 8.0f * style::uiScale;
    }

    static float dist(ImVec2 a, ImVec2 b) {
        float dx = a.x - b.x, dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    }

    // Innermost visible window under pos that can scroll vertically.
    // Stops at popup / top-level windows so a combo list never scrolls the menu behind it.
    static ImGuiWindow* findScrollable(ImVec2 pos) {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx) { return NULL; }
        ImGuiContext& g = *ctx;
        ImGuiWindow* hit = NULL;
        for (int i = g.Windows.Size - 1; i >= 0; i--) {
            ImGuiWindow* w = g.Windows[i];
            if (!w->Active || w->Hidden || (w->Flags & ImGuiWindowFlags_NoMouseInputs)) { continue; }
            if (w->OuterRectClipped.Contains(pos)) {
                hit = w;
                break;
            }
        }
        while (hit) {
            // Only child windows (menu panels, lists) and popups (combo lists) scroll by touch;
            // the full-screen main window must never swallow waterfall gestures.
            bool scrollable = (hit->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup)) && !(hit->Flags & ImGuiWindowFlags_NoScrollWithMouse);
            if (scrollable && hit->ScrollMax.y > 2.0f * style::uiScale) { return hit; }
            if (!(hit->Flags & ImGuiWindowFlags_ChildWindow) || (hit->Flags & ImGuiWindowFlags_Popup)) { break; }
            hit = hit->ParentWindow;
        }
        return NULL;
    }

    static bool ready() {
        return ImGui::GetCurrentContext() != NULL;
    }

    static void clearHover() {
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    static void press(ImVec2 at, ImVec2 cur) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        io.AddMouseButtonEvent(0, true);
        if (cur.x != at.x || cur.y != at.y) { io.AddMousePosEvent(cur.x, cur.y); }
        state = TS_PRESSED;
    }

    void fingerDown(float x, float y) {
        if (!ready()) { state = TS_IDLE; return; }
        ImVec2 p(x, y);
        downPos = lastPos = p;
        downTime = lastMoveTime = now();
        flingTarget = NULL;
        velocity = 0.0f;
        pendingScroll = 0.0f;

        target = findScrollable(p);
        if (target) {
            // Show hover feedback, but hold the button back until we know the direction
            ImGui::GetIO().AddMousePosEvent(x, y);
            state = TS_PENDING;
        }
        else {
            press(p, p);
        }
    }

    void fingerMove(float x, float y) {
        if (!ready()) { state = TS_IDLE; return; }
        ImVec2 p(x, y);
        double t = now();
        switch (state) {
        case TS_PENDING: {
            float dx = fabsf(x - downPos.x), dy = fabsf(y - downPos.y);
            if (dy > slop() && dy > dx) {
                state = TS_SCROLLING;
                clearHover();
                pendingScroll += y - lastPos.y;
            }
            else if (dx > slop()) {
                press(downPos, p);
            }
            break;
        }
        case TS_SCROLLING: {
            float dy = y - lastPos.y;
            pendingScroll += dy;
            double dt = t - lastMoveTime;
            if (dt > 0.0) {
                float inst = dy / (float)dt;
                velocity = 0.6f * inst + 0.4f * velocity;
            }
            break;
        }
        case TS_PRESSED:
            ImGui::GetIO().AddMousePosEvent(x, y);
            break;
        default:
            break;
        }
        lastPos = p;
        lastMoveTime = t;
    }

    void fingerUp(float x, float y) {
        if (!ready()) { state = TS_IDLE; return; }
        ImGuiIO& io = ImGui::GetIO();
        switch (state) {
        case TS_PENDING:
            // Short tap: replay as a full click at the touch-down position
            press(downPos, downPos);
            io.AddMouseButtonEvent(0, false);
            clearHover();
            break;
        case TS_PRESSED:
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, false);
            clearHover();
            break;
        case TS_SCROLLING:
            if (now() - lastMoveTime > 0.08) { velocity = 0.0f; }
            if (fabsf(velocity) > 50.0f * style::uiScale) { flingTarget = target; }
            break;
        default:
            clearHover();
            break;
        }
        state = TS_IDLE;
    }

    void pinchBegin(ImVec2 p0, ImVec2 p1) {
        if (!ready()) { state = TS_IDLE; return; }
        if (state == TS_PRESSED) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMouseButtonEvent(0, false);
        }
        clearHover();
        flingTarget = NULL;
        state = TS_PINCHING;
        pinchDist0 = std::max<float>(dist(p0, p1), 1.0f);
        pinch.factor = 1.0f;
        pinch.center = pinch.center0 = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        pinchStartPending = true;
        pinch.active = true;
    }

    void pinchMove(ImVec2 p0, ImVec2 p1) {
        if (state != TS_PINCHING) { return; }
        pinch.factor = std::max<float>(dist(p0, p1), 1.0f) / pinchDist0;
        pinch.center = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    }

    void pinchEnd() {
        pinch.active = false;
        state = TS_IGNORE; // remaining finger does nothing until it is lifted
    }

    void cancel() {
        target = NULL;
        pinch.active = false;
        if (!ready()) { flingTarget = NULL; state = TS_IDLE; return; }
        if (state == TS_PRESSED) { ImGui::GetIO().AddMouseButtonEvent(0, false); }
        clearHover();
        pinch.active = false;
        flingTarget = NULL;
        state = TS_IDLE;
    }

    void update() {
        if (!ready()) { return; }
        double t = now();
        float dt = (float)std::clamp<double>(t - lastUpdate, 0.0, 0.1);
        lastUpdate = t;

        pinch.started = pinchStartPending;
        pinchStartPending = false;

        // A finger resting on a widget becomes a press (e.g. hold, then drag a slider)
        if (state == TS_PENDING && t - downTime > HOLD_TIME) {
            press(downPos, lastPos);
        }

        if (state == TS_SCROLLING && target && pendingScroll != 0.0f) {
            ImGui::SetScrollY(target, std::clamp<float>(target->Scroll.y - pendingScroll, 0.0f, target->ScrollMax.y));
            pendingScroll = 0.0f;
        }

        if (state == TS_IDLE && flingTarget) {
            float ny = flingTarget->Scroll.y - velocity * dt;
            if (ny <= 0.0f || ny >= flingTarget->ScrollMax.y || !flingTarget->Active) {
                ny = std::clamp<float>(ny, 0.0f, flingTarget->ScrollMax.y);
                velocity = 0.0f;
            }
            ImGui::SetScrollY(flingTarget, ny);
            velocity *= expf(-3.0f * dt);
            if (fabsf(velocity) < 20.0f * style::uiScale) { flingTarget = NULL; }
        }
    }

    void applyStyle(ImGuiStyle& s, float scale) {
        // Expressed in units of the base font height so it tracks the UI scale
        float f = 16.0f * scale;
        s.WindowPadding = ImVec2(0.5f * f, 0.5f * f);
        s.FramePadding = ImVec2(0.45f * f, 0.40f * f);
        s.ItemSpacing = ImVec2(0.5f * f, 0.45f * f);
        s.ItemInnerSpacing = ImVec2(0.4f * f, 0.35f * f);
        // Must stay below half the item spacing or neighbouring widgets steal touches
        s.TouchExtraPadding = ImVec2(0.15f * f, 0.2f * f);
        s.IndentSpacing = 1.2f * f;
        s.ScrollbarSize = 0.9f * f;
        s.GrabMinSize = 1.1f * f;
        s.FrameRounding = 0.25f * f;
        s.GrabRounding = 0.25f * f;
        s.ScrollbarRounding = 0.45f * f;
    }

    bool narrow(ImVec2 winSize) {
        return enabled && winSize.x < winSize.y;
    }
}
