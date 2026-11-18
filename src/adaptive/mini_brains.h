// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The seven mini-brains of Architecture v1.0 sec. 4.
//
// Each owns the knobs of its domain and turns the shared Snapshot into at most a couple of
// proposals per tick. They hold no locks, own no threads and touch no emulator state; their
// only output is a Proposal, which the Safety Gate may refuse.
//
// Knob defaults are the emulator's built-in values, passed in by the glue that knows them, so a
// mini-brain can never drift from what the emulator really does without it.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adaptive/contracts.h"

namespace Adaptive {

/// Defaults of the emulator build the brains run in (filled by adaptive_glue.cpp).
struct EmulatorDefaults {
    std::int64_t readback_window_kb = 128;
    std::int64_t prefetch_backoff_max = 1;
    std::int64_t hot_window_secs = 5;
    std::int64_t ahead_max = 16;
    std::int64_t upload_wake_jobs = 8;
    std::int64_t pipeline_workers = 4;
    std::int64_t pipeline_workers_max = 12; ///< logical cores - 2, at most 12
    std::int64_t present_mode = 0;          ///< 0 Mailbox, 1 Fifo, 2 Immediate (settings)
    std::int64_t frame_limit = 0;           ///< fps shown at most, 0 = off (F3 > Graphics)
    bool has_live_texture_gc = true;
    bool has_live_upload_wake = true;
};

/// Global memory authority. Owns no knob; tells the central brain when VRAM is under pressure
/// so changes that would use more of it are vetoed (Safety Gate rule R7).
class GpuMemoryBrain final : public MiniBrain {
public:
    Domain GetDomain() const override {
        return Domain::GpuMemory;
    }
    std::string_view Name() const override {
        return "GPU Memory";
    }
    std::vector<KnobSpec> Knobs() const override {
        return {};
    }
    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>&) override;
    std::string Status() const override;

    /// used / budget, or -1 if unknown.
    double Pressure() const {
        return pressure;
    }
    bool Veto() const {
        return veto;
    }
    bool OverBudgetGrowing() const {
        return over_budget_growing;
    }

private:
    double pressure = -1.0;
    bool veto = false;
    bool over_budget_growing = false;
    std::uint32_t last_over_budget = 0;
    bool have_last = false;
};

std::unique_ptr<MiniBrain> MakeBufferGeometryBrain(const EmulatorDefaults& d);
std::unique_ptr<MiniBrain> MakeTextureCacheBrain(const EmulatorDefaults& d,
                                                 const GpuMemoryBrain* memory);
std::unique_ptr<MiniBrain> MakeTransferSyncBrain(const EmulatorDefaults& d);
std::unique_ptr<MiniBrain> MakeShaderPipelineBrain(const EmulatorDefaults& d);
std::unique_ptr<MiniBrain> MakeFramePacingBrain(const EmulatorDefaults& d);
std::unique_ptr<MiniBrain> MakeCpuWorkloadBrain(const EmulatorDefaults& d);

/// Knob ids (also environment variable names for NextLaunch knobs).
namespace KnobId {
inline constexpr const char* ReadbackWindowKb = "SHAD_READBACK_WINDOW_KB";
inline constexpr const char* PrefetchBackoffMax = "SHAD_PREFETCH_BACKOFF_MAX";
inline constexpr const char* HotWindowSecs = "SHAD_HOT_WINDOW_SECS";
inline constexpr const char* AheadMax = "SHAD_AHEAD_MAX";
inline constexpr const char* PipelineWorkers = "SS_PIPELINE_WORKERS";
inline constexpr const char* UploadWakeJobs = "SS_UPLOAD_WAKE_JOBS";
inline constexpr const char* TextureGcHeadroomMb = "SS_TEXCACHE_GC_HEADROOM_MB";
inline constexpr const char* PresentMode = "SS_PRESENT_MODE";
inline constexpr const char* FrameLimit = "SS_FRAME_LIMIT";
inline constexpr const char* AsyncCompute = "SHADPS4_ASYNC_COMPUTE";
} // namespace KnobId

} // namespace Adaptive
