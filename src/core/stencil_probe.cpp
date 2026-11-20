// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/stencil_probe.h"

#include <atomic>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/mouse_aim.h"
#include "input/controller.h"
#include "input/input_mouse.h"

#ifdef _WIN32
#include <chrono>
#include <mutex>
#include <thread>
#include <windows.h>
#endif

namespace Core::StencilProbe {

namespace {

std::atomic<VAddr> g_base{0};
std::atomic<u64> g_size{0};

bool Wanted() {
    static const bool wanted = [] {
        const char* v = std::getenv("SS_STENCIL_PROBE");
        return !(v && v[0] == '0');
    }();
    return wanted;
}

#ifdef _WIN32

/// Gameplay (the camera hook drives the camera), the spray can held upright, held sideways.
constexpr std::size_t NumBuckets = 3;
constexpr const char* BucketNames[NumBuckets] = {"gameplay", "spray can upright",
                                                 "spray can sideways"};
/// About four minutes of each at 250 samples a second, gameplay less.
constexpr u32 MaxSamples[NumBuckets] = {15000, 60000, 60000};
constexpr auto SampleInterval = std::chrono::milliseconds{4};
constexpr auto StopAfter = std::chrono::minutes{45};
constexpr std::size_t StackBytes = 32 * 1024;
constexpr std::size_t MaxKeys = 200000;

struct State {
    std::mutex mutex;
    HANDLE thread{};
    std::vector<std::unordered_map<u64, u32>> counts =
        std::vector<std::unordered_map<u64, u32>>(NumBuckets);
    std::vector<u32> samples = std::vector<u32>(NumBuckets, 0);
    std::jthread sampler;
};

State& GetState() {
    static State state;
    return state;
}

/// Which bucket the thread would be sampled for now, or -1: the spray can buckets only while
/// the mouse turns the motion sensor and moved a moment ago, so menus and cut-scenes stay out.
int CurrentBucket() {
    if (!Core::MouseAim::Installed()) {
        return -1;
    }
    if (Core::MouseAim::CameraLive()) {
        return 0;
    }
    if (!Input::MouseTurnsMotionSensor() || Input::MsSinceMouseMotion() > 500) {
        return -1;
    }
    return Input::CurrentSprayHold() == Input::SprayHold::Normal ? 1 : 2;
}

void Sample(State& state, std::vector<u64>& words, std::vector<u64>& found) {
    const int bucket = CurrentBucket();
    if (bucket < 0) {
        return;
    }
    {
        std::scoped_lock lock{state.mutex};
        if (state.samples[bucket] >= MaxSamples[bucket]) {
            return;
        }
    }
    const VAddr base = g_base.load(std::memory_order_relaxed);
    const u64 size = g_size.load(std::memory_order_relaxed);
    if (base == 0 || size == 0) {
        return;
    }
    // Suspended only to copy the registers and the stack: nothing is allocated or locked meanwhile
    // (the thread may hold the heap's lock).
    words.clear();
    std::size_t copied = 0;
    if (SuspendThread(state.thread) == static_cast<DWORD>(-1)) {
        return;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(state.thread, &context)) {
        words.push_back(context.Rip);
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const u64 end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
            const u64 rsp = context.Rsp & ~u64{7};
            copied = static_cast<std::size_t>(std::min<u64>(end - rsp, StackBytes)) / 8;
            const auto* stack = reinterpret_cast<const u64*>(rsp);
            for (std::size_t i = 0; i < copied && words.size() < words.capacity(); ++i) {
                words.push_back(stack[i]);
            }
        }
    }
    ResumeThread(state.thread);

    // The game's addresses in it, each counted once a sample.
    found.clear();
    for (const u64 w : words) {
        if (w >= base && w < base + size) {
            found.push_back(w - base);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::scoped_lock lock{state.mutex};
    auto& counts = state.counts[bucket];
    for (const u64 offset : found) {
        const auto it = counts.find(offset);
        if (it != counts.end()) {
            ++it->second;
        } else if (counts.size() < MaxKeys) {
            counts.emplace(offset, 1);
        }
    }
    ++state.samples[bucket];
}

void Run(std::stop_token stop) {
    auto& state = GetState();
    std::vector<u64> words;
    words.reserve(StackBytes / 8 + 1);
    std::vector<u64> found;
    found.reserve(StackBytes / 8 + 1);
    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    u32 reported_sideways = 0;
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(SampleInterval);
        Sample(state, words, found);
        const auto now = std::chrono::steady_clock::now();
        bool full = true;
        u32 sideways = 0;
        {
            std::scoped_lock lock{state.mutex};
            for (std::size_t b = 0; b < NumBuckets; ++b) {
                full = full && state.samples[b] >= MaxSamples[b];
            }
            sideways = state.samples[2];
        }
        const bool done = full || now - start >= StopAfter;
        if (done || (now - last_report >= std::chrono::minutes{1} && sideways != reported_sideways)) {
            last_report = now;
            reported_sideways = sideways;
            Report();
        }
        if (done) {
            LOG_INFO(Core, "Stencil probe: done sampling");
            return;
        }
    }
}

#endif

} // namespace

void SetGameCode(VAddr base, u64 size) {
    g_base.store(base, std::memory_order_relaxed);
    g_size.store(size, std::memory_order_relaxed);
}

void NoteGameThread() {
#ifdef _WIN32
    static std::atomic<bool> noted{false};
    if (!Wanted() || noted.exchange(true)) {
        return;
    }
    auto& state = GetState();
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_LIMITED_INFORMATION,
                         FALSE, 0)) {
        LOG_WARNING(Core, "Stencil probe: cannot sample the game's main thread");
        return;
    }
    state.thread = handle;
    state.sampler = std::jthread{Run};
    LOG_INFO(Core, "Stencil probe: sampling the game's main thread while the spray can is out "
                   "(SS_STENCIL_PROBE=0 turns it off)");
#endif
}

void Report() {
#ifdef _WIN32
    auto& state = GetState();
    std::vector<std::unordered_map<u64, u32>> counts;
    std::vector<u32> samples;
    {
        std::scoped_lock lock{state.mutex};
        counts = state.counts;
        samples = state.samples;
    }
    LOG_INFO(Core, "Stencil probe: {} samples in gameplay, {} with the spray can upright, {} sideways",
             samples[0], samples[1], samples[2]);
    for (const std::size_t bucket : {std::size_t{2}, std::size_t{1}}) {
        if (samples[bucket] < 500) {
            continue;
        }
        const auto found = Distinctive(counts, samples, bucket, 0.2, 0.02, 25);
        std::string text;
        for (const auto& c : found) {
            text += fmt::format("{}{:#x} {:.0f}% ({:.1f}%)", text.empty() ? "" : ", ", c.offset,
                                c.share * 100.0, c.other_max * 100.0);
        }
        LOG_INFO(Core, "Stencil probe: only with the {}: {}", BucketNames[bucket],
                 text.empty() ? std::string{"nothing yet"} : text);
    }
#endif
}

} // namespace Core::StencilProbe
