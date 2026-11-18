// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "adaptive/knobs.h"
#include "adaptive/mini_brains.h"

using namespace Adaptive;

namespace {

Snapshot Steady(Bottleneck b) {
    Snapshot s;
    s.window_frames = 300;
    s.in_gameplay = true;
    s.frame_ms_p50 = 33.0;
    s.frame_ms_p99 = 40.0;
    s.bottleneck = b;
    return s;
}

void SetStall(Snapshot& s, StallKind k, double v) {
    s.stall_ms_per_frame[static_cast<std::size_t>(k)] = v;
}

std::vector<std::int64_t> Defaults(const MiniBrain& b) {
    std::vector<std::int64_t> v;
    for (const auto& k : b.Knobs()) {
        v.push_back(k.default_value);
    }
    return v;
}

} // namespace

TEST(MiniBrains, EverySpecIsRegistrableAndOwnedByItsBrain) {
    EmulatorDefaults d;
    GpuMemoryBrain mem;
    std::vector<std::unique_ptr<MiniBrain>> all;
    all.push_back(MakeBufferGeometryBrain(d));
    all.push_back(MakeTextureCacheBrain(d, &mem));
    all.push_back(MakeTransferSyncBrain(d));
    all.push_back(MakeShaderPipelineBrain(d));
    all.push_back(MakeFramePacingBrain(d));
    all.push_back(MakeCpuWorkloadBrain(d));
    KnobRegistry reg;
    std::atomic<int> dummy{0};
    std::size_t knobs = 0;
    for (const auto& b : all) {
        for (const auto& k : b->Knobs()) {
            EXPECT_EQ(k.domain, b->GetDomain()) << k.id;
            std::optional<Actuator> act;
            if (k.scope == Scope::Live) {
                dummy = static_cast<int>(k.default_value);
                act = AtomicActuator(dummy);
            }
            EXPECT_TRUE(reg.Register(k, act)) << k.id;
            ++knobs;
        }
    }
    EXPECT_GE(knobs, 9u);
}

TEST(MiniBrains, AsyncComputeIsNeverAutomatic) {
    const auto b = MakeTransferSyncBrain({});
    for (const auto& k : b->Knobs()) {
        if (k.id == KnobId::AsyncCompute) {
            EXPECT_EQ(k.scope, Scope::Never);
            return;
        }
    }
    FAIL() << "async compute knob missing";
}

TEST(MiniBrains, TextureCacheReactsToPressureOnly) {
    GpuMemoryBrain mem;
    const auto tex = MakeTextureCacheBrain({}, &mem);
    Snapshot s = Steady(Bottleneck::MemoryPressure);
    s.resources.vram_budget_bytes = 1000;
    s.resources.vram_used_bytes = 960;
    mem.Think(s, {});
    EXPECT_TRUE(mem.Veto());
    auto p = tex->Think(s, Defaults(*tex));
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].value, 256);
    s.resources.vram_used_bytes = 600;
    mem.Think(s, {});
    EXPECT_FALSE(mem.Veto());
    EXPECT_TRUE(tex->Think(s, Defaults(*tex)).empty());
}

TEST(MiniBrains, OverBudgetAllocationsCountAsPressure) {
    GpuMemoryBrain mem;
    Snapshot s = Steady(Bottleneck::Unknown);
    s.resources.vram_budget_bytes = 1000;
    s.resources.vram_used_bytes = 700;
    s.resources.over_budget_allocations = 3;
    mem.Think(s, {});
    EXPECT_FALSE(mem.OverBudgetGrowing());
    s.resources.over_budget_allocations = 5;
    mem.Think(s, {});
    EXPECT_TRUE(mem.OverBudgetGrowing());
    EXPECT_TRUE(mem.Veto());
}

TEST(MiniBrains, ShaderBrainWantsMoreCompilersOnlyWhenFramesWaitForThem) {
    EmulatorDefaults d;
    d.pipeline_workers = 4;
    d.pipeline_workers_max = 6;
    const auto b = MakeShaderPipelineBrain(d);
    Snapshot s = Steady(Bottleneck::ShaderCompile);
    SetStall(s, StallKind::PipelineWait, 6.0);
    auto p = b->Think(s, Defaults(*b));
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].value, 6);
    s = Steady(Bottleneck::HostGpuBound);
    EXPECT_TRUE(b->Think(s, Defaults(*b)).empty());
}

TEST(MiniBrains, FramePacingOnlyRecommendsUserSettings) {
    const auto b = MakeFramePacingBrain({});
    for (const auto& k : b->Knobs()) {
        EXPECT_EQ(k.scope, Scope::Never);
    }
    Snapshot s = Steady(Bottleneck::GuestBound);
    s.frame_ms_p99 = 80.0;
    const auto p = b->Think(s, Defaults(*b));
    ASSERT_EQ(p.size(), 1u);
    EXPECT_EQ(p[0].value, 1);
}

TEST(MiniBrains, QuietWhenNotInGameplay) {
    GpuMemoryBrain mem;
    std::vector<std::unique_ptr<MiniBrain>> all;
    all.push_back(MakeBufferGeometryBrain({}));
    all.push_back(MakeTextureCacheBrain({}, &mem));
    all.push_back(MakeTransferSyncBrain({}));
    all.push_back(MakeFramePacingBrain({}));
    Snapshot s = Steady(Bottleneck::TransferBound);
    s.in_gameplay = false;
    s.resources.vram_budget_bytes = 1000;
    s.resources.vram_used_bytes = 990;
    mem.Think(s, {});
    for (auto& b : all) {
        EXPECT_TRUE(b->Think(s, Defaults(*b)).empty()) << b->Name();
    }
}
