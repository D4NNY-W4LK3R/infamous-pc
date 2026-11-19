// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end behaviour of the central brain against a simulated emulator: frames are generated
// whose times depend on the knob values the brain sets, on a fake clock.

#include <filesystem>
#include <fstream>
#include <sstream>
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
    /// extra per-frame changes (e.g. a shader-compile stall); returns extra frame time in ms
    std::function<double(World&, FrameSample&)> decorate = [](World&, FrameSample&) { return 0.0; };
    /// Draw count per frame (0 = not reported, as in the older tests) and whether the player is
    /// pressing keys this second: together they make the scene (gameplay, cutscene, menu).
    std::function<std::uint32_t(World&)> draws = [](World&) { return 0u; };
    bool input = false;

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
            if (input) {
                brain.NoteInput();
            }
            for (int i = 0; i < 30; ++i) {
                FrameSample f;
                f.draws = draws(*this);
                f.dispatches = f.draws / 4;
                f.stall_ms[static_cast<std::size_t>(dominant)] = static_cast<float>(dominant_ms);
                const double extra = decorate(*this, f);
                const double ms =
                    std::max(5.0, frame_ms(*this) + noise(rng) + spike(*this) + extra);
                now += static_cast<std::uint64_t>(ms * 1e6);
                f.frame_index = frame++;
                f.timestamp_ns = now;
                f.frame_ms = static_cast<float>(ms);
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

TEST(Brain, OneHitchDuringATestIsNotAnEmergency) {
    // Field case (build 14 log): the new value was faster (29.1 -> 27.5 ms medians) but one second
    // held a 130 ms scene-streaming hitch, and v1 rolled it back and blocked it.
    World w;
    w.frame_ms = [](World& x) { return x.tex_headroom.load() >= 256 ? 30.5 : 33.3; };
    std::uint64_t first_new_frame = 0;
    w.spike = [&first_new_frame](World& x) {
        if (x.tex_headroom.load() < 256) {
            return 0.0;
        }
        if (first_new_frame == 0) {
            first_new_frame = x.frame;
        }
        // One second, 5 s into the first new-value block: a few frames near 130 ms.
        const std::uint64_t f = x.frame - first_new_frame;
        return f >= 150 && f < 153 ? 100.0 : 0.0;
    };
    const auto dir = TempDir("one-hitch");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 260);
    EXPECT_FALSE(rig.brain->Status().kill_switch);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    ASSERT_NE(first_new_frame, 0u) << "the test never started";
    EXPECT_FALSE(p.rejected.contains(KnobId::TextureGcHeadroomMb));
    EXPECT_EQ(p.validated.at(KnobId::TextureGcHeadroomMb), 256);
}

TEST(Brain, ShaderCompileStallsDuringATestAreLeftOut) {
    // Field case (build 16, fresh shader cache): compile stalls of 0.3-0.9 s during the test of
    // a value measuring 6% faster rolled it back twice and blocked it.
    World w;
    w.frame_ms = [](World& x) { return x.tex_headroom.load() >= 256 ? 30.5 : 33.3; };
    std::uint64_t first_new_frame = 0;
    w.decorate = [&first_new_frame](World& x, FrameSample& f) {
        if (x.tex_headroom.load() >= 256 && first_new_frame == 0) {
            first_new_frame = x.frame;
        }
        if (first_new_frame == 0) {
            return 0.0;
        }
        // Two bursts of 60 ms frames that wait 40 ms for pipelines, a few seconds each.
        const std::uint64_t n = x.frame - first_new_frame;
        if ((n >= 90 && n < 210) || (n >= 900 && n < 1020)) {
            f.stall_ms[static_cast<std::size_t>(StallKind::PipelineWait)] = 40.0f;
            return 27.0;
        }
        return 0.0;
    };
    const auto dir = TempDir("compile-stalls");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 300);
    EXPECT_FALSE(rig.brain->Status().kill_switch);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    ASSERT_NE(first_new_frame, 0u) << "the test never started";
    EXPECT_FALSE(p.rejected.contains(KnobId::TextureGcHeadroomMb));
    EXPECT_EQ(p.validated.at(KnobId::TextureGcHeadroomMb), 256);
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

