// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

namespace Vulkan {
class Instance;
}

// VRAM monitor (build 41). On a 4 GB card inFAMOUS Second Son uses about 150% of the memory
// budget: allocations past it are allowed, and Windows keeps the rest in system memory and moves
// it back and forth over PCIe. With the compute rings on a second queue the card drew 20 times
// slower per draw for minutes at a time. Every 30 s the log gets what the emulator holds in video
// memory (buffers made resident, images), what Vulkan and Windows report (video memory and
// shared system memory of this process) and how busy the card's engines were, the copy engine
// being what Windows moves memory with.
//
// VRAM priorities (F3, off by default, next launch): the memory buffers are made resident in is
// given the highest priority and the memory blocks of images drawn to a higher one than the
// rest, so that when the driver has to move memory out, textures go first.
namespace VideoCore::VramMonitor {

/// Kept by the caches.
extern std::atomic<u64> buffer_resident_bytes;
extern std::atomic<u64> image_bytes;
extern std::atomic<u32> image_count;
extern std::atomic<u32> raised_image_blocks;
/// Buffer memory left out of video memory as zeros (build 44).
extern std::atomic<u64> lazy_zero_bytes;
/// Binds that woke blocks left out as zeros: waiting for the work before them, and those that
/// could not (build 51).
extern std::atomic<u64> lazy_wake_binds;
extern std::atomic<u64> lazy_wake_unordered;
/// Pages written again every frame kept unprotected (build 51 experiment): whether it is on,
/// how many pages are kept, and the bytes uploaded for them because they were.
extern std::atomic<bool> keep_rewritten_pages;
extern std::atomic<s64> rewritten_pages_kept;
extern std::atomic<u64> rewritten_upload_bytes;

/// Starts the monitor's thread (once), and stops it before the instance goes.
void Start(const Vulkan::Instance& instance);
void Stop();

/// VRAM priorities for this launch: the setting as it was when first asked, and only if the
/// driver can take priorities.
bool PrioritiesOn(const Vulkan::Instance& instance);

} // namespace VideoCore::VramMonitor
