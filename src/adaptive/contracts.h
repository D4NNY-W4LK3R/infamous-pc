// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Second Son Adaptive Optimization System - Architecture v1.0 contracts.
//
// Every module talks to the others only through the types in this header. The header has no
// dependency on the emulator, so the whole optimizer builds and is unit tested on its own.
//
// Hierarchy (Architecture v1.0, frozen 2026-11-17):
//   Central brain      - objectives, budgets, bottleneck diagnosis, arbitration, learning.
//   Shared services    - Telemetry, Knowledge Base, Safety Gate (not owned by any mini-brain).
//   Mini-brains        - one per domain; propose changes to the knobs they own, nothing else.
//   Workers            - Actuators: the emulator's own fast paths, reached through one atomic
//                        store per knob. No brain is ever on a per-draw / per-frame hot path.
//
// Versioning: a mini-brain declares the contract version it was written against. The brain
// refuses to register one with a different major version, so a stale module can never act on
// fields whose meaning changed.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Adaptive {

inline constexpr std::uint32_t ContractVersionMajor = 1;
inline constexpr std::uint32_t ContractVersionMinor = 0;
/// Bumped whenever the meaning of a stored knowledge record changes. Older files are not
/// reinterpreted: their learned values are dropped and re-measured (their history is kept).
/// 2: emergency rollback needs sustained evidence; v1 "Regressed" blocks (often from one
///    hitch) are forgiven on load. Crash blocks are kept.
/// 3: profiles are keyed by hardware + game only (an emulator update re-checks learned values
///    instead of starting an empty profile); baseline seconds carry a scene signature; an
///    unfinished live A/B test is kept for the next session; the fps-target choice is kept.
inline constexpr std::uint32_t KnowledgeSchemaVersion = 3;

/// Optimization control mode, chosen by the user (Architecture v1.0 sec. 3).
enum class Mode : std::uint8_t {
    Observe,     ///< Measure and report only. Nothing is ever changed. (default)
    Manual,      ///< Measure and recommend; a change is made only when the user applies it.
    Automatic,   ///< Apply validated, reversible live changes during gameplay within user limits.
    Calibration, ///< Additionally run bounded experiments (one per launch) and keep winners.
};

std::string_view ToString(Mode mode);
std::optional<Mode> ParseMode(std::string_view text);

/// When a knob's new value takes effect.
enum class Scope : std::uint8_t {
    Live,       ///< Immediately, through an atomic the emulator reads; reversible at any time.
    NextLaunch, ///< Read once at startup by the emulator; a change is applied on the next launch.
    Never,      ///< Recommend only. Changes gameplay timing or image output, so never automatic.
};

/// The domains of Architecture v1.0 sec. 4. A knob belongs to exactly one domain, and only that
/// domain's mini-brain may propose changes to it.
enum class Domain : std::uint8_t {
    GpuMemory,
    BufferGeometry,
    TextureCache,
    TransferSync,
    ShaderPipeline,
    FramePacing,
    CpuWorkload,
    Count,
};
inline constexpr std::size_t NumDomains = static_cast<std::size_t>(Domain::Count);
std::string_view ToString(Domain domain);

/// Description of one tunable. Values are integers; a knob's meaning (KB, seconds, percent) is
/// part of its description, never inferred.
struct KnobSpec {
    std::string id;          ///< Stable name, also the environment variable for NextLaunch knobs.
    Domain domain;
    Scope scope;
    std::int64_t default_value; ///< The emulator's built-in value: always a safe fallback.
    std::int64_t min_value;
    std::int64_t max_value;
    /// Values the optimizer may try, in order of preference; must include default_value. An
    /// empty list means the knob is observe-only (the brain never proposes values for it).
    std::vector<std::int64_t> candidates;
    std::string unit;
    std::string description;
};

/// Stall categories, mirroring Common::Perf::Stall so the brains can reason about the frame.
enum class StallKind : std::uint8_t {
    ShaderTranslate,
    PipelineCreate,
    PipelineWait,
    GpuWait,
    TextureUpload,
    TextureEvict,
    BufferUpload,
    BufferDownload,
    ReadbackWait,
    Residency,
    SparseBind,
    PageFault,
    PageProtect,
    PageProtectGpu,
    DmaSync,
    GpuThread,
    GuestWait,
    Present,
    FrameWait,
    CommandRecording,
    UploadThread,
    UploadWait,
    Count,
};
inline constexpr std::size_t NumStallKinds = static_cast<std::size_t>(StallKind::Count);
std::string_view ToString(StallKind kind);

/// One presented guest frame, as published by the flip thread. Plain data, 200-odd bytes.
struct FrameSample {
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp_ns = 0; ///< steady clock
    float frame_ms = 0.0f;
    std::array<float, NumStallKinds> stall_ms{};
    std::uint32_t draws = 0;
    std::uint32_t dispatches = 0;
    std::uint32_t submits = 0;
    std::uint32_t buffer_uploads = 0;
    float gpu_busy_ms = 0.0f; ///< host GPU time spent on the frame's command buffers, if timed
};