TEST(Brain, AnAssertionCrashCountsLikeAVanishedProcess) {
    // The emulator's assertion handler shuts down in order (the report is written) but tells the
    // brain it is a crash: the next launch must still blame the experiment.
    const auto dir = TempDir("assert-crash");
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
    PendingExperiment crashed;
    {
        World w;
        Rig rig(w, dir, Mode::Calibration);
        const auto r = rig.Start();
        ASSERT_TRUE(r.experiment.has_value());
        crashed = *r.experiment;
        w.Seconds(*rig.brain, 30);
        const auto report = rig.brain->Shutdown(true);
        EXPECT_TRUE(std::filesystem::exists(report));
    }
    World w;
    Rig rig(w, dir, Mode::Calibration);
    rig.Start();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.rejected.at(crashed.knob).at(crashed.value), Outcome::Crashed);
    EXPECT_EQ(p.unclean_sessions, 1u);
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

TEST(Brain, ABusyGraphicsCardIsNamedEvenWithVramOverBudget) {
    // Field (GTX 1650): VRAM at 140% of budget and the card busy 31 of 33 ms a frame.
    Snapshot s;
    s.window_frames = 300;
    s.in_gameplay = true;
    s.frame_ms_p50 = 33.0;
    s.stall_ms_per_frame[static_cast<std::size_t>(StallKind::ReadbackWait)] = 30.0;
    s.gpu_busy_ms_p50 = 31.0;
    double c = 0;
    EXPECT_EQ(CentralBrain::Diagnose(s, 1.4, false, c), Bottleneck::HostGpuBound);
    s.gpu_busy_ms_p50 = 18.0; // a 30 fps limit with room to spare: not the card
    EXPECT_EQ(CentralBrain::Diagnose(s, 1.4, false, c), Bottleneck::MemoryPressure);
}


namespace {
SecondSummary SecondWith(std::uint32_t draws, bool still) {
    SecondSummary s;
    s.frames = 30;
    s.draws_median = draws;
    s.dispatches_median = draws / 4;
    s.same_every_frame = still;
    s.signature = still ? (1ull << 63) | draws : 0;
    return s;
}
/// A moving scene: the draw count changes a little from frame to frame.
std::uint32_t Moving(World& w) {
    return 3000u + static_cast<std::uint32_t>(w.frame % 7);
}
} // namespace

TEST(Brain, ScenesAreClassifiedFromDrawsInputAndStillness) {
    EXPECT_EQ(CentralBrain::ClassifySecond(SecondWith(0, false), true, 0), Scene::Unknown);
    EXPECT_EQ(CentralBrain::ClassifySecond(SecondWith(12, false), false, 0), Scene::Loading);
    EXPECT_EQ(CentralBrain::ClassifySecond(SecondWith(3000, false), true, 0), Scene::Gameplay);
    EXPECT_EQ(CentralBrain::ClassifySecond(SecondWith(3000, false), false, 0), Scene::Cutscene);
    EXPECT_EQ(CentralBrain::ClassifySecond(SecondWith(4347, true), false, 2), Scene::Cutscene)
        << "two still seconds are not yet a menu";
    EXPECT_EQ(CentralBrain::ClassifySecond(SecondWith(4347, true), true, 3), Scene::Menu)
        << "a still picture is a menu or pause even while keys are pressed";
}

