// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_gpu_hang.h"
#include "video_core/vram_monitor.h"

namespace Vulkan::GpuHang {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point launch = Clock::now();

double SecondsSinceLaunch(Clock::time_point t = Clock::now()) {
    return std::chrono::duration<double>(t - launch).count();
}

/// What a checkpoint stands for. Written by the thread recording the work, read when the device
/// is lost; an entry whose seq doesn't match was written over by later work.
struct Entry {
    std::atomic<u64> seq{0};
    u64 hash{};
    u64 hash2{};
    u64 count{};
    u64 tick{};
    float t{};
    Work work{};
    bool compute_queue{};
};
constexpr u64 NumEntries = 1u << 15;
std::array<Entry, NumEntries> entries{};
std::atomic<u64> next_seq{1};

struct Op {
    std::atomic<u64> n{0};
    u64 value{};
    u64 extra{};
    float t{};
    QueueOp op{};
};
constexpr u64 NumOps = 128;
std::array<Op, NumOps> ops{};
std::atomic<u64> next_op{1};

/// The last shader and pipeline compiles (build 52).
struct Compile {
    std::atomic<u64> n{0};
    u64 hash{};
    float t{};
    const char* kind{};
};
constexpr u64 NumCompiles = 64;
std::array<Compile, NumCompiles> compiles{};
std::atomic<u64> next_compile{1};

vk::Device device_handle{};
vk::Queue graphics_queue{};
vk::Queue compute_queue{};
vk::Queue async_queue{};
bool checkpoints_on = false;
bool fault_on = false;
std::once_flag record_once;

std::string_view WorkName(Work work) {
    switch (work) {
    case Work::CommandBuffer:
        return "command buffer start";
    case Work::Draw:
        return "draw";
    case Work::DrawIndirect:
        return "indirect draw";
    case Work::Dispatch:
        return "dispatch";
    case Work::DispatchIndirect:
        return "indirect dispatch";
    case Work::CopyBack:
        return "copy back";
    case Work::Upload:
        return "upload";
    case Work::Clear:
        return "clear";
    case Work::Present:
        return "present blit";
    }
    return "?";
}

std::string_view OpName(QueueOp op) {
    switch (op) {
    case QueueOp::GraphicsSubmit:
        return "graphics submit, tick";
    case QueueOp::ComputeSubmit:
        return "compute submit, tick";
    case QueueOp::SparseBind:
        return "sparse bind (empty buffer memory left out), blocks";
    case QueueOp::Present:
        return "present, frame";
    case QueueOp::SparseBindZeros:
        return "sparse bind, also to the shared block of zeros, binds";
    }
    return "?";
}

std::string Describe(u64 seq) {
    const Entry& e = entries[seq % NumEntries];
    if (e.seq.load(std::memory_order_acquire) != seq) {
        return fmt::format("#{} (details overwritten)", seq);
    }
    std::string s = fmt::format("#{} {} at {:.1f} s, tick {}{}", seq, WorkName(e.work), e.t, e.tick,
                                e.compute_queue ? ", compute queue" : "");
    switch (e.work) {
    case Work::Draw:
    case Work::DrawIndirect:
        s += fmt::format(", pixel shader {:#x}, vertex shader {:#x}", e.hash, e.hash2);
        if (e.work == Work::Draw) {
            s += fmt::format(", {} vertices x instances", e.count);
        }
        break;
    case Work::Dispatch:
    case Work::DispatchIndirect:
        s += fmt::format(", compute shader {:#x}", e.hash);
        if (e.work == Work::Dispatch) {
            s += fmt::format(", {} groups", e.count);
        }
        break;
    case Work::CopyBack:
    case Work::Upload:
    case Work::Clear:
        s += fmt::format(", address {:#x}, {} bytes", e.hash, e.count);
        break;
    default:
        break;
    }
    return s;
}

void Checkpoints(vk::Queue queue, std::string_view name, std::vector<std::string>& out) {
    if (!queue || !checkpoints_on) {
        return;
    }
    u32 count = 0;
    queue.getCheckpointDataNV(&count, nullptr);
    std::vector<vk::CheckpointDataNV> data(count);
    if (count) {
        queue.getCheckpointDataNV(&count, data.data());
        data.resize(count);
    }
    if (data.empty()) {
        out.push_back(fmt::format("{} queue: no checkpoints reported", name));
        return;
    }
    // The driver reports, per pipeline stage, the last checkpoint the work reached there: work
    // past the one at the top of the pipe had not started, and work up to the one at the bottom
    // had finished. What lies between was running when the card stopped.
    u64 top = 0;
    u64 bottom = 0;
    for (const auto& d : data) {
        const u64 seq = reinterpret_cast<u64>(d.pCheckpointMarker);
        out.push_back(fmt::format("{} queue, {}: {}", name, vk::to_string(d.stage),
                                  seq ? Describe(seq) : std::string("(none)")));
        if (d.stage & vk::PipelineStageFlagBits::eTopOfPipe) {
            top = std::max(top, seq);
        }
        if (d.stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
            bottom = std::max(bottom, seq);
        }
    }
    if (top == 0) {
        return;
    }
    const u64 first = bottom ? bottom + 1 : top;
    if (first > top) {
        out.push_back(fmt::format("{} queue: all work it started had finished (last #{}): the "
                                  "card stopped between work, not inside a draw or dispatch",
                                  name, top));
        return;
    }
    out.push_back(fmt::format("{} queue: running when the card stopped ({} pieces of work):", name,
                              top - first + 1));
    // The work recorded on this queue in between, the last 24 of it at most.
    std::vector<std::string> running;
    const bool on_compute = name != "graphics";
    for (u64 seq = top, steps = 0; seq >= first && running.size() < 24 && steps < 100000;
         --seq, ++steps) {
        const Entry& e = entries[seq % NumEntries];
        if (e.seq.load(std::memory_order_acquire) == seq && e.compute_queue != on_compute) {
            continue;
        }
        running.push_back("  " + Describe(seq));
    }
    std::reverse(running.begin(), running.end());
    out.insert(out.end(), running.begin(), running.end());
}

void DeviceFault(std::vector<std::string>& out) {
    if (!fault_on || !VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT) {
        out.push_back("device fault report: not available from this driver");
        return;
    }
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
    if (get(device_handle, &counts, nullptr) != VK_SUCCESS) {
        out.push_back("device fault report: none");
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = addresses.empty() ? nullptr : addresses.data(),
        .pVendorInfos = vendors.empty() ? nullptr : vendors.data(),
    };
    const VkResult r = get(device_handle, &counts, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        out.push_back(fmt::format("device fault report: failed ({})", static_cast<int>(r)));
        return;
    }
    out.push_back(fmt::format("device fault report: {}", info.description));
    for (u32 i = 0; i < counts.addressInfoCount; ++i) {
        const auto& a = addresses[i];
        out.push_back(
            fmt::format("  address {:#x} (+/- {:#x}), {}", a.reportedAddress, a.addressPrecision,
                        vk::to_string(static_cast<vk::DeviceFaultAddressTypeEXT>(a.addressType))));
    }
    for (u32 i = 0; i < counts.vendorInfoCount; ++i) {
        const auto& v = vendors[i];
        out.push_back(fmt::format("  vendor: {} (code {:#x}, data {:#x})", v.description,
                                  v.vendorFaultCode, v.vendorFaultData));
    }
}

} // namespace

