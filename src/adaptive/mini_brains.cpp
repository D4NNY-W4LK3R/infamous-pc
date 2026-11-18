// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <optional>

#include "adaptive/mini_brains.h"

namespace Adaptive {

namespace {

double Stall(const Snapshot& s, StallKind k) {
    return s.stall_ms_per_frame[static_cast<std::size_t>(k)];
}

std::string Fmt(const char* fmt, double a, double b = 0.0, double c = 0.0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, a, b, c);
    return buf;
}

std::vector<std::int64_t> Unique(std::vector<std::int64_t> v) {
    std::vector<std::int64_t> out;
    for (const auto x : v) {
        if (std::find(out.begin(), out.end(), x) == out.end()) {
            out.push_back(x);
        }
    }
    return out;
}

/// Next candidate in a direction from the current value (candidates sorted ascending).
std::optional<std::int64_t> Step(const KnobSpec& spec, std::int64_t current, int direction) {
    std::vector<std::int64_t> c = spec.candidates;
    std::sort(c.begin(), c.end());
    if (direction > 0) {
        for (const auto v : c) {
            if (v > current) {
                return v;
            }
        }
    } else {
        for (auto it = c.rbegin(); it != c.rend(); ++it) {
            if (*it < current) {
                return *it;
            }
        }
    }
    return std::nullopt;
}

/// Base for the brains that own knobs: keeps their specs and a status line.
class KnobBrain : public MiniBrain {
public:
    KnobBrain(Domain d, std::string name, std::vector<KnobSpec> specs)
        : domain{d}, name{std::move(name)}, specs{std::move(specs)} {}
    Domain GetDomain() const override {
        return domain;
    }
    std::string_view Name() const override {
        return name;
    }
    std::vector<KnobSpec> Knobs() const override {
        return specs;
    }
    std::string Status() const override {
        return status;
    }

protected:
    const KnobSpec& Spec(std::size_t i) const {
        return specs[i];
    }
    Proposal Propose(std::size_t i, std::int64_t value, std::string reason, double gain) const {
        return Proposal{specs[i].id, value, std::move(reason), gain, domain};
    }

    Domain domain;
    std::string name;
    std::vector<KnobSpec> specs;
    std::string status = "no data yet";
};

// --- Buffer & Geometry -------------------------------------------------------------------------
// Owns the readback-prefetch heuristics. Evidence: game threads waiting for copies back
// (ReadbackWait) and page faults on GPU-written memory.
class BufferGeometryBrain final : public KnobBrain {
public:
    explicit BufferGeometryBrain(const EmulatorDefaults& d)
        : KnobBrain(Domain::BufferGeometry, "Buffer & Geometry",
                    {
                        KnobSpec{KnobId::PrefetchBackoffMax, Domain::BufferGeometry,
                                 Scope::NextLaunch, d.prefetch_backoff_max, 0, 8,
                                 Unique({d.prefetch_backoff_max, 2, 4}), "windows",
                                 "readback prefetch: stale windows skipped this many times"},
                        KnobSpec{KnobId::HotWindowSecs, Domain::BufferGeometry, Scope::NextLaunch,
                                 d.hot_window_secs, 1, 30, Unique({d.hot_window_secs, 3, 8}), "s",
                                 "how long read-back memory keeps being copied ahead"},
                    }) {}

    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>& v) override {
        const double wait = Stall(s, StallKind::ReadbackWait);
        const double download = Stall(s, StallKind::BufferDownload);
        status = Fmt("readback wait %.2f ms/frame, downloads %.2f ms/frame", wait, download);
        std::vector<Proposal> out;
        if (!s.in_gameplay) {
            return out;
        }
        // Game threads stall on copies back: copy ahead for longer.
        if (wait > 1.0 && s.bottleneck == Bottleneck::TransferBound) {
            if (const auto n = Step(Spec(1), v[1], +1)) {
                out.push_back(Propose(1, *n,
                                      Fmt("game threads wait %.1f ms/frame for copies back; keep "
                                          "copying read-back memory ahead for longer",
                                          wait),
                                      wait));
            }
        }
        // Prefetch work is mostly wasted (downloads cost more than the waits they save).
        if (download > 2.0 && wait < 0.2 && s.bottleneck == Bottleneck::GpuThreadBound) {
            if (const auto n = Step(Spec(0), v[0], +1)) {
                out.push_back(Propose(0, *n,
                                      Fmt("downloads cost %.1f ms/frame while waits are only "
                                          "%.2f ms; skip stale prefetch windows more",
                                          download, wait),
                                      download - wait));
            }
        }
        return out;
    }
};

