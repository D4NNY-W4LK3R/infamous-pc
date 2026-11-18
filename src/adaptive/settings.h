// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// User configuration of the optimizer: <user dir>/adaptive/settings.ini, plain NAME=VALUE lines.
//
//   mode = manual          observe | manual | automatic | calibration
//   enabled = 1            0 turns the whole optimizer off (no thread, no telemetry)
//   overlay = 1            the F9 status window is available
//   calibrate_on_battery = 0
//   tick_ms = 1000
//
// Environment variables win over the file (SS_ADAPTIVE_MODE, SS_ADAPTIVE=0). The overlay writes
// the file back when the user changes the mode, so the choice is remembered.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "adaptive/contracts.h"

namespace Adaptive {

struct UserSettings {
    bool enabled = true;
    Mode mode = Mode::Manual;
    bool overlay = true;
    bool calibrate_on_battery = false;
    std::uint32_t tick_ms = 1000;
    std::vector<std::string> warnings; ///< unknown keys, bad values (not fatal)
};

UserSettings ParseSettings(const std::string& text);
std::string SerializeSettings(const UserSettings& s);

/// Reads the file (a missing file gives defaults), then applies environment overrides.
UserSettings LoadSettings(const std::filesystem::path& file,
                          const std::function<std::optional<std::string>(const std::string&)>& env);
bool SaveSettings(const std::filesystem::path& file, const UserSettings& s);

} // namespace Adaptive
