// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/buffer_cache/copy_back_log.h"

namespace VideoCore::CopyBackLog {
namespace {

using Clock = std::chrono::steady_clock;

constexpr u64 PageSize = 4096;
/// A sample is compared this long after it was taken: the game fills what it writes within the
/// frame it starts it in (33 ms at 30 fps).
constexpr auto CompareAfter = std::chrono::milliseconds{70};
constexpr auto SampleEvery = std::chrono::milliseconds{20};
constexpr size_t MaxPendingSamples = 64;
constexpr u64 MaxSampleBytes = 512 * 1024;
constexpr size_t MaxWindows = 8192;
constexpr auto ReportEvery = std::chrono::seconds{30};

struct FaultContext {
    u64 rip{};
    u64 address{};
    bool is_write{};
    bool set{};
};
thread_local FaultContext fault{};
thread_local u64 last_page{};

struct Window {
    u64 start{};
    u64 end{};
    u64 count{};
    u64 writes{};
    u64 gpu_thread{};
    u64 following{};
    u64 wait_ns{};
    u64 max_ns{};
    std::array<std::pair<u64, u64>, 4> rips{};
    // Samples of copy-backs on a write, compared later.
    u64 sampled{};
    u64 superseded{};
    u64 gpu_again{};
    u64 unreadable{};
    u64 checked{};
    double page_changed{};
    double window_changed{};
    u64 page_mostly{}; // 90% or more of the touched page changed
    u64 page_none{};   // nothing of it changed

