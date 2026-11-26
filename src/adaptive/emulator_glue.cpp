// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include <hwinfo/hwinfo.h>

#include "adaptive/brain.h"
#include "adaptive/emulator_glue.h"
#include "adaptive/quality_ladder.h"
#include "adaptive/glue_internal.h"
#include "adaptive/settings.h"
#include "common/frame_limit.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/thread.h"
#include "common/shared_user_dir.h"
#include "core/emulator_settings.h"
#include "core/stencil_probe.h"
#include "core/second_son_patches.h"
#include "imgui/notice_layer.h"
#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace Adaptive::Glue {

namespace {

std::unique_ptr<CentralBrain> g_brain;
std::filesystem::path g_dir;
UserSettings g_settings;
std::atomic<bool> g_crashed{false};
std::int32_t g_launch_perf_mode = -1; ///< chosen by the frame-rate target for this launch
std::uint32_t g_launch_render_height = 0; ///< chosen with it (0 = not in use)
bool g_late_read_turned_off = false;
std::mutex g_vram_mutex;
std::function<std::uint64_t()> g_vram_used;
std::function<std::uint64_t()> g_vram_budget;
HardwareInfo g_hardware;
std::uint64_t g_frame_index = 0;

std::optional<std::string> EnvGet(const std::string& name) {
    if (const char* v = std::getenv(name.c_str())) {
        return std::string(v);
    }
    return std::nullopt;
}

void EnvSet(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 0);
#endif
}

std::uint32_t ParseHexId(const std::string& s) {
    std::string t;
    for (const char c : s) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            t += c;
        }
    }
    if (t.size() > 2 && (s.find("0x") != std::string::npos || s.find("0X") != std::string::npos)) {
        t = t.substr(1); // drop the 0 of "0x"
    }
    return t.empty() ? 0u : static_cast<std::uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
}

std::string EbootSignature(const std::filesystem::path& eboot) {
    std::ifstream in(eboot, std::ios::binary);
    if (!in) {
        return "unknown";
    }
    std::uint64_t h = 0xcbf29ce484222325ull;
    std::uint64_t total = 0;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        h = Fnv1a64(buf.data(), n, h);
        total += n;
    }
    return Hex64(h) + "-" + std::to_string(total);
}

HardwareInfo Discover() {
    HardwareInfo hw;
    try {
        const auto cpus = hwinfo::getAllCPUs();
        if (!cpus.empty()) {
            hw.cpu_model = cpus[0].modelName();
            hw.cpu_physical_cores = static_cast<std::uint32_t>(cpus[0].numPhysicalCores());
            hw.cpu_logical_cores = static_cast<std::uint32_t>(cpus[0].numLogicalCores());
        }
        hwinfo::Memory ram;
        hw.ram_total_bytes = static_cast<std::uint64_t>(ram.total_Bytes());
        hw.ram_available_bytes = static_cast<std::uint64_t>(ram.available_Bytes());
        hwinfo::OS os;
        hw.os_name = os.name() + " " + os.version();
        // Prefer a discrete GPU: on Optimus laptops the renderer uses the NVIDIA one.
        const auto gpus = hwinfo::getAllGPUs();
        const hwinfo::GPU* pick = nullptr;
        for (const auto& g : gpus) {
            const std::uint32_t vendor = ParseHexId(g.vendor_id());
            if (vendor == 0x10de || vendor == 0x1002) {
                pick = &g;
                break;
            }
        }
        if (!pick && !gpus.empty()) {
            pick = &gpus[0];
        }
        if (pick) {
            hw.gpu.name = pick->name();
            hw.gpu.vendor_id = ParseHexId(pick->vendor_id());
            hw.gpu.device_id = ParseHexId(pick->device_id());
            hw.gpu.driver_version = pick->driverVersion();
        }
    } catch (...) {
        LOG_WARNING(Render, "Adaptive: hardware discovery failed partly; continuing with less");
    }
    if (hw.cpu_logical_cores == 0) {
        hw.cpu_logical_cores = std::max(1u, std::thread::hardware_concurrency());
    }
#if defined(__x86_64__) || defined(_M_X64)
    // AVX2 / SSE4a, the CPU features the emulator's patches care about.
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 0);
    if (r[0] >= 7) {
        __cpuidex(r, 7, 0);
        hw.cpu_avx2 = (r[1] & (1 << 5)) != 0;
    }
    __cpuid(r, 0x80000001);
    hw.cpu_sse4a = (r[2] & (1 << 6)) != 0;
