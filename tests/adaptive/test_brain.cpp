// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end behaviour of the performance analyser against a simulated emulator: frames are
// generated on a fake clock, and the analyser must measure them without changing anything.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <map>
#include <random>
#include <thread>

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
    std::map<std::string, std::string> env;
    std::mt19937 rng{42};
    double vram_fraction = 0.95;
    std::uint32_t over_budget = 0;
    /// frame time (ms)
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
    Rig(World& world, const std::filesystem::path& dir) : w{world} {
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
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeTextureCacheBrain(d, &brain->Memory())));
        EXPECT_TRUE(brain->RegisterMiniBrain(MakeTransferSyncBrain(d)));
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

TEST(Brain, TheAnalyserChangesNothing) {
    // VRAM near the budget, 19 fps: earlier builds started live tests and start-up tests here.
    for (const Mode m : {Mode::Observe, Mode::Manual, Mode::Automatic, Mode::Calibration}) {
        World w;
        w.frame_ms = [](World&) { return 52.0; };
        const auto dir = TempDir("changes-nothing");
        BrainConfig cfg;
        cfg.mode = m; // an old settings.ini's mode: read, and no longer changes anything
        cfg.data_dir = dir;
        cfg.warmup_min_seconds = 20;
        CentralBrain brain(cfg, [&w] { return w.now; });
        EmulatorDefaults d;
        ASSERT_TRUE(brain.RegisterMiniBrain(MakeTextureCacheBrain(d, &brain.Memory())));
        ASSERT_TRUE(brain.RegisterMiniBrain(MakeTransferSyncBrain(d)));
        ASSERT_TRUE(brain.RegisterMiniBrain(MakeCpuWorkloadBrain(d)));
        brain.Startup(w.Hw(), w.Sw(), w.Get(), w.Set());
        w.Seconds(brain, 300);
        EXPECT_TRUE(w.env.empty()) << "no start-up value is set";
        const auto st = brain.Status();
        for (const auto& [k, v] : st.knob_values) {
            EXPECT_EQ(v, brain.Knobs().Find(k)->spec.default_value) << k;
        }
        brain.Shutdown();
    }
}

TEST(Brain, LiveKnobsAreRefused) {
    struct LiveBrain : MiniBrain {
        Domain GetDomain() const override {
            return Domain::CpuWorkload;
        }
        std::string_view Name() const override {
            return "live";
        }
        std::vector<KnobSpec> Knobs() const override {
            return {KnobSpec{"LIVE", Domain::CpuWorkload, Scope::Live, 1, 0, 4, {1, 2}, "", ""}};
        }
        std::vector<Proposal> Think(const Snapshot&, const std::vector<std::int64_t>&) override {
            return {};
        }
        std::string Status() const override {
            return {};
        }
    };
    BrainConfig cfg;
    cfg.data_dir = TempDir("live-refused");
    CentralBrain brain(cfg);
    EXPECT_FALSE(brain.RegisterMiniBrain(std::make_unique<LiveBrain>()));
    EXPECT_EQ(brain.Knobs().Find("LIVE"), nullptr);
}

TEST(Brain, RecommendsOnlyThePlayersOwnSettings) {
    World w;
    w.frame_ms = [](World&) { return 52.0; }; // 19 fps, VRAM at 95% of the budget
    const auto dir = TempDir("recommend");
    Rig rig(w, dir);
    rig.Start();
    w.Seconds(*rig.brain, 120);
    const auto st = rig.brain->Status();
    EXPECT_EQ(st.snapshot.bottleneck, Bottleneck::MemoryPressure);
    ASSERT_FALSE(st.recommendations.empty());
    bool perf_mode = false;
    for (const auto& r : st.recommendations) {
        EXPECT_EQ(rig.brain->Knobs().Find(r.knob)->spec.scope, Scope::Never) << r.knob;
        perf_mode |= r.knob == KnobId::PerfMode && r.value == 1;
    }
    EXPECT_TRUE(perf_mode) << "the game's low quality level, as text";
    rig.brain->RequestNoteUserSetting(KnobId::PerfMode, 1);
    w.Seconds(*rig.brain, 1);
    EXPECT_EQ(rig.brain->Knobs().Find(KnobId::PerfMode)->value, 1);
    const auto path = rig.brain->Shutdown();
    std::ifstream in{path};
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_NE(ss.str().find("performance analyser report"), std::string::npos);
    EXPECT_NE(ss.str().find("== Recommendations (settings that are yours to change)"),
              std::string::npos);
}

