// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Knob registry and workers (actuators).
//
// A Live knob is backed by an Actuator: the emulator side holds a std::atomic it reads on its own
// schedule, and the actuator stores into it. That single store is the only thing the optimizer
// ever does to a running emulator subsystem.
//
// A NextLaunch knob is an environment variable the emulator reads once (Common::Tune and
// friends). The brain sets it at startup, before any subsystem reads it - unless the user set it
// already, in which case the knob is "pinned" and the optimizer never touches it.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "adaptive/contracts.h"

namespace Adaptive {

struct Actuator {
    std::function<void(std::int64_t)> apply;
    std::function<std::int64_t()> read;
};

/// An actuator for a plain atomic owned by the emulator.
template <typename T>
Actuator AtomicActuator(std::atomic<T>& target) {
    return Actuator{
        [&target](std::int64_t v) { target.store(static_cast<T>(v), std::memory_order_relaxed); },
        [&target] { return static_cast<std::int64_t>(target.load(std::memory_order_relaxed)); }};
}

struct KnobState {
    KnobSpec spec;
    std::optional<Actuator> actuator; ///< Live knobs only
    std::int64_t value = 0;           ///< current (Live) / value in effect this launch (NextLaunch)
    bool user_pinned = false;         ///< the user set it themselves; hands off
    /// memory effect of raising the value: +1 uses more VRAM, -1 less, 0 none.
    int vram_direction = 0;
    std::uint64_t last_change_ns = 0;
};

class KnobRegistry {
public:
    /// Returns false if the id is taken or the spec is inconsistent (default outside bounds,
    /// candidates outside bounds, a Live knob without an actuator).
    bool Register(KnobSpec spec, std::optional<Actuator> actuator, int vram_direction = 0);

    bool Remove(const std::string& id) {
        return knobs.erase(id) != 0;
    }
    KnobState* Find(const std::string& id);
    const KnobState* Find(const std::string& id) const;
    std::vector<std::string> Ids() const;
    std::vector<std::string> IdsOf(Domain domain) const;

    /// Live: stores through the actuator. NextLaunch / Never: records the value only.
    bool Set(const std::string& id, std::int64_t value, std::uint64_t now_ns);
    /// Every Live knob back to its default (the kill switch / Observe mode / shutdown).
    void ResetLiveToDefaults(std::uint64_t now_ns);

    /// NextLaunch: decides each variable's value for this launch. `chosen` holds the optimizer's
    /// picks; env_get / env_set access the process environment. A variable that is already set
    /// pins the knob. Returns knob -> value actually applied by the optimizer.
    std::map<std::string, std::int64_t> ApplyLaunchValues(
        const std::map<std::string, std::int64_t>& chosen,
        const std::function<std::optional<std::string>(const std::string&)>& env_get,
        const std::function<void(const std::string&, const std::string&)>& env_set);

private:
    std::map<std::string, KnobState> knobs;
};

} // namespace Adaptive
