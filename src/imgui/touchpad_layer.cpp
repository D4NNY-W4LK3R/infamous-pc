// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>

#include <imgui.h>

#include "imgui/controls_hint_layer.h"
#include "imgui/imgui_layer.h"
#include "imgui/touchpad_layer.h"
#include "input/controller.h"
#include "input/input_mouse.h"

namespace Overlay::Touchpad {

namespace {

using Clock = std::chrono::steady_clock;

class TouchpadLayer final : public ImGui::Layer {
public:
    void Draw() override {
        if (!Overlay::ControlsHint::Visible()) {
            return; // Home hides it together with the key panel
        }
        const Input::TouchpadSnapshot pad = Input::LastTouchpad();
        const bool grip = Input::TouchGripActive();
        const Input::TouchModeView mode = Input::GetTouchModeView();
        const bool touched = pad.down[0] || pad.down[1] || pad.click;
        const auto now = Clock::now();
        if (touched || grip || mode.active) {
            shown_until = now + std::chrono::milliseconds(600);
        }
        if (now >= shown_until) {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        const float w = std::max(160.0f, io.DisplaySize.x * 0.14f);
        const float h = w * (942.0f / 1920.0f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 12.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##ss_touchpad", nullptr, flags)) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(w, h));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p1(p0.x + w, p0.y + h);
            dl->AddRectFilled(p0, p1, IM_COL32(30, 30, 34, 220), 8.0f);
            dl->AddRect(p0, p1,
                        pad.click ? IM_COL32(255, 200, 60, 255) : IM_COL32(150, 150, 160, 255),
                        8.0f, 0, pad.click ? 3.0f : 1.5f);
            dl->AddLine(ImVec2(p0.x + w * 0.5f, p0.y + 4.0f),
                        ImVec2(p0.x + w * 0.5f, p1.y - 4.0f), IM_COL32(80, 80, 90, 160));
            for (int i = 0; i < 2; ++i) {
                if (!pad.down[i]) {
                    continue;
                }
                const ImVec2 c(p0.x + pad.x[i] * w, p0.y + pad.y[i] * h);
                dl->AddCircleFilled(c, h * 0.09f, IM_COL32(90, 170, 255, 235));
                dl->AddCircle(c, h * 0.12f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
            }
            const char* hint =
                grip ? "Move the mouse to slide - right button: second finger"
                : mode.active ? "Touch mode (Delete): click where the finger goes"
                : pad.click   ? "Touchpad pressed"
                              : "Touchpad";
            ImGui::TextUnformatted(hint);
        }
        ImGui::End();
    }

private:
    Clock::time_point shown_until{};
};

TouchpadLayer g_layer;
bool g_registered = false;

} // namespace

void Register() {
    if (!g_registered) {
        g_registered = true;
        ImGui::Layer::AddLayer(&g_layer);
    }
}

} // namespace Overlay::Touchpad