#else
    hw.cpu_avx2 = __builtin_cpu_supports("avx2");
#endif
#endif
#ifdef _WIN32
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        hw.on_battery = ps.ACLineStatus == 0;
    }
#endif
    return hw;
}

} // namespace

void Detail::PublishFrame(double frame_ms, const double* stall_ms, std::uint32_t num_stalls,
                          std::uint32_t draws, std::uint32_t dispatches, std::uint32_t submits,
                          std::uint32_t buffer_uploads, double gpu_busy_ms) {
    FrameSample f;
    f.frame_index = g_frame_index++;
    f.timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    f.frame_ms = static_cast<float>(frame_ms);
    const std::uint32_t n = std::min<std::uint32_t>(num_stalls, NumStallKinds);
    for (std::uint32_t i = 0; i < n; ++i) {
        f.stall_ms[i] = static_cast<float>(stall_ms[i]);
    }
    f.draws = draws;
    f.dispatches = dispatches;
    f.submits = submits;
    f.buffer_uploads = buffer_uploads;
    f.gpu_busy_ms = static_cast<float>(gpu_busy_ms);
    g_brain->PublishFrame(f);
}

void Detail::NoteInputSlow() {
    g_brain->NoteInput();
}

void Startup(const std::string& game_serial, const std::string& app_version,
             const std::filesystem::path& eboot_host_path) {
    if (g_brain) {
        return;
    }
    g_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    const auto settings_file = g_dir / "settings.ini";
    const bool had_settings = std::filesystem::exists(settings_file, ec);
    g_settings = LoadSettings(settings_file, EnvGet);
    for (const auto& w : g_settings.warnings) {
        LOG_WARNING(Render, "Adaptive settings: {}", w);
    }
    if (!had_settings) {
        SaveSettings(settings_file, g_settings); // a commented file the user can edit
    }
    // A launch that ended in a crash while the particle late-read was on: that patch changes
    // when the game reads GPU-written memory, so it is the first suspect. It stays off until the
    // player turns it back on (field: an access-tracking assertion with it on, build 19).
    {
        const auto marker = g_dir / "late-read-active.txt";
        if (std::filesystem::exists(marker, ec)) {
            std::filesystem::remove(marker, ec);
            if (g_settings.particle_late_read) {
                g_settings.particle_late_read = false;
                SaveSettings(settings_file, g_settings);
                g_late_read_turned_off = true;
                Overlay::Notice::Show("The last launch crashed with the particle timing fix on - "
                                      "it is off now (F3 > Graphics to turn it back on)",
                                      8.0);
                LOG_WARNING(Render, "Second Son: the last launch did not end cleanly with the "
                                    "particle late-read on; it is turned off (F3 > Graphics)");
            }
        }
    }
    // The same for the game's compute work on a second queue (F3, experimental): a launch with it
    // on that did not end cleanly turns it off, so a hang it may cause is never repeated unasked.
    {
        const auto marker = g_dir / "async-compute-active.txt";
        if (std::filesystem::exists(marker, ec)) {
            std::filesystem::remove(marker, ec);
            if (EmulatorSettings.IsAsyncCompute()) {
                EmulatorSettings.SetAsyncCompute(false, true);
                EmulatorSettings.Save(game_serial);
                Overlay::Notice::Show("The last launch did not end cleanly with the compute work "
                                      "on a second queue - it is off now (F3 to turn it back on)",
                                      8.0);
                LOG_WARNING(Render, "The last launch did not end cleanly with compute rings on a "
                                    "second queue; it is turned off for this game (F3)");
            }
        }
        if (EmulatorSettings.IsAsyncCompute()) {
            std::ofstream(marker) << "compute rings on a second queue were on in this launch\n";
            LOG_INFO(Render, "Compute rings run on a second queue (experimental): a launch that "
                             "does not end cleanly turns this off");
        }
    }
    // The frame rate limit is the user's, not the optimizer's: it applies even when the
    // optimizer is turned off.
    Common::FrameLimit::Set(g_settings.frame_limit);
    if (g_settings.frame_limit != 0) {
        LOG_INFO(Render, "Frame rate limit: {} fps", g_settings.frame_limit);
    }
    if (!g_settings.enabled) {
        LOG_INFO(Render, "Adaptive optimizer: disabled (the emulator runs with its built-in "
                         "values)");
        return;
    }

    g_hardware = Discover();
    SoftwareInfo sw;
    sw.emulator_build = std::string(Common::g_version) + "-" + Common::g_scm_rev;
    sw.game_serial = game_serial;
    sw.game_version = app_version;
    sw.eboot_signature = EbootSignature(eboot_host_path);

    BrainConfig cfg;
    cfg.mode = g_settings.mode;
    cfg.data_dir = g_dir;
    cfg.tick_ms = g_settings.tick_ms;
    cfg.calibrate_on_battery = g_settings.calibrate_on_battery;
    cfg.fps_target = g_settings.fps_target;
    cfg.perf_mode_floor = g_settings.perf_mode;
    cfg.perf_mode_max = std::max(g_settings.auto_perf_max, g_settings.perf_mode);
    cfg.render_height = g_settings.render_height;
    cfg.render_height_min = g_settings.auto_render_min;
    cfg.on_thread_start = [] {
        Common::SetCurrentThreadName("shadPS4:AdaptiveBrain");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    };
    g_brain = std::make_unique<CentralBrain>(cfg);

    EmulatorDefaults d;
    d.pipeline_workers = Vulkan::PipelineCompiler::BuiltinNumWorkers();
    d.pipeline_workers_max =
        std::clamp<std::int64_t>(static_cast<std::int64_t>(g_hardware.cpu_logical_cores) - 2,
                                 d.pipeline_workers, 12);
    d.upload_wake_jobs = 8;
    d.frame_limit = g_settings.frame_limit;
    d.perf_mode = g_settings.perf_mode;
    {
        const std::string pm = EmulatorSettings.GetPresentMode();
        d.present_mode = pm == "Fifo" ? 1 : pm == "Immediate" ? 2 : 0;
    }
    bool ok = true;
    ok &= g_brain->RegisterMiniBrain(MakeBufferGeometryBrain(d));
    ok &= g_brain->RegisterMiniBrain(
        MakeTextureCacheBrain(d, &g_brain->Memory()),
        {{KnobId::TextureGcHeadroomMb, AtomicActuator(g_texture_gc_headroom_mb)}},
        {{KnobId::TextureGcHeadroomMb, -1}});
    ok &= g_brain->RegisterMiniBrain(
        MakeTransferSyncBrain(d), {{KnobId::UploadWakeJobs, AtomicActuator(g_upload_wake_jobs)}});
    ok &= g_brain->RegisterMiniBrain(MakeShaderPipelineBrain(d));
    ok &= g_brain->RegisterMiniBrain(MakeFramePacingBrain(d));
    ok &= g_brain->RegisterMiniBrain(MakeCpuWorkloadBrain(d));
    if (!ok) {
        LOG_ERROR(Render, "Adaptive: a mini-brain was refused; see the session report");
    }
    g_brain->SetResourceSampler([] {
        ResourceSample r;
        std::scoped_lock lk{g_vram_mutex};
        if (g_vram_used) {
            r.vram_used_bytes = g_vram_used();
        }
        if (g_vram_budget) {
            r.vram_budget_bytes = g_vram_budget();
        }
        r.over_budget_allocations = g_over_budget_allocations.load(std::memory_order_relaxed);
        return r;
    });

    const StartupResult res = g_brain->Startup(g_hardware, sw, EnvGet, EnvSet);
    g_launch_perf_mode = res.perf_mode;
    g_launch_render_height = res.render_height;
    if (!Common::FS::SharedUserDirNote().empty()) {
        LOG_INFO(Render, "Adaptive: {}", Common::FS::SharedUserDirNote());
    }
    LOG_INFO(Render, "Adaptive optimizer: mode {}, knowledge {}, profile {}", ToString(cfg.mode),
             ToString(res.load_status), MakeFingerprint(g_hardware, sw).ProfileKey());
    LOG_INFO(Render, "Adaptive hardware: {} | {} threads | {} GB RAM | GPU {} (driver {}){}",
             g_hardware.cpu_model, g_hardware.cpu_logical_cores,
             g_hardware.ram_total_bytes >> 30, g_hardware.gpu.name,
             g_hardware.gpu.driver_version, g_hardware.on_battery ? " | ON BATTERY" : "");
    for (const auto& [k, v] : res.launch_values) {
        LOG_INFO(Render, "Adaptive: start-up value {} = {} (learned or under test)", k, v);
    }
    for (const auto& n : res.notes) {
        LOG_INFO(Render, "Adaptive: {}", n);
    }
    Detail::g_active.store(true, std::memory_order_release);
    g_brain->Start();
}

