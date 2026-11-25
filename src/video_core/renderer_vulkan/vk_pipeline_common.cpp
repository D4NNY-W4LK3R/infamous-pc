// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <boost/container/static_vector.hpp>
#include <fmt/format.h>

#include "common/perf_profiler.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace PipelineTiming {
namespace {
const std::chrono::steady_clock::time_point launch = std::chrono::steady_clock::now();
std::atomic<bool> preloading{false};

struct Counts {
    std::atomic<u64> created{};
    std::atomic<u64> hits{};
    std::atomic<u64> misses{};
    std::atomic<u64> no_feedback{};
    std::atomic<u64> compute{};
    std::atomic<u64> hit_ns{};
    std::atomic<u64> miss_ns{};
    std::atomic<u64> longest_ns{};
    std::atomic<u64> slow{}; // 100 ms or more
};
Counts preload_counts;
Counts game_counts;
std::atomic<u64> module_ns{};
std::atomic<u64> database_ns{};

u64 Ns(std::chrono::steady_clock::duration d) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}
} // namespace

double SecondsSinceLaunch() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count();
}

void SetPreloading(bool value) noexcept {
    preloading.store(value, std::memory_order_release);
}

void NoteCreation(bool compute, const vk::PipelineCreationFeedback& feedback,
                  std::chrono::steady_clock::duration took) noexcept {
    Counts& c = preloading.load(std::memory_order_acquire) ? preload_counts : game_counts;
    const u64 ns = Ns(took);
    c.created.fetch_add(1, std::memory_order_relaxed);
    c.compute.fetch_add(compute ? 1 : 0, std::memory_order_relaxed);
    if (!(feedback.flags & vk::PipelineCreationFeedbackFlagBits::eValid)) {
        c.no_feedback.fetch_add(1, std::memory_order_relaxed);
    } else if (feedback.flags & vk::PipelineCreationFeedbackFlagBits::eApplicationPipelineCacheHit) {
        c.hits.fetch_add(1, std::memory_order_relaxed);
        c.hit_ns.fetch_add(ns, std::memory_order_relaxed);
    } else {
        c.misses.fetch_add(1, std::memory_order_relaxed);
        c.miss_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    if (ns >= 100'000'000) {
        c.slow.fetch_add(1, std::memory_order_relaxed);
    }
    u64 longest = c.longest_ns.load(std::memory_order_relaxed);
    while (ns > longest &&
           !c.longest_ns.compare_exchange_weak(longest, ns, std::memory_order_relaxed)) {
    }
}

void NoteModule(std::chrono::steady_clock::duration took) noexcept {
    if (preloading.load(std::memory_order_acquire)) {
        module_ns.fetch_add(Ns(took), std::memory_order_relaxed);
    }
}

void NoteDatabaseRead(std::chrono::steady_clock::duration took) noexcept {
    database_ns.fetch_add(Ns(took), std::memory_order_relaxed);
}

u64 Created(bool preload) noexcept {
    return (preload ? preload_counts : game_counts).created.load(std::memory_order_relaxed);
}

u64 ModuleNanoseconds() noexcept {
    return module_ns.load(std::memory_order_relaxed);
}

u64 DatabaseNanoseconds() noexcept {
    return database_ns.load(std::memory_order_relaxed);
}

std::string Summary(bool preload) {
    const Counts& c = preload ? preload_counts : game_counts;
    const u64 created = c.created.load();
    const u64 hits = c.hits.load();
    const u64 misses = c.misses.load();
    const auto avg_ms = [](u64 ns, u64 n) { return n ? static_cast<double>(ns) / n / 1e6 : 0.0; };
    return fmt::format(
        "{} pipelines created by the driver ({} compute): {} from its saved cache ({:.0f}%, "
        "{:.2f} ms each on average, {:.1f} s in all), {} compiled anew ({:.1f} ms each on "
        "average, {:.1f} s in all), {} without the driver saying; longest {:.0f} ms, {} took "
        "100 ms or more (times summed over the compiler threads)",
        created, c.compute.load(), hits, created ? hits * 100.0 / created : 0.0,
        avg_ms(c.hit_ns.load(), hits), c.hit_ns.load() / 1e9, misses,
        avg_ms(c.miss_ns.load(), misses), c.miss_ns.load() / 1e9, c.no_feedback.load(),
        c.longest_ns.load() / 1e6, c.slow.load());
}
} // namespace PipelineTiming

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache_,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      pipeline_cache{pipeline_cache_}, is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    CancelCompile();
}

void Pipeline::CancelCompile() noexcept {
    if (compile_job) {
        compile_job->Cancel();
    }
    std::scoped_lock lock{optimize_mutex};
    if (optimize_job) {
        optimize_job->Cancel();
    }
}

void Pipeline::SwapInOptimized() const {
    optimize_job.reset();
    if (optimized_pipeline) {
        // Commands recorded so far may still use the old one.
        scheduler.DeferOperation([old = std::move(pipeline)]() mutable { old.reset(); });
        pipeline = std::move(optimized_pipeline);
    }
    has_optimize_job.store(false, std::memory_order_release);
}

void Pipeline::WaitReady() const {
    if (!compile_job || compile_job->IsDone()) {
        return;
    }
    Common::Perf::ScopedStall stall{Common::Perf::Stall::PipelineWait};
    compile_job->Wait();
}

void Pipeline::LogPipelineCreation(std::string_view kind, std::string_view debug_str,
                                   std::chrono::steady_clock::time_point start) const {
    // Creation this slow is worth seeing in the log, the rest only clutters it.
    constexpr auto SlowCreation = std::chrono::milliseconds{5};

    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!PipelineCompiler::IsCompilerThread()) {
        Common::Perf::Record(Common::Perf::Stall::PipelineCreate,
                             std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    if (elapsed >= SlowCreation && !preloaded) {
        LOG_INFO(Render_Vulkan, "Created {} pipeline {} in {:.1f} ms", kind, debug_str, elapsed_ms);
    } else {
        LOG_DEBUG(Render_Vulkan, "Created {} pipeline {} in {:.1f} ms", kind, debug_str,
                  elapsed_ms);
    }
}

bool Pipeline::RetryPipelineCreation(vk::Result result, u32 attempt, std::string_view kind,
                                     std::string_view debug_str) {
    // With the driver's cache cold, as after an update, the compiler threads build hundreds of
    // pipelines at once while the game loads, and the driver failed one of them now and then with
    // an unknown error, which ended the game. Building it again a little later can succeed.
    constexpr u32 MaxAttempts = 5;
    const bool may_recover = result == vk::Result::eErrorUnknown ||
                             result == vk::Result::eErrorOutOfHostMemory ||
                             result == vk::Result::eErrorOutOfDeviceMemory ||
                             result == vk::Result::eErrorInitializationFailed;
    if (!may_recover || attempt >= MaxAttempts) {
        return false;
    }
    LOG_WARNING(Render_Vulkan, "Creating {} pipeline {} failed with {}, trying again", kind,
                debug_str, vk::to_string(result));
    std::this_thread::sleep_for(std::chrono::milliseconds{100} * attempt);
    return true;
}

void Pipeline::BindResources(Scheduler& target, DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = target.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    target.GetDynamicState().PushConstants(cmdbuf, *pipeline_layout, IsCompute(), stage_flags,
                                           &push_data, sizeof(push_data));

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    cmdbuf.updateAndBindDescriptorSet(instance.GetDevice(), bind_point, *pipeline_layout, desc_set,
                                      set_writes);
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