    void AddRip(u64 rip) {
        if (rip == 0) {
            return;
        }
        for (auto& [r, n] : rips) {
            if (r == rip) {
                ++n;
                return;
            }
        }
        auto least = std::ranges::min_element(rips, {}, &std::pair<u64, u64>::second);
        if (least->second <= 1) {
            *least = {rip, 1};
        }
    }
};

struct Totals {
    u64 game{};
    u64 game_writes{};
    u64 game_wait_ns{};
    u64 gpu{};
    u64 gpu_wait_ns{};
    u64 checked{};
    u64 page_mostly{};
    u64 page_none{};
    double page_changed{};
    double window_changed{};
};

struct Sample {
    u64 start{};
    u64 end{};
    u64 address{};
    Clock::time_point taken{};
    std::vector<u8> data;
    bool superseded{};
};

struct Result {
    u64 start{};
    enum class Kind { Checked, Superseded, GpuAgain, Unreadable } kind{};
    double page_changed{};
    double window_changed{};
};

std::mutex mutex;
std::unordered_map<u64, Window> interval;
std::unordered_map<u64, Window> session;
Totals interval_totals;
Totals session_totals;
std::deque<Sample> samples;
Clock::time_point last_sample{};
Clock::time_point last_report = Clock::now();
const Clock::time_point launch = Clock::now();

Window& Get(std::unordered_map<u64, Window>& map, u64 start, u64 end) {
    auto it = map.find(start);
    if (it == map.end()) {
        if (map.size() >= MaxWindows) {
            // Too many to tell apart: the rest are counted together.
            auto& other = map[0];
            other.start = 0;
            other.end = 0;
            return other;
        }
        it = map.emplace(start, Window{.start = start, .end = end}).first;
    }
    it->second.end = std::max(it->second.end, end);
    return it->second;
}

void Apply(Window& w, const Result& r) {
    switch (r.kind) {
    case Result::Kind::Superseded:
        ++w.superseded;
        break;
    case Result::Kind::GpuAgain:
        ++w.gpu_again;
        break;
    case Result::Kind::Unreadable:
        ++w.unreadable;
        break;
    case Result::Kind::Checked:
        ++w.checked;
        w.page_changed += r.page_changed;
        w.window_changed += r.window_changed;
        w.page_mostly += r.page_changed >= 0.9 ? 1 : 0;
        w.page_none += r.page_changed == 0.0 ? 1 : 0;
        break;
    }
}

void Apply(Totals& t, const Result& r) {
    if (r.kind != Result::Kind::Checked) {
        return;
    }
    ++t.checked;
    t.page_changed += r.page_changed;
    t.window_changed += r.window_changed;
    t.page_mostly += r.page_changed >= 0.9 ? 1 : 0;
    t.page_none += r.page_changed == 0.0 ? 1 : 0;
}

std::string Describe(const Window& w) {
    std::string s = w.start == 0
                        ? std::string("  other windows")
                        : fmt::format("  {:#x} ({} KB)", w.start, (w.end - w.start) / 1024);
    s += fmt::format(": {} copy-backs ({} on writes, {} for the GPU thread), waited {:.1f} ms "
                     "({:.1f} ms longest), {:.0f}% on the page after the last one",
                     w.count, w.writes, w.gpu_thread, w.wait_ns / 1e6, w.max_ns / 1e6,
                     w.count ? w.following * 100.0 / w.count : 0.0);
    std::vector<std::pair<u64, u64>> rips;
    for (const auto& rip : w.rips) {
        if (rip.first) {
            rips.push_back(rip);
        }
    }
    std::ranges::sort(rips, std::greater{}, &std::pair<u64, u64>::second);
    if (!rips.empty()) {
        s += "; code";
        for (const auto& [rip, n] : rips) {
            s += fmt::format(" {:#x} ({}x)", rip, n);
        }
    }
    if (w.sampled) {
        s += fmt::format("; after a write, sampled {}", w.sampled);
        if (w.checked) {
            s += fmt::format(": the touched page {:.0f}% rewritten on average ({} of {} 90%+, {} "
                             "not at all), the window {:.0f}%",
                             w.page_changed * 100.0 / w.checked, w.page_mostly, w.checked,
                             w.page_none, w.window_changed * 100.0 / w.checked);
        }
        if (w.superseded || w.gpu_again || w.unreadable) {
            s += fmt::format(" ({} copied back again, {} written by the GPU again, {} unreadable "
                             "before the check)",
                             w.superseded, w.gpu_again, w.unreadable);
        }
    }
    return s;
}

void Log(const char* title, const std::unordered_map<u64, Window>& map, const Totals& t,
         double seconds, size_t top) {
    LOG_INFO(Render,
             "Copy-backs ({}): game threads waited for {} ({} on writes), {:.0f} ms in all, "
             "{:.1f} ms a second; the GPU thread for {}, {:.0f} ms; {} windows",
             title, t.game, t.game_writes, t.game_wait_ns / 1e6,
             seconds > 0 ? t.game_wait_ns / 1e6 / seconds : 0.0, t.gpu, t.gpu_wait_ns / 1e6,
             map.size());
    if (t.checked) {
        LOG_INFO(Render,
                 "Copy-backs ({}): of {} copied back for a write and checked {} ms later, the "
                 "game rewrote the touched page {:.0f}% on average, 90% or more of it in {:.0f}% "
                 "of them and none of it in {:.0f}%; the whole window {:.0f}%",
                 title, t.checked, CompareAfter.count(), t.page_changed * 100.0 / t.checked,
                 t.page_mostly * 100.0 / t.checked, t.page_none * 100.0 / t.checked,
                 t.window_changed * 100.0 / t.checked);
    }
    std::vector<const Window*> windows;
    windows.reserve(map.size());
    for (const auto& [_, w] : map) {
        windows.push_back(&w);
    }
    const size_t n = std::min(top, windows.size());
    std::partial_sort(windows.begin(), windows.begin() + n, windows.end(),
                      [](const Window* a, const Window* b) { return a->wait_ns > b->wait_ns; });
    for (size_t i = 0; i < n; ++i) {
        LOG_INFO(Render, "Copy-backs ({}): {}", title, Describe(*windows[i]));
    }
}

double Changed(const u8* before, const u8* now, u64 size) {
    if (size == 0) {
        return 0.0;
    }
    u64 changed = 0;
    for (u64 i = 0; i < size; ++i) {
        changed += before[i] != now[i] ? 1 : 0;
    }
    return static_cast<double>(changed) / static_cast<double>(size);
}

} // namespace

void SetFault(u64 rip, u64 address, bool is_write) noexcept {
    fault = {.rip = rip, .address = address, .is_write = is_write, .set = true};
}

void ClearFault() noexcept {
    fault.set = false;
}

void NoteCopyBack(Core::MemoryManager* memory, u64 address, u64 size, u64 window_start,
                  u64 window_end, bool is_write, bool gpu_thread, u64 wait_ns) {
    if (window_end <= window_start) {
        return;
    }
    const u64 rip = fault.set ? fault.rip : 0;
    const u64 page = address / PageSize;
    const bool following = !gpu_thread && page == last_page + 1;
    if (!gpu_thread) {
        last_page = page;
    }
    const auto now = Clock::now();
    bool take_sample = false;
    {
        std::scoped_lock lock{mutex};
        for (auto* map : {&interval, &session}) {
            Window& w = Get(*map, window_start, window_end);
            ++w.count;
            w.writes += is_write ? 1 : 0;
            w.gpu_thread += gpu_thread ? 1 : 0;
            w.following += following ? 1 : 0;
            w.wait_ns += wait_ns;
            w.max_ns = std::max(w.max_ns, wait_ns);
            w.AddRip(rip);
        }
        for (auto* t : {&interval_totals, &session_totals}) {
            if (gpu_thread) {
                ++t->gpu;
                t->gpu_wait_ns += wait_ns;
            } else {
                ++t->game;
                t->game_writes += is_write ? 1 : 0;
                t->game_wait_ns += wait_ns;
            }
        }
        // Only copies back for a write can have been unneeded: the game read the others.
        if (is_write && !gpu_thread && memory && samples.size() < MaxPendingSamples &&
            now - last_sample >= SampleEvery && window_end - window_start <= MaxSampleBytes) {
            last_sample = now;
            take_sample = true;
        }
    }
    if (!take_sample) {
        return;
    }
    // What was copied back, before the instruction that faulted writes: it goes on once this
    // returns. Read through the backing, as the pages may still be protected.
    Sample sample{.start = window_start, .end = window_end, .address = address, .taken = now};
    sample.data.resize(window_end - window_start);
    if (!memory->TryReadBacking(reinterpret_cast<const void*>(window_start), sample.data.data(),
                                sample.data.size())) {
        return;
    }
    std::scoped_lock lock{mutex};
    for (auto* map : {&interval, &session}) {
        ++Get(*map, window_start, window_end).sampled;
    }
    samples.push_back(std::move(sample));
}

void NoteWrittenBack(u64 start, u64 end) noexcept {
    std::scoped_lock lock{mutex};
    for (auto& sample : samples) {
        if (sample.start < end && start < sample.end) {
            sample.superseded = true;
        }
    }
}

void Tick(Core::MemoryManager* memory, const std::function<bool(u64, u64)>& gpu_modified) {
    const auto now = Clock::now();
    std::vector<Sample> due;
    {
        std::scoped_lock lock{mutex};
        while (!samples.empty() && now - samples.front().taken >= CompareAfter) {
            due.push_back(std::move(samples.front()));
            samples.pop_front();
        }
    }
    std::vector<Result> results;
    results.reserve(due.size());
    std::vector<u8> current;
    for (const auto& sample : due) {
        Result r{.start = sample.start};
        if (sample.superseded) {
            r.kind = Result::Kind::Superseded;
        } else if (gpu_modified(sample.start, sample.end - sample.start)) {
            r.kind = Result::Kind::GpuAgain;
        } else {
            current.resize(sample.data.size());
            if (!memory || !memory->TryReadBacking(reinterpret_cast<const void*>(sample.start),
                                                   current.data(), current.size())) {
                r.kind = Result::Kind::Unreadable;
            } else {
                r.kind = Result::Kind::Checked;
                const u64 page_start = std::max(sample.start, sample.address / PageSize * PageSize);
                const u64 page_end = std::min(sample.end, page_start + PageSize);
                const u64 offset = page_start - sample.start;
                r.page_changed = Changed(sample.data.data() + offset, current.data() + offset,
                                         page_end > page_start ? page_end - page_start : 0);
                r.window_changed = Changed(sample.data.data(), current.data(), current.size());
            }
        }
        results.push_back(r);
    }

    std::scoped_lock lock{mutex};
    for (const auto& r : results) {
        // A sample may have been for a window counted with the others.
        for (auto* map : {&interval, &session}) {
            const auto it = map->find(r.start);
            Apply(it != map->end() ? it->second : (*map)[0], r);
        }
        Apply(interval_totals, r);
        Apply(session_totals, r);
    }
    if (now - last_report < ReportEvery) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - last_report).count();
    last_report = now;
    if (interval_totals.game || interval_totals.gpu) {
        Log("last 30 s", interval, interval_totals, seconds, 8);
    }
    interval.clear();
    interval_totals = {};
}

void Report() {
    std::scoped_lock lock{mutex};
    const double seconds = std::chrono::duration<double>(Clock::now() - launch).count();
    Log("whole session", session, session_totals, seconds, 16);
}

} // namespace VideoCore::CopyBackLog
