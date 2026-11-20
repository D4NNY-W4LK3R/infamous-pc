// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

// Stencil art probe (inFAMOUS Second Son, build 38). The game holds the spray can sideways for
// stencil art and upright for other graffiti, and only the game knows which it is doing: the
// player presses P to tell the emulated motion sensor. To find code of the game that runs only
// during stencil art, so a later build can switch the hold by itself, the game's main thread is
// sampled while the spray can is out: where it is, and which return addresses into the game's
// code are on its stack, counted by the hold P selected. The log gets the addresses found in
// most samples with one hold and in almost none with the other or in gameplay.
//
// Only Windows samples. A sample suspends the thread just to copy its registers and a slice of
// its stack, with nothing allocated meanwhile, and sampling ends after a few minutes of each.
// SS_STENCIL_PROBE=0 turns it off.
namespace Core::StencilProbe {

/// The game's own code (the eboot), once it is loaded.
void SetGameCode(VAddr base, u64 size);

/// Called by the game's thread that reads the pad (its main thread): sampled from then on.
void NoteGameThread();

/// Logs what was found: at shutdown (Adaptive::Glue::Shutdown), and every minute while the
/// counts grow.
void Report();

/// Pure part, tested: addresses in at least `min_share` of the samples of `bucket` and at most
/// `max_other` of those of every other bucket with samples, most frequent first.
struct Candidate {
    u64 offset;
    double share;      ///< of the bucket's samples
    double other_max;  ///< the largest share in another bucket
};
inline std::vector<Candidate> Distinctive(const std::vector<std::unordered_map<u64, u32>>& counts,
                                          const std::vector<u32>& samples, std::size_t bucket,
                                          double min_share, double max_other, std::size_t limit) {
    std::vector<Candidate> out;
    if (bucket >= counts.size() || bucket >= samples.size() || samples[bucket] == 0) {
        return out;
    }
    const double n = static_cast<double>(samples[bucket]);
    for (const auto& [offset, count] : counts[bucket]) {
        const double share = static_cast<double>(count) / n;
        if (share < min_share) {
            continue;
        }
        double other_max = 0.0;
        for (std::size_t b = 0; b < counts.size() && b < samples.size(); ++b) {
            if (b == bucket || samples[b] == 0) {
                continue;
            }
            const auto it = counts[b].find(offset);
            const double other =
                it == counts[b].end() ? 0.0
                                      : static_cast<double>(it->second) / samples[b];
            other_max = std::max(other_max, other);
        }
        if (other_max <= max_other) {
            out.push_back({offset, share, other_max});
        }
    }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        return a.share != b.share ? a.share > b.share : a.offset < b.offset;
    });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

} // namespace Core::StencilProbe
