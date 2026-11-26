// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/vram_monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <map>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "adaptive/emulator_glue.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace VideoCore::VramMonitor {

std::atomic<u64> buffer_resident_bytes{0};
std::atomic<u64> image_bytes{0};
std::atomic<u32> image_count{0};
std::atomic<u32> raised_image_blocks{0};
std::atomic<u64> lazy_zero_bytes{0};
std::atomic<u64> lazy_wake_binds{0};
std::atomic<u64> lazy_wake_unordered{0};
std::atomic<bool> shared_zero_on{false};
std::atomic<u64> shared_zero_bound_blocks{0};
std::atomic<u64> shared_zero_guard_wakes{0};
std::atomic<u64> last_used_bytes{0};
std::atomic<u64> last_budget_bytes{0};
std::atomic<double> last_sample_seconds{0.0};
std::atomic<u64> recent_peak_used_bytes{0};

namespace {

/// The GPU hang record counts seconds from its own start, which is static initialisation too.
const auto monitor_launch = std::chrono::steady_clock::now();

constexpr auto SampleInterval = std::chrono::seconds{2};
constexpr int SamplesPerReport = 15; // 30 s

constexpr u64 MB = 1ull << 20;

#ifdef _WIN32
/// This process's GPU memory and engine use as Windows counts it (Task Manager's numbers), from
/// the GPU performance counters. The adapter is the one this process holds the most video memory
/// on, which is the card the game runs on rather than a laptop's integrated one.
class WindowsCounters {
public:
    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            return false;
        }
        const bool ok =
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                                  &engines) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0,
                                  &dedicated) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared) ==
                ERROR_SUCCESS;
        if (!ok) {
            PdhCloseQuery(query);
            query = nullptr;
            return false;
        }
        prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        PdhCollectQueryData(query); // rates need two samples
        return true;
    }

    struct Sample {
        bool valid = false;
        u64 dedicated = 0;
        u64 shared = 0;
        std::map<std::string, double> engine_busy; ///< by engine type, busiest engine of it
    };

    Sample Read() {
        Sample out;
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return out;
        }
        // Memory first: it tells which adapter is the card.
        std::wstring card_luid;
        u64 best = 0;
        std::map<std::wstring, u64> shared_by_luid;
        for (const auto& [name, value] : Items(dedicated)) {
            const auto luid = Luid(name);
            if (luid.empty()) {
                continue;
            }
            if (static_cast<u64>(value) >= best) {
                best = static_cast<u64>(value);
                card_luid = luid;
            }
        }
        for (const auto& [name, value] : Items(shared)) {
            shared_by_luid[Luid(name)] += static_cast<u64>(value);
        }
        if (card_luid.empty()) {
            return out;
        }
        out.valid = true;
        out.dedicated = best;
        out.shared = shared_by_luid[card_luid];
        for (const auto& [name, value] : Items(engines)) {
            if (Luid(name) != card_luid) {
                continue;
            }
            const auto at = name.find(L"engtype_");
            if (at == std::wstring::npos) {
                continue;
            }
            std::string type;
            for (const wchar_t c : name.substr(at + 8)) {
                type += static_cast<char>(c < 128 ? c : '?');
            }
            if (type.empty()) {
                type = "unnamed";
            }
            auto& busy = out.engine_busy[type];
            busy = std::max(busy, value);
        }
        return out;
    }

private:
    /// The instances of this process, with their values.
    std::vector<std::pair<std::wstring, double>> Items(PDH_HCOUNTER counter) {
        std::vector<std::pair<std::wstring, double>> out;
        DWORD size = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) !=
            static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            return out;
        }
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) !=
            ERROR_SUCCESS) {
            return out;
        }
        for (DWORD i = 0; i < count; ++i) {
            const std::wstring name = items[i].szName ? items[i].szName : L"";
            if (name.rfind(prefix, 0) != 0 || items[i].FmtValue.CStatus != ERROR_SUCCESS) {
                continue;
            }
            out.emplace_back(name, items[i].FmtValue.doubleValue);
        }
        return out;
    }

    /// "pid_1234_luid_0x00000000_0x0000C3E1_phys_0..." -> "luid_0x00000000_0x0000C3E1".
    static std::wstring Luid(const std::wstring& name) {
        const auto at = name.find(L"luid_");
        if (at == std::wstring::npos) {
            return {};
        }
        const auto end = name.find(L"_phys", at);
        return name.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
    }

    PDH_HQUERY query{};
    PDH_HCOUNTER engines{};
    PDH_HCOUNTER dedicated{};
    PDH_HCOUNTER shared{};
    std::wstring prefix;
    std::vector<BYTE> buffer;
};
#endif