// --- Texture & Cache ---------------------------------------------------------------------------
// Owns extra headroom for the texture cache's garbage collector. Raising it makes the existing
// collector (which already checks GPU completion before freeing anything) start earlier; it
// never frees anything the collector would not.
class TextureCacheBrain final : public KnobBrain {
public:
    TextureCacheBrain(const EmulatorDefaults& d, const GpuMemoryBrain* memory_)
        : KnobBrain(Domain::TextureCache, "Texture & Cache",
                    d.has_live_texture_gc
                        ? std::vector<KnobSpec>{KnobSpec{
                              KnobId::TextureGcHeadroomMb, Domain::TextureCache, Scope::Live, 0, 0,
                              1024, {0, 256, 512}, "MB",
                              "texture cache starts cleaning up this much earlier"}}
                        : std::vector<KnobSpec>{}),
          memory{memory_} {}

    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>& v) override {
        const double evict = Stall(s, StallKind::TextureEvict);
        const double upload = Stall(s, StallKind::TextureUpload);
        const double pressure = memory ? memory->Pressure() : -1.0;
        status = Fmt("VRAM %.0f%% of budget, evict %.2f ms/frame, upload %.2f ms/frame",
                     pressure * 100.0, evict, upload);
        std::vector<Proposal> out;
        if (specs.empty() || !s.in_gameplay || pressure < 0.0) {
            return out;
        }
        const bool over_budget = memory && memory->OverBudgetGrowing();
        if (pressure >= 0.93 || over_budget) {
            if (const auto n = Step(Spec(0), v[0], +1)) {
                out.push_back(Propose(0, *n,
                                      over_budget
                                          ? std::string("allocations went past the VRAM budget; "
                                                        "clean the texture cache earlier")
                                          : Fmt("VRAM at %.0f%% of budget; clean the texture "
                                                "cache earlier",
                                                pressure * 100.0),
                                      pressure));
            }
        } else if (pressure < 0.70 && v[0] > 0 && upload > 1.5) {
            if (const auto n = Step(Spec(0), v[0], -1)) {
                out.push_back(Propose(0, *n,
                                      Fmt("VRAM only %.0f%% used but textures are re-uploaded "
                                          "(%.1f ms/frame); give the cache its room back",
                                          pressure * 100.0, upload),
                                      upload));
            }
        }
        return out;
    }

private:
    const GpuMemoryBrain* memory;
};

// --- Transfer & Synchronization ----------------------------------------------------------------
class TransferSyncBrain final : public KnobBrain {
public:
    explicit TransferSyncBrain(const EmulatorDefaults& d)
        : KnobBrain(Domain::TransferSync, "Transfer & Sync", MakeSpecs(d)),
          live_wake{d.has_live_upload_wake} {}