TEST(Brain, AnUncleanLaunchIsCountedAndBlamesNothing) {
    const auto dir = TempDir("unclean");
    {
        World w;
        Rig rig(w, dir);
        rig.Start();
        w.Seconds(*rig.brain, 30);
        const auto report = rig.brain->Shutdown(true); // an emulator assertion
        EXPECT_TRUE(std::filesystem::exists(report));
    }
    {
        World w;
        Rig rig(w, dir);
        rig.Start();
        w.Seconds(*rig.brain, 30);
        // The game dies: Shutdown never runs (the destructor only stops the thread).
    }
    World w;
    Rig rig(w, dir);
    rig.Start();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.unclean_sessions, 2u);
    EXPECT_TRUE(p.rejected.empty());
    rig.brain->Shutdown();
    EXPECT_FALSE(LoadK(dir).session_open);
}

TEST(Brain, EntriesOfEarlierBuildsAreReadKeptAndNotUsed) {
    const auto dir = TempDir("old-entries");
    {
        Knowledge k;
        World w;
        std::vector<std::string> notes;
        Profile& p = SelectProfile(k, MakeFingerprint(w.Hw(), w.Sw()), notes);
        p.validated[KnobId::PrefetchBackoffMax] = 2;
        p.rejected[KnobId::TextureGcHeadroomMb][256] = Outcome::Regressed;
        p.partial = PartialEvaluation{};
        p.partial->knob = KnobId::UploadWakeJobs;
        p.partial->old_value = 8;
        p.partial->new_value = 4;
        k.session_open = true; // an older build's launch under test that never shut down
        k.session_profile = p.fingerprint.ProfileKey();
        k.session_experiment = PendingExperiment{KnobId::AheadMax, 16, 32};
        ASSERT_TRUE(KnowledgeStore(dir / "knowledge.txt").Save(k));
    }
    World w;
    Rig rig(w, dir);
    const auto r = rig.Start();
    EXPECT_EQ(r.load_status, LoadStatus::Loaded);
    EXPECT_TRUE(w.env.empty()) << "learned values are not set any more";
    w.Seconds(*rig.brain, 60);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.validated.at(KnobId::PrefetchBackoffMax), 2);
    EXPECT_EQ(p.rejected.at(KnobId::TextureGcHeadroomMb).at(256), Outcome::Regressed);
    EXPECT_FALSE(p.rejected.contains(KnobId::AheadMax)) << "an old crash blames nothing";
    EXPECT_EQ(p.unclean_sessions, 1u);
}

TEST(Brain, UserEnvironmentIsNotedAndLeftAlone) {
    const auto dir = TempDir("pinned");
    World w;
    w.env[KnobId::ReadbackWindowKb] = "512";
    Rig rig(w, dir);
    rig.Start();
    EXPECT_EQ(w.env.at(KnobId::ReadbackWindowKb), "512");
    const KnobState* st = rig.brain->Knobs().Find(KnobId::ReadbackWindowKb);
    ASSERT_NE(st, nullptr);
    EXPECT_TRUE(st->user_pinned);
    EXPECT_EQ(st->value, 512);
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

TEST(Brain, ABrainCannotRecommendAnotherDomainsKnob) {
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
            // Claims to be frame pacing.
            return {Proposal{KnobId::FrameLimit, 30, "", 9.0, Domain::FramePacing}};
        }
        std::string Status() const override {
            return {};
        }
    };
    World w;
    const auto dir = TempDir("rogue");
    BrainConfig cfg;
    cfg.data_dir = dir;
    cfg.warmup_min_seconds = 5;
    CentralBrain brain(cfg, [&w] { return w.now; });
    ASSERT_TRUE(brain.RegisterMiniBrain(MakeFramePacingBrain({})));
    ASSERT_TRUE(brain.RegisterMiniBrain(std::make_unique<Rogue>()));
    brain.Startup(w.Hw(), w.Sw(), w.Get(), w.Set());
    w.Seconds(brain, 60); // steady 30 fps: frame pacing itself has nothing to say
    EXPECT_TRUE(brain.Status().recommendations.empty());
    brain.Shutdown();
}