void SetPowerNote(const std::string& note) {
    if (g_brain) {
        g_brain->SetPowerNote(note);
    }
}

void SetCheatsNote(const std::string& note) {
    if (g_brain) {
        g_brain->SetCheatsNote(note);
    }
}

void ReportVulkanDevice(const std::string& name, std::uint32_t vendor_id, std::uint32_t device_id,
                        std::uint32_t driver_version, std::uint32_t api_version,
                        std::uint64_t device_local_bytes, std::uint64_t budget_bytes,
                        bool memory_budget_ext, bool integrated) {
    LOG_INFO(Render, "Adaptive: renderer GPU {} {:04x}:{:04x}, Vulkan driver {}, {} MB local, "
                     "budget {} MB{}",
             name, vendor_id, device_id, DecodeDriverVersion(vendor_id, driver_version),
             device_local_bytes >> 20, budget_bytes >> 20,
             memory_budget_ext ? "" : " (no VK_EXT_memory_budget)");
    if (!g_brain) {
        return;
    }
    if (g_hardware.gpu.device_id != 0 &&
        (g_hardware.gpu.device_id != device_id || g_hardware.gpu.vendor_id != vendor_id)) {
        LOG_WARNING(Render,
                    "Adaptive: the renderer uses {:04x}:{:04x}, but start-up discovery picked "
                    "{:04x}:{:04x}; switching to Observe for this session so nothing learned on "
                    "one GPU is applied to the other",
                    vendor_id, device_id, g_hardware.gpu.vendor_id, g_hardware.gpu.device_id);
        g_brain->RequestMode(Mode::Observe);
    }
    GpuInfo g;
    g.name = name;
    g.vendor_id = vendor_id;
    g.device_id = device_id;
    g.driver_version = DecodeDriverVersion(vendor_id, driver_version);
    g.vulkan_api_version = api_version;
    g.device_local_bytes = device_local_bytes;
    g.budget_bytes = budget_bytes;
    g.memory_budget_ext = memory_budget_ext;
    g.integrated = integrated;
    g_brain->SetRendererGpu(g);
}

