// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <mutex>

#include <imgui.h>

#include "imgui/imgui_layer.h"
#include "imgui/notice_layer.h"

namespace Overlay::Notice {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
std::string g_text;
Clock::time_point g_until{};

class NoticeLayer final : public ImGui::Layer {
public:
    void Draw() override {
        std::string text;
        {
            std::scoped_lock lk{g_mutex};
            if (g_text.empty() || Clock::now() >= g_until) {
                return;
            }
            text = g_text;
        }
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.08f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##ss_notice", nullptr, flags)) {
            ImGui::TextUnformatted(text.c_str());
        }
        ImGui::End();
    }
};

NoticeLayer g_layer;
bool g_registered = false;

} // namespace

void Show(std::string text, double seconds) {
    std::scoped_lock lk{g_mutex};
    g_text = std::move(text);
    g_until = Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
}

void Register() {
    if (!g_registered) {
        g_registered = true;
        ImGui::Layer::AddLayer(&g_layer);
    }
}

} // namespace Overlay::Notice
