// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Long start-up work (the shader preload) reports progress here. The window shows it in its
// title and keeps handling Windows messages meanwhile, so the start does not look frozen ("Not
// Responding") and nobody ends it halfway (a player did, build 32, while 6,918 shaders loaded).

#include <atomic>
#include <string>
#include <thread>

namespace Common {

using StartupProgressHook = void (*)(const std::string& text);

namespace Detail {
inline std::atomic<StartupProgressHook> g_startup_hook{nullptr};
inline std::thread::id g_startup_thread{};
} // namespace Detail

/// Installed by the window on the thread that owns it. nullptr removes it.
inline void SetStartupProgressHook(StartupProgressHook hook) {
    Detail::g_startup_thread = std::this_thread::get_id();
    Detail::g_startup_hook.store(hook);
}

/// Shows `text` (empty: back to the plain title). Only acts on the window's own thread.
inline void ReportStartupProgress(const std::string& text) {
    const auto hook = Detail::g_startup_hook.load();
    if (hook && std::this_thread::get_id() == Detail::g_startup_thread) {
        hook(text);
    }
}

} // namespace Common