void SetVramSampler(std::function<std::uint64_t()> used, std::function<std::uint64_t()> budget) {
    std::scoped_lock lk{g_vram_mutex};
    g_vram_used = std::move(used);
    g_vram_budget = std::move(budget);
}

void NoteCrash() {
    g_crashed.store(true, std::memory_order_relaxed);
}

void NoteGamePatchesApplied(bool late_read) {
    if (!late_read || g_dir.empty()) {
        return;
    }
    std::ofstream(g_dir / "late-read-active.txt") << "particle late-read was on in this launch\n";
}

bool LateReadTurnedOffAfterCrash() {
    return g_late_read_turned_off;
}

void Shutdown() {
    Core::StencilProbe::Report();
    if (!g_crashed.load(std::memory_order_relaxed) && !g_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove(g_dir / "late-read-active.txt", ec);
        std::filesystem::remove(g_dir / "async-compute-active.txt", ec);
    }
    if (!g_brain) {
        return;
    }
    Detail::g_active.store(false, std::memory_order_release);
    const auto report = g_brain->Shutdown(g_crashed.load(std::memory_order_relaxed));
    // Live knobs are back to their defaults; leave the atomics consistent for any late reader.
    g_texture_gc_headroom_mb.store(0, std::memory_order_relaxed);
    g_upload_wake_jobs.store(8, std::memory_order_relaxed);
    LOG_INFO(Render, "Adaptive: session report written to {}", report.string());
    {
        std::scoped_lock lk{g_vram_mutex};
        g_vram_used = {};
        g_vram_budget = {};
    }
}

// ---- Overlay plumbing used by adaptive_overlay.cpp ---------------------------------------------

CentralBrain* BrainForOverlay() {
    return g_brain.get();
}

UserSettings& SettingsForOverlay() {
    return g_settings;
}

void SaveSettingsFromOverlay() {
    SaveSettings(g_dir / "settings.ini", g_settings);
}

