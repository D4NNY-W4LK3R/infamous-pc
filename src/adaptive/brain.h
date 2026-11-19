// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Central brain (Architecture v1.0 sec. 3).
//
// Runs on one low-priority thread, about once a second. Each tick it:
//   1. drains the telemetry ring and samples resources;
//   2. builds a Snapshot and diagnoses the bottleneck;
//   3. advances the change under evaluation (A/B blocks, emergency rollback);
//   4. asks every mini-brain for proposals, orders them by relevance to the bottleneck and
//      passes them through the Safety Gate; at most one change starts per tick;
//   5. keeps the calibration measurement of this launch.
// At startup it loads the Knowledge Base, detects a crashed previous session, chooses this
// launch's NextLaunch values (learned values, plus at most one calibration experiment) and sets
// them in the environment before the emulator reads them. At shutdown it judges the launch's
// experiment, saves what was learned and writes a session report.
//
// Everything the brain did is in the decision log, with the rule and the evidence behind it.

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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "adaptive/contracts.h"
#include "adaptive/hardware.h"
#include "adaptive/knobs.h"
#include "adaptive/knowledge.h"
#include "adaptive/mini_brains.h"
#include "adaptive/safety.h"
#include "adaptive/telemetry.h"

namespace Adaptive {

struct BrainConfig {
    Mode mode = Mode::Manual;
    std::filesystem::path data_dir; ///< knowledge.txt, reports/ live here
    std::uint32_t tick_ms = 1000;
    bool calibrate_on_battery = false;
    // Live A/B evaluation: alternate old / new value in blocks of counted seconds (seconds of
    // another scene, shader compiling or settling do not count), so a cutscene pauses a test
    // instead of cancelling it.
    double eval_block_seconds = 10.0; ///< counted seconds per block, settle included
    std::uint32_t eval_blocks = 8;    ///< 4 with the new value, 4 with the old, interleaved
    double settle_seconds = 2.0;
    /// A test that has not finished after this long stops for now; its seconds are kept and a
    /// later session continues it.
    double eval_timeout_seconds = 1800.0;
    /// Start-up tests in Automatic mode: judged only on still pictures (pause, menus) that also
    /// appear in the baseline, so the same scene is compared.
    bool automatic_startup_tests = true;
    // Frame-rate target (fps_target in settings.ini): the optimizer picks the game's quality
    // level per launch, never below the user's limit.
    std::uint32_t fps_target = 0;      ///< 0 = off
    std::uint32_t perf_mode_floor = 0; ///< the player's own choice (0 off, 1 low, 2 office)
    std::uint32_t perf_mode_max = 0;   ///< the lowest quality the player allows
    // Launch A/B calibration.
    std::uint32_t calibration_target_seconds = 90;
    std::uint32_t calibration_min_seconds = 30;
    std::uint32_t baseline_min_seconds = 90;
    double idle_input_seconds = 5.0;
    double warmup_min_seconds = 60.0; ///< nothing is judged in the first minute of frames
    // Verdict thresholds.
    double improve_threshold = 0.02;  ///< median frame time at least 2% lower
    double regress_threshold = 0.02;
    double p99_tolerance = 0.05;      ///< a winner may not raise p99 by more than 5%
    double significance = 0.05;
    /// Run first on the brain thread (the emulator names it and lowers its priority).
    std::function<void()> on_thread_start;
};

struct DecisionLogEntry {
    std::uint64_t t_ns = 0;
    std::string proposer;
    std::string knob;
    std::int64_t from = 0;
    std::int64_t to = 0;
    Decision decision;
    std::string reason;
};

struct Recommendation {
    std::string knob;
    std::int64_t value = 0;
    std::int64_t current = 0;
    Scope scope = Scope::Live;
    std::string owner;
    std::string reason;
    std::string why_not_automatic;
};

struct EvaluationView {
    std::string knob;
    std::int64_t old_value = 0;
    std::int64_t new_value = 0;
    std::uint32_t block = 0;
    std::uint32_t blocks = 0;
    std::size_t seconds_a = 0;
    std::size_t seconds_b = 0;
};

struct StatusView {
    Mode mode = Mode::Manual;
    Snapshot snapshot;
    std::vector<std::pair<std::string, std::string>> brains; ///< name, status
    std::vector<Recommendation> recommendations;
    std::optional<EvaluationView> evaluation;
    std::vector<std::string> notes;
    std::vector<DecisionLogEntry> recent;
    std::map<std::string, std::int64_t> knob_values;
    std::string calibration;
    double brain_ms_per_tick = 0.0;
    std::uint64_t ticks = 0;
    std::uint64_t dropped_frames = 0;
    bool kill_switch = false;
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
};

struct StartupResult {
    LoadStatus load_status = LoadStatus::Fresh;
    /// Performance mode for this launch chosen by the fps-target controller (-1: not in use).
    std::int32_t perf_mode = -1;
    std::map<std::string, std::int64_t> launch_values; ///< set in the environment by the brain
    std::optional<PendingExperiment> experiment;
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

