// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Safety Gate (Architecture v1.0 shared service, independent of every mini-brain).
//
// No change reaches the emulator without passing Evaluate(). The rules are deliberately simple
// and ordered; the first one that does not pass decides, and its name goes into the audit log.
// The gate also owns the rollback triggers used while a change is being evaluated, and a
// session kill switch: after repeated emergency rollbacks it stops automatic changes entirely
// until the next launch.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>

#include "adaptive/contracts.h"
#include "adaptive/knobs.h"
#include "adaptive/knowledge.h"

namespace Adaptive {

struct SafetyLimits {
    std::uint64_t min_interval_per_knob_ns = 60'000'000'000ull; ///< 60 s between changes
    std::uint32_t max_changes_per_window = 6;
    std::uint64_t change_window_ns = 600'000'000'000ull; ///< per 10 minutes
    double warmup_shader_ms_per_frame = 2.0; ///< above this, shaders are still compiling
    std::uint32_t max_emergency_rollbacks = 2;
    /// A "slow" second: median above the reference median by this factor.
    double emergency_median_factor = 1.35;
    /// A "spike" second: p99 above the reference p99 by this factor (and above 50 ms).
    double emergency_p99_factor = 2.0;
    /// Emergency rollback needs sustained evidence, never one second: this many slow seconds in
    /// a row, or this many spike seconds among the last emergency_spike_window. (Field data:
    /// Second Son on a 4 GB card hitches ~6 times a minute on its own; one 130 ms frame during a
    /// test is the game, not the change.)
    std::uint32_t emergency_slow_seconds = 2;
    std::uint32_t emergency_spike_seconds = 3;
    std::uint32_t emergency_spike_window = 5;
};

struct GateContext {
    Mode mode = Mode::Observe;
    std::uint64_t now_ns = 0;
    bool in_gameplay = false;
    bool warming_up = false;
    bool evaluation_active = false;
    bool on_battery = false;
    bool memory_veto = false; ///< GPU Memory authority: no change that raises VRAM use
    const Profile* profile = nullptr;
};

class SafetyGate {
public:
    explicit SafetyGate(SafetyLimits limits = {}) : limits{limits} {}

    Decision Evaluate(const Proposal& p, const KnobRegistry& knobs, const GateContext& ctx) const;

    /// Records an applied change for rate limiting.
    void NoteApplied(const std::string& knob, std::uint64_t now_ns);

    /// True when one second measured during an evaluation is bad (slow or spiking). A single
    /// bad second is not an emergency; see SustainedEmergency.
    bool IsEmergency(double second_median_ms, double second_p99_ms, double ref_median_ms,
                     double ref_p99_ms) const;
    bool IsSlowSecond(double second_median_ms, double ref_median_ms) const;
    bool IsSpikeSecond(double second_p99_ms, double ref_p99_ms) const;
    /// slow_streak: consecutive slow seconds; recent_spikes: spike flags of the latest seconds.
    bool SustainedEmergency(std::uint32_t slow_streak, const std::deque<bool>& recent_spikes) const;
    void NoteEmergencyRollback();
    bool KillSwitchTripped() const {
        return emergency_rollbacks >= limits.max_emergency_rollbacks;
    }
    std::uint32_t EmergencyRollbacks() const {
        return emergency_rollbacks;
    }
    const SafetyLimits& Limits() const {
        return limits;
    }

private:
    SafetyLimits limits;
    std::deque<std::uint64_t> recent_changes;
    std::uint32_t emergency_rollbacks = 0;
};

} // namespace Adaptive
