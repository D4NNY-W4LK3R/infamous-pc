// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "input/camera_hook_signature.h"

using namespace Input::CameraHook;

namespace {

std::vector<std::uint8_t> Image(std::uint64_t size) {
    std::vector<std::uint8_t> img(size);
    for (std::uint64_t i = 0; i < size; ++i) {
        img[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 13); // filler, not the pattern
    }
    return img;
}

void Place(std::vector<std::uint8_t>& img, std::uint64_t at) {
    const auto& b = KnownBuilds()[0];
    std::memcpy(img.data() + at, b.expected.data(), b.expected.size());
}

} // namespace

TEST(CameraHookSignature, KnownBuildIsSafeToHook) {
    const auto& b = KnownBuilds()[0];
    auto img = Image(b.invert_y_offset + 4096);
    Place(img, b.expected_start);
    const auto r = Match(img.data(), img.size());
    EXPECT_EQ(r.kind, MatchKind::Exact);
    ASSERT_TRUE(r.SafeToHook());
    EXPECT_EQ(r.profile->store_offset, 0x14ebceu);
    EXPECT_EQ(r.profile->store_offset - r.profile->expected_start, 9u);
}

TEST(CameraHookSignature, RelocatedCodeIsReportedButNeverHooked) {
    const auto& b = KnownBuilds()[0];
    auto img = Image(b.invert_y_offset + 4096);
    Place(img, b.expected_start + 0x400000 - 0x400000 + 0x1230); // same code, other offset
    const auto r = Match(img.data(), img.size());
    EXPECT_EQ(r.kind, MatchKind::Relocated);
    EXPECT_FALSE(r.SafeToHook());
    EXPECT_EQ(r.found_start, b.expected_start + 0x1230);
}

TEST(CameraHookSignature, OneChangedByteMeansNoHook) {
    const auto& b = KnownBuilds()[0];
    for (std::size_t i = 0; i < b.expected.size(); ++i) {
        auto img = Image(b.invert_y_offset + 64);
        Place(img, b.expected_start);
        img[b.expected_start + i] ^= 0x01;
        const auto r = Match(img.data(), img.size());
        EXPECT_FALSE(r.SafeToHook()) << "byte " << i;
    }
}

TEST(CameraHookSignature, AnotherGameIsNotFound) {
    auto img = Image(16 << 20);
    const auto r = Match(img.data(), img.size());
    EXPECT_EQ(r.kind, MatchKind::NotFound);
    EXPECT_FALSE(r.SafeToHook());
}

TEST(CameraHookSignature, SmallImageIsRefused) {
    auto img = Image(0x100000);
    const auto r = Match(img.data(), img.size());
    EXPECT_EQ(r.kind, MatchKind::ImageTooSmall);
}

TEST(CameraHookSignature, DuplicatePatternIsAmbiguous) {
    const auto& b = KnownBuilds()[0];
    auto img = Image(b.invert_y_offset + 64);
    Place(img, 0x1000);
    Place(img, 0x9000);
    EXPECT_EQ(Match(img.data(), img.size()).kind, MatchKind::Ambiguous);
}

TEST(CameraHookSignature, UnreadableRangesAreSkipped) {
    const auto& b = KnownBuilds()[0];
    auto img = Image(b.invert_y_offset + 64);
    Place(img, b.expected_start);
    // The data page with the invert flags is not mapped: never treat the build as known.
    const auto r = Match(img.data(), img.size(),
                         [](const std::uint8_t*, std::uint64_t off, std::uint64_t n) {
                             return !(off <= 0xefb3cd && off + n > 0xefb3cd);
                         });
    EXPECT_FALSE(r.SafeToHook());
}