TEST(Brain, ACutscenePausesATestInsteadOfCancellingIt) {
    World w;
    w.draws = Moving;
    w.input = true;
    w.frame_ms = [](World& x) { return x.tex_headroom.load() >= 256 ? 30.5 : 33.3; };
    const auto dir = TempDir("cutscene-pause");
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 50); // warm up; the test starts
    ASSERT_TRUE(rig.brain->Status().evaluation.has_value());
    // A long cutscene: no input, and slower frames that must not count for either side.
    w.input = false;
    const auto gameplay_ms = w.frame_ms;
    w.frame_ms = [](World&) { return 45.0; };
    w.Seconds(*rig.brain, 90);
    EXPECT_TRUE(rig.brain->Status().evaluation.has_value()) << "paused, not cancelled";
    w.input = true;
    w.frame_ms = gameplay_ms;
    w.Seconds(*rig.brain, 120);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    ASSERT_FALSE(p.history.empty());
    EXPECT_EQ(p.history.front().outcome, Outcome::Improved);
    EXPECT_EQ(p.validated.at(KnobId::TextureGcHeadroomMb), 256);
}

TEST(Brain, ATestCutShortByQuittingContinuesNextSession) {
    const auto dir = TempDir("resume");
    const auto fast = [](World& x) { return x.tex_headroom.load() >= 256 ? 30.5 : 33.3; };
    {
        World w;
        w.draws = Moving;
        w.input = true;
        w.frame_ms = fast;
        Rig rig(w, dir, Mode::Automatic);
        rig.Start();
        w.Seconds(*rig.brain, 60); // the test runs about 30 s, then the player quits
        ASSERT_TRUE(rig.brain->Status().evaluation.has_value());
        rig.brain->Shutdown();
        const Profile p = LoadProfile(dir);
        ASSERT_TRUE(p.partial.has_value());
        EXPECT_GT(p.partial->a_median.size() + p.partial->b_median.size(), 10u);
        EXPECT_TRUE(p.history.empty()) << "nothing is judged on half a test";
    }
    World w;
    w.draws = Moving;
    w.input = true;
    w.frame_ms = fast;
    Rig rig(w, dir, Mode::Automatic);
    rig.Start();
    w.Seconds(*rig.brain, 120);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    EXPECT_FALSE(p.partial.has_value());
    ASSERT_FALSE(p.history.empty());
    EXPECT_EQ(p.history.front().outcome, Outcome::Improved);
    EXPECT_EQ(p.validated.at(KnobId::TextureGcHeadroomMb), 256);
}

TEST(Brain, SceneSwitchesAreMeasured) {
    World w;
    w.draws = Moving;
    w.input = true;
    const auto dir = TempDir("switches");
    Rig rig(w, dir, Mode::Observe);
    rig.Start();
    w.Seconds(*rig.brain, 30);
    // Loading screen with a long freeze at its start (a scene load).
    w.draws = [](World&) { return 5u; };
    int first = 1;
    w.spike = [&first](World&) { return first-- > 0 ? 900.0 : 0.0; };
    w.Seconds(*rig.brain, 6);
    w.draws = Moving;
    w.Seconds(*rig.brain, 10);
    const auto sw = rig.brain->SceneSwitches();
    ASSERT_GE(sw.size(), 2u);
    EXPECT_EQ(sw[0].from, Scene::Gameplay);
    EXPECT_EQ(sw[0].to, Scene::Loading);
    EXPECT_GE(sw[0].worst_ms, 900.0);
    rig.brain->Shutdown();
}

