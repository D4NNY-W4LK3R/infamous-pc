// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end behaviour of the central brain against a simulated emulator: frames are generated
// whose times depend on the knob values the brain sets, on a fake clock.

#include <filesystem>
#include <algorithm>
#include <functional>
#include <map>
#include <random>

#include <gtest/gtest.h>

#include "adaptive/brain.h"

using namespace Adaptive;

namespace {

constexpr std::uint64_t S = 1'000'000'000ull;

std::filesystem::path TempDir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / "ssa_brain_tests" / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

/// The simulated machine: a 4 GB card near its budget, 30 fps.
struct World {
    std::uint64_t now = 10 * S;
    std::uint64_t frame = 0;
    std::atomic<int> tex_headroom{0};
    std::atomic<int> upload_wake{8};
    std::map<std::string, std::string> env;
    std::mt19937 rng{42};
    double vram_fraction = 0.95;
    std::uint32_t over_budget = 0;
    /// frame time (ms) as a function of the live knobs and the launch environment
    std::function<double(World&)> frame_ms = [](World&) { return 33.3; };
    std::function<double(World&)> spike = [](World&) { return 0.0; };
    StallKind dominant = StallKind::Residency;
    double dominant_ms = 8.0;

    HardwareInfo Hw() const {
        HardwareInfo hw;
        hw.cpu_model = "Test CPU";
        hw.cpu_physical_cores = 4;
        hw.cpu_logical_cores = 8;
        hw.ram_total_bytes = 16ull << 30;
        hw.gpu.name = "NVIDIA GeForce GTX 1650";
        hw.gpu.vendor_id = 0x10de;
        hw.gpu.device_id = 0x1f99;
        hw.gpu.driver_version = "576.80.0.0";
        return hw;
    }
    SoftwareInfo Sw() const {
        return SoftwareInfo{"test-emu", "CUSA00223", "01.07", "sig"};
    }
    EnvGet Get() {
        return [this](const std::string& k) -> std::optional<std::string> {
            const auto it = env.find(k);
            return it == env.end() ? std::nullopt : std::optional{it->second};
        };
    }
    EnvSet Set() {
        return [this](const std::string& k, const std::string& v) { env[k] = v; };
    }
    void Seconds(CentralBrain& brain, int seconds) {
        std::normal_distribution<double> noise(0.0, 0.6);
        for (int s = 0; s < seconds; ++s) {
            for (int i = 0; i < 30; ++i) {
                const double ms = std::max(5.0, frame_ms(*this) + noise(rng) + spike(*this));
                now += static_cast<std::uint64_t>(ms * 1e6);
                FrameSample f;
                f.frame_index = frame++;
                f.timestamp_ns = now;
                f.frame_ms = static_cast<float>(ms);
                f.stall_ms[static_cast<std::size_t>(dominant)] = static_cast<float>(dominant_ms);
                brain.PublishFrame(f);
            }
            brain.Tick();
        }
    }
};

struct Rig {
    World& w;
    BrainConfig cfg;
    std::unique_ptr<CentralBrain> brain;
    Rig(World& world, const std::filesystem::path& dir, Mode mode) : w{world} {
        cfg.mode = mode;
        cfg.data_dir = dir;
        cfg.warmup_min_seconds = 20;
        brain = std::make_unique<CentralBrain>(cfg, [&world] { return world.now; });
        EmulatorDefaults d;
        brain->SetResourceSampler([&world] {
            ResourceSample r;
            r.vram_budget_bytes = 3500ull << 20;
            r.vram_used_bytes = static_cast<std::uint64_t>(world.vram_fraction * (3500ull << 20));
            r.over_budget_allocations = world.over_budget;
            return r;
        });
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeTextureCacheBrain(d, &brain->Memory()),
                                             {{KnobId::TextureGcHeadroomMb,
                                               AtomicActuator(world.tex_headroom)}},
                                             {{KnobId::TextureGcHeadroomMb, -1}}));
        EXPECT_TRUE(brain->RegisterMiniBrain(
            MakeTransferSyncBrain(d), {{KnobId::UploadWakeJobs, AtomicActuator(world.upload_wake)}}));
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeBufferGeometryBrain(d)));
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeShaderPipelineBrain(d)));
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeFramePacingBrain(d)));
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeCpuWorkloadBrain(d)));
    }
    StartupResult Start() {
        return brain->Startup(w.Hw(), w.Sw(), w.Get(), w.Set());
    }
};