    static std::vector<KnobSpec> MakeSpecs(const EmulatorDefaults& d) {
        std::vector<KnobSpec> v{
            KnobSpec{KnobId::ReadbackWindowKb, Domain::TransferSync, Scope::NextLaunch,
                     d.readback_window_kb, 4, 4096, Unique({d.readback_window_kb, 64, 256}), "KB",
                     "size of the window a GPU readback is widened to"},
            KnobSpec{KnobId::AsyncCompute, Domain::TransferSync, Scope::Never, 0, 0, 1, {0, 1}, "",
                     "second compute queue; can hang the GPU on this game - user only"},
        };
        if (d.has_live_upload_wake) {
            v.push_back(KnobSpec{KnobId::UploadWakeJobs, Domain::TransferSync, Scope::Live,
                                 d.upload_wake_jobs, 1, 64, Unique({d.upload_wake_jobs, 4, 16}),
                                 "jobs", "upload worker is woken once this many jobs queue up"});
        }
        return v;
    }

    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>& v) override {
        const double upload_wait = Stall(s, StallKind::UploadWait);
        const double readback_wait = Stall(s, StallKind::ReadbackWait);
        const double download = Stall(s, StallKind::BufferDownload);
        status = Fmt("upload wait %.2f, readback wait %.2f, downloads %.2f ms/frame", upload_wait,
                     readback_wait, download);
        std::vector<Proposal> out;
        if (!s.in_gameplay) {
            return out;
        }
        if (live_wake) {
            const std::size_t wake = 2;
            if (upload_wait > 0.5) {
                if (const auto n = Step(Spec(wake), v[wake], -1)) {
                    out.push_back(Propose(wake, *n,
                                          Fmt("submissions wait %.2f ms/frame for queued uploads; "
                                              "wake the upload worker sooner",
                                              upload_wait),
                                          upload_wait));
                }
            } else if (upload_wait < 0.05 && s.bottleneck == Bottleneck::GpuThreadBound &&
                       v[wake] < 16) {
                if (const auto n = Step(Spec(wake), v[wake], +1)) {
                    out.push_back(Propose(wake, *n,
                                          "the GPU thread is the limit and never waits on "
                                          "uploads; wake the worker less often",
                                          0.1));
                }
            }
        }
        if (s.bottleneck == Bottleneck::TransferBound && readback_wait > 1.0 && download > 1.0) {
            if (const auto n = Step(Spec(0), v[0], -1)) {
                out.push_back(Propose(0, *n,
                                      Fmt("readbacks dominate (%.1f ms wait, %.1f ms downloads); "
                                          "try a narrower readback window",
                                          readback_wait, download),
                                      readback_wait));
            }
        }
        return out;
    }

private:
    bool live_wake;
};

// --- Shader & Pipeline -------------------------------------------------------------------------
class ShaderPipelineBrain final : public KnobBrain {
public:
    explicit ShaderPipelineBrain(const EmulatorDefaults& d)
        : KnobBrain(Domain::ShaderPipeline, "Shader & Pipeline",
                    {KnobSpec{KnobId::PipelineWorkers, Domain::ShaderPipeline, Scope::NextLaunch,
                              d.pipeline_workers, 1, std::max<std::int64_t>(d.pipeline_workers_max, d.pipeline_workers),
                              Unique({d.pipeline_workers,
                                      std::min(d.pipeline_workers + 2,
                                               std::max(d.pipeline_workers_max, d.pipeline_workers)),
                                      std::max<std::int64_t>(1, d.pipeline_workers - 1)}),
                              "threads", "background pipeline compiler threads"}}) {}

    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>& v) override {
        const double translate = Stall(s, StallKind::ShaderTranslate);
        const double create = Stall(s, StallKind::PipelineCreate);
        const double wait = Stall(s, StallKind::PipelineWait);
        status = Fmt("shader translate %.2f, pipeline create %.2f, pipeline wait %.2f ms/frame",
                     translate, create, wait);
        std::vector<Proposal> out;
        if (s.bottleneck == Bottleneck::ShaderCompile && wait > 2.0) {
            if (const auto n = Step(Spec(0), v[0], +1)) {
                out.push_back(Propose(0, *n,
                                      Fmt("frames wait %.1f ms for pipelines being compiled; use "
                                          "more compiler threads",
                                          wait),
                                      wait));
            }
        } else if (s.bottleneck == Bottleneck::GuestBound && wait < 0.1 && translate < 0.5) {
            if (const auto n = Step(Spec(0), v[0], -1)) {
                out.push_back(Propose(0, *n,
                                      "the game's CPU work is the limit and pipelines are ready "
                                      "in time; leave the game more CPU",
                                      0.2));
            }
        }
        return out;
    }
};

