// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Performance analyser + frame-rate target (build 55; before it, the "adaptive optimizer").
//
// Runs on one low-priority thread, about once a second. Each tick it drains the telemetry ring,
// samples resources, sorts every second into a scene, builds a Snapshot, names what limits the
// frame rate and collects the numbers the session report shows. It changes nothing while the game
// runs: the live A/B tests and start-up tests of earlier builds were removed after 79 sessions in
// which none of them was kept. The mini-brains only describe their area and recommend settings
// that are the player's to change (frame rate limit, present mode, performance mode).
//
// The one thing it decides is the frame-rate target's quality for each launch (performance mode
// and render size, never below the player's own choice), from the gameplay of the launch before.
// The knowledge file keeps that choice; entries of earlier builds are read and kept unchanged.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "adaptive/contracts.h"
#include "adaptive/quality_ladder.h"
#include "adaptive/hardware.h"
#include "adaptive/knobs.h"
#include "adaptive/knowledge.h"
#include "adaptive/mini_brains.h"
#include "adaptive/telemetry.h"

namespace Adaptive {

struct BrainConfig {
    Mode mode = Mode::Automatic; ///< read from settings.ini for old files; no longer changes anything
    std::filesystem::path data_dir; ///< knowledge.txt, reports/ live here
    std::uint32_t tick_ms = 1000;
    // Frame-rate target (fps_target in settings.ini): the analyser picks the game's quality
    // level per launch, never below the user's limit.
    std::uint32_t fps_target = 0;      ///< 0 = off
    std::uint32_t perf_mode_floor = 0; ///< the player's own choice (0 off, 1 low, 2 office)
    std::uint32_t perf_mode_max = 0;   ///< the lowest quality the player allows
    std::uint32_t render_height = 1080;     ///< the player's own render size (quality_ladder.h)
    std::uint32_t render_height_min = 1080; ///< the smallest render size the player allows
    double warmup_min_seconds = 60.0; ///< the first minute of frames is shader warm-up
    /// Run first on the brain thread (the emulator names it and lowers its priority).
    std::function<void()> on_thread_start;
};

/// A setting that is the player's to change, with the measurement behind it (text only).
struct Recommendation {
    std::string knob;
    std::int64_t value = 0;
    std::int64_t current = 0;
    std::string owner;
    std::string reason;
};

struct StatusView {
    Snapshot snapshot;
    std::vector<std::pair<std::string, std::string>> brains; ///< area, what it measures
    std::vector<Recommendation> recommendations;
    std::vector<std::string> notes;
    std::map<std::string, std::int64_t> knob_values;
    double brain_ms_per_tick = 0.0;
    std::uint64_t ticks = 0;
    std::uint64_t dropped_frames = 0;
    std::string fps_target; ///< what the frame-rate target chose, empty when it is off
    std::size_t scene_switches = 0;
    double worst_switch_ms = 0.0;
};

/// A switch between scenes and what the next few seconds cost (Architecture v1.1).
struct SceneSwitch {
    std::uint64_t t_ns = 0;
    Scene from = Scene::Unknown;
    Scene to = Scene::Unknown;
    double worst_ms = 0.0;
    std::uint32_t frames_over_100ms = 0;
    std::array<double, NumStallKinds> stall_ms{}; ///< summed over the switch's seconds
    std::uint32_t seconds_left = 0;               ///< still being measured while > 0
};

struct SceneStats {
    std::uint32_t seconds = 0;
    std::vector<double> medians; ///< capped
    std::uint32_t freezes = 0;   ///< frames of 100 ms or more
    double frozen_ms = 0.0;      ///< their time beyond the usual frame
    std::array<double, NumStallKinds> freeze_stall_ms{}; ///< the stalls of seconds with a freeze
};

/// Gameplay seconds under or at the frame-rate target, and where their frame time went: the
/// graphics card, the emulator's GPU thread, or the game's own threads waiting (build 35).
struct LimitStats {
    std::uint32_t seconds = 0;
    double frames = 0.0;
    double card_ms = 0.0; ///< graphics card busy, summed over the frames
    std::array<double, NumStallKinds> stall_ms{};
};

struct StartupResult {
    LoadStatus load_status = LoadStatus::Fresh;
    /// Performance mode for this launch chosen by the fps-target controller (-1: not in use).
    std::int32_t perf_mode = -1;
    /// Render height for this launch chosen with it (0: not in use).
    std::uint32_t render_height = 0;
    std::vector<std::string> notes;
};

using EnvGet = std::function<std::optional<std::string>(const std::string&)>;
using EnvSet = std::function<void(const std::string&, const std::string&)>;

class CentralBrain {
public:
    using Clock = std::function<std::uint64_t()>;
    using ResourceSampler = std::function<ResourceSample()>;

    explicit CentralBrain(BrainConfig config, Clock clock = {});
    ~CentralBrain();
    CentralBrain(const CentralBrain&) = delete;
    CentralBrain& operator=(const CentralBrain&) = delete;