std::jthread g_thread;

/// Mains power and battery (build 51): a laptop switching to battery lowers its clocks, and that
/// stretch reads like the emulator slowing down. A log line at each switch, and a report line.
class PowerWatch {
public:
    void Poll() {
#ifdef _WIN32
        SYSTEM_POWER_STATUS status{};
        if (!GetSystemPowerStatus(&status) || status.ACLineStatus > 1) {
            return; // 255: unknown, a desktop without a battery says 1
        }
        const bool battery = status.ACLineStatus == 0;
        const auto now = std::chrono::steady_clock::now();
        const double minute = std::chrono::duration<double>(now - start).count() / 60.0;
        const int percent = status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : -1;
        if (!known) {
            known = true;
            on_battery = battery;
            since = now;
            LOG_INFO(Render, "Power: {} at the start{}", battery ? "on battery" : "on mains power",
                     percent >= 0 && status.BatteryFlag != 128
                         ? fmt::format(" (battery {}%)", percent)
                         : std::string{});
        } else if (battery != on_battery) {
            if (on_battery) {
                battery_seconds += std::chrono::duration<double>(now - since).count();
            }
            on_battery = battery;
            since = now;
            ++switches;
            LOG_INFO(Render,
                     "Power: {} at minute {:.1f}{}; frame rates measured {} come from a laptop "
                     "running {}",
                     battery ? "switched to battery" : "back on mains power", minute,
                     percent >= 0 ? fmt::format(" (battery {}%)", percent) : std::string{},
                     battery ? "from now on" : "before this",
                     battery ? "on battery, usually at lower clocks" : "on battery");
        }
        double on_battery_s = battery_seconds;
        if (on_battery) {
            on_battery_s += std::chrono::duration<double>(now - since).count();
        }
        if (switches > 0 || on_battery) {
            Adaptive::Glue::SetPowerNote(fmt::format(
                "on battery {:.1f} of {:.1f} minutes, {} switches between mains and battery; "
                "seconds on battery are usually slower because the laptop lowers its clocks",
                on_battery_s / 60.0, minute, switches));
        }
#endif
    }

private:
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point since{};
    bool known{};
    bool on_battery{};
    int switches{};
    double battery_seconds{};
};

