// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstdio>
#include <sstream>

#include "adaptive/hardware.h"

namespace Adaptive {

std::uint64_t Fnv1a64(const void* data, std::size_t size, std::uint64_t seed) {
    std::uint64_t h = seed;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::uint64_t Fnv1a64(const std::string& text) {
    return Fnv1a64(text.data(), text.size());
}

std::string Hex64(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

std::string DecodeDriverVersion(std::uint32_t vendor_id, std::uint32_t v) {
    char buf[48];
    if (vendor_id == 0x10de) { // NVIDIA
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff,
                      (v >> 6) & 0xff, v & 0x3f);
    } else if (vendor_id == 0x8086) { // Intel on Windows
        std::snprintf(buf, sizeof(buf), "%u.%u", v >> 14, v & 0x3fff);
    } else {
        std::snprintf(buf, sizeof(buf), "%u.%u.%u", v >> 22, (v >> 12) & 0x3ff, v & 0xfff);
    }
    return buf;
}

std::uint32_t RamClassGb(std::uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb <= 0.0) {
        return 0;
    }
    // Round up to the next power of two after removing up to 12% reserved by firmware / iGPU.
    std::uint32_t cls = 1;
    while (static_cast<double>(cls) < gb * 0.88) {
        cls *= 2;
    }
    return cls;
}

Fingerprint MakeFingerprint(const HardwareInfo& hw, const SoftwareInfo& sw) {
    Fingerprint f;
    std::ostringstream h;
    char ids[32];
    std::snprintf(ids, sizeof(ids), "%04x:%04x", hw.gpu.vendor_id, hw.gpu.device_id);
    h << hw.cpu_model << '|' << hw.cpu_physical_cores << 'c' << hw.cpu_logical_cores << 't' << '|'
      << RamClassGb(hw.ram_total_bytes) << "GB|" << ids << '|' << hw.gpu.name;
    f.hardware = h.str();
    f.driver = hw.gpu.driver_version;
    f.game = sw.game_serial + '|' + sw.game_version + '|' + sw.eboot_signature;
    f.emulator = sw.emulator_build;
    return f;
}

std::string Fingerprint::ProfileKey() const {
    return Hex64(Fnv1a64(hardware + '#' + game));
}

std::vector<std::string> Differences(const Fingerprint& a, const Fingerprint& b) {
    std::vector<std::string> d;
    if (a.hardware != b.hardware) {
        d.push_back("hardware: '" + a.hardware + "' -> '" + b.hardware + "'");
    }
    if (a.driver != b.driver) {
        d.push_back("GPU driver: " + a.driver + " -> " + b.driver);
    }
    if (a.game != b.game) {
        d.push_back("game build: '" + a.game + "' -> '" + b.game + "'");
    }
    if (a.emulator != b.emulator) {
        d.push_back("emulator build: " + a.emulator + " -> " + b.emulator);
    }
    return d;
}

std::string Describe(const HardwareInfo& hw) {
    std::ostringstream o;
    o << "CPU: " << hw.cpu_model << " (" << hw.cpu_physical_cores << " cores, "
      << hw.cpu_logical_cores << " threads" << (hw.cpu_avx2 ? ", AVX2" : "")
      << (hw.cpu_sse4a ? ", SSE4a" : "") << ")\n";
    o << "RAM: " << std::lround(static_cast<double>(hw.ram_total_bytes) / 1073741824.0)
      << " GB total, " << std::lround(static_cast<double>(hw.ram_available_bytes) / 1073741824.0)
      << " GB available at start\n";
    o << "GPU: " << hw.gpu.name << " (driver " << hw.gpu.driver_version << ", "
      << (hw.gpu.device_local_bytes >> 20) << " MB device-local, budget "
      << (hw.gpu.budget_bytes >> 20) << " MB"
      << (hw.gpu.memory_budget_ext ? "" : ", no VK_EXT_memory_budget")
      << (hw.gpu.integrated ? ", integrated" : "") << ")\n";
    o << "OS: " << hw.os_name;
    if (hw.display_refresh_hz != 0) {
        o << ", display " << hw.display_refresh_hz << " Hz";
    }
    if (hw.on_battery) {
        o << ", ON BATTERY";
    }
    o << '\n';
    return o.str();
}

} // namespace Adaptive
