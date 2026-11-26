// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "core/second_son_patches.h"

using namespace Core::SecondSonPatches;

namespace {

constexpr std::uint64_t ImageSize = 0x7f2d2c0 + 0x100;

std::unique_ptr<std::uint8_t[]> Image(const Options& with) {
    auto img = std::make_unique<std::uint8_t[]>(ImageSize);
    std::memset(img.get(), 0xcc, ImageSize);
    for (const auto& w : ExpectedOriginals(with)) {
        std::memcpy(img.get() + w.offset, w.bytes.data(), w.bytes.size());
    }
    return img;
}

const ReadableFn All = [](std::uint64_t, std::uint64_t) { return true; };

Options Everything() {
    Options o;
    o.particle_late_read = true;
    o.light_one_submit = true;
    o.perf_mode = 2;
    return o;
}

} // namespace

TEST(SecondSonPatches, Update107GetsEveryPatch) {
    const auto img = Image(Everything());
    const Plan p = MakePlan(img.get(), ImageSize, Everything(), All);
    EXPECT_TRUE(p.eboot_107);
    EXPECT_TRUE(p.late_read);
    EXPECT_TRUE(p.light_one_submit);
    EXPECT_EQ(p.perf_mode, 2u);
    // late-read writes 3 sites, one-submit 1, office 7
    EXPECT_EQ(p.writes.size(), 3u + 1u + 7u);
    for (const auto& w : p.writes) {
        EXPECT_LE(w.offset + w.bytes.size(), ImageSize);
    }
}

TEST(SecondSonPatches, OtherGameCodeIsNeverTouched) {
    auto img = std::make_unique<std::uint8_t[]>(ImageSize);
    std::memset(img.get(), 0x90, ImageSize);
    const Plan p = MakePlan(img.get(), ImageSize, Everything(), All);
    EXPECT_FALSE(p.eboot_107);
    EXPECT_TRUE(p.writes.empty());
}

TEST(SecondSonPatches, OneDifferentByteRefusesOnlyThatPatch) {
    auto img = Image(Everything());
    img[0x29cd1e] ^= 0xff; // inside the late-read's sites
    const Plan p = MakePlan(img.get(), ImageSize, Everything(), All);
    EXPECT_FALSE(p.late_read);
    EXPECT_FALSE(p.light_one_submit) << "needs late-read";
    EXPECT_EQ(p.perf_mode, 2u) << "an independent patch still applies";
    EXPECT_EQ(p.writes.size(), 7u);
}

TEST(SecondSonPatches, SmallOrUnreadableImagesAreRefused) {
    const auto img = Image(Everything());
    EXPECT_TRUE(MakePlan(img.get(), 0x600000, Everything(), All).writes.empty());
    const ReadableFn gap = [](std::uint64_t off, std::uint64_t) { return off < 0x300000; };
    const Plan p = MakePlan(img.get(), ImageSize, Everything(), gap);
    EXPECT_TRUE(p.writes.empty());
}

TEST(SecondSonPatches, DefaultsAreLateReadOnly) {
    const Options d;
    const auto img = Image(d);
    const Plan p = MakePlan(img.get(), ImageSize, d, All);
    EXPECT_TRUE(p.late_read);
    EXPECT_FALSE(p.light_one_submit);
    EXPECT_EQ(p.perf_mode, 0u);
    EXPECT_EQ(p.writes.size(), 3u);
}

TEST(SecondSonPatches, LowModeWritesTheLadderRow) {
    Options o;
    o.particle_late_read = false;
    o.perf_mode = 1;
    const auto img = Image(o);
    const Plan p = MakePlan(img.get(), ImageSize, o, All);
    ASSERT_EQ(p.writes.size(), 5u);
    float lod = 0.0f;
    std::memcpy(&lod, p.writes[0].bytes.data(), 4);
    EXPECT_FLOAT_EQ(lod, 0.7f);
    EXPECT_EQ(p.writes[0].offset, 0xda08c0u);
}

namespace {
std::uint32_t Read32(const std::uint8_t* img, std::uint64_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, img + off, 4);
    return v;
}
} // namespace