Knowledge LoadK(const std::filesystem::path& dir) {
    Knowledge k;
    KnowledgeStore(dir / "knowledge.txt").Load(k);
    return k;
}

const Profile& OnlyProfile(const Knowledge& k) {
    EXPECT_EQ(k.profiles.size(), 1u);
    return k.profiles.begin()->second;
}

/// A copy: the Knowledge it came from is a temporary.
Profile LoadProfile(const std::filesystem::path& dir) {
    const Knowledge k = LoadK(dir);
    if (k.profiles.size() != 1) {
        ADD_FAILURE() << "expected exactly one profile, found " << k.profiles.size();
        return {};
    }
    return k.profiles.begin()->second;
}

} // namespace

TEST(Brain, ObserveModeChangesNothing) {
    World w;
    const auto dir = TempDir("observe");
    Rig rig(w, dir, Mode::Observe);
    rig.Start();
    w.Seconds(*rig.brain, 300);
    EXPECT_EQ(w.tex_headroom.load(), 0);
    EXPECT_EQ(w.upload_wake.load(), 8);
    EXPECT_TRUE(w.env.empty());
    rig.brain->Shutdown();
}

TEST(Brain, ManualModeRecommendsButDoesNotApply) {
    World w;
    const auto dir = TempDir("manual");
    Rig rig(w, dir, Mode::Manual);
    rig.Start();
    w.Seconds(*rig.brain, 120);
    EXPECT_EQ(w.tex_headroom.load(), 0);
    const auto st = rig.brain->Status();
    const bool recommended =
        std::any_of(st.recommendations.begin(), st.recommendations.end(),
                    [](const auto& r) { return r.knob == KnobId::TextureGcHeadroomMb; });
    EXPECT_TRUE(recommended);
    EXPECT_EQ(st.snapshot.bottleneck, Bottleneck::MemoryPressure);
}

TEST(Brain, AutomaticKeepsAMeasuredImprovementAndReusesItNextLaunch) {
    World w;
    w.frame_ms = [](World& x) { return x.tex_headroom.load() >= 256 ? 29.0 : 33.3; };
    const auto dir = TempDir("improve");
    {
        Rig rig(w, dir, Mode::Automatic);
        rig.Start();
        w.Seconds(*rig.brain, 400);
        EXPECT_EQ(w.tex_headroom.load(), 256);
        rig.brain->Shutdown();
        EXPECT_EQ(w.tex_headroom.load(), 0) << "live knobs go back to defaults at shutdown";
    }
    const Knowledge k = LoadK(dir);
    const Profile& p = OnlyProfile(k);
    EXPECT_EQ(p.validated.at(KnobId::TextureGcHeadroomMb), 256);
    ASSERT_FALSE(p.history.empty());
    const auto& e = p.history.front();
    EXPECT_EQ(e.outcome, Outcome::Improved);
    EXPECT_EQ(e.method, "live-abab");
    EXPECT_LT(e.median_after_ms, e.median_before_ms);
    EXPECT_FALSE(k.session_open);
    EXPECT_TRUE(std::filesystem::exists(dir / "latest-report.txt"));

    // Next launch: the learned value is applied once gameplay is steady, with no new A/B.
    World w2;
    w2.frame_ms = w.frame_ms;
    Rig rig2(w2, dir, Mode::Automatic);
    rig2.Start();
    w2.Seconds(*rig2.brain, 40);
    EXPECT_EQ(w2.tex_headroom.load(), 256);
    w2.Seconds(*rig2.brain, 80);
    rig2.brain->Shutdown();
    const Profile p2 = LoadProfile(dir);
    EXPECT_EQ(p2.validated.at(KnobId::TextureGcHeadroomMb), 256);
    EXPECT_EQ(p2.history.back().method, "live-confirm");
}

