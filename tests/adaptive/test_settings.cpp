// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>

#include <gtest/gtest.h>

#include "adaptive/settings.h"

using namespace Adaptive;

TEST(Settings, DefaultsAreManualAndEnabled) {
    const auto s = ParseSettings("");
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.mode, Mode::Manual);
    EXPECT_TRUE(s.warnings.empty());
}

TEST(Settings, ParsesAndWarnsWithoutFailing) {
    const auto s = ParseSettings("mode = Automatic # comment\n enabled=0\nbogus=1\ntick_ms=5\n"
                                 "overlay = maybe\nno equals here\n");
    EXPECT_EQ(s.mode, Mode::Automatic);
    EXPECT_FALSE(s.enabled);
    EXPECT_EQ(s.tick_ms, 1000u);
    EXPECT_EQ(s.warnings.size(), 4u);
}

TEST(Settings, RoundTripAndEnvironmentOverride) {
    UserSettings s;
    s.mode = Mode::Calibration;
    s.overlay = false;
    const auto back = ParseSettings(SerializeSettings(s));
    EXPECT_EQ(back.mode, Mode::Calibration);
    EXPECT_FALSE(back.overlay);

    const auto dir = std::filesystem::temp_directory_path() / "ssa_settings";
    std::filesystem::remove_all(dir);
    ASSERT_TRUE(SaveSettings(dir / "settings.ini", s));
    const auto env = [](const std::string& k) -> std::optional<std::string> {
        if (k == "SS_ADAPTIVE_MODE") {
            return "observe";
        }
        return std::nullopt;
    };
    const auto loaded = LoadSettings(dir / "settings.ini", env);
    EXPECT_EQ(loaded.mode, Mode::Observe);
    EXPECT_FALSE(loaded.overlay);
}
