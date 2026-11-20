// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <vector>

#include "common/interval_set.h"
#include "common/types.h"

// The pure parts of leaving empty buffer memory out of video memory (build 44), see
// BufferCache::EnsureResidentLazy: block ranges are in blocks, addresses in bytes.
namespace VideoCore::LazyZero {

/// `from` without what `removed` covers.
inline IntervalList<> Subtract(const IntervalList<>& from,
                                       const IntervalList<>& removed) {
    IntervalList<> kept;
    kept.Reserve(from.Size() + removed.Size());
    auto r = removed.begin();
    for (const auto& range : from) {
        u64 at = range.start;
        while (r != removed.end() && r->end <= at) {
            ++r;
        }
        for (auto it = r; it != removed.end() && it->start < range.end; ++it) {
            if (it->start > at) {
                kept.Add({at, it->start});
            }
            at = std::max(at, it->end);
            if (it->end >= range.end) {
                break;
            }
        }
        if (at < range.end) {
            kept.Add({at, range.end});
        }
    }
    return kept;
}

struct Piece {
    u64 addr;
    u64 size;
    bool operator==(const Piece&) const = default;
};

struct Split {
    /// What still has to be copied, in order, neighbours merged: nothing in blocks taken as zeros.
    std::vector<Piece> kept;
    /// Blocks taken as zeros that get data: to be made resident and uploaded whole.
    IntervalList<> wake;
    /// Bytes left out, all zeros.
    u64 dropped = 0;
};

/// Splits uploads around the blocks taken as zeros: a part landing in one is left out, and the
/// block woken unless `is_zero(addr, size)` says the guest memory there is all zeros. A woken
/// block is uploaded whole from guest memory, which holds what the GPU should see for it.
template <class IsZero>
Split SplitUploads(const std::vector<Piece>& copies, const IntervalList<>& lazy,
                   u32 block_shift, IsZero&& is_zero) {
    Split out;
    const u64 block_size = u64{1} << block_shift;
    const auto keep = [&](u64 addr, u64 size) {
        if (size == 0) {
            return;
        }
        if (!out.kept.empty() && out.kept.back().addr + out.kept.back().size == addr) {
            out.kept.back().size += size;
        } else {
            out.kept.push_back({addr, size});
        }
    };
    for (const auto& copy : copies) {
        if (copy.size == 0) {
            continue;
        }
        const u64 start = copy.addr;
        const u64 stop = copy.addr + copy.size;
        u64 at = start;
        lazy.ForEachInRange(start >> block_shift, ((stop - 1) >> block_shift) + 1,
                            [&](const Interval& range) {
                                const u64 lazy_start = std::max(start, range.start << block_shift);
                                const u64 lazy_stop = std::min(stop, range.end << block_shift);
                                keep(at, lazy_start - at);
                                for (u64 p = lazy_start; p < lazy_stop;) {
                                    const u64 block_start = p & ~(block_size - 1);
                                    const u64 q = std::min(lazy_stop, block_start + block_size);
                                    if (is_zero(p, q - p)) {
                                        out.dropped += q - p;
                                    } else {
                                        out.wake.Add({block_start >> block_shift,
                                                      (block_start >> block_shift) + 1});
                                    }
                                    p = q;
                                }
                                at = lazy_stop;
                            });
        keep(at, stop - at);
    }
    return out;
}

} // namespace VideoCore::LazyZero