TEST(Brain, RegressionIsRevertedAndRemembered) {
    World w;
    w.frame_ms = [](World& x) { return x.tex_headroom.load() >= 256 ? 37.5 : 33.3; };
    const auto dir = TempDir("regress");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 220);
    EXPECT_EQ(w.tex_headroom.load(), 0);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    EXPECT_FALSE(p.validated.contains(KnobId::TextureGcHeadroomMb));
    EXPECT_EQ(p.rejected.at(KnobId::TextureGcHeadroomMb).at(256), Outcome::Regressed);
}

TEST(Brain, NeutralChangeIsNotKept) {
    World w; // frame time does not depend on anything
    const auto dir = TempDir("neutral");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 220);
    EXPECT_EQ(w.tex_headroom.load(), 0);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    ASSERT_FALSE(p.history.empty());
    EXPECT_EQ(p.history.front().outcome, Outcome::Neutral);
    EXPECT_FALSE(p.validated.contains(KnobId::TextureGcHeadroomMb));
}

TEST(Brain, EmergencyRollbackIsImmediateAndTripsTheKillSwitch) {
    World w;
    w.spike = [](World& x) { return x.tex_headroom.load() >= 256 ? 40.0 : 0.0; };
    const auto dir = TempDir("emergency");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 25); // warm up and fill the window
    // Run until the change starts, then count how long the bad value survives.
    int bad_seconds = 0;
    for (int i = 0; i < 400; ++i) {
        w.Seconds(*rig.brain, 1);
        bad_seconds += w.tex_headroom.load() >= 256 ? 1 : 0;
    }
    EXPECT_LE(bad_seconds, 2 * 4) << "each bad attempt must be undone within seconds";
    const auto st = rig.brain->Status();
    EXPECT_TRUE(st.kill_switch);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.rejected.at(KnobId::TextureGcHeadroomMb).at(256), Outcome::Regressed);
}

TEST(Brain, CalibrationLearnsAStartupValueAcrossLaunches) {
    const auto dir = TempDir("calibration");
    // The simulated emulator reads SHAD_PREFETCH_BACKOFF_MAX at startup; 2 is faster here.
    auto model = [](World& x) {
        const auto it = x.env.find(KnobId::PrefetchBackoffMax);
        return it != x.env.end() && it->second == "2" ? 30.5 : 33.3;
    };
    std::string tested;
    for (int launch = 0; launch < 8; ++launch) {
        World w;
        w.frame_ms = model;
        w.vram_fraction = 0.5;
        w.dominant = StallKind::GuestWait; // nothing for the live brains to do
        w.dominant_ms = 15.0;
        Rig rig(w, dir, Mode::Calibration);
        const auto r = rig.Start();
        if (r.experiment) {
            tested += r.experiment->knob + "=" + std::to_string(r.experiment->value) + ";";
        }
        w.Seconds(*rig.brain, 130); // 20 s warm-up + 90 idle seconds + margin
        rig.brain->Shutdown();
        const Profile p = LoadProfile(dir);
        if (p.validated.contains(KnobId::PrefetchBackoffMax)) {
            EXPECT_EQ(p.validated.at(KnobId::PrefetchBackoffMax), 2);
            // The next launch uses it from the start.
            World w2;
            Rig rig2(w2, dir, Mode::Automatic);
            const auto r2 = rig2.Start();
            EXPECT_EQ(r2.launch_values.at(KnobId::PrefetchBackoffMax), 2);
            EXPECT_EQ(w2.env.at(KnobId::PrefetchBackoffMax), "2");
            rig2.brain->Shutdown();
            SUCCEED() << "tested: " << tested;
            return;
        }
    }
    FAIL() << "calibration never validated the faster value; tested: " << tested;
}