TEST(Brain, AutomaticModeTestsStartupValuesOnPauseScreens) {
    // The player plays, and pauses now and then on the same spot (a still picture). Launch 1
    // learns how that pause screen runs; launch 2 tests one start-up value and is judged only on
    // the same pause screen.
    const auto dir = TempDir("auto-startup");
    const auto play = [](World& w, CentralBrain& b, int rounds) {
        for (int i = 0; i < rounds; ++i) {
            w.input = true;
            w.draws = Moving;
            w.Seconds(b, 20);
            w.input = false;
            w.draws = [](World&) { return 4347u; }; // paused: the same picture every frame
            w.Seconds(b, 30);
        }
    };
    const auto frame = [](World& x) {
        const bool paused = x.draws(x) == 4347u;
        const bool testing = !x.env.empty();
        return paused ? (testing ? 18.0 : 20.0) : 33.3;
    };
    {
        World w;
        w.frame_ms = frame;
        Rig rig(w, dir, Mode::Automatic);
        const auto r = rig.Start();
        EXPECT_FALSE(r.experiment.has_value()) << "no baseline yet";
        play(w, *rig.brain, 7);
        rig.brain->Shutdown();
        const Profile p = LoadProfile(dir);
        EXPECT_GE(p.baseline_seconds_ms.size(), 90u);
        EXPECT_TRUE(std::all_of(p.baseline_signatures.begin(), p.baseline_signatures.end(),
                                [](std::uint64_t s) { return s != 0; }));
    }
    World w;
    w.frame_ms = frame;
    Rig rig(w, dir, Mode::Automatic);
    const auto r = rig.Start();
    ASSERT_TRUE(r.experiment.has_value());
    play(w, *rig.brain, 7);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    ASSERT_FALSE(p.history.empty());
    const auto& rec = p.history.back();
    EXPECT_EQ(rec.method, "launch-ab-still");
    EXPECT_EQ(rec.outcome, Outcome::Improved);
    EXPECT_EQ(p.validated.at(r.experiment->knob), r.experiment->value);
}

TEST(Brain, StartupTestsNeverCompareDifferentScenes) {
    // Launch 2 pauses somewhere else (another still picture, much heavier): nothing matches the
    // baseline, so nothing is judged, however different the numbers are.
    const auto dir = TempDir("auto-startup-other");
    const auto play = [](World& w, CentralBrain& b, std::uint32_t picture) {
        for (int i = 0; i < 7; ++i) {
            w.input = true;
            w.draws = Moving;
            w.Seconds(b, 20);
            w.input = false;
            w.draws = [picture](World&) { return picture; };
            w.Seconds(b, 30);
        }
    };
    {
        World w;
        w.frame_ms = [](World& x) { return x.draws(x) == 4347u ? 20.0 : 33.3; };
        Rig rig(w, dir, Mode::Automatic);
        rig.Start();
        play(w, *rig.brain, 4347u);
        rig.brain->Shutdown();
    }
    World w;
    w.frame_ms = [](World& x) { return x.draws(x) == 9000u ? 45.0 : 33.3; };
    Rig rig(w, dir, Mode::Automatic);
    const auto r = rig.Start();
    ASSERT_TRUE(r.experiment.has_value());
    play(w, *rig.brain, 9000u);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    ASSERT_FALSE(p.history.empty());
    EXPECT_EQ(p.history.back().outcome, Outcome::Abandoned);
    EXPECT_TRUE(p.rejected.empty());
    EXPECT_TRUE(p.validated.empty());
}

TEST(Brain, FrameRateTargetLowersQualityOnlyWithinTheLimit) {
    const auto dir = TempDir("fps-target");
    std::vector<std::int32_t> modes;
    for (int launch = 0; launch < 4; ++launch) {
        World w;
        w.draws = Moving;
        w.input = true;
        Rig rig(w, dir, Mode::Automatic);
        rig.cfg.fps_target = 30;
        rig.cfg.perf_mode_floor = 0;
        rig.cfg.perf_mode_max = 2;
        // Re-create the brain with the target settings (the Rig built it with the defaults).
        rig.brain = std::make_unique<CentralBrain>(rig.cfg, [&w] { return w.now; });
        const auto r = rig.Start();
        modes.push_back(r.perf_mode);
        // 19 fps at full quality, 24 at low, 31 at office (the game draws less each step).
        const double ms = r.perf_mode == 0 ? 52.0 : r.perf_mode == 1 ? 41.0 : 32.5;
        w.frame_ms = [ms](World&) { return ms; };
        w.Seconds(*rig.brain, 200);
        rig.brain->Shutdown();
    }
    // 19 fps is more than 25% under the target: two steps at once (build 30), straight to office.
    EXPECT_EQ(modes, (std::vector<std::int32_t>{0, 2, 2, 2})) << "never beyond office";
}