void Init(vk::Device device, vk::Queue graphics, vk::Queue compute, vk::Queue async,
          bool checkpoints, bool device_fault) {
    device_handle = device;
    graphics_queue = graphics;
    compute_queue = compute;
    async_queue = async;
    // Off by default since build 51: the hang record keeps its queue operations and fault report,
    // and checkpoints at every draw are only wanted while hunting a hang.
    const char* env = std::getenv("SS_GPU_CHECKPOINTS");
    const bool wanted = env && env[0] == '1';
    checkpoints_on = checkpoints && wanted;
    fault_on = device_fault;
    LOG_INFO(Render_Vulkan, "GPU hang record: checkpoints {}, device fault report {}",
             checkpoints_on
                 ? "on (SS_GPU_CHECKPOINTS=1)"
                 : (checkpoints ? "off (SS_GPU_CHECKPOINTS=1 turns them on)" : "unavailable"),
             fault_on ? "available" : "unavailable");
}

bool CheckpointsOn() noexcept {
    return checkpoints_on;
}

const void* Mark(Work work, bool on_compute, u64 hash, u64 hash2, u64 count, u64 tick) noexcept {
    if (!checkpoints_on) {
        return nullptr;
    }
    const u64 seq = next_seq.fetch_add(1, std::memory_order_relaxed);
    Entry& e = entries[seq % NumEntries];
    e.seq.store(0, std::memory_order_relaxed);
    e.work = work;
    e.compute_queue = on_compute;
    e.hash = hash;
    e.hash2 = hash2;
    e.count = count;
    e.tick = tick;
    e.t = static_cast<float>(SecondsSinceLaunch());
    e.seq.store(seq, std::memory_order_release);
    return reinterpret_cast<const void*>(seq);
}

void NoteQueueOp(QueueOp op, u64 value, u64 extra) noexcept {
    const u64 n = next_op.fetch_add(1, std::memory_order_relaxed);
    Op& o = ops[n % NumOps];
    o.n.store(0, std::memory_order_relaxed);
    o.op = op;
    o.value = value;
    o.extra = extra;
    o.t = static_cast<float>(SecondsSinceLaunch());
    o.n.store(n, std::memory_order_release);
}