TEST(Brain, ThreadedRunIsCheapAndStopsCleanly) {
    World w;
    const auto dir = TempDir("thread");
    BrainConfig cfg;
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

TEST(Brain, ForgetProfileClearsTheFrameRateTargetsChoice) {
    const auto dir = TempDir("forget");
    {
        Knowledge k;
        World w;
        std::vector<std::string> notes;
        Profile& p = SelectProfile(k, MakeFingerprint(w.Hw(), w.Sw()), notes);
        p.auto_perf_mode = 2;
        p.validated[KnobId::PrefetchBackoffMax] = 2;
        ASSERT_TRUE(KnowledgeStore(dir / "knowledge.txt").Save(k));
    }
    World w;
    Rig rig(w, dir);
    rig.Start();
    rig.brain->RequestForgetProfile();
    w.Seconds(*rig.brain, 2);
    rig.brain->Shutdown();
    const Profile p = LoadProfile(dir);
    EXPECT_EQ(p.auto_perf_mode, -1);
    EXPECT_TRUE(p.validated.empty());
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
TEST(Brain, SceneSwitchesAreMeasured) {
    World w;
    w.draws = Moving;
    w.input = true;
    const auto dir = TempDir("switches");
    Rig rig(w, dir);
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
TEST(Brain, FrameRateTargetLowersQualityOnlyWithinTheLimit) {
    const auto dir = TempDir("fps-target");
    std::vector<std::int32_t> modes;
    for (int launch = 0; launch < 4; ++launch) {
        World w;
        w.draws = Moving;
        w.input = true;
        Rig rig(w, dir);
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
        Rig rig(w, dir);
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
        Rig rig(w, dir);
        rig.Start();
        rig.brain->Shutdown();
    }
    // An older build left "autoperf 2" (office, no render size).
    {
        const auto path = dir / "knowledge.txt";
        Knowledge k;
        bool sc = false;
        std::ifstream in{path, std::ios::binary};
        std::stringstream ss;
        ss << in.rdbuf();
        ASSERT_TRUE(KnowledgeStore::Parse(ss.str(), k, sc));
        for (auto& [key, p] : k.profiles) {
            p.auto_perf_mode = 2;
            p.auto_render_height = 0;
        }
        std::ofstream out{path, std::ios::binary | std::ios::trunc}; // no CRLF on Windows
        out << KnowledgeStore::Serialize(k);
    }
    World w;
    Rig rig(w, dir);
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
    Rig rig(w, dir);
    rig.cfg.fps_target = 30;
    rig.cfg.perf_mode_max = 0; // no quality reduction allowed
    rig.brain = std::make_unique<CentralBrain>(rig.cfg, [&w] { return w.now; });
    EXPECT_EQ(rig.Start().perf_mode, -1);
    rig.brain->Shutdown();
}

TEST(Brain, ReportCountsFreezesPerScene) {
    const auto dir = TempDir("freezes");
    World w;
    w.draws = Moving;
    w.input = true;
    int frame = 0;
    // A 400 ms frame every 300 frames (about every 10 s) during gameplay.
    w.spike = [&frame](World&) { return (++frame % 300 == 0) ? 400.0 : 0.0; };
    Rig rig(w, dir);
    rig.Start();
    w.Seconds(*rig.brain, 120);
    const auto path = rig.brain->Shutdown();
    ASSERT_FALSE(path.empty());
    std::ifstream in{path};
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string report = ss.str();
    const auto at = report.find("== Freezes");
    ASSERT_NE(at, std::string::npos);
    const std::string section = report.substr(at, report.find("== Scene switches") - at);
    EXPECT_NE(section.find("gameplay: "), std::string::npos) << section;
    EXPECT_EQ(section.find("gameplay: 0 freezes"), std::string::npos) << section;
    EXPECT_NE(section.find("per 10 minutes"), std::string::npos) << section;
}

TEST(Brain, ReportNamesWhatHoldsTheFrameRateBack) {
    const auto dir = TempDir("limits");
    World w;
    w.draws = Moving;
    w.input = true;
    // 25 fps with the card busy half the frame, and the game's threads stalled in page faults
    // and copies back, as in the player's sessions.
    w.frame_ms = [](World&) { return 40.0; };
    w.decorate = [](World&, FrameSample& f) {
        f.gpu_busy_ms = 20.0f;
        f.stall_ms[static_cast<std::size_t>(StallKind::PageFault)] = 15.0f;
        f.stall_ms[static_cast<std::size_t>(StallKind::ReadbackWait)] = 5.0f;
        return 0.0;
    };
    Rig rig(w, dir);
    rig.Start();
    w.Seconds(*rig.brain, 90);
    const auto path = rig.brain->Shutdown();
    ASSERT_FALSE(path.empty());
    std::ifstream in{path};
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string report = ss.str();
    const auto at = report.find("== What holds the frame rate back");
    ASSERT_NE(at, std::string::npos) << report;
    const std::string section = report.substr(at, report.find("== Freezes") - at);
    EXPECT_NE(section.find("seconds under 30 fps"), std::string::npos) << section;
    EXPECT_EQ(section.find(" 0 of "), std::string::npos) << section;
    EXPECT_NE(section.find("graphics card busy 20.0 ms ("), std::string::npos) << section;
    EXPECT_NE(section.find("game's own threads"), std::string::npos) << section;
}