namespace {
/// The settings file, read fresh when no game has started (launcher settings window).
UserSettings CurrentSettings() {
    if (!g_dir.empty()) {
        return g_settings;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
    return LoadSettings(dir / "settings.ini", [](const std::string&) { return std::nullopt; });
}

void SaveChanged(const std::function<void(UserSettings&)>& change) {
    if (!g_dir.empty()) {
        change(g_settings);
        SaveSettings(g_dir / "settings.ini", g_settings);
        return;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    UserSettings s =
        LoadSettings(dir / "settings.ini", [](const std::string&) { return std::nullopt; });
    change(s);
    SaveSettings(dir / "settings.ini", s);
}
} // namespace

GamePatchSettings GamePatches() {
    const UserSettings s = CurrentSettings();
    return GamePatchSettings{s.particle_late_read, s.particle_light_one_submit, s.perf_mode,
                             s.render_height};
}

std::uint32_t EffectivePerfMode() {
    return g_launch_perf_mode >= 0 ? static_cast<std::uint32_t>(g_launch_perf_mode)
                                   : CurrentSettings().perf_mode;
}

std::uint32_t EffectiveRenderHeight() {
    return g_launch_render_height != 0 ? g_launch_render_height
                                       : NormalizeRenderHeight(CurrentSettings().render_height);
}

FpsTarget GetFpsTarget() {
    const UserSettings s = CurrentSettings();
    return FpsTarget{s.fps_target, s.auto_perf_max, s.auto_render_min};
}

void SetFpsTarget(const FpsTarget& t) {
    SaveChanged([&t](UserSettings& s) {
        s.fps_target = t.fps == 0 ? 0 : std::clamp<std::uint32_t>(t.fps, 15, 240);
        s.auto_perf_max = std::min<std::uint32_t>(t.lowest_quality, 2);
        s.auto_render_min = NormalizeRenderHeight(t.lowest_render_height);
    });
    LOG_INFO(Render,
             "Frame-rate target for the next launch: {} fps, lowest quality {}, smallest render "
             "size {}",
             t.fps, t.lowest_quality == 1 ? "low" : t.lowest_quality == 2 ? "office" : "off",
             DescribeRenderHeight(t.lowest_render_height));
}

void SetGamePatches(const GamePatchSettings& patches) {
    SaveChanged([&patches](UserSettings& s) {
        s.particle_late_read = patches.particle_late_read;
        s.particle_light_one_submit = patches.particle_light_one_submit;
        s.perf_mode = patches.perf_mode > 2 ? 0 : patches.perf_mode;
        s.render_height = NormalizeRenderHeight(patches.render_height);
    });
    LOG_INFO(Render,
             "Second Son game patches for the next launch: particle late-read {}, lights in one "
             "submit {}, performance mode {}",
             patches.particle_late_read ? "on" : "off",
             patches.particle_light_one_submit ? "on" : "off",
             patches.perf_mode == 1 ? "low" : patches.perf_mode == 2 ? "office" : "off");
    if (g_brain) {
        g_brain->RequestNoteUserSetting(KnobId::PerfMode, patches.perf_mode);
    }
}

std::vector<std::string> GamePatchNotes() {
    return Core::SecondSonPatches::Applied().notes;
}

bool VramPriorities() {
    if (!g_dir.empty()) {
        return g_settings.vram_priorities;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
    return LoadSettings(dir / "settings.ini", EnvGet).vram_priorities;
}

void SetVramPriorities(bool on) {
    const auto dir = g_dir.empty()
                         ? Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive"
                         : g_dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    UserSettings s = g_dir.empty() ? LoadSettings(dir / "settings.ini",
                                                  [](const std::string&) { return std::nullopt; })
                                   : g_settings;
    if (s.vram_priorities == on) {
        return;
    }
    s.vram_priorities = on;
    SaveSettings(dir / "settings.ini", s);
    if (!g_dir.empty()) {
        g_settings.vram_priorities = on;
    }
    LOG_INFO(Render, "VRAM priorities {} from the next launch", on ? "on" : "off");
}

namespace {
template <bool UserSettings::*Field>
bool GetFlag() {
    if (!g_dir.empty()) {
        return g_settings.*Field;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
    return LoadSettings(dir / "settings.ini", EnvGet).*Field;
}

template <bool UserSettings::*Field>
void SetFlag(bool on, const char* what) {
    const auto dir =
        g_dir.empty() ? Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive" : g_dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    UserSettings s = g_dir.empty() ? LoadSettings(dir / "settings.ini",
                                                  [](const std::string&) { return std::nullopt; })
                                   : g_settings;
    if (s.*Field == on) {
        return;
    }
    s.*Field = on;
    SaveSettings(dir / "settings.ini", s);
    if (!g_dir.empty()) {
        g_settings.*Field = on;
    }
    LOG_INFO(Render, "{} {} from the next launch", what, on ? "on" : "off");
}
} // namespace

namespace {
/// Reads the settings (the live copy once the optimizer has loaded them), changes them and
/// saves them (build 52, cheats kept on).
template <class Change>
void ChangeSettings(Change&& change) {
    const auto dir =
        g_dir.empty() ? Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive" : g_dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    UserSettings s = g_dir.empty() ? LoadSettings(dir / "settings.ini",
                                                  [](const std::string&) { return std::nullopt; })
                                   : g_settings;
    if (!change(s)) {
        return;
    }
    SaveSettings(dir / "settings.ini", s);
    if (!g_dir.empty()) {
        g_settings.cheats_kept_on = s.cheats_kept_on;
        g_settings.smoke_dash_kept = s.smoke_dash_kept;
    }
}

} // namespace

std::uint32_t CheatsKeptOn() {
    return CurrentSettings().cheats_kept_on;
}

float SmokeDashKept() {
    return CurrentSettings().smoke_dash_kept;
}

void SetCheatKeptOn(unsigned index, bool on) {
    if (index >= CheatKeys.size()) {
        return;
    }
    ChangeSettings([&](UserSettings& s) {
        const std::uint32_t bit = 1u << index;
        const std::uint32_t next = on ? (s.cheats_kept_on | bit) : (s.cheats_kept_on & ~bit);
        if (next == s.cheats_kept_on) {
            return false;
        }
        s.cheats_kept_on = next;
        return true;
    });
    LOG_INFO(Core, "Cheat {}: kept on from launch to launch {}", CheatKeys[index],
             on ? "on" : "off");
}

void SetSmokeDashKept(float scale) {
    ChangeSettings([&](UserSettings& s) {
        if (s.smoke_dash_kept == scale) {
            return false;
        }
        s.smoke_dash_kept = scale;
        return true;
    });
}

bool LazyZeroMemory() {
    if (!g_dir.empty()) {
        return g_settings.lazy_zero_memory;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
    return LoadSettings(dir / "settings.ini", EnvGet).lazy_zero_memory;
}

void SetLazyZeroMemory(bool on) {
    const auto dir = g_dir.empty()
                         ? Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive"
                         : g_dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    UserSettings s = g_dir.empty() ? LoadSettings(dir / "settings.ini",
                                                  [](const std::string&) { return std::nullopt; })
                                   : g_settings;
    if (s.lazy_zero_memory == on) {
        return;
    }
    s.lazy_zero_memory = on;
    SaveSettings(dir / "settings.ini", s);
    if (!g_dir.empty()) {
        g_settings.lazy_zero_memory = on;
    }
    LOG_INFO(Render, "Empty buffer memory left out of video memory {} from the next launch",
             on ? "on" : "off");
}

std::uint32_t FrameLimit() {
    return Common::FrameLimit::Get();
}

void SetFrameLimit(std::uint32_t fps) {
    if (fps != 0) {
        fps = std::clamp<std::uint32_t>(fps, 15, 240);
    }
    if (fps == Common::FrameLimit::Get() && fps == g_settings.frame_limit) {
        return;
    }
    Common::FrameLimit::Set(fps);
    if (!g_dir.empty()) {
        g_settings.frame_limit = fps;
        SaveSettings(g_dir / "settings.ini", g_settings);
    } else {
        // Before a game has started (launcher settings): change only this line of the file.
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "adaptive";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        UserSettings s = LoadSettings(dir / "settings.ini",
                                      [](const std::string&) { return std::nullopt; });
        s.frame_limit = fps;
        SaveSettings(dir / "settings.ini", s);
    }
    LOG_INFO(Render, "Frame rate limit: {}",
             fps == 0 ? std::string{"off"} : std::to_string(fps) + " fps");
    if (g_brain) {
        g_brain->RequestNoteUserSetting(KnobId::FrameLimit, fps);
    }
}

} // namespace Adaptive::Glue
