// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "adaptive/safety.h"

namespace Adaptive {

namespace {
Decision Make(Verdict v, const char* rule, std::string detail = {}) {
    return Decision{v, rule, std::move(detail)};
}
} // namespace

Decision SafetyGate::Evaluate(const Proposal& p, const KnobRegistry& knobs,
                              const GateContext& ctx) const {
    const KnobState* st = knobs.Find(p.knob);
    if (!st) {
        return Make(Verdict::Rejected, "R1-registered", "unknown knob");
    }
    const KnobSpec& spec = st->spec;
    if (p.owner != spec.domain) {
        return Make(Verdict::Rejected, "R2-ownership",
                    std::string(ToString(p.owner)) + " does not own " + p.knob + " (owner " +
                        std::string(ToString(spec.domain)) + ")");
    }
    if (p.value < spec.min_value || p.value > spec.max_value) {
        return Make(Verdict::Rejected, "R3-bounds", "value outside validated bounds");
    }
    if (std::find(spec.candidates.begin(), spec.candidates.end(), p.value) ==
        spec.candidates.end()) {
        return Make(Verdict::Rejected, "R3-candidates", "value is not a vetted candidate");
    }
    if (p.value == st->value) {
        return Make(Verdict::Rejected, "R3-no-op", "already in effect");
    }
    if (st->user_pinned) {
        return Make(Verdict::Rejected, "R4-user-pinned",
                    "the user set " + p.knob + " explicitly; the optimizer never overrides that");
    }
    if (ctx.profile) {
        if (const auto it = ctx.profile->rejected.find(p.knob); it != ctx.profile->rejected.end()) {
            if (const auto v = it->second.find(p.value); v != it->second.end()) {
                return Make(Verdict::Rejected, "R5-known-bad",
                            "this value " + std::string(ToString(v->second)) +
                                " before in this context");
            }
        }
    }
    if (ctx.mode == Mode::Observe) {
        return Make(Verdict::Rejected, "R6-mode", "Observe mode never changes anything");
    }
    if (spec.scope == Scope::Never) {
        return Make(Verdict::Recommended, "R6-scope-never",
                    "affects game timing or image output; only the user may change it");
    }
    if (ctx.mode == Mode::Manual) {
        return Make(Verdict::Recommended, "R6-mode", "Manual mode: waiting for the user");
    }
    if (spec.scope == Scope::NextLaunch) {
        return Make(Verdict::Recommended, "R6-scope-next-launch",
                    "takes effect on the next launch; Calibration mode tests it there");
    }
    if (spec.scope == Scope::Live && st->vram_direction != 0 && ctx.memory_veto) {
        const bool raises = (p.value > st->value) == (st->vram_direction > 0);
        if (raises) {
            return Make(Verdict::Rejected, "R7-memory-authority",
                        "VRAM is under pressure; changes that use more of it are vetoed");
        }
    }
    if (KillSwitchTripped()) {
        return Make(Verdict::Rejected, "R8-kill-switch",
                    "automatic changes stopped for this session after repeated rollbacks");
    }
    if (!ctx.in_gameplay) {
        return Make(Verdict::Deferred, "R9-gameplay", "not in steady gameplay");
    }
    if (ctx.warming_up) {
        return Make(Verdict::Deferred, "R9-warmup", "shaders are still compiling");
    }
    if (ctx.on_battery) {
        return Make(Verdict::Deferred, "R9-battery", "on battery: measurements not comparable");
    }
    if (ctx.evaluation_active) {
        return Make(Verdict::Deferred, "R10-one-at-a-time", "another change is being evaluated");
    }
    if (st->last_change_ns != 0 &&
        ctx.now_ns - st->last_change_ns < limits.min_interval_per_knob_ns) {
        return Make(Verdict::Deferred, "R11-rate-knob", "changed too recently");
    }
    std::size_t in_window = 0;
    for (const auto t : recent_changes) {
        if (ctx.now_ns - t < limits.change_window_ns) {
            ++in_window;
        }
    }
    if (in_window >= limits.max_changes_per_window) {
        return Make(Verdict::Deferred, "R11-rate-global", "too many changes in 10 minutes");
    }
    return Make(Verdict::Accepted, "accepted", "will be measured A/B before it is kept");
}

void SafetyGate::NoteApplied(const std::string&, std::uint64_t now_ns) {
    recent_changes.push_back(now_ns);
    while (recent_changes.size() > 64) {
        recent_changes.pop_front();
    }
}

bool SafetyGate::IsEmergency(double second_median_ms, double second_p99_ms, double ref_median_ms,
                             double ref_p99_ms) const {
    if (ref_median_ms <= 0.0) {
        return false;
    }
    return second_median_ms > ref_median_ms * limits.emergency_median_factor ||
           (ref_p99_ms > 0.0 && second_p99_ms > ref_p99_ms * limits.emergency_p99_factor &&
            second_p99_ms > 50.0);
}

void SafetyGate::NoteEmergencyRollback() {
    ++emergency_rollbacks;
}

} // namespace Adaptive
