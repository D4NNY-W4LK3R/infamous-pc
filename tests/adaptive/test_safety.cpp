// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "adaptive/safety.h"

using namespace Adaptive;

namespace {

struct Fixture {
    std::atomic<int> live{8};
    KnobRegistry knobs;
    SafetyGate gate;
    Profile profile;
    Fixture() {
        EXPECT_TRUE(knobs.Register(KnobSpec{"LIVE", Domain::TransferSync, Scope::Live, 8, 1, 64,
                                            {8, 4, 16}, "jobs", ""},
                                   AtomicActuator(live), +1));
        EXPECT_TRUE(knobs.Register(KnobSpec{"BOOT", Domain::TransferSync, Scope::NextLaunch, 128,
                                            4, 4096, {128, 64, 256}, "KB", ""},
                                   std::nullopt));
        EXPECT_TRUE(knobs.Register(
            KnobSpec{"USER", Domain::FramePacing, Scope::Never, 0, 0, 2, {0, 1, 2}, "", ""},
            std::nullopt));
    }
    GateContext Ctx(Mode m) {
        GateContext c;
        c.mode = m;
        c.now_ns = 1'000'000'000'000ull;
        c.in_gameplay = true;
        c.profile = &profile;
        return c;
    }
};

} // namespace

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
    Fixture f;
    std::map<std::string, std::string> env{{"BOOT", "512"}};
    const auto get = [&](const std::string& k) -> std::optional<std::string> {
        const auto it = env.find(k);
        return it == env.end() ? std::nullopt : std::optional{it->second};
    };
    const auto set = [&](const std::string& k, const std::string& v) { env[k] = v; };
    const auto applied = f.knobs.ApplyLaunchValues({{"BOOT", 64}}, get, set);
    EXPECT_TRUE(applied.empty());
    EXPECT_EQ(env["BOOT"], "512");
    EXPECT_TRUE(f.knobs.Find("BOOT")->user_pinned);
    EXPECT_EQ(f.knobs.Find("BOOT")->value, 512);
}

TEST(SafetyGate, ObserveModeNeverChangesAnything) {
    Fixture f;
    const auto d = f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs,
                                   f.Ctx(Mode::Observe));
    EXPECT_EQ(d.verdict, Verdict::Rejected);
    EXPECT_EQ(d.rule, "R6-mode");
}

TEST(SafetyGate, ManualModeOnlyRecommends) {
    Fixture f;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs,
                              f.Ctx(Mode::Manual))
                  .verdict,
              Verdict::Recommended);
}

TEST(SafetyGate, NeverScopeIsUserOnlyEvenInAutomatic) {
    Fixture f;
    const auto d =
        f.gate.Evaluate({"USER", 1, "", 1, Domain::FramePacing}, f.knobs, f.Ctx(Mode::Automatic));
    EXPECT_EQ(d.verdict, Verdict::Recommended);
    EXPECT_EQ(d.rule, "R6-scope-never");
}

TEST(SafetyGate, OwnershipBoundsAndCandidates) {
    Fixture f;
    const auto ctx = f.Ctx(Mode::Automatic);
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::CpuWorkload}, f.knobs, ctx).rule,
              "R2-ownership");
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 99, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R3-bounds");
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 5, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R3-candidates");
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 8, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R3-no-op");
    EXPECT_EQ(f.gate.Evaluate({"NOPE", 8, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R1-registered");
}

TEST(SafetyGate, KnownBadValuesAreBlocked) {
    Fixture f;
    f.profile.rejected["LIVE"][4] = Outcome::Crashed;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs,
                              f.Ctx(Mode::Automatic))
                  .rule,
              "R5-known-bad");
}

TEST(SafetyGate, MemoryAuthorityVetoesOnlyIncreases) {
    Fixture f; // LIVE has vram_direction +1: higher uses more VRAM
    auto ctx = f.Ctx(Mode::Automatic);
    ctx.memory_veto = true;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 16, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R7-memory-authority");
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs, ctx).verdict,
              Verdict::Accepted);
}

TEST(SafetyGate, DefersOutsideSteadyGameplayAndDuringEvaluation) {
    Fixture f;
    auto ctx = f.Ctx(Mode::Automatic);
    ctx.in_gameplay = false;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs, ctx).verdict,
              Verdict::Deferred);
    ctx = f.Ctx(Mode::Automatic);
    ctx.warming_up = true;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R9-warmup");
    ctx = f.Ctx(Mode::Automatic);
    ctx.evaluation_active = true;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R10-one-at-a-time");
}

TEST(SafetyGate, RateLimits) {
    Fixture f;
    auto ctx = f.Ctx(Mode::Automatic);
    f.knobs.Set("LIVE", 4, ctx.now_ns);
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 16, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R11-rate-knob");
    ctx.now_ns += 61'000'000'000ull;
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 16, "", 1, Domain::TransferSync}, f.knobs, ctx).verdict,
              Verdict::Accepted);
    for (int i = 0; i < 6; ++i) {
        f.gate.NoteApplied("LIVE", ctx.now_ns);
    }
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 16, "", 1, Domain::TransferSync}, f.knobs, ctx).rule,
              "R11-rate-global");
}

TEST(SafetyGate, KillSwitchAfterRepeatedEmergencies) {
    Fixture f;
    EXPECT_TRUE(f.gate.IsEmergency(60.0, 80.0, 33.0, 40.0));  // median way up
    EXPECT_TRUE(f.gate.IsEmergency(34.0, 120.0, 33.0, 40.0)); // p99 tripled past 50 ms
    EXPECT_FALSE(f.gate.IsEmergency(34.0, 45.0, 33.0, 40.0));
    f.gate.NoteEmergencyRollback();
    EXPECT_FALSE(f.gate.KillSwitchTripped());
    f.gate.NoteEmergencyRollback();
    EXPECT_TRUE(f.gate.KillSwitchTripped());
    EXPECT_EQ(f.gate.Evaluate({"LIVE", 4, "", 1, Domain::TransferSync}, f.knobs,
                              f.Ctx(Mode::Automatic))
                  .rule,
              "R8-kill-switch");
}
