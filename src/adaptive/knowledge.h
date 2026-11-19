// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Knowledge Base (Architecture v1.0 shared service): what was learned, kept across launches.
//
// Stored: validated knob values per profile, every measured experiment with its evidence, values
// that regressed or crashed (never retried in the same context), the measured baseline, and an
// open-session marker so a crash during an experiment is detected on the next launch.
//
// Not stored: addresses, handles, resource lifetimes or anything else that is only meaningful
// inside one process. Only decisions are reused, never execution state.
//
// File safety: plain text with a checksum line; saved by writing a temporary file and renaming
// it over the old one, which is first kept as a .bak (last known good). A torn or corrupted
// file is detected by the checksum and the .bak is loaded instead.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "adaptive/contracts.h"
#include "adaptive/hardware.h"

namespace Adaptive {

struct ExperimentRecord {
    std::string knob;
    std::int64_t baseline_value = 0;
    std::int64_t value = 0;
    Outcome outcome = Outcome::Abandoned;
    double median_before_ms = 0.0;
    double median_after_ms = 0.0;
    double p99_before_ms = 0.0;
    double p99_after_ms = 0.0;
    double hitches_before = 0.0; ///< per minute
    double hitches_after = 0.0;
    std::uint32_t seconds_before = 0;
    std::uint32_t seconds_after = 0;
    double p_value = 1.0;
    std::int64_t unix_time = 0;
    std::string driver;
    std::string method; ///< "live-abab" or "launch-ab"
};

/// A live A/B test that a session ended before it was finished: its seconds are kept and the
/// next session continues it (same knob, same values) instead of starting over.
struct PartialEvaluation {
    std::string knob;
    std::int64_t old_value = 0;
    std::int64_t new_value = 0;
    std::vector<double> a_median, b_median, a_p99, b_p99;
    std::uint32_t a_hitches = 0, b_hitches = 0;
    std::int64_t started_unix = 0;
    std::uint32_t sessions = 1; ///< sessions that contributed seconds
};

struct Profile {
    Fingerprint fingerprint;
    std::map<std::string, std::int64_t> validated; ///< knob -> value proven not worse
    /// knob -> values that regressed (retried only after a driver change) or crashed (never).
    std::map<std::string, std::map<std::int64_t, Outcome>> rejected;
    std::vector<ExperimentRecord> history; ///< newest last, capped
    /// Values learned under a different driver or schema: not applied until re-measured.
    /// Calibration re-tests them first; a confirmed one moves back to `validated`.
    std::map<std::string, std::int64_t> revalidate;
    std::string revalidation_reason;
    /// Dominant bottleneck of the last session, to order the next experiments.
    std::string last_bottleneck;
    std::uint32_t unclean_sessions = 0;
    std::uint32_t sessions = 0;
    /// Per-second medians of baseline (all-default) calibration sessions, newest last, capped.
    std::vector<double> baseline_seconds_ms;
    std::vector<double> baseline_seconds_p99_ms;
    /// Scene signature of each baseline second (0 = unknown, from schema 2): start-up tests
    /// compare only seconds of the same scene.
    std::vector<std::uint64_t> baseline_signatures;
    std::optional<PartialEvaluation> partial;
    /// Performance mode the fps-target controller chose for the next launch (-1 = none yet).
    std::int32_t auto_perf_mode = -1;
    /// Render height the frame-rate target chose with auto_perf_mode (0 = the game's own). Kept
    /// as a second number on the autoperf line: builds before 30 read the first and ignore it.
    std::uint32_t auto_render_height = 0;

    static constexpr std::size_t MaxHistory = 256;
    static constexpr std::size_t MaxBaselineSeconds = 1200;
};

/// The calibration experiment a launch runs (NextLaunch knobs are judged across launches).
struct PendingExperiment {
    std::string knob;
    std::int64_t baseline_value = 0;
    std::int64_t value = 0;
};

struct Knowledge {
    std::uint32_t schema_version = KnowledgeSchemaVersion;
    std::map<std::string, Profile> profiles; ///< ProfileKey -> profile
    /// Set at launch, cleared at clean shutdown. Found set at the next launch = crash.
    bool session_open = false;
    std::string session_profile;
    std::optional<PendingExperiment> session_experiment;
    /// Values a NextLaunch experiment set for this launch (knob -> value), for crash attribution.
    std::map<std::string, std::int64_t> session_launch_values;
};

enum class LoadStatus : std::uint8_t {
    Fresh,          ///< no file yet
    Loaded,         ///< main file valid
    RecoveredBackup,///< main file corrupt / torn, .bak loaded
    BothCorrupt,    ///< neither valid; started fresh, the corrupt files were set aside
    SchemaChanged,  ///< older schema, converted: history kept (v1 learned values re-checked)
};
std::string_view ToString(LoadStatus status);

class KnowledgeStore {
public:
    explicit KnowledgeStore(std::filesystem::path file);

    LoadStatus Load(Knowledge& out);
    /// Atomic replace; returns false (and keeps the old file) if anything fails.
    bool Save(const Knowledge& k);

    const std::filesystem::path& Path() const {
        return file;
    }

    /// Serialization, exposed for tests.
    static std::string Serialize(const Knowledge& k);
    /// Returns false on checksum / syntax error.
    static bool Parse(const std::string& text, Knowledge& out, bool& schema_changed);

private:
    std::filesystem::path file;
};

/// Applies the invalidation rules of Architecture v1.0 sec. 1 to the profile for `current`:
/// returns the profile (created if missing) after marking its learned values for re-checking
/// when the GPU driver or the emulator build changed. Profiles are keyed by hardware + game, so
/// what was learned survives emulator updates (as values to re-check, not as values to trust).
Profile& SelectProfile(Knowledge& k, const Fingerprint& current, std::vector<std::string>& notes);

} // namespace Adaptive