    /// Registers a mini-brain and its knobs. `actuators` must hold one entry per Live knob.
    /// Refused (false) for a contract major version mismatch, a duplicate domain, or a knob
    /// that fails registration; nothing of a refused brain is kept.
    bool RegisterMiniBrain(std::unique_ptr<MiniBrain> brain,
                           std::map<std::string, Actuator> actuators = {},
                           std::map<std::string, int> vram_direction = {});
    /// The GPU Memory authority; created by the constructor, always registered.
    const GpuMemoryBrain& Memory() const {
        return *memory;
    }
    void SetResourceSampler(ResourceSampler sampler);
    /// Details only the renderer knows (heap sizes, Vulkan driver version, budget support), for
    /// the report. Does not change the fingerprint chosen at startup. Thread safe.
    void SetRendererGpu(const GpuInfo& gpu);

    StartupResult Startup(const HardwareInfo& hw, const SoftwareInfo& sw, const EnvGet& env_get,
                          const EnvSet& env_set);

    /// Flip thread. Lock free.
    void PublishFrame(const FrameSample& sample) noexcept {
        ring.Push(sample);
    }
    /// Any input event (keyboard, mouse, pad). Calibration only counts idle seconds.
    void NoteInput() noexcept;

    /// One supervisory step. Called by the brain thread; tests call it directly.
    void Tick();
    void Start();
    void Stop();
    /// Judges this launch, saves the knowledge, closes the session, writes the report.
    /// Returns the report path (empty if it could not be written).
    /// crashed: the launch is ending in a crash; the session stays marked open so the next
    /// launch blames whatever was being tested, as for a launch that never shut down.
    std::filesystem::path Shutdown(bool crashed = false);

    // User controls; thread safe, executed on the next tick.
    void RequestMode(Mode mode);
    void RequestApply(const std::string& knob, std::int64_t value);
    /// The user changed a user-only setting (e.g. the frame rate limit in F3): record its value
    /// so the brains reason about it, and drop the recommendation it answers.
    void RequestNoteUserSetting(const std::string& knob, std::int64_t value);
    void RequestRestoreDefaults();
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
    struct Evaluation {
        std::string knob;
        std::string proposer;
        std::int64_t old_value = 0;
        std::int64_t new_value = 0;
        std::uint64_t start_ns = 0;
        std::uint64_t block_start_ns = 0;
        std::uint32_t block = 0; ///< even = new value (B), odd = old value (A)
        std::uint64_t last_second_seen = 0;
        std::vector<double> a_median, b_median, a_p99, b_p99;
        std::uint32_t a_hitches = 0, b_hitches = 0; ///< seconds that held at least one hitch
        double ref_median = 0.0, ref_p99 = 0.0;
        bool confirm_only = false; ///< reapplying a learned value: watch, do not A/B
        std::uint32_t slow_streak = 0;  ///< consecutive slow seconds with the new value
        std::deque<bool> recent_spikes; ///< spike flags of the latest new-value seconds
        std::uint32_t compile_seconds_skipped = 0; ///< seconds left out: shaders were compiling
        std::uint32_t other_scene_seconds = 0;     ///< seconds left out: another scene
        Scene scene = Scene::Unknown;              ///< the scene this test measures
        std::uint32_t block_counted = 0;           ///< counted seconds in the current block
        std::uint32_t settle_left = 0;             ///< seconds still to skip after a switch
        std::uint32_t resumed_sessions = 0;        ///< earlier sessions this test continues
        bool paused = false; ///< another scene: the knob is back on the old value meanwhile
        std::string reason;
    };

