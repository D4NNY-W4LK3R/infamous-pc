// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The emulator side of the adaptive optimizer: the only file the emulator includes.
//
// With the optimizer disabled (SS_ADAPTIVE=0 or enabled=0 in settings.ini) none of these do
// anything beyond one relaxed atomic load, and every live knob keeps the emulator's built-in
// value, so the emulator behaves exactly like the unmodified 4 GB build.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Adaptive::Glue {

// ---- Live knobs: the workers read these on their own schedule (one relaxed load). ----------

/// Texture cache: start garbage collection this many MB earlier (0 = built-in thresholds).
inline std::atomic<std::int32_t> g_texture_gc_headroom_mb{0};
/// Buffer cache upload worker: wake it once this many jobs are queued (built-in: 8).
inline std::atomic<std::int32_t> g_upload_wake_jobs{8};
/// Count of allocations that had to go past the VRAM budget (texture + buffer), for the
/// GPU Memory authority.
inline std::atomic<std::uint32_t> g_over_budget_allocations{0};

namespace Detail {
inline std::atomic<bool> g_active{false};
void PublishFrame(double frame_ms, const double* stall_ms, std::uint32_t num_stalls,
                  std::uint32_t draws, std::uint32_t dispatches, std::uint32_t submits,
                  std::uint32_t buffer_uploads, double gpu_busy_ms);
void NoteInputSlow();
} // namespace Detail

inline bool Active() {
    return Detail::g_active.load(std::memory_order_relaxed);
}

/// Emulator::Run, after param.sfo is read and before the window / renderer exist. Reads the
/// settings, discovers the hardware, loads the knowledge, and sets this launch's start-up values
/// in the environment. Starts the brain thread.
void Startup(const std::string& game_serial, const std::string& app_version,
             const std::filesystem::path& eboot_host_path);

/// Vulkan::Instance, once the device is chosen: the GPU the renderer really uses.
void ReportVulkanDevice(const std::string& name, std::uint32_t vendor_id, std::uint32_t device_id,
                        std::uint32_t driver_version, std::uint32_t api_version,
                        std::uint64_t device_local_bytes, std::uint64_t budget_bytes,
                        bool memory_budget_ext, bool integrated);

/// Renderer: how to read VRAM use and budget (called from the brain thread about once a
/// second). Both return 0 when unknown.
void SetVramSampler(std::function<std::uint64_t()> used, std::function<std::uint64_t()> budget);

/// Common::Perf::OnFlip: one call per presented frame.
inline void OnFrame(double frame_ms, const double* stall_ms, std::uint32_t num_stalls,
                    std::uint32_t draws, std::uint32_t dispatches, std::uint32_t submits,
                    std::uint32_t buffer_uploads, double gpu_busy_ms) {
    if (Active()) {
        Detail::PublishFrame(frame_ms, stall_ms, num_stalls, draws, dispatches, submits,
                             buffer_uploads, gpu_busy_ms);
    }
}

/// SDL event loop: any keyboard / mouse / pad input (calibration measures idle seconds only).
inline void NoteInput() {
    if (Active()) {
        Detail::NoteInputSlow();
    }
}

/// Emulator::Shutdown: judges this launch, saves what was learned, writes the report.
void Shutdown();

/// Second Son game patches (core/second_son_patches.h), from user/adaptive/settings.ini; they
/// are written into the game before its entry runs, so a change applies at the next launch.
struct GamePatchSettings {
    bool particle_late_read = true;
    bool particle_light_one_submit = false;
    std::uint32_t perf_mode = 0; ///< 0 off, 1 low, 2 office
};
GamePatchSettings GamePatches();
/// Saves them for the next launch.
void SetGamePatches(const GamePatchSettings& patches);
/// Patch decisions of this launch, one line each (empty before the game started).
std::vector<std::string> GamePatchNotes();

/// Frame rate limit (F3 settings > Graphics): 0 = off. Applies at once and is saved to
/// user/adaptive/settings.ini.
std::uint32_t FrameLimit();
void SetFrameLimit(std::uint32_t fps);

/// Toggles the F9 status window (drawn by the ImGui layer registered at startup).
void ToggleOverlay();
/// Registers the overlay layer; called by the presenter after ImGui is up.
void RegisterOverlay();

} // namespace Adaptive::Glue