TEST(Brain, FrameRateTargetUsesSmallerRenderSizesBeforeOffice) {
    const auto dir = TempDir("fps-target-render");
    std::vector<QualityStep> steps;
    for (int launch = 0; launch < 6; ++launch) {
        World w;
        w.draws = Moving;
        w.input = true;
        Rig rig(w, dir, Mode::Automatic);
        rig.cfg.fps_target = 30;
        rig.cfg.perf_mode_floor = 0;
        rig.cfg.perf_mode_max = 2;
        rig.cfg.render_height = 1080;
        rig.cfg.render_height_min = 720;
        rig.brain = std::make_unique<CentralBrain>(rig.cfg, [&w] { return w.now; });
        const auto r = rig.Start();
        ASSERT_GE(r.perf_mode, 0);
        const QualityStep s{static_cast<std::uint32_t>(r.perf_mode), r.render_height};
        steps.push_back(s);
        // GPU-bound: frame time follows the pixels; low saves 20%, office 40%.
        const double pixels = static_cast<double>(s.render_height) * s.render_height /
                              (1080.0 * 1080.0);
        const double mode = s.perf_mode == 0 ? 1.0 : s.perf_mode == 1 ? 0.8 : 0.6;
        const double ms = 45.0 * mode * (0.3 + 0.7 * pixels);
        w.frame_ms = [ms](World&) { return ms; };
        w.Seconds(*rig.brain, 200);
        rig.brain->Shutdown();
    }
    // 45 ms -> two steps (more than 25% over) -> low 1008: 33.3 ms... stays once on target.
    EXPECT_EQ(steps[0], (QualityStep{0, 1080}));
    EXPECT_EQ(steps[1], (QualityStep{1, 1008}));
    for (const auto& s : steps) {
        EXPECT_NE(s.perf_mode, 2u) << "office is never needed when a smaller size is enough";
    }
    EXPECT_EQ(steps.back(), steps[steps.size() - 2]) << "it settles";
}

TEST(Brain, FrameRateTargetFromAnOlderBuildsOfficeStartsAtLowOneSizeSmaller) {
    const auto dir = TempDir("fps-target-upgrade");
    {
        World w;
        Rig rig(w, dir, Mode::Automatic);
        rig.Start();
        rig.brain->Shutdown();
    }
    // An older build left "autoperf 2" (office, no render size).
    {
        const auto path = dir / "knowledge.txt";
        Knowledge k;
        bool sc = false;
        std::ifstream in{path};
        std::stringstream ss;
        ss << in.rdbuf();
        ASSERT_TRUE(KnowledgeStore::Parse(ss.str(), k, sc));
        for (auto& [key, p] : k.profiles) {
            p.auto_perf_mode = 2;
            p.auto_render_height = 0;
        }
        std::ofstream out{path, std::ios::trunc};
        out << KnowledgeStore::Serialize(k);
    }
    World w;
    Rig rig(w, dir, Mode::Automatic);
    rig.cfg.fps_target = 30;
    rig.cfg.perf_mode_max = 2;
    rig.cfg.render_height_min = 720;
    rig.brain = std::make_unique<CentralBrain>(rig.cfg, [&w] { return w.now; });
    const auto r = rig.Start();
    EXPECT_EQ(r.perf_mode, 1);
    EXPECT_EQ(r.render_height, 1008u);
    rig.brain->Shutdown();
}

TEST(Brain, FrameRateTargetIsOffUnlessThePlayerAllowsIt) {
    World w;
    const auto dir = TempDir("fps-target-off");
    Rig rig(w, dir, Mode::Automatic);
    rig.cfg.fps_target = 30;
    rig.cfg.perf_mode_max = 0; // no quality reduction allowed
    rig.brain = std::make_unique<CentralBrain>(rig.cfg, [&w] { return w.now; });
    EXPECT_EQ(rig.Start().perf_mode, -1);
    rig.brain->Shutdown();
}