// --- Rendering & Frame Pacing ------------------------------------------------------------------
// Present mode and vblank rate change what the player sees and the game's timing, so this
// brain only recommends (Scope::Never); the Safety Gate enforces that.
class FramePacingBrain final : public KnobBrain {
public:
    explicit FramePacingBrain(const EmulatorDefaults& d)
        : KnobBrain(Domain::FramePacing, "Rendering & Frame Pacing",
                    {KnobSpec{KnobId::PresentMode, Domain::FramePacing, Scope::Never,
                              d.present_mode, 0, 2, {0, 1, 2}, "0 Mailbox / 1 Fifo / 2 Immediate",
                              "swapchain present mode (emulator setting)"},
                     KnobSpec{KnobId::FrameLimit, Domain::FramePacing, Scope::Never,
                              d.frame_limit, 0, 240, {0, 30, 40, 45, 60}, "fps, 0 = off",
                              "frame rate limit (F3 settings > Graphics)"},
                     KnobSpec{KnobId::PerfMode, Domain::FramePacing, Scope::Never, d.perf_mode,
                              0, 2, {0, 1, 2}, "0 off / 1 low / 2 office",
                              "Second Son's own quality levels (F3 > Graphics, next launch)"}}) {}

    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>& v) override {
        const double frame_wait = Stall(s, StallKind::FrameWait);
        const double present = Stall(s, StallKind::Present);
        const double ratio = s.frame_ms_p50 > 0.0 ? s.frame_ms_p99 / s.frame_ms_p50 : 0.0;
        status = Fmt("p99/p50 %.2f, frame wait %.2f, present %.2f ms/frame", ratio, frame_wait,
                     present);
        std::vector<Proposal> out;
        if (!s.in_gameplay) {
            return out;
        }
        // Uneven pacing below ~45 fps: the game cannot keep up with a 60 Hz display, and vsync
        // (Fifo) would make frames alternate between one and two refreshes. A 30 fps limit shows
        // every frame for the same time instead (field data: GTX 1650, 30-35 fps, p99 1.8x).
        const bool slow = s.frame_ms_p50 > 22.0;
        if (ratio > 1.8 && slow && (v[1] == 0 || v[1] > 30)) {
            out.push_back(Propose(1, 30,
                                  Fmt("frame times are uneven (p99 %.2fx the median) at %.0f fps; "
                                      "a 30 fps limit (F3 > Graphics) shows every frame for the "
                                      "same time",
                                      ratio, 1000.0 / s.frame_ms_p50),
                                  0.1));
        }
        // Well under 30 fps with the graphics card as the limit: the game's own lower quality
        // level draws less (field data: GTX 1650, 19 fps in the city after the powers). It changes
        // the picture, so it is only ever recommended.
        const bool card_bound = s.bottleneck == Bottleneck::HostGpuBound ||
                                s.bottleneck == Bottleneck::MemoryPressure;
        if (card_bound && s.frame_ms_p50 > 36.0 && v[2] < 2) {
            out.push_back(Propose(2, v[2] + 1,
                                  Fmt("%.0f fps with the graphics card as the limit; the game's "
                                      "lower quality level draws less (next launch)",
                                      1000.0 / s.frame_ms_p50) +
                                      (v[2] == 0 ? " - low" : " - office"),
                                  0.15));
        }
        // Uneven pacing at higher rates while presentation is not the limit: Fifo paces frames
        // evenly.
        if (ratio > 1.8 && !slow && frame_wait < 0.5 && v[0] != 1 &&
            s.bottleneck != Bottleneck::PresentBound) {
            out.push_back(Propose(0, 1,
                                  Fmt("frame times are uneven (p99 %.2fx the median); Fifo "
                                      "(vsync) presentation paces frames evenly",
                                      ratio),
                                  0.1));
        }
        // Presentation waits dominate with Fifo: Mailbox removes the wait.
        if (s.bottleneck == Bottleneck::PresentBound && v[0] == 1) {
            out.push_back(Propose(0, 0,
                                  Fmt("%.1f ms/frame waiting for the swapchain; Mailbox removes "
                                      "the wait without tearing",
                                      frame_wait + present),
                                  frame_wait));
        }
        return out;
    }
};

