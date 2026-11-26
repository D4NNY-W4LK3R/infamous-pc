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
    EXPECT_FALSE(d.particle_late_read); // off by default since build 25
    EXPECT_FALSE(d.particle_light_one_submit);
    EXPECT_TRUE(ParseSettings("particle_late_read = 1\n").particle_late_read);
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

TEST(Settings, VramPrioritiesOffByDefaultRoundTripAndEnvironment) {
    EXPECT_FALSE(ParseSettings("").vram_priorities);
    const auto on = ParseSettings("vram_priorities = 1\n");
    EXPECT_TRUE(on.vram_priorities);
    EXPECT_TRUE(on.warnings.empty());
    EXPECT_TRUE(ParseSettings(SerializeSettings(on)).vram_priorities);
    EXPECT_FALSE(ParseSettings("vram_priorities = maybe\n").warnings.empty());
    const auto dir = std::filesystem::temp_directory_path() / "ssa_vram_priorities";
    std::filesystem::create_directories(dir);
    const auto file = dir / "settings.ini";
    ASSERT_TRUE(SaveSettings(file, UserSettings{}));
    const auto loaded = LoadSettings(file, [](const std::string& k) -> std::optional<std::string> {
        return k == "SS_VRAM_PRIORITIES" ? std::optional<std::string>{"1"} : std::nullopt;
    });
    EXPECT_TRUE(loaded.vram_priorities);
}

TEST(Settings, EmptyMemoryOutOnByDefaultAndOldKeyIgnored) {
    EXPECT_TRUE(ParseSettings("").lazy_zero_memory);
    // Build 44-50 wrote lazy_zero_memory while the switch was off by default: build 51 turns it
    // on once.
    const auto old = ParseSettings("lazy_zero_memory = 0\n");
    EXPECT_TRUE(old.lazy_zero_memory);
    EXPECT_TRUE(old.warnings.empty());
    UserSettings off;
    off.lazy_zero_memory = false;
    EXPECT_FALSE(ParseSettings(SerializeSettings(off)).lazy_zero_memory);
}

TEST(Settings, Build51ExperimentsOffByDefaultAndRoundTrip) {
    const auto d = ParseSettings("");
    EXPECT_FALSE(d.early_copy_back);
    EXPECT_FALSE(d.keep_rewritten_pages);
    UserSettings on;
    on.early_copy_back = true;
    on.keep_rewritten_pages = true;
    const auto back = ParseSettings(SerializeSettings(on));
    EXPECT_TRUE(back.early_copy_back);
    EXPECT_TRUE(back.keep_rewritten_pages);
    EXPECT_TRUE(back.warnings.empty());
}