void Run(std::stop_token stop, const Vulkan::Instance* instance) {
#ifdef _WIN32
    WindowsCounters windows;
    const bool have_windows = windows.Open();
    if (!have_windows) {
        LOG_WARNING(Render, "VRAM monitor: the Windows GPU counters are not available");
    }
#endif
    PowerWatch power;
    std::map<std::string, std::pair<double, double>> engine_sum_max; // over the report
    u64 dedicated_max = 0;
    u64 shared_max = 0;
    int samples = 0;
    int windows_samples = 0;
    u64 peak_this = 0;
    u64 peak_before = 0;
    std::mutex mutex;
    std::condition_variable_any cv;
    while (true) {
        {
            std::unique_lock lock{mutex};
            cv.wait_for(lock, stop, SampleInterval, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        power.Poll();
#ifdef _WIN32
        if (have_windows) {
            const auto s = windows.Read();
            if (s.valid) {
                ++windows_samples;
                dedicated_max = std::max(dedicated_max, s.dedicated);
                shared_max = std::max(shared_max, s.shared);
                for (const auto& [type, busy] : s.engine_busy) {
                    auto& [sum, max] = engine_sum_max[type];
                    sum += busy;
                    max = std::max(max, busy);
                }
            }
        }
#endif
        u64 used = 0;
        u64 budget = 0;
        const auto all = instance->GetHeapUsage();
        for (const auto& h : all) {
            if (h.device_local) {
                used += h.usage;
                budget += h.budget;
            }
        }
        // Every 2 s, for the GPU hang record (build 52): the peak is over this report and the
        // one before, so about the last 30 to 60 s.
        last_used_bytes.store(used);
        last_budget_bytes.store(budget);
        last_sample_seconds.store(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - monitor_launch)
                .count());
        peak_this = std::max(peak_this, used);
        recent_peak_used_bytes.store(std::max(peak_this, peak_before));
        if (++samples < SamplesPerReport) {
            continue;
        }
        peak_before = peak_this;
        peak_this = 0;
        std::string heaps;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const auto& h = all[i];
            heaps += fmt::format("{}{} {}: {}/{} of {} MB", heaps.empty() ? "" : ", ",
                                 h.device_local ? "video" : "system", i, h.usage / MB,
                                 h.budget / MB, h.size / MB);
        }
        std::string windows_text = "Windows counters not read";
        if (windows_samples > 0) {
            std::string engines;
            for (const auto& [type, sum_max] : engine_sum_max) {
                engines += fmt::format("{}{} {:.0f}/{:.0f}%", engines.empty() ? "" : ", ", type,
                                       sum_max.first / windows_samples, sum_max.second);
            }
            windows_text = fmt::format(
                "Windows: this process at most {} MB in video memory and {} MB in shared system "
                "memory; engines busy (average/most): {}",
                dedicated_max / MB, shared_max / MB, engines.empty() ? "none seen" : engines);
        }
        LOG_INFO(Render,
                 "VRAM: Vulkan video memory in use {} MB of a {} MB budget ({}); buffers made "
                 "resident {} MB ({} MB left out as zeros, bound to {}; {} binds of them waited "
                 "for earlier work, {} could not; {} GPU writes found on the shared zeros), "
                 "images {} MB in {}, {} memory blocks raised for images drawn to; {}",
                 used / MB, budget / MB, heaps, buffer_resident_bytes.load() / MB,
                 lazy_zero_bytes.load() / MB,
                 shared_zero_on.load() ? "the shared block of zeros" : "nothing",
                 lazy_wake_binds.load(), lazy_wake_unordered.load(),
                 shared_zero_guard_wakes.load(),
                 image_bytes.load() / MB, image_count.load(), raised_image_blocks.load(),
                 windows_text);
        samples = 0;
        windows_samples = 0;
        engine_sum_max.clear();
        dedicated_max = 0;
        shared_max = 0;
    }
}

} // namespace

void Start(const Vulkan::Instance& instance) {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    LOG_INFO(Render, "VRAM priorities: {}",
             PrioritiesOn(instance)
                 ? "on (buffers highest, memory of images drawn to higher than textures)"
                 : (instance.IsPageableDeviceLocalMemorySupported()
                        ? "off (SS_VRAM_PRIORITIES=1 turns them on; slower on a GTX 1650)"
                        : "not available (the driver takes no memory priorities)"));
    g_thread = std::jthread{[&instance](std::stop_token stop) { Run(stop, &instance); }};
}

void Stop() {
    if (g_thread.joinable()) {
        g_thread.request_stop();
        g_thread.join();
    }
}

bool PrioritiesOn(const Vulkan::Instance& instance) {
    // Only with SS_VRAM_PRIORITIES=1 since build 43: on a GTX 1650 they made Windows move twice
    // as much memory (copy engine 10-17% busy against 5-8%) and gameplay ran at 17.3 fps against
    // 29.9, as buffers and images drawn to at the higher priorities did not fit.
    static const bool on = [&] {
        const char* v = std::getenv("SS_VRAM_PRIORITIES");
        return v && v[0] == '1' && instance.IsPageableDeviceLocalMemorySupported();
    }();
    return on;
}

} // namespace VideoCore::VramMonitor