// --- CPU Workload ------------------------------------------------------------------------------
// Owns the page-invalidation lookahead: how far ahead the emulator unprotects guest pages after
// a write fault, trading protection syscalls on the GPU thread against fault count on the
// game's threads.
class CpuWorkloadBrain final : public KnobBrain {
public:
    explicit CpuWorkloadBrain(const EmulatorDefaults& d)
        : KnobBrain(Domain::CpuWorkload, "CPU Workload",
                    {KnobSpec{KnobId::AheadMax, Domain::CpuWorkload, Scope::NextLaunch,
                              d.ahead_max, 1, 64, Unique({d.ahead_max, 8, 32}), "pages",
                              "page-invalidation lookahead after a write fault"}}) {}

    std::vector<Proposal> Think(const Snapshot& s, const std::vector<std::int64_t>& v) override {
        const double faults = Stall(s, StallKind::PageFault);
        const double protect_gpu = Stall(s, StallKind::PageProtectGpu);
        status = Fmt("page faults %.2f, GPU-thread page protect %.2f ms/frame", faults,
                     protect_gpu);
        std::vector<Proposal> out;
        if (s.bottleneck == Bottleneck::PageFaultBound) {
            if (const auto n = Step(Spec(0), v[0], +1)) {
                out.push_back(Propose(0, *n,
                                      Fmt("game threads spend %.1f ms/frame in page faults; look "
                                          "further ahead",
                                          faults),
                                      faults));
            }
        } else if (s.bottleneck == Bottleneck::GpuThreadBound && protect_gpu > 1.0) {
            if (const auto n = Step(Spec(0), v[0], -1)) {
                out.push_back(Propose(0, *n,
                                      Fmt("the GPU thread spends %.1f ms/frame changing page "
                                          "protection; look less far ahead",
                                          protect_gpu),
                                      protect_gpu));
            }
        }
        return out;
    }
};

} // namespace

std::vector<Proposal> GpuMemoryBrain::Think(const Snapshot& s, const std::vector<std::int64_t>&) {
    const auto& r = s.resources;
    pressure = r.vram_budget_bytes > 0
                   ? static_cast<double>(r.vram_used_bytes) / static_cast<double>(r.vram_budget_bytes)
                   : -1.0;
    over_budget_growing = have_last && r.over_budget_allocations > last_over_budget;
    last_over_budget = r.over_budget_allocations;
    have_last = true;
    veto = pressure >= 0.90 || over_budget_growing;
    return {};
}

std::string GpuMemoryBrain::Status() const {
    if (pressure < 0.0) {
        return "VRAM budget unknown (no VK_EXT_memory_budget)";
    }
    return Fmt("VRAM %.0f%% of budget", pressure * 100.0) +
           (over_budget_growing ? ", allocations past budget!" : "") +
           (veto ? " - vetoing changes that use more VRAM" : "");
}

std::unique_ptr<MiniBrain> MakeBufferGeometryBrain(const EmulatorDefaults& d) {
    return std::make_unique<BufferGeometryBrain>(d);
}
std::unique_ptr<MiniBrain> MakeTextureCacheBrain(const EmulatorDefaults& d,
                                                 const GpuMemoryBrain* memory) {
    return std::make_unique<TextureCacheBrain>(d, memory);
}
std::unique_ptr<MiniBrain> MakeTransferSyncBrain(const EmulatorDefaults& d) {
    return std::make_unique<TransferSyncBrain>(d);
}
std::unique_ptr<MiniBrain> MakeShaderPipelineBrain(const EmulatorDefaults& d) {
    return std::make_unique<ShaderPipelineBrain>(d);
}
std::unique_ptr<MiniBrain> MakeFramePacingBrain(const EmulatorDefaults& d) {
    return std::make_unique<FramePacingBrain>(d);
}
std::unique_ptr<MiniBrain> MakeCpuWorkloadBrain(const EmulatorDefaults& d) {
    return std::make_unique<CpuWorkloadBrain>(d);
}

} // namespace Adaptive