TEST(Brain, CrashDuringExperimentBlocksThatValueForGood) {
    const auto dir = TempDir("crash");
    // Launch 1: baseline.
    {
        World w;
        w.vram_fraction = 0.5;
        w.dominant = StallKind::GuestWait;
        w.dominant_ms = 15.0;
        Rig rig(w, dir, Mode::Calibration);
        rig.Start();
        w.Seconds(*rig.brain, 130);
        rig.brain->Shutdown();
    }
    // Launch 2: an experiment, and the "game" dies: Shutdown never runs, so nothing after the
    // startup save reaches the disk (the destructor only stops the thread, as a crash would).
    PendingExperiment crashed;
    {
        World w;
        Rig rig(w, dir, Mode::Calibration);
        const auto r = rig.Start();
        ASSERT_TRUE(r.experiment.has_value());
        crashed = *r.experiment;
        w.Seconds(*rig.brain, 30);
    }
    // Launch 3 sees the open session and blames the experiment.
    World w;
    Rig rig(w, dir, Mode::Calibration);
    const auto r = rig.Start();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.rejected.at(crashed.knob).at(crashed.value), Outcome::Crashed);
    EXPECT_EQ(p.unclean_sessions, 1u);
    if (r.experiment) {
        EXPECT_FALSE(r.experiment->knob == crashed.knob && r.experiment->value == crashed.value);
    }
    rig.brain->Shutdown();
}

TEST(Brain, DriverChangeStopsUsingLearnedValuesUntilReMeasured) {
    const auto dir = TempDir("driver");
    {
        Knowledge k;
        World w;
        std::vector<std::string> notes;
        Profile& p = SelectProfile(k, MakeFingerprint(w.Hw(), w.Sw()), notes);
        p.validated[KnobId::PrefetchBackoffMax] = 2;
        ASSERT_TRUE(KnowledgeStore(dir / "knowledge.txt").Save(k));
    }
    World w;
    {
        Rig rig(w, dir, Mode::Automatic);
        EXPECT_EQ(rig.Start().launch_values.at(KnobId::PrefetchBackoffMax), 2);
        rig.brain->Shutdown();
    }
    World w2;
    HardwareInfo hw = w2.Hw();
    hw.gpu.driver_version = "581.15.0.0";
    Rig rig(w2, dir, Mode::Automatic);
    const auto r = rig.brain->Startup(hw, w2.Sw(), w2.Get(), w2.Set());
    EXPECT_TRUE(r.launch_values.empty());
    EXPECT_FALSE(w2.env.contains(KnobId::PrefetchBackoffMax));
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.revalidate.at(KnobId::PrefetchBackoffMax), 2);
}

TEST(Brain, UserEnvironmentAlwaysWins) {
    const auto dir = TempDir("pinned");
    {
        Knowledge k;
        World w;
        std::vector<std::string> notes;
        SelectProfile(k, MakeFingerprint(w.Hw(), w.Sw()), notes).validated[KnobId::ReadbackWindowKb] =
            64;
        ASSERT_TRUE(KnowledgeStore(dir / "knowledge.txt").Save(k));
    }
    World w;
    w.env[KnobId::ReadbackWindowKb] = "512";
    Rig rig(w, dir, Mode::Automatic);
    const auto r = rig.Start();
    EXPECT_FALSE(r.launch_values.contains(KnobId::ReadbackWindowKb));
    EXPECT_EQ(w.env.at(KnobId::ReadbackWindowKb), "512");
    rig.brain->Shutdown();
}

TEST(Brain, ContractVersionMismatchIsRefused) {
    struct OldBrain : MiniBrain {
        Domain GetDomain() const override {
            return Domain::CpuWorkload;
        }
        std::string_view Name() const override {
            return "old";
        }
        std::uint32_t ContractMajor() const override {
            return 0;
        }
        std::vector<KnobSpec> Knobs() const override {
            return {};
        }
        std::vector<Proposal> Think(const Snapshot&, const std::vector<std::int64_t>&) override {
            return {};
        }
        std::string Status() const override {
            return {};
        }
    };
    BrainConfig cfg;
    cfg.data_dir = TempDir("contract");
    CentralBrain brain(cfg);
    EXPECT_FALSE(brain.RegisterMiniBrain(std::make_unique<OldBrain>()));
    EXPECT_TRUE(brain.RegisterMiniBrain(MakeCpuWorkloadBrain({})));
    EXPECT_FALSE(brain.RegisterMiniBrain(MakeCpuWorkloadBrain({}))) << "one brain per domain";
}

