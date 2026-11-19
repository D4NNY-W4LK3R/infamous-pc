// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>

#include "adaptive/contracts.h"

namespace Adaptive {

std::string_view ToString(Mode mode) {
    switch (mode) {
    case Mode::Observe:
        return "observe";
    case Mode::Manual:
        return "manual";
    case Mode::Automatic:
        return "automatic";
    case Mode::Calibration:
        return "calibration";
    }
    return "?";
}

std::optional<Mode> ParseMode(std::string_view text) {
    std::string t(text);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const Mode m : {Mode::Observe, Mode::Manual, Mode::Automatic, Mode::Calibration}) {
        if (t == ToString(m)) {
            return m;
        }
    }
    if (t == "auto") {
        return Mode::Automatic;
    }
    return std::nullopt;
}

std::string_view ToString(Domain domain) {
    switch (domain) {
    case Domain::GpuMemory:
        return "GPU Memory";
    case Domain::BufferGeometry:
        return "Buffer & Geometry";
    case Domain::TextureCache:
        return "Texture & Cache";
    case Domain::TransferSync:
        return "Transfer & Sync";
    case Domain::ShaderPipeline:
        return "Shader & Pipeline";
    case Domain::FramePacing:
        return "Rendering & Frame Pacing";
    case Domain::CpuWorkload:
        return "CPU Workload";
    case Domain::Count:
        break;
    }
    return "?";
}

std::string_view ToString(StallKind kind) {
    static constexpr std::string_view names[] = {
        "shader translate", "pipeline create",  "pipeline wait", "gpu wait",
        "texture upload",   "texture evict",    "buffer upload", "buffer download",
        "readback wait",    "residency",        "sparse bind",   "page faults",
        "page protect",     "gpu page protect", "dma sync",      "gpu thread busy",
        "waiting on game",  "present",          "frame wait",    "command recording",
        "upload thread",    "upload wait",
    };
    static_assert(std::size(names) == NumStallKinds);
    const auto i = static_cast<std::size_t>(kind);
    return i < NumStallKinds ? names[i] : "?";
}

std::string_view ToString(Scene scene) {
    switch (scene) {
    case Scene::Unknown:
        return "unknown";
    case Scene::Gameplay:
        return "gameplay";
    case Scene::Cutscene:
        return "cutscene / watching";
    case Scene::Menu:
        return "menu / pause";
    case Scene::Loading:
        return "loading / video";
    }
    return "unknown";
}

std::string_view ToString(Bottleneck b) {
    switch (b) {
    case Bottleneck::Unknown:
        return "unknown";
    case Bottleneck::Warmup:
        return "warmup";
    case Bottleneck::GuestBound:
        return "game CPU";
    case Bottleneck::GpuThreadBound:
        return "GPU command thread";
    case Bottleneck::HostGpuBound:
        return "host GPU";
    case Bottleneck::PresentBound:
        return "presentation";
    case Bottleneck::ShaderCompile:
        return "shader compilation";
    case Bottleneck::MemoryPressure:
        return "VRAM pressure";
    case Bottleneck::TransferBound:
        return "transfers";
    case Bottleneck::PageFaultBound:
        return "page faults";
    }
    return "?";
}

std::string_view ToString(Verdict v) {
    switch (v) {
    case Verdict::Accepted:
        return "accepted";
    case Verdict::Rejected:
        return "rejected";
    case Verdict::Deferred:
        return "deferred";
    case Verdict::Recommended:
        return "recommended";
    }
    return "?";
}

std::string_view ToString(Outcome o) {
    switch (o) {
    case Outcome::Improved:
        return "improved";
    case Outcome::Neutral:
        return "neutral";
    case Outcome::Regressed:
        return "regressed";
    case Outcome::Crashed:
        return "crashed";
    case Outcome::Abandoned:
        return "abandoned";
    }
    return "?";
}

} // namespace Adaptive
