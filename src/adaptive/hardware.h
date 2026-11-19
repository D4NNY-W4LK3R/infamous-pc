// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Automatic hardware discovery (Architecture v1.0 sec. 2).
//
// Static facts (CPU, GPU, RAM, OS, driver) are gathered once per launch by the emulator glue
// (adaptive_glue.cpp: hwinfo + the Vulkan instance) and condensed into a Fingerprint. Learned
// optimization results are keyed by the fingerprint's parts, so a different GPU, driver, game
// build or emulator build invalidates exactly the results that depended on it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Adaptive {

struct GpuInfo {
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string driver_version; ///< decoded per vendor (NVIDIA uses its own packing)
    std::uint32_t vulkan_api_version = 0;
    std::uint64_t device_local_bytes = 0; ///< sum of device-local heaps
    std::uint64_t budget_bytes = 0;       ///< budget at startup (VK_EXT_memory_budget)
    bool memory_budget_ext = false;
    bool integrated = false;
};

struct HardwareInfo {
    std::string cpu_model;
    std::uint32_t cpu_physical_cores = 0;
    std::uint32_t cpu_logical_cores = 0;
    bool cpu_avx2 = false;
    bool cpu_sse4a = false;
    std::uint64_t ram_total_bytes = 0;
    std::uint64_t ram_available_bytes = 0;
    std::string os_name;
    GpuInfo gpu;
    std::uint32_t display_refresh_hz = 0;
    bool on_battery = false; ///< laptop on battery: never calibrate (results are not comparable)
};

struct SoftwareInfo {
    std::string emulator_build;  ///< emulator version + source revision
    std::string game_serial;     ///< e.g. CUSA00223
    std::string game_version;    ///< param.sfo APP_VER, e.g. 01.07
    std::string eboot_signature; ///< hash of the eboot's code, distinguishes builds of a version
};

/// Identifies the context results were measured in. Each part is compared separately.
struct Fingerprint {
    std::string hardware; ///< CPU model + core counts + RAM size class + GPU vendor/device
    std::string driver;   ///< GPU driver version
    std::string game;     ///< serial + version + eboot signature
    std::string emulator; ///< emulator build
    bool operator==(const Fingerprint&) const = default;
    /// The key used to file results: the hardware and game parts. A driver or emulator change
    /// does not file into a new profile; it marks the existing one's values for re-checking.
    std::string ProfileKey() const;
};

Fingerprint MakeFingerprint(const HardwareInfo& hw, const SoftwareInfo& sw);

/// What differs between a stored fingerprint and the current one, for the report.
std::vector<std::string> Differences(const Fingerprint& stored, const Fingerprint& current);

/// 64-bit FNV-1a; stable across platforms and builds (used for keys and file checksums).
std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t seed = 0xcbf29ce484222325ull);
std::uint64_t Fnv1a64(const std::string& text);
std::string Hex64(std::uint64_t value);

/// NVIDIA packs driver versions as 10.8.8.6 bits; others use the Vulkan major.minor.patch.
std::string DecodeDriverVersion(std::uint32_t vendor_id, std::uint32_t driver_version);

/// RAM rounded to a size class (8, 16, 32 GB...), so a few MB of reserved memory reported
/// differently by Windows does not invalidate a profile.
std::uint32_t RamClassGb(std::uint64_t bytes);

/// Human readable multi-line description for the report.
std::string Describe(const HardwareInfo& hw);

} // namespace Adaptive