void NoteCompile(const char* kind, u64 hash) noexcept {
    const u64 n = next_compile.fetch_add(1, std::memory_order_relaxed);
    Compile& c = compiles[n % NumCompiles];
    c.n.store(0, std::memory_order_relaxed);
    c.kind = kind;
    c.hash = hash;
    c.t = static_cast<float>(SecondsSinceLaunch());
    c.n.store(n, std::memory_order_release);
}

void OnDeviceLost(std::string_view where) noexcept {
    std::call_once(record_once, [where] {
        std::vector<std::string> lines;
        lines.push_back(fmt::format("GPU hang record: the device was lost at {:.1f} s ({} min) "
                                    "after launch, seen by {}",
                                    SecondsSinceLaunch(), SecondsSinceLaunch() / 60.0, where));
        lines.push_back(fmt::format(
            "video memory held: buffers {} MB resident, {} images {} MB, empty buffer memory "
            "left out {} MB",
            VideoCore::VramMonitor::buffer_resident_bytes.load() >> 20,
            VideoCore::VramMonitor::image_count.load(),
            VideoCore::VramMonitor::image_bytes.load() >> 20,
            VideoCore::VramMonitor::lazy_zero_bytes.load() >> 20));
        {
            // Build 52: how far over the budget video memory was, as of the monitor's last look
            // (every 2 s), and how the blocks left out were bound.
            namespace VM = VideoCore::VramMonitor;
            const u64 used = VM::last_used_bytes.load();
            const u64 budget = VM::last_budget_bytes.load();
            const double age = SecondsSinceLaunch() - VM::last_sample_seconds.load();
            if (budget != 0) {
                lines.push_back(fmt::format(
                    "VRAM: {} MB in use of a {} MB budget {:.1f} s before ({}{} MB), highest in "
                    "the last minute {} MB",
                    used >> 20, budget >> 20, age, used >= budget ? "over by " : "under by ",
                    (used >= budget ? used - budget : budget - used) >> 20,
                    VM::recent_peak_used_bytes.load() >> 20));
            } else {
                lines.push_back("VRAM: no budget reading yet");
            }
            lines.push_back(fmt::format(
                "empty buffer memory left out bound to: {}; {} blocks bound to it in all, {} "
                "GPU writes found on it and woken first",
                VM::shared_zero_on.load() ? "the shared block of zeros" : "nothing (build 51 way)",
                VM::shared_zero_bound_blocks.load(), VM::shared_zero_guard_wakes.load()));
            const double now = SecondsSinceLaunch();
            const u64 last_compile = next_compile.load(std::memory_order_acquire);
            std::vector<std::string> recent;
            for (u64 n = last_compile > NumCompiles ? last_compile - NumCompiles : 1;
                 n < last_compile; ++n) {
                const Compile& c = compiles[n % NumCompiles];
                if (c.n.load(std::memory_order_acquire) != n || now - c.t > 10.0) {
                    continue;
                }
                recent.push_back(fmt::format("  {:.3f} s ({:.1f} s before) {} {:#x}", c.t,
                                             now - c.t, c.kind ? c.kind : "?", c.hash));
            }
            lines.push_back(
                fmt::format("shader and pipeline compiles in the last 10 s: {}", recent.size()));
            lines.insert(lines.end(), recent.begin(), recent.end());
        }
        try {
            if (!checkpoints_on) {
                lines.push_back("checkpoints: off, so the work running is not known");
            }
            Checkpoints(graphics_queue, "graphics", lines);
            if (compute_queue && compute_queue != graphics_queue) {
                Checkpoints(compute_queue, "compute", lines);
            }
            if (async_queue && async_queue != graphics_queue && async_queue != compute_queue) {
                Checkpoints(async_queue, "second graphics-family", lines);
            }
            DeviceFault(lines);
        } catch (...) {
            lines.push_back("reading the driver's report failed");
        }
        lines.push_back("last queue operations (newest last):");
        const u64 last = next_op.load(std::memory_order_acquire);
        for (u64 n = last > NumOps ? last - NumOps : 1; n < last; ++n) {
            const Op& o = ops[n % NumOps];
            if (o.n.load(std::memory_order_acquire) != n) {
                continue;
            }
            lines.push_back(fmt::format("  {:.3f} s {} {}{}", o.t, OpName(o.op), o.value,
                                        o.extra ? fmt::format(" ({})", o.extra) : std::string{}));
        }

        std::string text;
        for (const auto& line : lines) {
            LOG_CRITICAL(Render_Vulkan, "{}", line);
            text += line;
            text += '\n';
        }
        try {
            const auto path =
                Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "gpu_hang_record.txt";
            Common::FS::IOFile file{path, Common::FS::FileAccessMode::Create,
                                    Common::FS::FileType::TextFile};
            if (file.IsOpen()) {
                file.WriteString(text);
                file.Flush();
            }
        } catch (...) {
        }
        Common::Log::Flush();
    });
}

} // namespace Vulkan::GpuHang
