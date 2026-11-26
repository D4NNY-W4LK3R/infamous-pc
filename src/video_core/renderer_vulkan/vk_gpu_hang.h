// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

/// The GPU hang record (build 50): after 32 minutes of normal play the card stopped responding
/// ("device lost") with nothing unusual before it. Each draw, dispatch and copy back leaves an
/// NVIDIA diagnostic checkpoint in its command buffer, and the last queue operations are kept,
/// so that when the device is lost the record names the work the card had started and not
/// finished. It is written once to the log and to gpu_hang_record.txt in the log folder.
namespace Vulkan::GpuHang {

enum class Work : u8 {
    CommandBuffer = 1,
    Draw,
    DrawIndirect,
    Dispatch,
    DispatchIndirect,
    CopyBack,
    Upload,
    Clear,
    Present,
};

enum class QueueOp : u8 {
    GraphicsSubmit = 1,
    ComputeSubmit,
    SparseBind,
    Present,
    /// A sparse bind that also binds blocks to the shared block of zeros (build 52).
    SparseBindZeros,
};

/// Called once the device exists, with its queues (null ones are skipped) and whether the
/// checkpoint and fault extensions are enabled.
void Init(vk::Device device, vk::Queue graphics, vk::Queue compute, vk::Queue async,
          bool checkpoints, bool device_fault);

/// True when checkpoints are written. On with SS_GPU_CHECKPOINTS=1 (off by default, build 51).
bool CheckpointsOn() noexcept;

/// Names a piece of work and returns the marker for vkCmdSetCheckpointNV, or nullptr when
/// checkpoints are off. hash/hash2: shader hashes (pixel/vertex, or compute), count: vertices
/// times instances, or groups, or bytes; tick: the work semaphore value it is submitted with.
const void* Mark(Work work, bool compute_queue, u64 hash, u64 hash2, u64 count, u64 tick) noexcept;

/// Keeps the last queue operations: what was submitted or bound just before the card stopped.
void NoteQueueOp(QueueOp op, u64 value, u64 extra = 0) noexcept;

/// Keeps the last shader and pipeline compiles (build 52): the build 51 device lost came right
/// after the game compiled a few new shaders. kind: "shader", "graphics pipeline", ...
void NoteCompile(const char* kind, u64 hash) noexcept;

/// Writes the record the first time a call returns VK_ERROR_DEVICE_LOST; the later calls only
/// return. where: what returned it.
void OnDeviceLost(std::string_view where) noexcept;

/// Checks a result and writes the record if it is VK_ERROR_DEVICE_LOST. Returns the result.
inline vk::Result Check(vk::Result result, std::string_view where) noexcept {
    if (result == vk::Result::eErrorDeviceLost) {
        OnDeviceLost(where);
    }
    return result;
}

} // namespace Vulkan::GpuHang