TEST(Brain, ABrainCannotTouchAnotherDomainsKnob) {
    struct Rogue : MiniBrain {
        Domain GetDomain() const override {
            return Domain::CpuWorkload;
        }
        std::string_view Name() const override {
            return "rogue";
        }
        std::vector<KnobSpec> Knobs() const override {
            return {};
        }
        std::vector<Proposal> Think(const Snapshot&, const std::vector<std::int64_t>&) override {
            // Claims to be the texture cache.
            return {Proposal{KnobId::TextureGcHeadroomMb, 512, "", 9.0, Domain::TextureCache}};
        }
        std::string Status() const override {
            return {};
        }
    };
    World w;
    w.vram_fraction = 0.5; // the real texture brain stays quiet
    const auto dir = TempDir("rogue");
    BrainConfig cfg;
    cfg.mode = Mode::Automatic;
    cfg.data_dir = dir;
    cfg.warmup_min_seconds = 5;
    CentralBrain brain(cfg, [&w] { return w.now; });
    ASSERT_TRUE(brain.RegisterMiniBrain(MakeTextureCacheBrain({}, &brain.Memory()),
                                        {{KnobId::TextureGcHeadroomMb, AtomicActuator(w.tex_headroom)}}));
    ASSERT_TRUE(brain.RegisterMiniBrain(std::make_unique<Rogue>()));
    brain.Startup(w.Hw(), w.Sw(), w.Get(), w.Set());
    w.Seconds(brain, 60);
    EXPECT_EQ(w.tex_headroom.load(), 0);
    const auto st = brain.Status();
    const bool logged = std::any_of(st.recent.begin(), st.recent.end(), [](const auto& e) {
        return e.decision.rule == "R2-ownership";
    });
    EXPECT_TRUE(logged);
    brain.Shutdown();
}

TEST(Brain, ThreadedRunIsCheapAndStopsCleanly) {
    World w;
    const auto dir = TempDir("thread");
    BrainConfig cfg;
    cfg.mode = Mode::Automatic;
    cfg.data_dir = dir;
    cfg.tick_ms = 250;
    CentralBrain brain(cfg);
    brain.RegisterMiniBrain(MakeCpuWorkloadBrain({}));
    brain.Startup(w.Hw(), w.Sw(), w.Get(), w.Set());
    brain.Start();
    std::thread producer([&] {
        for (int i = 0; i < 3000; ++i) {
            FrameSample f;
            f.frame_index = static_cast<std::uint64_t>(i);
            f.timestamp_ns = brain.Now();
            f.frame_ms = 16.6f;
            brain.PublishFrame(f);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    producer.join();
    const auto path = brain.Shutdown();
    EXPECT_FALSE(path.empty());
    const auto st = brain.Status();
    EXPECT_GE(st.ticks, 2u);
    EXPECT_LT(st.brain_ms_per_tick, 50.0);
}

TEST(Brain, ForgetProfileClearsLearnedData) {
    World w;
    w.frame_ms = [](World& x) { return x.tex_headroom.load() >= 256 ? 29.0 : 33.3; };
    const auto dir = TempDir("forget");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 400);
    ASSERT_EQ(w.tex_headroom.load(), 256);
    rig.brain->RequestForgetProfile();
    w.Seconds(*rig.brain, 2);
    EXPECT_EQ(w.tex_headroom.load(), 0);
    rig.brain->Shutdown();
    EXPECT_TRUE(LoadProfile(dir).validated.empty());
}

TEST(Brain, DiagnosisPicksTheDominantCost) {
    Snapshot s;
    s.window_frames = 300;
    s.in_gameplay = true;
    s.frame_ms_p50 = 33.0;
    double c = 0;
    EXPECT_EQ(CentralBrain::Diagnose(s, 0.5, false, c), Bottleneck::Unknown);
    s.stall_ms_per_frame[static_cast<std::size_t>(StallKind::GuestWait)] = 15.0;
    EXPECT_EQ(CentralBrain::Diagnose(s, 0.5, false, c), Bottleneck::GuestBound);
    EXPECT_EQ(CentralBrain::Diagnose(s, 0.95, false, c), Bottleneck::MemoryPressure);
    s.stall_ms_per_frame[static_cast<std::size_t>(StallKind::PipelineWait)] = 8.0;
    EXPECT_EQ(CentralBrain::Diagnose(s, 0.5, true, c), Bottleneck::Warmup);
    EXPECT_EQ(CentralBrain::Diagnose(s, 0.5, false, c), Bottleneck::ShaderCompile);
}
