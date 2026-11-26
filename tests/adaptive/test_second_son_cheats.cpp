// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/second_son_cheats.h"

using namespace Core::SecondSonCheats;

namespace {

constexpr std::uint64_t ImageSize = 0x1d0cfe0 + 0x100;
const ReadableFn All = [](std::uint64_t, std::uint64_t) { return true; };

/// An image holding the game's own bytes at every byte cheat's sites.
std::vector<std::uint8_t> GameImage() {
    std::vector<std::uint8_t> img(ImageSize, 0xcc);
    for (std::size_t i = 0; i < NumCheats; ++i) {
        for (const auto& w : Writes(static_cast<Id>(i))) {
            std::memcpy(img.data() + w.offset, w.off.data(), w.off.size());
        }
    }
    return img;
}

void Put(std::vector<std::uint8_t>& img, const Write& w, bool on) {
    const auto& b = on ? w.on : w.off;
    std::memcpy(img.data() + w.offset, b.data(), b.size());
}

} // namespace

TEST(SecondSonCheats, WritesKeepTheirSizes) {
    for (std::size_t i = 0; i < NumCheats; ++i) {
        for (const auto& w : Writes(static_cast<Id>(i))) {
            EXPECT_EQ(w.off.size(), w.on.size()) << Name(static_cast<Id>(i));
            EXPECT_NE(w.off, w.on);
        }
    }
    EXPECT_TRUE(Writes(Id::SmokeDash).empty());
    ASSERT_EQ(Writes(Id::Finisher).size(), 2u);
    // The cave comes first, so switching on writes it before the jump to it.
    EXPECT_EQ(Writes(Id::Finisher)[0].offset, 0xcc7800u);
}

TEST(SecondSonCheats, GoldHenCheatsFollowTheirBytes) {
    auto img = GameImage();
    for (const Id id : {Id::Energy, Id::Bombs, Id::Shards, Id::Finisher}) {
        EXPECT_EQ(Check(img.data(), img.size(), id, All), Found::Off) << Name(id);
        for (const auto& w : Writes(id)) {
            Put(img, w, true);
        }
        EXPECT_EQ(Check(img.data(), img.size(), id, All), Found::On) << Name(id);
        for (const auto& w : Writes(id)) {
            Put(img, w, false);
        }
    }
    // Other bytes there (another version, or the emulator's own patches): not available.
    img[0x20c7f9] = 0x48;
    EXPECT_EQ(Check(img.data(), img.size(), Id::Energy, All), Found::Different);
}

TEST(SecondSonCheats, FinisherCaveWithoutTheJumpIsOff) {
    auto img = GameImage();
    const auto writes = Writes(Id::Finisher);
    Put(img, writes[0], true);
    EXPECT_EQ(Check(img.data(), img.size(), Id::Finisher, All), Found::Off);
    // The jump without the cave would jump into zeros: never "on".
    Put(img, writes[0], false);
    Put(img, writes[1], true);
    EXPECT_EQ(Check(img.data(), img.size(), Id::Finisher, All), Found::Different);
}

TEST(SecondSonCheats, DashCheatsNeedTheirSignatures) {
    auto img = GameImage();
    // Only the branch bytes are there, not the code around them: not this version.
    EXPECT_EQ(Check(img.data(), img.size(), Id::NeonDash, All), Found::Different);
    EXPECT_EQ(Check(img.data(), img.size(), Id::VideoDash, All), Found::Different);
    EXPECT_EQ(Check(img.data(), img.size(), Id::SmokeDash, All), Found::Different);

    const std::uint8_t neon[] = {0x48, 0x8d, 0x3d, 0x4f, 0xb3, 0xc0, 0x00, 0xbe,
                                 0x59, 0x00, 0x00, 0x00, 0x31, 0xd2, 0xe8, 0xbb,
                                 0x34, 0xef, 0xff, 0x84, 0xc0, 0x74, 0x35};
    std::memcpy(img.data() + 0x2ef9a2, neon, sizeof(neon));
    EXPECT_EQ(Check(img.data(), img.size(), Id::NeonDash, All), Found::Off);
    Put(img, Writes(Id::NeonDash)[0], true);
    EXPECT_EQ(Check(img.data(), img.size(), Id::NeonDash, All), Found::On);

    const std::uint8_t video[] = {0xc5, 0xf8, 0x2e, 0x05, 0x15, 0x44, 0x58, 0x00,
                                  0x77, 0x10, 0x34, 0x01, 0x74, 0x15, 0xeb, 0x0a};
    std::memcpy(img.data() + 0x55ddf7, video, sizeof(video));
    EXPECT_EQ(Check(img.data(), img.size(), Id::VideoDash, All), Found::Different); // no limit
    const float limit = 1.5f;
    std::memcpy(img.data() + 0xae2214, &limit, sizeof(limit));
    EXPECT_EQ(Check(img.data(), img.size(), Id::VideoDash, All), Found::Off);

    const std::uint8_t magic[] = {0xc5, 0xfa, 0x10, 0x97, 0xe8, 0xa1, 0x01, 0x00,
                                  0xc5, 0xfa, 0x10, 0x8f, 0xf0, 0xa1, 0x01, 0x00};
    std::memcpy(img.data() + 0x214b35, magic, sizeof(magic));
    // lea rax, [rip + disp] reaching the hero pointer, then its tail.
    const std::int32_t disp = static_cast<std::int32_t>(0x1d0cfe0 - (0x2659a5 + 7));
    const std::uint8_t lea[] = {0x48, 0x8d, 0x05};
    std::memcpy(img.data() + 0x2659a5, lea, sizeof(lea));
    std::memcpy(img.data() + 0x2659a5 + 3, &disp, sizeof(disp));
    const std::uint8_t tail[] = {0x48, 0x8b, 0x38, 0xc5, 0xfa, 0x10, 0x87, 0xf0, 0xa1, 0x01, 0x00};
    std::memcpy(img.data() + 0x2659a5 + 7, tail, sizeof(tail));
    EXPECT_EQ(Check(img.data(), img.size(), Id::SmokeDash, All), Found::Off);
}

TEST(SecondSonCheats, UnreadableOrShortImagesAreNotAvailable) {
    auto img = GameImage();
    const ReadableFn none = [](std::uint64_t, std::uint64_t) { return false; };
    EXPECT_EQ(Check(img.data(), img.size(), Id::Energy, none), Found::Different);
    EXPECT_EQ(Check(img.data(), 0x1000, Id::Energy, All), Found::Different);
    EXPECT_EQ(Check(nullptr, 0, Id::Bombs, All), Found::Different);
}