    void ProcessCommands();
    void ClassifySeconds();
    Scene SceneOf(std::uint64_t second_start) const;
    void ResumePartialEvaluation(std::uint64_t now);
    void KeepPartialEvaluation(const std::string& why, std::uint64_t now);
    void DecideNextPerfMode();
    bool StillPictureSecond(std::uint64_t second_start, std::uint64_t& signature) const;
    void UpdateCalibration(std::uint64_t now, bool finishing = false);
    void AdvanceEvaluation(std::uint64_t now);
    void FinishEvaluation(Outcome outcome, const std::string& why, std::uint64_t now);
    void ConsiderProposals(std::uint64_t now);
    void ApplyLearnedLiveValues(std::uint64_t now);
    bool StartEvaluation(const std::string& knob, std::int64_t value, const std::string& proposer,
                         const std::string& reason, bool confirm_only, std::uint64_t now);
    void Log(const std::string& proposer, const std::string& knob, std::int64_t from,
             std::int64_t to, const Decision& d, const std::string& reason);
    void Note(std::string note);
    Outcome Judge(const std::vector<double>& a_med, const std::vector<double>& b_med,
                  const std::vector<double>& a_p99, const std::vector<double>& b_p99,
                  double a_hitch_rate, double b_hitch_rate, double& p_value) const;
    void SaveKnowledge();
    std::string BuildReport() const;
    Profile* CurrentProfile();
    void UpdateStatus(std::uint64_t now);

    BrainConfig config;
    Clock clock;
    FrameRing ring;
    TelemetryWindow window{10.0};
    KnobRegistry knobs;
    SafetyGate gate;
    std::vector<std::unique_ptr<MiniBrain>> brains; ///< brains[0] is the GPU Memory authority
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

    Mode mode;
    std::optional<Evaluation> evaluation;
    Snapshot last_snapshot;
    std::uint64_t first_frame_ns = 0;
    std::uint64_t dropped_frames = 0;
    std::atomic<std::uint64_t> last_input_ns{0};
    bool learned_live_applied = false;
    std::map<std::pair<std::string, std::int64_t>, std::uint32_t> emergency_counts;
    /// Values measured this session without a win: not proposed again until the next launch,
    /// so a neutral change is not re-tested over and over.
    std::set<std::pair<std::string, std::int64_t>> session_tried;
    std::vector<Recommendation> recommendations;
    std::deque<DecisionLogEntry> log;     ///< full log for the report (capped)
    std::vector<std::string> notes;
    std::map<std::string, std::uint32_t> bottleneck_seconds;

    // Scenes (one entry per classified second, capped) and switches between them.
    struct SceneSecond {
        std::uint64_t start_ns = 0;
        Scene scene = Scene::Unknown;
        std::uint64_t signature = 0;
    };
    std::deque<SceneSecond> scene_seconds;
    /// Input moments seen at each tick (the latest input time then), for classifying seconds
    /// once the few seconds after them are known too.
    std::deque<std::uint64_t> input_times;
    std::uint64_t scene_last_second = 0;
    std::uint64_t still_signature = 0;
    std::uint32_t still_run = 0;
    Scene last_scene = Scene::Unknown;
    std::array<SceneStats, NumScenes> scene_stats{};
    std::vector<double> gameplay_gpu_busy; ///< per-second host GPU ms in gameplay (capped)
    std::deque<SceneSwitch> switches;      ///< newest last, capped
    std::int32_t launch_perf_mode = -1;    ///< chosen by the fps-target controller
    std::string perf_mode_decision;

    // This launch's calibration measurement.
    std::optional<PendingExperiment> experiment;
    bool baseline_session = false;
    std::vector<double> cal_median, cal_p99;
    std::vector<std::uint64_t> cal_signatures;
    std::uint32_t cal_hitches = 0;
    std::uint64_t cal_last_second = 0;
    bool cal_done = false;
    std::string cal_result;

    // Overhead accounting.
    std::uint64_t ticks = 0;
    double tick_ms_total = 0.0;

    // Cross-thread.
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
