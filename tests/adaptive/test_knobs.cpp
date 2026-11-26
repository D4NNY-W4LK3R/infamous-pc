// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <map>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "adaptive/knobs.h"

using namespace Adaptive;

TEST(KnobRegistry, RejectsInconsistentSpecs) {
    KnobRegistry r;
    std::atomic<int> a{0};
    EXPECT_FALSE(r.Register(KnobSpec{"X", Domain::CpuWorkload, Scope::Live, 5, 0, 4, {5}, "", ""},
                            AtomicActuator(a)));                       // default out of bounds
    EXPECT_FALSE(r.Register(KnobSpec{"X", Domain::CpuWorkload, Scope::Live, 1, 0, 4, {2, 3}, "", ""},
                            AtomicActuator(a)));                       // default not a candidate
    EXPECT_FALSE(r.Register(KnobSpec{"X", Domain::CpuWorkload, Scope::Live, 1, 0, 4, {1}, "", ""},
                            std::nullopt));                            // live without actuator
    EXPECT_FALSE(r.Register(KnobSpec{"X", Domain::CpuWorkload, Scope::NextLaunch, 1, 0, 4, {1, 9},
                                     "", ""},
                            std::nullopt));                            // candidate out of bounds
    EXPECT_TRUE(r.Register(KnobSpec{"X", Domain::CpuWorkload, Scope::Live, 1, 0, 4, {1, 2}, "", ""},
                           AtomicActuator(a)));
    EXPECT_FALSE(r.Register(KnobSpec{"X", Domain::CpuWorkload, Scope::Live, 1, 0, 4, {1}, "", ""},
                            AtomicActuator(a))); // duplicate
}

TEST(KnobRegistry, LaunchValuesRespectUserEnvironment) {
    KnobRegistry knobs;
    ASSERT_TRUE(knobs.Register(KnobSpec{"BOOT", Domain::TransferSync, Scope::NextLaunch, 128, 4,
                                        4096, {128, 64, 256}, "KB", ""},
                               std::nullopt));
    std::map<std::string, std::string> env{{"BOOT", "512"}};
    const auto get = [&](const std::string& k) -> std::optional<std::string> {
        const auto it = env.find(k);
        return it == env.end() ? std::nullopt : std::optional{it->second};
    };
    const auto set = [&](const std::string& k, const std::string& v) { env[k] = v; };
    const auto applied = knobs.ApplyLaunchValues({{"BOOT", 64}}, get, set);
    EXPECT_TRUE(applied.empty());
    EXPECT_EQ(env["BOOT"], "512");
    EXPECT_TRUE(knobs.Find("BOOT")->user_pinned);
    EXPECT_EQ(knobs.Find("BOOT")->value, 512);
}
