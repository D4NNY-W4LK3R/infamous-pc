// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "common/shared_user_dir.h"

namespace fs = std::filesystem;
using Common::FS::ChooseSharedUserDir;

namespace {

fs::path Fresh(const std::string& name) {
    const auto p = fs::temp_directory_path() / "ssa_shared_dir_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void Touch(const fs::path& file, const std::string& text, int age_minutes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(age_minutes));
}

/// A build folder like the player's: the emulator, and a user folder with one save.
fs::path Build(const fs::path& parent, const std::string& name, int save_age_minutes) {
    const auto dir = parent / name;
    fs::create_directories(dir);
    std::ofstream(dir / "shadPS4.exe") << "exe";
    if (save_age_minutes >= 0) {
        Touch(dir / "user/home/1000/savedata/CUSA00004/infamousssUser0/dis.dat", name,
              save_age_minutes);
    }
    return dir;
}

} // namespace

TEST(SharedUserDir, FirstLaunchCopiesTheMostRecentlyPlayedBuildsData) {
    const auto parent = Fresh("adopt");
    Build(parent, "SecondSonAdaptive-windows-x64", 600);
    const auto played = Build(parent, "SecondSonAdaptive-windows-x64 (1)", 5);
    const auto fresh = Build(parent, "SecondSonAdaptive-windows-x64 (2)", -1);
    fs::create_directories(fresh / "user"); // the new build's own, empty
    const auto r = ChooseSharedUserDir(fresh, false);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->user_dir, parent / "SecondSonAdaptive-data" / "user");
    EXPECT_EQ(r->copied_from, played / "user");
    std::ifstream save(r->user_dir / "home/1000/savedata/CUSA00004/infamousssUser0/dis.dat");
    std::string text;
    std::getline(save, text);
    EXPECT_EQ(text, "SecondSonAdaptive-windows-x64 (1)");
    EXPECT_TRUE(fs::exists(played / "user/home")) << "copied, never moved";
    EXPECT_TRUE(fs::exists(parent / "SecondSonAdaptive-data" / "README.txt"));
    // The next launch, from any build folder, uses it as it is.
    const auto again = ChooseSharedUserDir(parent / "SecondSonAdaptive-windows-x64", false);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->user_dir, r->user_dir);
    EXPECT_TRUE(again->copied_from.empty());
}

TEST(SharedUserDir, PortableBuildsKeepTheirOwnFolder) {
    const auto parent = Fresh("portable");
    const auto dir = Build(parent, "build", 5);
    EXPECT_FALSE(ChooseSharedUserDir(dir, true).has_value());
    std::ofstream(dir / "portable.txt") << "";
    EXPECT_FALSE(ChooseSharedUserDir(dir, false).has_value());
    EXPECT_FALSE(fs::exists(parent / "SecondSonAdaptive-data"));
}

TEST(SharedUserDir, NoEarlierDataStartsEmpty) {
    const auto parent = Fresh("empty");
    const auto dir = Build(parent, "build", -1);
    const auto r = ChooseSharedUserDir(dir, false);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->copied_from.empty());
    EXPECT_TRUE(fs::is_directory(r->user_dir));
}
