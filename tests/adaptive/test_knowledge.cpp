// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <random>

#include <gtest/gtest.h>

#include "adaptive/knowledge.h"

using namespace Adaptive;

namespace {

std::filesystem::path TempDir(const char* name) {
    auto p = std::filesystem::temp_directory_path() / "ssa_tests" / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

Knowledge Sample() {
    Knowledge k;
    Profile& p = k.profiles["abc"];
    p.fingerprint = {"Ryzen 5|6c12t|16GB|10de:1f99|GTX 1650 Ti", "576.80.0.0",
                     "CUSA00223|01.07|deadbeef", "0.19.1 hybrid"};
    p.validated["SS_TEXCACHE_GC_HEADROOM_MB"] = 256;
    p.rejected["SHAD_READBACK_WINDOW_KB"][256] = Outcome::Regressed;
    p.rejected["SS_PIPELINE_WORKERS"][6] = Outcome::Crashed;
    p.revalidate["SHAD_AHEAD_MAX"] = 32;
    p.revalidation_reason = "GPU driver changed";
    p.last_bottleneck = "GPU command thread";
    p.sessions = 7;
    p.unclean_sessions = 1;
    p.baseline_seconds_ms = {33.1, 33.4, 32.9};
    p.baseline_seconds_p99_ms = {40.0, 41.5, 39.0};
    ExperimentRecord e;
    e.knob = "SHAD_READBACK_WINDOW_KB";
    e.baseline_value = 128;
    e.value = 256;
    e.outcome = Outcome::Regressed;
    e.median_before_ms = 33.2;
    e.median_after_ms = 35.0;
    e.seconds_before = 90;
    e.seconds_after = 90;
    e.p_value = 0.001;
    e.unix_time = 1795000000;
    e.driver = "576.80.0.0";
    e.method = "launch-ab";
    p.history.push_back(e);
    k.session_open = true;
    k.session_profile = "abc";
    k.session_experiment = PendingExperiment{"SHAD_HOT_WINDOW_SECS", 5, 8};
    k.session_launch_values["SHAD_HOT_WINDOW_SECS"] = 8;
    return k;
}

} // namespace

TEST(Knowledge, RoundTripPreservesEverything) {
    const Knowledge k = Sample();
    const std::string text = KnowledgeStore::Serialize(k);
    Knowledge back;
    bool schema_changed = true;
    ASSERT_TRUE(KnowledgeStore::Parse(text, back, schema_changed));
    EXPECT_FALSE(schema_changed);
    EXPECT_EQ(KnowledgeStore::Serialize(back), text);
    const Profile& p = back.profiles.at("abc");
    EXPECT_EQ(p.fingerprint, k.profiles.at("abc").fingerprint);
    EXPECT_EQ(p.validated.at("SS_TEXCACHE_GC_HEADROOM_MB"), 256);
    EXPECT_EQ(p.rejected.at("SS_PIPELINE_WORKERS").at(6), Outcome::Crashed);
    EXPECT_EQ(p.revalidate.at("SHAD_AHEAD_MAX"), 32);
    EXPECT_EQ(p.history.size(), 1u);
    EXPECT_EQ(p.history[0].method, "launch-ab");
    ASSERT_TRUE(back.session_experiment.has_value());
    EXPECT_EQ(back.session_experiment->value, 8);
}

TEST(Knowledge, EveryFlippedByteIsDetected) {
    const std::string text = KnowledgeStore::Serialize(Sample());
    const auto cpos = text.rfind("checksum ");
    int detected = 0, total = 0;
    for (std::size_t i = 0; i < cpos; i += 7) {
        std::string bad = text;
        bad[i] = static_cast<char>(bad[i] ^ 0x20);
        Knowledge k;
        bool sc = false;
        ++total;
        detected += KnowledgeStore::Parse(bad, k, sc) ? 0 : 1;
    }
    EXPECT_EQ(detected, total);
}

TEST(Knowledge, TruncatedFileIsRejected) {
    const std::string text = KnowledgeStore::Serialize(Sample());
    for (std::size_t len : {std::size_t{0}, std::size_t{10}, text.size() / 2, text.size() - 3}) {
        Knowledge k;
        bool sc = false;
        EXPECT_FALSE(KnowledgeStore::Parse(text.substr(0, len), k, sc)) << len;
    }
}

TEST(Knowledge, StringsWithSpacesAndPercentSurvive) {
    Knowledge k;
    k.profiles["x"].fingerprint.hardware = "Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz 100%\nok";
    Knowledge back;
    bool sc = false;
    ASSERT_TRUE(KnowledgeStore::Parse(KnowledgeStore::Serialize(k), back, sc));
    EXPECT_EQ(back.profiles["x"].fingerprint.hardware, k.profiles["x"].fingerprint.hardware);
}

TEST(KnowledgeStore, SaveLoadAndBackupRecovery) {
    const auto dir = TempDir("backup");
    KnowledgeStore store(dir / "knowledge.txt");
    Knowledge k;
    EXPECT_EQ(store.Load(k), LoadStatus::Fresh);
    Knowledge a = Sample();
    ASSERT_TRUE(store.Save(a));
    a.profiles["abc"].sessions = 8;
    ASSERT_TRUE(store.Save(a)); // first version is now the .bak
    ASSERT_TRUE(std::filesystem::exists(dir / "knowledge.txt.bak"));
    // Corrupt the main file as a crash mid-write on a non-atomic FS would.
    {
        std::ofstream f(dir / "knowledge.txt", std::ios::binary | std::ios::trunc);
        f << "SSADAPTIVE-KNOWLEDGE 1\nprofile abc\nfp.hard";
    }
    Knowledge loaded;
    EXPECT_EQ(store.Load(loaded), LoadStatus::RecoveredBackup);
    EXPECT_EQ(loaded.profiles.at("abc").sessions, 7u);
    EXPECT_TRUE(std::filesystem::exists(dir / "knowledge.txt.corrupt"));
}

TEST(KnowledgeStore, BothCorruptStartsFreshAndKeepsEvidence) {
    const auto dir = TempDir("bothbad");
    KnowledgeStore store(dir / "knowledge.txt");
    std::ofstream(dir / "knowledge.txt") << "garbage";
    std::ofstream(dir / "knowledge.txt.bak") << "garbage too";
    Knowledge k;
    EXPECT_EQ(store.Load(k), LoadStatus::BothCorrupt);
    EXPECT_TRUE(k.profiles.empty());
    EXPECT_TRUE(std::filesystem::exists(dir / "knowledge.txt.corrupt"));
}

TEST(KnowledgeStore, HundredSaveReloadCycles) {
    const auto dir = TempDir("cycles");
    KnowledgeStore store(dir / "knowledge.txt");
    Knowledge k = Sample();
    std::mt19937 rng(3);
    for (int i = 0; i < 100; ++i) {
        k.profiles["abc"].sessions = static_cast<std::uint32_t>(i);
        k.profiles["abc"].baseline_seconds_ms.push_back(30.0 + (rng() % 1000) / 100.0);
        ASSERT_TRUE(store.Save(k));
        Knowledge back;
        ASSERT_EQ(store.Load(back), LoadStatus::Loaded);
        ASSERT_EQ(KnowledgeStore::Serialize(back), KnowledgeStore::Serialize(k));
    }
}

TEST(KnowledgeStore, SchemaChangeKeepsHistoryDropsLearnedValues) {
    std::string text = KnowledgeStore::Serialize(Sample());
    // Rewrite as schema 0 with a valid checksum.
    text = text.substr(0, text.rfind("checksum "));
    text.replace(text.find(" 1\n"), 3, " 0\n");
    text += "checksum " + Hex64(Fnv1a64(text.data(), text.size())) + "\n";
    Knowledge k;
    bool sc = false;
    ASSERT_TRUE(KnowledgeStore::Parse(text, k, sc));
    EXPECT_TRUE(sc);
    const Profile& p = k.profiles.at("abc");
    EXPECT_TRUE(p.validated.empty());
    EXPECT_EQ(p.revalidate.at("SS_TEXCACHE_GC_HEADROOM_MB"), 256);
    EXPECT_EQ(p.history.size(), 1u);
    EXPECT_EQ(p.rejected.at("SS_PIPELINE_WORKERS").at(6), Outcome::Crashed);
}

TEST(SelectProfile, DriverChangeRevalidatesAndForgivesOnlyRegressions) {
    Knowledge k = Sample();
    k.profiles.clear();
    Fingerprint fp{"hw", "1.0", "game", "emu"};
    std::vector<std::string> notes;
    Profile& p = SelectProfile(k, fp, notes);
    p.validated["A"] = 1;
    p.rejected["B"][2] = Outcome::Regressed;
    p.rejected["C"][3] = Outcome::Crashed;
    fp.driver = "2.0";
    notes.clear();
    Profile& q = SelectProfile(k, fp, notes);
    EXPECT_EQ(&p, &q); // same profile: a driver change is not a new PC
    EXPECT_TRUE(q.validated.empty());
    EXPECT_EQ(q.revalidate.at("A"), 1);
    EXPECT_FALSE(q.rejected.contains("B"));
    EXPECT_EQ(q.rejected.at("C").at(3), Outcome::Crashed);
    EXPECT_EQ(q.fingerprint.driver, "2.0");
    ASSERT_FALSE(notes.empty());
}

TEST(SelectProfile, NewGameBuildGetsItsOwnProfile) {
    Knowledge k;
    std::vector<std::string> notes;
    Fingerprint a{"hw", "1.0", "CUSA00223|01.07|aaaa", "emu"};
    SelectProfile(k, a, notes).validated["A"] = 1;
    Fingerprint b = a;
    b.game = "CUSA00223|01.08|bbbb";
    Profile& pb = SelectProfile(k, b, notes);
    EXPECT_TRUE(pb.validated.empty());
    EXPECT_EQ(k.profiles.size(), 2u);
}

TEST(Hardware, FingerprintToleratesReservedRamButNotGpuChange) {
    HardwareInfo hw;
    hw.cpu_model = "AMD Ryzen 5 4600H";
    hw.cpu_physical_cores = 6;
    hw.cpu_logical_cores = 12;
    hw.ram_total_bytes = 15ull * 1024 * 1024 * 1024 + 300ull * 1024 * 1024; // 15.3 GB usable
    hw.gpu = {"NVIDIA GeForce GTX 1650", 0x10de, 0x1f99, "576.80", 0, 4ull << 30, 3ull << 30, true,
              false};
    SoftwareInfo sw{"emu", "CUSA00223", "01.07", "x"};
    const auto a = MakeFingerprint(hw, sw);
    hw.ram_total_bytes = 16ull * 1024 * 1024 * 1024;
    EXPECT_EQ(MakeFingerprint(hw, sw).ProfileKey(), a.ProfileKey());
    hw.gpu.device_id = 0x2191;
    EXPECT_NE(MakeFingerprint(hw, sw).ProfileKey(), a.ProfileKey());
    EXPECT_EQ(RamClassGb(16ull << 30), 16u);
    EXPECT_EQ(RamClassGb(15ull << 30), 16u);
    EXPECT_EQ(RamClassGb(8ull << 30), 8u);
}

TEST(Hardware, NvidiaDriverVersionDecoding) {
    // 576.80 is packed by NVIDIA as (576 << 22) | (80 << 14).
    EXPECT_EQ(DecodeDriverVersion(0x10de, (576u << 22) | (80u << 14)), "576.80.0.0");
    EXPECT_EQ(DecodeDriverVersion(0x1002, (2u << 22) | (0u << 12) | 300u), "2.0.300");
}