/// Slow-changing resource state, sampled by the brain itself (about once a second).
struct ResourceSample {
    std::uint64_t timestamp_ns = 0;
    std::uint64_t vram_used_bytes = 0;   ///< VK_EXT_memory_budget heap usage, 0 = unknown
    std::uint64_t vram_budget_bytes = 0; ///< heap budget, 0 = unknown
    std::uint64_t texture_cache_bytes = 0;
    std::uint32_t over_budget_allocations = 0; ///< cumulative count from the emulator
    double process_cpu_percent = -1.0;   ///< -1 = unknown
};

/// What the central brain hands every mini-brain once per supervisory tick.
enum class Bottleneck : std::uint8_t {
    Unknown,
    Warmup,          ///< shaders still compiling at a high rate; nothing is judged yet
    GuestBound,      ///< the game's own CPU work sets the frame rate (GPU thread waits on it)
    GpuThreadBound,  ///< the emulator's command processor is the limit
    HostGpuBound,    ///< the host GPU is the limit
    PresentBound,    ///< presentation / vsync wait dominates (frame rate capped)
    ShaderCompile,   ///< frame spikes coincide with pipeline creation / waits
    MemoryPressure,  ///< VRAM near/over budget, evictions, residency stalls
    TransferBound,   ///< uploads, downloads, readback waits dominate
    PageFaultBound,  ///< game threads stall in GPU-tracked page faults
};
std::string_view ToString(Bottleneck bottleneck);

/// What the game is showing, decided per second (Architecture v1.1: compare like with like).
enum class Scene : std::uint8_t {
    Unknown,  ///< no draw counts reported (tests, very early frames): treated like gameplay
    Gameplay, ///< the player is giving input
    Cutscene, ///< a moving scene without input: a cutscene, or the player watching
    Menu,     ///< a still picture: pause or main menu, a still screen (same draws every frame)
    Loading,  ///< almost nothing drawn: a loading screen or a video
};
inline constexpr std::size_t NumScenes = 5;
std::string_view ToString(Scene scene);

struct Snapshot {
    std::uint64_t timestamp_ns = 0;
    std::uint64_t window_frames = 0;
    double window_seconds = 0.0;
    double fps = 0.0;
    double frame_ms_p50 = 0.0;
    double frame_ms_p95 = 0.0;
    double frame_ms_p99 = 0.0;
    double frame_ms_max = 0.0;
    double hitches_per_minute = 0.0; ///< frames >= 2x the median and >= 25 ms
    std::array<double, NumStallKinds> stall_ms_per_frame{};
    ResourceSample resources{};
    Bottleneck bottleneck = Bottleneck::Unknown;
    double bottleneck_confidence = 0.0; ///< 0..1
    bool in_gameplay = false;           ///< frames flowing steadily (not a load screen / pause)
    Scene scene = Scene::Unknown;       ///< scene of the latest complete second
    double gpu_busy_ms_p50 = 0.0;       ///< median host GPU time per frame, 0 = not measured
};

/// A mini-brain's request. It names a knob of its own domain; the Safety Gate decides.
struct Proposal {
    std::string knob;
    std::int64_t value = 0;
    std::string reason;     ///< human readable evidence, shown in the report and the UI
    double expected_gain = 0.0; ///< mini-brain's own estimate, used only to order proposals
    Domain owner = Domain::Count;
};

enum class Verdict : std::uint8_t {
    Accepted,
    Rejected,
    Deferred, ///< valid, but not now (rate limit, warmup, another change being evaluated)
    Recommended, ///< valid, but the mode or scope only allows recommending it to the user
};
std::string_view ToString(Verdict verdict);

struct Decision {
    Verdict verdict = Verdict::Rejected;
    std::string rule; ///< the Safety Gate rule that decided it
    std::string detail;
};

/// Outcome of a measured change, kept in the Knowledge Base.
enum class Outcome : std::uint8_t {
    Improved,
    Neutral,
    Regressed,
    Crashed,   ///< the session using the value ended without a clean shutdown
    Abandoned, ///< not enough comparable evidence was collected
};
std::string_view ToString(Outcome outcome);

/// Interface every mini-brain implements. Mini-brains are logical modules, not threads: the
/// central brain calls them from its own low-priority thread, one at a time.
class MiniBrain {
public:
    virtual ~MiniBrain() = default;
    virtual Domain GetDomain() const = 0;
    virtual std::string_view Name() const = 0;
    virtual std::uint32_t ContractMajor() const {
        return ContractVersionMajor;
    }
    /// Knobs this brain owns (registered by the central brain on its behalf).
    virtual std::vector<KnobSpec> Knobs() const = 0;
    /// Looks at the snapshot and the knobs' current values; returns at most a few proposals.
    virtual std::vector<Proposal> Think(const Snapshot& snapshot,
                                        const std::vector<std::int64_t>& current_values) = 0;
    /// Short status line for the report / overlay.
    virtual std::string Status() const = 0;
};

} // namespace Adaptive
