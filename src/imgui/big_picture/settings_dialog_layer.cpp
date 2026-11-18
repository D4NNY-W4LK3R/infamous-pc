// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include <imgui.h>

#include "imgui/imgui_std.h"
#include "settings_dialog_layer.h"

namespace ImGuiEmuSettings {

static bool running = false;
// Read by the window's event loop (main thread) to hand the mouse to the dialog.
static std::atomic<bool> g_open{false};
static std::optional<SettingsLayer> settingsLayer = std::nullopt;

SettingsLayer::SettingsLayer() {
    AddLayer(this);
    settingsWindow.Prepare();
}

SettingsLayer::~SettingsLayer() {}

void SettingsLayer::Finish() {
    RemoveLayer(this);
    g_open.store(false, std::memory_order_relaxed);
    settingsLayer.reset();
}

void SettingsLayer::Draw() {
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[IMGUI_FONT_SETTINGS_WINDOW]);
    settingsWindow.DrawSettings(&running, nullptr);
    ImGui::PopFont();

    if (!running) {
        Finish();
    }
}

void OpenInGameSettingsDialog() {
    if (settingsLayer.has_value()) {
        return;
    }

    settingsLayer.emplace();
    running = true;
    g_open.store(true, std::memory_order_relaxed);
}

bool IsInGameSettingsDialogOpen() {
    return g_open.load(std::memory_order_relaxed);
}

} // namespace ImGuiEmuSettings
