// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "input/camera_hook_signature.h"

namespace Input::CameraHook {

namespace {

// inFAMOUS Second Son 01.07, measured 2026-10-17 on the eboot of the native keyboard/mouse fork
// (shadPS4-second-son v1.1.1, CUSA00004). The two stores before the camera's pan / tilt store
// and the one after it, as the game ships them.
constexpr BuildProfile Builds[] = {
    BuildProfile{
        "inFAMOUS Second Son 01.07 (camera store at 0x14ebce)",
        0x14ebc5,
        {
            0xc5, 0xf8, 0x11, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00, // vmovups [rsp+0x100], xmm0
            0xc4, 0xc1, 0x78, 0x11, 0x85, 0x30, 0x03, 0x00, 0x00, // vmovups [r13+0x330], xmm0
            0xc4, 0xc1, 0x7a, 0x7f, 0x8d, 0x40, 0x03, 0x00, 0x00, // vmovdqu [r13+0x340], xmm1
        },
        0x14ebce,
        0xefb3cd,
        0xefb3ce,
        0xe16c40,
    },
};

} // namespace

std::span<const BuildProfile> KnownBuilds() {
    return Builds;
}

std::string ToString(MatchKind kind) {
    switch (kind) {
    case MatchKind::Exact:
        return "exact";
    case MatchKind::Relocated:
        return "relocated";
    case MatchKind::Ambiguous:
        return "ambiguous";
    case MatchKind::NotFound:
        return "not found";
    case MatchKind::ImageTooSmall:
        return "image too small";
    }
    return "?";
}

MatchResult Match(const std::uint8_t* base, std::uint64_t size,
                  bool (*readable)(const std::uint8_t*, std::uint64_t, std::uint64_t)) {
    MatchResult r;
    const auto can_read = [&](std::uint64_t off, std::uint64_t n) {
        if (off + n > size || off + n < off) {
            return false;
        }
        return readable == nullptr || readable(base, off, n);
    };
    bool any_fits = false;
    for (const auto& b : Builds) {
        const std::uint64_t need =
            std::max({b.expected_start + b.expected.size(), b.invert_x_offset + 1,
                      b.invert_y_offset + 1, b.no_follow_offset + 1});
        if (need > size) {
            continue;
        }
        any_fits = true;
        if (!can_read(b.expected_start, b.expected.size()) || !can_read(b.invert_x_offset, 1) ||
            !can_read(b.invert_y_offset, 1) || !can_read(b.no_follow_offset, 1)) {
            continue;
        }
        if (std::memcmp(base + b.expected_start, b.expected.data(), b.expected.size()) == 0) {
            r.kind = MatchKind::Exact;
            r.profile = &b;
            r.found_start = b.expected_start;
            r.hits = 1;
            r.detail = std::string("known build: ") + b.name;
            return r;
        }
    }
    // Not at a known place: look for the camera code anywhere, to tell a relocated build from a
    // different game. Readable runs are scanned in 64 KB chunks.
    const auto& pattern = Builds[0].expected;
    std::vector<std::uint64_t> hits;
    constexpr std::uint64_t Chunk = 64 * 1024;
    for (std::uint64_t off = 0; off < size; off += Chunk) {
        const std::uint64_t len = std::min<std::uint64_t>(Chunk + pattern.size() - 1, size - off);
        if (len < pattern.size() || !can_read(off, len)) {
            continue;
        }
        const std::uint8_t* p = base + off;
        const std::uint8_t* end = p + len;
        while (true) {
            const auto* f = std::search(p, end, pattern.begin(), pattern.end());
            if (f == end) {
                break;
            }
            const std::uint64_t at = off + static_cast<std::uint64_t>(f - (base + off));
            if (std::find(hits.begin(), hits.end(), at) == hits.end()) {
                hits.push_back(at);
            }
            p = f + 1;
        }
    }
    r.hits = hits.size();
    char buf[200];
    if (hits.size() == 1) {
        r.kind = MatchKind::Relocated;
        r.found_start = hits[0];
        std::snprintf(buf, sizeof(buf),
                      "camera code found at %#llx instead of %#llx: a build whose data offsets "
                      "were never measured",
                      static_cast<unsigned long long>(hits[0]),
                      static_cast<unsigned long long>(Builds[0].expected_start));
        r.detail = buf;
    } else if (hits.size() > 1) {
        r.kind = MatchKind::Ambiguous;
        std::snprintf(buf, sizeof(buf), "camera code pattern found %zu times", hits.size());
        r.detail = buf;
    } else if (!any_fits) {
        r.kind = MatchKind::ImageTooSmall;
        r.detail = "the executable is smaller than the measured build";
    } else {
        r.kind = MatchKind::NotFound;
        r.detail = "the camera code is not in this executable (another game or game version)";
    }
    return r;
}

} // namespace Input::CameraHook