    /// Registers a mini-brain (one area of measurements) and its knobs, for the report.
    /// Refused (false) for a contract major version mismatch, a duplicate domain, or a knob
    /// that fails registration; nothing of a refused brain is kept.
    bool RegisterMiniBrain(std::unique_ptr<MiniBrain> brain);
    /// The GPU Memory area; created by the constructor, always registered.
    const GpuMemoryBrain& Memory() const {
        return *memory;
    }
    void SetResourceSampler(ResourceSampler sampler);
    /// Details only the renderer knows (heap sizes, Vulkan driver version, budget support), for
    /// the report. Does not change the fingerprint chosen at startup. Thread safe.
    void SetRendererGpu(const GpuInfo& gpu);
    /// What the report says about mains power and battery this launch (build 51).
    void SetPowerNote(std::string note);
    /// The "Cheats:" line (build 52): what was on this launch, so numbers are read with it.
    void SetCheatsNote(std::string note);

    StartupResult Startup(const HardwareInfo& hw, const SoftwareInfo& sw, const EnvGet& env_get,
                          const EnvSet& env_set);

    /// Flip thread. Lock free.
    void PublishFrame(const FrameSample& sample) noexcept {
        ring.Push(sample);
    }
    /// Any input event (keyboard, mouse, pad): tells gameplay from cutscenes.
    void NoteInput() noexcept;

    /// One measuring step. Called by the brain thread; tests call it directly.
    void Tick();
    void Start();
    void Stop();
    /// Decides the frame-rate target's next launch, saves the knowledge, closes the session and
    /// writes the report. Returns the report path (empty if it could not be written).
    /// crashed: the launch is ending in a crash; the session stays marked open so the next
    /// launch counts it as unclean, as for a launch that never shut down.
    std::filesystem::path Shutdown(bool crashed = false);

    // User controls; thread safe, executed on the next tick.
    /// The user changed a user-only setting (e.g. the frame rate limit in F3): record its value
    /// and drop the recommendation it answers.
    void RequestNoteUserSetting(const std::string& knob, std::int64_t value);
    /// Forgets what the frame-rate target learned on this PC (and any old entries).
    void RequestForgetProfile();

    StatusView Status() const;
    std::string Report() const;
    std::uint64_t Now() const {
        return clock();
    }
    KnobRegistry& Knobs() {
        return knobs;
    }

    /// Exposed for tests.
    static Bottleneck Diagnose(const Snapshot& s, double vram_pressure, bool warmup,
                               double& confidence);
    /// One second's scene. still_run: consecutive seconds that showed this same still picture.
    static Scene ClassifySecond(const SecondSummary& s, bool input_recent,
                                std::uint32_t still_run);
    std::vector<SceneSwitch> SceneSwitches() const;

private:
    void ProcessCommands();
    void ClassifySeconds();
    void CollectRecommendations();
    void DecideNextPerfMode();
    void Note(std::string note);
    void SaveKnowledge();
    std::string BuildReport() const;
    Profile* CurrentProfile();
    void UpdateStatus(std::uint64_t now);

    BrainConfig config;
    Clock clock;
    FrameRing ring;
    TelemetryWindow window{10.0};
    KnobRegistry knobs;
    std::vector<std::unique_ptr<MiniBrain>> brains; ///< brains[0] is the GPU Memory area
    GpuMemoryBrain* memory = nullptr;
    ResourceSampler sampler;
    KnowledgeStore store;
    Knowledge knowledge;
    std::string profile_key;
    HardwareInfo hardware;
    SoftwareInfo software;
    Fingerprint fingerprint;
    bool started = false;
    bool shut_down = false;

    Snapshot last_snapshot;
    std::uint64_t first_frame_ns = 0;
    std::uint64_t dropped_frames = 0;
    std::atomic<std::uint64_t> last_input_ns{0};
    std::vector<Recommendation> recommendations;
    std::vector<std::string> notes;
    std::map<std::string, std::uint32_t> bottleneck_seconds;

    /// Input moments seen at each tick (the latest input time then), for classifying seconds
    /// once the few seconds after them are known too.
    std::deque<std::uint64_t> input_times;
    std::uint64_t scene_last_second = 0;
    std::uint64_t still_signature = 0;
    std::uint32_t still_run = 0;
    Scene last_scene = Scene::Unknown;
    std::array<SceneStats, NumScenes> scene_stats{};
    std::vector<double> gameplay_gpu_busy; ///< per-second host GPU ms in gameplay (capped)
    std::array<LimitStats, 2> gameplay_limits{}; ///< [0] under the target, [1] at it
    /// The frame rate gameplay seconds are held to: the fps target, or 30 (the game's own cap).
    std::uint32_t LimitTargetFps() const {
        return config.fps_target > 0 ? config.fps_target : 30;
    }
    std::deque<SceneSwitch> switches;      ///< newest last, capped
    std::int32_t launch_perf_mode = -1;    ///< chosen by the fps-target controller
    std::vector<QualityStep> quality_ladder; ///< this launch's steps (empty: target off)
    std::size_t launch_step = 0;             ///< the step this launch uses
    std::string perf_mode_decision;

    // Overhead accounting.
    std::uint64_t ticks = 0;
    double tick_ms_total = 0.0;

    // Cross-thread.
    std::string power_note; ///< guarded by renderer_gpu_mutex
    std::string cheats_note; ///< guarded by renderer_gpu_mutex
    mutable std::mutex renderer_gpu_mutex;
    std::optional<GpuInfo> renderer_gpu;
    mutable std::mutex status_mutex;
    StatusView status;
    std::mutex command_mutex;
    std::vector<std::function<void()>> commands;
    std::thread thread;
    std::mutex thread_mutex;
    std::condition_variable thread_cv;
    bool stop_requested = false;
    std::vector<FrameSample> drain_buffer;
};

} // namespace Adaptive