TEST(SecondSonPatches, RenderSizeWritesTheSixSizesAndKeepsTheHudLayout) {
    Options o;
    o.particle_late_read = false;
    o.render_height = 792;
    auto img = Image(o);
    const Plan p = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_TRUE(p.eboot_107);
    EXPECT_EQ(p.render_width, 1408u);
    EXPECT_EQ(p.render_height, 792u);
    ASSERT_EQ(p.writes.size(), 6u);
    for (const auto& w : p.writes) {
        std::memcpy(img.get() + w.offset, w.bytes.data(), w.bytes.size());
    }
    for (const std::uint64_t off : {0x1eff4aull, 0x1eff57ull, 0x1f0d3eull}) {
        EXPECT_EQ(Read32(img.get(), off), 1408u) << std::hex << off;
    }
    for (const std::uint64_t off : {0x1eff51ull, 0x1eff5eull, 0x1f0d4dull}) {
        EXPECT_EQ(Read32(img.get(), off), 792u) << std::hex << off;
    }
    EXPECT_EQ(Read32(img.get(), 0x1f0403), 1920u); // HUD layout untouched
    EXPECT_EQ(Read32(img.get(), 0x1f03fb), 1080u);
}

TEST(SecondSonPatches, RenderSizeIsRefusedAloneWhenItsCodeDiffers) {
    Options o;
    o.particle_late_read = true;
    o.perf_mode = 1;
    o.render_height = 720;
    auto img = Image(o);
    img[0x1f0403] = 0x00; // the HUD layout's 1920 is something else here
    const Plan p = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_TRUE(p.late_read);
    EXPECT_EQ(p.perf_mode, 1u);
    EXPECT_EQ(p.render_height, 1080u);
    for (const auto& w : p.writes) {
        EXPECT_FALSE(w.offset >= 0x1eff40 && w.offset < 0x1f0d60) << "no size was written";
    }
}

TEST(SecondSonPatches, OnlyTheListedRenderSizes) {
    Options o;
    o.particle_late_read = false;
    o.render_height = 700;
    auto img = Image(o);
    const Plan p = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_TRUE(p.writes.empty());
    EXPECT_EQ(p.render_height, 1080u);
    o.render_height = 1080; // the game's own: nothing to do
    const Plan q = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_TRUE(q.writes.empty());
}

TEST(SecondSonPatches, OfficeTexturesAloneTouchOnlyTheTextureSlots) {
    Options o;
    o.particle_late_read = false;
    o.office_textures = true;
    const auto img = Image(o);
    const Plan p = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_TRUE(p.office_textures);
    EXPECT_EQ(p.perf_mode, 0u);
    ASSERT_EQ(p.writes.size(), 2u);
    for (const auto& w : p.writes) {
        EXPECT_TRUE(w.offset == 0xfe9a2 || w.offset == 0xfe9b2);
        EXPECT_EQ(w.bytes, std::vector<std::uint8_t>{0x00});
    }
    auto changed = Image(o);
    changed[0xfe9b2] = 0x03;
    const Plan refused = MakePlan(changed.get(), ImageSize, o, All);
    EXPECT_FALSE(refused.office_textures);
    EXPECT_TRUE(refused.writes.empty());
}

TEST(SecondSonPatches, LowWithOfficeTexturesHasNoHighResBlocks) {
    Options o;
    o.particle_late_read = false;
    o.perf_mode = 1;
    o.office_textures = true;
    const auto img = Image(o);
    const Plan p = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_EQ(p.perf_mode, 1u);
    EXPECT_TRUE(p.office_textures);
    int slots = 0;
    for (const auto& w : p.writes) {
        if (w.offset == 0xfe9a2 || w.offset == 0xfe9b2) {
            EXPECT_EQ(w.bytes, std::vector<std::uint8_t>{0x00});
            ++slots;
        }
    }
    EXPECT_EQ(slots, 2);
    o.office_textures = false;
    const Plan low = MakePlan(img.get(), ImageSize, o, All);
    EXPECT_FALSE(low.office_textures);
    for (const auto& w : low.writes) {
        if (w.offset == 0xfe9a2) {
            EXPECT_EQ(w.bytes, std::vector<std::uint8_t>{0x02}) << "low keeps 2 blocks";
        }
    }
}
