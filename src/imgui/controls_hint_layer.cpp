// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// On-screen controls hint: the game keeps showing its PlayStation button prompts, and this panel
// shows the keyboard / mouse key for each of them next to it - both at once. Read live from the
// active input config, so rebinding a key (F8 reloads) updates the panel. Home shows / hides it.

#include <atomic>

#include <imgui.h>

#include "adaptive/glue_internal.h"
#include "core/mouse_aim.h"
#include "imgui/controls_hint_layer.h"
#include "imgui/imgui_layer.h"
#include "input/input_handler.h"
#include "input/input_mouse.h"

namespace Overlay::ControlsHint {

namespace {

std::atomic<int> g_visible{-1}; // -1: not decided yet (taken from settings.ini on first draw)
bool g_registered = false;

ImVec4 PadColour(const std::string& pad) {
    if (pad == "Cross") {
        return ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
    }
    if (pad == "Circle") {
        return ImVec4(1.0f, 0.42f, 0.42f, 1.0f);
    }
    if (pad == "Square") {
        return ImVec4(1.0f, 0.55f, 0.85f, 1.0f);
    }
    if (pad == "Triangle") {
        return ImVec4(0.35f, 0.9f, 0.6f, 1.0f);
    }
    return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
}

const char* MouseState() {
    if (!Core::MouseAim::Enabled()) {
        return nullptr;
    }
    if (!Input::IsMouseJoystickMode()) {
        return Input::IsMouseGyroMode() ? "Mouse: motion sensor (F6 again to go back)"
                                        : "Mouse: free cursor (F7 captures it)";
    }
    if (Core::MouseAim::Installed() && Core::MouseAim::CameraLive()) {
        return "Mouse: camera (1:1 mouse look)";
    }
    return "Mouse: right stick + motion sensor (spray can)";
}

class ControlsHintLayer final : public ImGui::Layer {
public:
    void Draw() override {
        int visible = g_visible.load(std::memory_order_relaxed);
        if (visible < 0) {
            visible = Adaptive::Glue::SettingsForOverlay().controls_hint ? 1 : 0;
            g_visible.store(visible, std::memory_order_relaxed);
        }
        if (visible == 0) {
            return;
        }
        const auto hints = Input::GetKeyboardHints();
        if (hints.empty()) {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(10.0f, io.DisplaySize.y - 10.0f), ImGuiCond_Always,
                                ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##ss_controls_hint", nullptr, flags)) {
            ImGui::SetWindowFontScale(0.85f);
            if (ImGui::BeginTable("##hints", 2, ImGuiTableFlags_SizingFixedFit)) {
                for (const auto& h : hints) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(PadColour(h.pad), "%s", h.pad.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(h.keys.c_str());
                }
                ImGui::EndTable();
            }
            if (const char* m = MouseState()) {
                ImGui::TextDisabled("%s", m);
            }
            ImGui::TextDisabled("Home hides this | F1 optimizer | F7 mouse | Del touchpad");
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::End();
    }
};

ControlsHintLayer g_layer;

} // namespace

void Toggle() {
    int v = g_visible.load(std::memory_order_relaxed);
    if (v < 0) {
        v = Adaptive::Glue::SettingsForOverlay().controls_hint ? 1 : 0;
    }
    v = v ? 0 : 1;
    g_visible.store(v, std::memory_order_relaxed);
    Adaptive::Glue::SettingsForOverlay().controls_hint = v != 0;
    Adaptive::Glue::SaveSettingsFromOverlay();
}

void Register() {
    if (!g_registered) {
        g_registered = true;
        ImGui::Layer::AddLayer(&g_layer);
    }
}

} // namespace Overlay::ControlsHint
