// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include "common/types.h"

namespace Core {
class MemoryManager;
}

/// The copy-back log (build 50). In the slow seconds the game's main thread waits 40-50 ms a
/// frame for memory the GPU wrote to be copied back before it may touch it. This logs which
/// memory those waits are for (the 128 KB window copied, the game code that touched it, whether
/// it read or wrote), and, for a sample of the waits on a write, whether the game then
/// overwrites what was copied back: if it does, the copy was not needed. Every 30 s and at the
/// end a "Copy-backs:" section goes to the log. It reads memory only through its backing, so it
/// never faults or copies anything back itself.
namespace VideoCore::CopyBackLog {

/// Set by the page fault handler around the copy back it causes, for the thread it runs on.
void SetFault(u64 rip, u64 address, bool is_write) noexcept;
void ClearFault() noexcept;

/// A copy back finished, for a game thread (gpu_thread false) or the GPU thread itself.
/// address/size: what was touched; [window_start, window_end): what was copied back.
void NoteCopyBack(Core::MemoryManager* memory, u64 address, u64 size, u64 window_start,
                  u64 window_end, bool is_write, bool gpu_thread, u64 wait_ns);

/// GPU data was written back over [start, end) again: samples there can't tell any more what
/// the game wrote.
void NoteWrittenBack(u64 start, u64 end) noexcept;

/// Compares the samples that are due and logs every 30 s. gpu_modified tells whether the GPU
/// wrote a range again since.
void Tick(Core::MemoryManager* memory, const std::function<bool(u64, u64)>& gpu_modified);

/// The whole session's summary, at the end.
void Report();

} // namespace VideoCore::CopyBackLog
