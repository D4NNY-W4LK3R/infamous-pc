// SPDX-FileCopyrightText: 2026 infamous-pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone regression tests; no game files needed.
// clang++ -std=c++20 -O2 -Wall -Wextra -pedantic tests/infamous_dash_cheats_test.cpp -o dash-tests && ./dash-tests
#include "../src/common/infamous_dash_cheats.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace InfamousDashCheats;

static void Seed(std::vector<std::uint8_t>& image) {
    using namespace Detail;
    std::memcpy(image.data() + NeonSite, NeonSignature.data(), NeonSignature.size());
    std::memcpy(image.data() + VideoSite, VideoSignature.data(), VideoSignature.size());
    float limit = 1.5f;
    std::memcpy(image.data() + VideoLimit, &limit, sizeof limit);
    std::memcpy(image.data() + MagicReader, MagicSignature.data(), MagicSignature.size());
    std::memcpy(image.data() + HeroReader, "\x48\x8d\x05", 3);
    const std::int32_t disp = std::int32_t(HeroGlobal - HeroReader - 7);
    std::memcpy(image.data() + HeroReader + 3, &disp, 4);
    std::memcpy(image.data() + HeroReader + 7, HeroReaderTail.data(), HeroReaderTail.size());
}

int main() {
    std::vector<std::uint8_t> image(Detail::HeroGlobal + 0x100, 0xcc);
    Seed(image);
    assert(Detail::NeonMatches(image.data(), image.size()));
    assert(Detail::VideoMatches(image.data(), image.size()));
    assert(Detail::SmokeMatches(image.data(), image.size()));

    const Settings both{true, true, 1.0f};
    auto result = Apply("CUSA00223", "01.07", image.data(), image.size(), both);
    assert(result.neon == Status::Applied && result.video == Status::Applied);
    assert(result.smoke == Status::Disabled);
    assert(image[Detail::NeonBranch] == 0x90 &&
           image[Detail::NeonBranch + 1] == 0x90 &&
           image[Detail::VideoBranch] == 0x90 &&
           image[Detail::VideoBranch + 1] == 0x90);

    for (int i = 0; i < 100; ++i) {
        Seed(image);
        const auto at = (i % 2) ? Detail::NeonSite : Detail::VideoSite;
        image[at] ^= 1;
        result = Apply("CUSA00223", "01.07", image.data(), image.size(), both);
        assert(((i % 2) ? result.neon : result.video) == Status::SignatureMismatch);
        assert(((i % 2) ? result.video : result.neon) == Status::Applied);
    }
    Seed(image);
    result = Apply("CUSA00004", "01.07", image.data(), image.size(), both);
    assert(result.neon == Status::Unsupported);
    assert(image[Detail::NeonBranch] == 0x74);
    result = Apply("CUSA00223", "01.00", image.data(), image.size(), both);
    assert(result.neon == Status::Unsupported);

    Seed(image);
    result = Apply("CUSA00223", "01.07", image.data(), image.size(), {});
    assert(result.neon == Status::Disabled && result.video == Status::Disabled);
    assert(image[Detail::NeonBranch] == 0x74 && image[Detail::VideoBranch] == 0x77);

    std::cout << "Second Son dash: signatures, mutation guards, version guard and 100 corruptions pass\n";
}
