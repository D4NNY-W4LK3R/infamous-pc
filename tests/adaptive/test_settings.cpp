// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>

#include <gtest/gtest.h>

#include "adaptive/settings.h"

using namespace Adaptive;

TEST(Settings, DefaultsAreAutomaticAndEnabled) {
    const auto s = ParseSettings("");
    EXPECT_TRUE(s.enabled);
    EXPECT_TRUE(s.controls_hint);
    EXPECT_EQ(s.mode, Mode::Automatic);
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
    s.controls_hint = false;
    const auto back = ParseSettings(SerializeSettings(s));
    EXPECT_FALSE(back.controls_hint);
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

TEST(Settings, FrameLimitParsesRoundTripsAndRejectsNonsense) {
    EXPECT_EQ(ParseSettings("").frame_limit, 0u);
    EXPECT_EQ(ParseSettings("frame_limit = 30\n").frame_limit, 30u);
    const auto bad = ParseSettings("frame_limit = 5\n");
    EXPECT_EQ(bad.frame_limit, 0u);
    EXPECT_EQ(bad.warnings.size(), 1u);
    UserSettings s;
    s.frame_limit = 45;
    EXPECT_EQ(ParseSettings(SerializeSettings(s)).frame_limit, 45u);
    const auto env = [](const std::string& k) -> std::optional<std::string> {
        return k == "SS_FRAME_LIMIT" ? std::optional<std::string>{"30"} : std::nullopt;
    };
    EXPECT_EQ(LoadSettings("/nonexistent/settings.ini", env).frame_limit, 30u);
}

TEST(Settings, GamePatchOptions) {
    const auto d = ParseSettings("");
    EXPECT_EQ(d.perf_mode, 0u);
    EXPECT_TRUE(d.particle_late_read);
    EXPECT_FALSE(d.particle_light_one_submit);
    const auto s = ParseSettings("perf_mode = Office\nparticle_late_read = 0\n"
                                 "particle_light_one_submit = 1\nperf_mode_typo = 1\n");
    EXPECT_EQ(s.perf_mode, 2u);
    EXPECT_FALSE(s.particle_late_read);
    EXPECT_TRUE(s.particle_light_one_submit);
    EXPECT_EQ(s.warnings.size(), 1u);
    EXPECT_EQ(ParseSettings("perf_mode = ultra\n").perf_mode, 0u);
    UserSettings r;
    r.perf_mode = 1;
    r.particle_late_read = false;
    const auto back = ParseSettings(SerializeSettings(r));
    EXPECT_EQ(back.perf_mode, 1u);
    EXPECT_FALSE(back.particle_late_read);
    EXPECT_TRUE(back.warnings.empty());
}

TEST(Settings, FrameRateTargetOptions) {
    const auto s = ParseSettings("fps_target = 30\nauto_perf_max = office\n");
    EXPECT_EQ(s.fps_target, 30u);
    EXPECT_EQ(s.auto_perf_max, 2u);
    EXPECT_TRUE(s.warnings.empty());
    EXPECT_EQ(ParseSettings("fps_target = 3\n").warnings.size(), 1u);
    UserSettings r;
    r.fps_target = 45;
    r.auto_perf_max = 1;
    const auto back = ParseSettings(SerializeSettings(r));
    EXPECT_EQ(back.fps_target, 45u);
    EXPECT_EQ(back.auto_perf_max, 1u);
}
