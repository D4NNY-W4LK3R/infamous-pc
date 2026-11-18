// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Frame rate limit (Second Son Adaptive): the most frames per second the emulator shows. 0 = no
// limit. The present thread holds a finished frame until its slot instead of showing it early;
// vblanks keep their rate, so the game's own timing is unchanged. Set from the F3 settings window
// (Graphics) and kept in user/adaptive/settings.ini as frame_limit.

#pragma once

#include <atomic>
#include <chrono>

#include "common/types.h"

namespace Common::FrameLimit {

inline std::atomic<u32> g_fps{0};

inline void Set(u32 fps) {
    g_fps.store(fps, std::memory_order_relaxed);
}

inline u32 Get() {
    return g_fps.load(std::memory_order_relaxed);
}

/// Shortest time between two shown frames; zero when there is no limit.
inline std::chrono::nanoseconds MinFrameInterval() {
    const u32 fps = Get();
    return fps == 0 ? std::chrono::nanoseconds{0} : std::chrono::nanoseconds{1'000'000'000 / fps};
}

} // namespace Common::FrameLimit
