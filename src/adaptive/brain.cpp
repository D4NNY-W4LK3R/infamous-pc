// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "adaptive/brain.h"
#include "adaptive/stats.h"

namespace Adaptive {

namespace {

std::uint64_t SteadyNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::int64_t UnixNow() {
    return static_cast<std::int64_t>(std::time(nullptr));
}


std::uint64_t ToNs(double seconds) {
    return static_cast<std::uint64_t>(seconds * 1e9);
}

double StallOf(const SecondSummary& s, StallKind k) {
    return s.stall_ms[static_cast<std::size_t>(k)];
}

double ShaderMsPerFrame(const SecondSummary& s) {
    if (s.frames == 0) {
        return 0.0;
    }
    return (StallOf(s, StallKind::ShaderTranslate) + StallOf(s, StallKind::PipelineCreate) +
            StallOf(s, StallKind::PipelineWait)) /
           static_cast<double>(s.frames);
}

std::vector<Domain> RelevantDomains(Bottleneck b) {
    switch (b) {
    case Bottleneck::MemoryPressure:
        return {Domain::TextureCache, Domain::GpuMemory};
    case Bottleneck::TransferBound:
        return {Domain::TransferSync, Domain::BufferGeometry};
    case Bottleneck::PageFaultBound:
        return {Domain::CpuWorkload, Domain::BufferGeometry};
    case Bottleneck::ShaderCompile:
        return {Domain::ShaderPipeline};
    case Bottleneck::GpuThreadBound:
        return {Domain::TransferSync, Domain::CpuWorkload, Domain::BufferGeometry};
    case Bottleneck::GuestBound:
        return {Domain::ShaderPipeline, Domain::CpuWorkload};
    case Bottleneck::PresentBound:
    case Bottleneck::HostGpuBound:
        return {Domain::FramePacing};
    default:
        return {};
    }
}

int Relevance(Domain d, Bottleneck b) {
    const auto r = RelevantDomains(b);
    const auto it = std::find(r.begin(), r.end(), d);
    return it == r.end() ? 100 : static_cast<int>(it - r.begin());
}

std::string F(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
    return buf;
}

std::optional<Bottleneck> ParseBottleneck(const std::string& s) {
    for (int i = 0; i <= static_cast<int>(Bottleneck::PageFaultBound); ++i) {
        if (ToString(static_cast<Bottleneck>(i)) == s) {
            return static_cast<Bottleneck>(i);
        }
    }
    return std::nullopt;
}

} // namespace

CentralBrain::CentralBrain(BrainConfig config_, Clock clock_)
    : config{std::move(config_)}, clock{clock_ ? std::move(clock_) : Clock{SteadyNs}},
      store{config.data_dir / "knowledge.txt"}, mode{config.mode} {
    auto mem = std::make_unique<GpuMemoryBrain>();
    memory = mem.get();
    brains.push_back(std::move(mem));
}

CentralBrain::~CentralBrain() {
    Stop();
}

bool CentralBrain::RegisterMiniBrain(std::unique_ptr<MiniBrain> brain,
                                     std::map<std::string, Actuator> actuators,
                                     std::map<std::string, int> vram_direction) {
    if (!brain || brain->ContractMajor() != ContractVersionMajor) {
        Note(brain ? "refused mini-brain '" + std::string(brain->Name()) +
                         "': contract version " + std::to_string(brain->ContractMajor()) +
                         ", the brain speaks " + std::to_string(ContractVersionMajor)
                   : "refused a null mini-brain");
        return false;
    }
    for (const auto& b : brains) {
        if (b->GetDomain() == brain->GetDomain()) {
            Note("refused mini-brain '" + std::string(brain->Name()) + "': domain " +
                 std::string(ToString(brain->GetDomain())) + " already has one");
            return false;
        }
    }
    std::vector<std::string> registered;
    for (auto& spec : brain->Knobs()) {
        if (spec.domain != brain->GetDomain()) {
            for (const auto& id : registered) {
                knobs.Remove(id);
            }
            Note("refused mini-brain '" + std::string(brain->Name()) + "': knob " + spec.id +
                 " belongs to another domain");
            return false;
        }
        std::optional<Actuator> act;
        if (const auto it = actuators.find(spec.id); it != actuators.end()) {
            act = it->second;
        }
        const int dir = vram_direction.contains(spec.id) ? vram_direction[spec.id] : 0;
        const std::string id = spec.id;
        if (!knobs.Register(std::move(spec), act, dir)) {
            for (const auto& r : registered) {
                knobs.Remove(r);
            }
            Note("refused mini-brain '" + std::string(brain->Name()) + "': knob " + id +
                 " failed registration (bounds, candidates or actuator)");
            return false;
        }
        registered.push_back(id);
    }
    brains.push_back(std::move(brain));
    return true;
}

void CentralBrain::SetResourceSampler(ResourceSampler s) {
    sampler = std::move(s);
}

void CentralBrain::NoteInput() noexcept {
    last_input_ns.store(clock(), std::memory_order_relaxed);
}

void CentralBrain::Note(std::string note) {
    notes.push_back(std::move(note));
    if (notes.size() > 200) {
        notes.erase(notes.begin());
    }
}

Profile* CentralBrain::CurrentProfile() {
    const auto it = knowledge.profiles.find(profile_key);
    return it == knowledge.profiles.end() ? nullptr : &it->second;
}

StartupResult CentralBrain::Startup(const HardwareInfo& hw, const SoftwareInfo& sw,
                                    const EnvGet& env_get, const EnvSet& env_set) {
    StartupResult r;
    if (started) {
        return r;
    }
    started = true;
    hardware = hw;
    software = sw;
    fingerprint = MakeFingerprint(hw, sw);
    r.load_status = store.Load(knowledge);
    Note("knowledge base " + store.Path().string() + ": " + std::string(ToString(r.load_status)));

    // A session left open means the last launch did not shut down cleanly.
    if (knowledge.session_open) {
        auto it = knowledge.profiles.find(knowledge.session_profile);
        if (it != knowledge.profiles.end()) {
            Profile& prev = it->second;
            ++prev.unclean_sessions;
            if (knowledge.session_experiment) {
                const auto& e = *knowledge.session_experiment;
                prev.rejected[e.knob][e.value] = Outcome::Crashed;
                ExperimentRecord rec;
                rec.knob = e.knob;
                rec.baseline_value = e.baseline_value;
                rec.value = e.value;
                rec.outcome = Outcome::Crashed;
                rec.unix_time = UnixNow();
                rec.driver = prev.fingerprint.driver;
                rec.method = "launch-ab";
                prev.history.push_back(rec);
                Note("the previous launch ended without a clean shutdown while testing " + e.knob +
                     "=" + std::to_string(e.value) + "; that value is blocked for good");
            } else if (!knowledge.session_launch_values.empty()) {
                for (const auto& [knob, v] : knowledge.session_launch_values) {
                    if (prev.validated.contains(knob)) {
                        prev.revalidate[knob] = v;
                        prev.validated.erase(knob);
                    }
                }
                prev.revalidation_reason = "previous session ended uncleanly";
                Note("the previous launch ended without a clean shutdown while using learned "
                     "values; they are not used again until re-measured");
            } else {
                Note("the previous launch ended without a clean shutdown (all defaults were in "
                     "use, so nothing is blamed on the optimizer)");
            }
        }
        knowledge.session_open = false;
        knowledge.session_experiment.reset();
        knowledge.session_launch_values.clear();
    }

    std::vector<std::string> profile_notes;
    Profile& p = SelectProfile(knowledge, fingerprint, profile_notes);
    for (auto& n : profile_notes) {
        Note(std::move(n));
    }
    profile_key = fingerprint.ProfileKey();
    ++p.sessions;

    // Learned values are used in the modes the user allowed to change things.
    std::map<std::string, std::int64_t> chosen;
    const bool may_apply = mode == Mode::Automatic || mode == Mode::Calibration;
    for (const auto& [knob, v] : p.validated) {
        const KnobState* st = knobs.Find(knob);
        if (st && st->spec.scope == Scope::NextLaunch) {
            if (may_apply) {
                chosen[knob] = v;
            } else {
                Recommendation rec;
                rec.knob = knob;
                rec.value = v;
                rec.current = st->spec.default_value;
                rec.scope = Scope::NextLaunch;
                rec.owner = std::string(ToString(st->spec.domain));
                rec.reason = "measured better on this PC in an earlier session";
                rec.why_not_automatic = "switch to Automatic or Calibration to use learned values";
                recommendations.push_back(rec);
            }
        }
    }
    if (!p.revalidate.empty()) {
        Note(std::to_string(p.revalidate.size()) + " learned value(s) wait for re-measurement (" +
             p.revalidation_reason + "); Calibration mode re-tests them");
    }

    // Calibration: one experiment per launch, judged against this profile's baseline.
    if (mode == Mode::Calibration) {
        if (hw.on_battery && !config.calibrate_on_battery) {
            Note("calibration skipped: on battery power, measurements would not be comparable");
        } else if (p.baseline_seconds_ms.size() < config.baseline_min_seconds) {
            baseline_session = true;
            Note("calibration: this launch measures the baseline (" +
                 std::to_string(p.baseline_seconds_ms.size()) + "/" +
                 std::to_string(config.baseline_min_seconds) + " idle seconds collected so far)");
        } else {
            const auto best = [&](const KnobState& st) {
                const auto it = chosen.find(st.spec.id);
                return it == chosen.end() ? st.spec.default_value : it->second;
            };
            const auto tried = [&](const std::string& knob, std::int64_t base, std::int64_t v) {
                return std::any_of(p.history.begin(), p.history.end(), [&](const auto& e) {
                    return e.knob == knob && e.value == v && e.baseline_value == base &&
                           e.method == "launch-ab" &&
                           (e.outcome == Outcome::Neutral || e.outcome == Outcome::Regressed ||
                            e.outcome == Outcome::Improved);
                });
            };
            const auto blocked = [&](const std::string& knob, std::int64_t v) {
                const auto it = p.rejected.find(knob);
                return it != p.rejected.end() && it->second.contains(v);
            };
            // 1. Values waiting for revalidation.
            for (const auto& [knob, v] : p.revalidate) {
                const KnobState* st = knobs.Find(knob);
                if (st && st->spec.scope == Scope::NextLaunch && !blocked(knob, v) &&
                    !env_get(knob)) {
                    experiment = PendingExperiment{knob, best(*st), v};
                    break;
                }
            }
            // 2. Untested candidates, the domains relevant to the last bottleneck first.
            if (!experiment) {
                const Bottleneck last =
                    ParseBottleneck(p.last_bottleneck).value_or(Bottleneck::Unknown);
                std::vector<const KnobState*> order;
                for (const auto& id : knobs.Ids()) {
                    const KnobState* st = knobs.Find(id);
                    if (st->spec.scope == Scope::NextLaunch && !st->spec.candidates.empty()) {
                        order.push_back(st);
                    }
                }
                std::stable_sort(order.begin(), order.end(), [&](auto* a, auto* b) {
                    return Relevance(a->spec.domain, last) < Relevance(b->spec.domain, last);
                });
                for (const KnobState* st : order) {
                    if (env_get(st->spec.id)) {
                        continue; // pinned by the user
                    }
                    const std::int64_t base = best(*st);
                    for (const auto v : st->spec.candidates) {
                        if (v != base && !blocked(st->spec.id, v) && !tried(st->spec.id, base, v)) {
                            experiment = PendingExperiment{st->spec.id, base, v};
                            break;
                        }
                    }
                    if (experiment) {
                        break;
                    }
                }
            }
            if (experiment) {
                chosen[experiment->knob] = experiment->value;
                Note("calibration: this launch tests " + experiment->knob + "=" +
                     std::to_string(experiment->value) + " against " +
                     std::to_string(experiment->baseline_value) +
                     " (leave the game idle on a steady scene, e.g. the title screen, for 2 "
                     "minutes after it loads)");
            } else {
                baseline_session = true;
                Note("calibration: every candidate has been measured on this PC; this launch "
                     "refreshes the baseline");
            }
        }
    }

    r.launch_values = knobs.ApplyLaunchValues(chosen, env_get, env_set);
    for (const auto& id : knobs.Ids()) {
        const KnobState* st = knobs.Find(id);
        if (st->user_pinned) {
            Note("knob " + id + " is set by the user (" + std::to_string(st->value) +
                 "); the optimizer leaves it alone");
        }
    }
    if (experiment && !r.launch_values.contains(experiment->knob)) {
        experiment.reset();
        baseline_session = false;
    }
    r.experiment = experiment;
    knowledge.session_open = true;
    knowledge.session_profile = profile_key;
    knowledge.session_experiment = experiment;
    knowledge.session_launch_values = r.launch_values;
    SaveKnowledge();
    r.notes = notes;
    UpdateStatus(clock());
    return r;
}

void CentralBrain::SaveKnowledge() {
    if (!store.Save(knowledge)) {
        Note("could not save the knowledge base (disk full or no permission?); learned results "
             "of this session may be lost");
    }
}

Bottleneck CentralBrain::Diagnose(const Snapshot& s, double vram_pressure, bool warmup,
                                  double& confidence) {
    confidence = 0.0;
    if (s.window_frames < 30 || !s.in_gameplay || s.frame_ms_p50 <= 0.0) {
        return Bottleneck::Unknown;
    }
    const auto st = [&](StallKind k) { return s.stall_ms_per_frame[static_cast<std::size_t>(k)]; };
    const double frame = std::max(s.frame_ms_p50, 1.0);
    const double shader =
        st(StallKind::ShaderTranslate) + st(StallKind::PipelineCreate) + st(StallKind::PipelineWait);
    if (warmup && shader > 2.0) {
        confidence = std::min(1.0, shader / frame);
        return Bottleneck::Warmup;
    }
    struct Candidate {
        Bottleneck b;
        double share;
        double threshold;
    };
    const double memory_share =
        (st(StallKind::Residency) + st(StallKind::SparseBind) + st(StallKind::TextureEvict)) / frame;
    const double busy = std::max(0.0, st(StallKind::GpuThread) - st(StallKind::GuestWait) -
                                          st(StallKind::FrameWait));
    const Candidate candidates[] = {
        {Bottleneck::MemoryPressure, vram_pressure >= 0.93 ? 1.0 : memory_share, 0.15},
        {Bottleneck::ShaderCompile, shader / frame, 0.15},
        {Bottleneck::PresentBound, (st(StallKind::FrameWait) + st(StallKind::Present)) / frame,
         0.30},
        {Bottleneck::TransferBound,
         (st(StallKind::ReadbackWait) + st(StallKind::UploadWait) + st(StallKind::BufferDownload) +
          st(StallKind::BufferUpload) + st(StallKind::DmaSync)) /
             frame,
         0.25},
        {Bottleneck::PageFaultBound, st(StallKind::PageFault) / frame, 0.15},
        {Bottleneck::HostGpuBound, st(StallKind::GpuWait) / frame, 0.25},
        {Bottleneck::GuestBound, st(StallKind::GuestWait) / frame, 0.35},
        {Bottleneck::GpuThreadBound, busy / frame, 0.70},
    };
    for (const auto& c : candidates) {
        if (c.share >= c.threshold) {
            confidence = std::min(1.0, c.share);
            return c.b;
        }
    }
    return Bottleneck::Unknown;
}

void CentralBrain::ProcessCommands() {
    std::vector<std::function<void()>> pending;
    {
        std::scoped_lock lk{command_mutex};
        pending.swap(commands);
    }
    for (auto& c : pending) {
        c();
    }
}

void CentralBrain::Tick() {
    const std::uint64_t t0 = SteadyNs();
    ProcessCommands();
    const std::uint64_t now = clock();
    drain_buffer.clear();
    dropped_frames += ring.Drain(drain_buffer);
    if (first_frame_ns == 0 && !drain_buffer.empty()) {
        first_frame_ns = drain_buffer.front().timestamp_ns;
    }
    window.Add(drain_buffer);
    if (sampler) {
        window.SetResources(sampler());
    }
    Snapshot s = window.Build(now);
    memory->Think(s, {});
    const bool warmup =
        first_frame_ns == 0 || now < first_frame_ns + ToNs(config.warmup_min_seconds);
    s.bottleneck = Diagnose(s, memory->Pressure(), warmup, s.bottleneck_confidence);
    if (s.in_gameplay && s.bottleneck != Bottleneck::Unknown && s.bottleneck != Bottleneck::Warmup) {
        bottleneck_seconds[std::string(ToString(s.bottleneck))] +=
            std::max<std::uint32_t>(1, config.tick_ms / 1000);
    }
    last_snapshot = s;

    if (mode == Mode::Observe) {
        if (evaluation) {
            FinishEvaluation(Outcome::Abandoned, "switched to Observe mode", now);
        }
        knobs.ResetLiveToDefaults(now);
    } else {
        if (evaluation) {
            AdvanceEvaluation(now);
        }
        if ((mode == Mode::Automatic || mode == Mode::Calibration) && !learned_live_applied &&
            !evaluation && s.in_gameplay && !warmup && !gate.KillSwitchTripped()) {
            ApplyLearnedLiveValues(now);
        }
        ConsiderProposals(now);
    }
    UpdateCalibration(now);

    ++ticks;
    tick_ms_total += static_cast<double>(SteadyNs() - t0) / 1e6;
    UpdateStatus(now);
}

void CentralBrain::ApplyLearnedLiveValues(std::uint64_t now) {
    const Profile* p = CurrentProfile();
    if (!p) {
        learned_live_applied = true;
        return;
    }
    for (const auto& [knob, v] : p->validated) {
        const KnobState* st = knobs.Find(knob);
        if (st && st->spec.scope == Scope::Live && st->value != v) {
            StartEvaluation(knob, v, "Knowledge Base", "value learned in an earlier session",
                            true, now);
            return;
        }
    }
    learned_live_applied = true;
}

bool CentralBrain::StartEvaluation(const std::string& knob, std::int64_t value,
                                   const std::string& proposer, const std::string& reason,
                                   bool confirm_only, std::uint64_t now) {
    KnobState* st = knobs.Find(knob);
    if (!st || st->spec.scope != Scope::Live) {
        return false;
    }
    Evaluation ev;
    ev.knob = knob;
    ev.proposer = proposer;
    ev.old_value = st->value;
    ev.new_value = value;
    ev.start_ns = now;
    ev.block_start_ns = now;
    ev.last_second_seen = now;
    ev.ref_median = last_snapshot.frame_ms_p50;
    ev.ref_p99 = last_snapshot.frame_ms_p99;
    ev.confirm_only = confirm_only;
    ev.reason = reason;
    if (!knobs.Set(knob, value, now)) {
        Log(proposer, knob, ev.old_value, value,
            Decision{Verdict::Rejected, "actuator", "the emulator did not take the value"}, reason);
        return false;
    }
    gate.NoteApplied(knob, now);
    Log(proposer, knob, ev.old_value, value,
        Decision{Verdict::Accepted, confirm_only ? "learned-value" : "accepted",
                 confirm_only ? "reapplied; watched for 60 s" : "A/B measurement started"},
        reason);
    evaluation = std::move(ev);
    return true;
}

void CentralBrain::AdvanceEvaluation(std::uint64_t now) {
    Evaluation& ev = *evaluation;
    if (now - ev.start_ns > ToNs(config.eval_timeout_seconds)) {
        FinishEvaluation(Outcome::Abandoned, "no steady gameplay to measure within 10 minutes", now);
        return;
    }
    const std::uint64_t settle = ToNs(config.settle_seconds);
    for (const auto& s : window.SecondsSince(ev.last_second_seen + 1)) {
        ev.last_second_seen = s.start_ns;
        if (s.frames < 10 || s.start_ns < ev.block_start_ns + settle) {
            continue;
        }
        const bool new_value_active = ev.confirm_only || ev.block % 2 == 0;
        if (new_value_active && gate.IsEmergency(s.median_ms, s.p99_ms, ev.ref_median, ev.ref_p99)) {
            const auto key = std::make_pair(ev.knob, ev.new_value);
            const std::uint32_t n = ++emergency_counts[key];
            gate.NoteEmergencyRollback();
            FinishEvaluation(n >= 2 ? Outcome::Regressed : Outcome::Abandoned,
                             F("emergency rollback: a second at %.1f ms median / %.1f ms p99 "
                               "against %.1f / %.1f before the change",
                               s.median_ms, s.p99_ms, ev.ref_median, ev.ref_p99),
                             now);
            if (n < 2) {
                // One bad second can be a scene change: allow one more try before blocking it.
                session_tried.erase(key);
            }
            return;
        }
        if (new_value_active) {
            ev.b_median.push_back(s.median_ms);
            ev.b_p99.push_back(s.p99_ms);
            ev.b_hitches += s.hitches;
        } else {
            ev.a_median.push_back(s.median_ms);
            ev.a_p99.push_back(s.p99_ms);
            ev.a_hitches += s.hitches;
        }
    }
    if (ev.confirm_only) {
        if (ev.b_median.size() >= 60) {
            FinishEvaluation(Outcome::Improved, "learned value held for 60 s of gameplay", now);
        }
        return;
    }
    if (now - ev.block_start_ns >= ToNs(config.eval_block_seconds)) {
        ++ev.block;
        if (ev.block >= config.eval_blocks) {
            const double a_rate = ev.a_median.empty() ? 0.0
                                                      : static_cast<double>(ev.a_hitches) * 60.0 /
                                                            static_cast<double>(ev.a_median.size());
            const double b_rate = ev.b_median.empty() ? 0.0
                                                      : static_cast<double>(ev.b_hitches) * 60.0 /
                                                            static_cast<double>(ev.b_median.size());
            double p = 1.0;
            const Outcome o = Judge(ev.a_median, ev.b_median, ev.a_p99, ev.b_p99, a_rate, b_rate, p);
            const auto cmp = Stats::Compare(ev.a_median, ev.b_median);
            FinishEvaluation(o,
                             F("median %.2f -> %.2f ms (%+.1f%%), p = %.3f", cmp.median_a,
                               cmp.median_b, cmp.relative_change * 100.0, p),
                             now);
            return;
        }
        knobs.Set(ev.knob, ev.block % 2 == 0 ? ev.new_value : ev.old_value, now);
        ev.block_start_ns = now;
    }
}

Outcome CentralBrain::Judge(const std::vector<double>& a_med, const std::vector<double>& b_med,
                            const std::vector<double>& a_p99, const std::vector<double>& b_p99,
                            double a_hitch_rate, double b_hitch_rate, double& p_value) const {
    const auto cmp = Stats::Compare(a_med, b_med);
    const auto p99 = Stats::Compare(a_p99, b_p99);
    p_value = cmp.p_value;
    if (cmp.n_a < 6 || cmp.n_b < 6) {
        return Outcome::Abandoned;
    }
    const bool regressed =
        (cmp.p_value < config.significance && cmp.relative_change >= config.regress_threshold) ||
        (p99.p_value < config.significance && p99.relative_change > 2.0 * config.p99_tolerance) ||
        b_hitch_rate > a_hitch_rate * 2.0 + 1.0;
    if (regressed) {
        return Outcome::Regressed;
    }
    const bool improved = cmp.p_value < config.significance &&
                          cmp.relative_change <= -config.improve_threshold &&
                          p99.relative_change <= config.p99_tolerance &&
                          b_hitch_rate <= a_hitch_rate * 1.25 + 0.5;
    return improved ? Outcome::Improved : Outcome::Neutral;
}

void CentralBrain::FinishEvaluation(Outcome outcome, const std::string& why, std::uint64_t now) {
    if (!evaluation) {
        return;
    }
    Evaluation ev = std::move(*evaluation);
    evaluation.reset();
    const bool keep = outcome == Outcome::Improved;
    knobs.Set(ev.knob, keep ? ev.new_value : ev.old_value, now);
    if (!keep) {
        session_tried.emplace(ev.knob, ev.new_value);
    }
    if (Profile* p = CurrentProfile()) {
        const KnobState* st = knobs.Find(ev.knob);
        const std::int64_t def = st ? st->spec.default_value : 0;
        if (keep) {
            if (ev.new_value == def) {
                p->validated.erase(ev.knob);
            } else {
                p->validated[ev.knob] = ev.new_value;
            }
        } else if (ev.confirm_only) {
            p->validated.erase(ev.knob); // a learned value that no longer holds is dropped
        }
        if (outcome == Outcome::Regressed) {
            p->rejected[ev.knob][ev.new_value] = Outcome::Regressed;
        }
        ExperimentRecord rec;
        rec.knob = ev.knob;
        rec.baseline_value = ev.old_value;
        rec.value = ev.new_value;
        rec.outcome = outcome;
        rec.median_before_ms = Stats::Percentile(ev.a_median, 0.5);
        rec.median_after_ms = Stats::Percentile(ev.b_median, 0.5);
        rec.p99_before_ms = Stats::Percentile(ev.a_p99, 0.5);
        rec.p99_after_ms = Stats::Percentile(ev.b_p99, 0.5);
        rec.seconds_before = static_cast<std::uint32_t>(ev.a_median.size());
        rec.seconds_after = static_cast<std::uint32_t>(ev.b_median.size());
        rec.p_value = Stats::Compare(ev.a_median, ev.b_median).p_value;
        rec.unix_time = UnixNow();
        rec.driver = fingerprint.driver;
        rec.method = ev.confirm_only ? "live-confirm" : "live-abab";
        p->history.push_back(rec);
        if (p->history.size() > Profile::MaxHistory) {
            p->history.erase(p->history.begin());
        }
    }
    Log(ev.proposer, ev.knob, ev.old_value, keep ? ev.new_value : ev.old_value,
        Decision{keep ? Verdict::Accepted : Verdict::Rejected,
                 "evaluation-" + std::string(ToString(outcome)),
                 why + (keep ? "; kept" : "; reverted to " + std::to_string(ev.old_value))},
        ev.reason);
    SaveKnowledge();
}

void CentralBrain::ConsiderProposals(std::uint64_t now) {
    const Snapshot& s = last_snapshot;
    GateContext ctx;
    ctx.mode = mode;
    ctx.now_ns = now;
    ctx.in_gameplay = s.in_gameplay;
    ctx.warming_up = s.bottleneck == Bottleneck::Warmup || first_frame_ns == 0 ||
                     now < first_frame_ns + ToNs(config.warmup_min_seconds);
    // A calibration measurement in progress counts as an evaluation: no live change may
    // contaminate it.
    ctx.evaluation_active = evaluation.has_value() ||
                            (mode == Mode::Calibration && !cal_done &&
                             (experiment.has_value() || baseline_session));
    ctx.on_battery = hardware.on_battery;
    ctx.memory_veto = memory->Veto();
    ctx.profile = CurrentProfile();

    struct Item {
        Proposal p;
        int relevance;
    };
    std::vector<Item> items;
    for (const auto& b : brains) {
        std::vector<std::int64_t> values;
        for (const auto& spec : b->Knobs()) {
            const KnobState* st = knobs.Find(spec.id);
            values.push_back(st ? st->value : spec.default_value);
        }
        for (auto& p : b->Think(s, values)) {
            p.owner = b->GetDomain(); // a brain speaks only for its own domain
            items.push_back(Item{std::move(p), Relevance(b->GetDomain(), s.bottleneck)});
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.relevance != b.relevance ? a.relevance < b.relevance
                                          : a.p.expected_gain > b.p.expected_gain;
    });
    bool started_one = false;
    for (const auto& item : items) {
        const Proposal& p = item.p;
        Decision d = gate.Evaluate(p, knobs, ctx);
        if (d.verdict == Verdict::Accepted && session_tried.contains({p.knob, p.value})) {
            d = Decision{Verdict::Deferred, "R12-tried-this-session",
                         "measured earlier this session without a win; retried next launch"};
        }
        if (d.verdict == Verdict::Accepted && started_one) {
            d = Decision{Verdict::Deferred, "R10-one-at-a-time", "one change per tick"};
        }
        const KnobState* st = knobs.Find(p.knob);
        const std::int64_t cur = st ? st->value : 0;
        if (d.verdict == Verdict::Recommended) {
            auto it = std::find_if(recommendations.begin(), recommendations.end(),
                                   [&](const Recommendation& r) { return r.knob == p.knob; });
            Recommendation r;
            r.knob = p.knob;
            r.value = p.value;
            r.current = cur;
            r.scope = st ? st->spec.scope : Scope::Never;
            r.owner = std::string(ToString(p.owner));
            r.reason = p.reason;
            r.why_not_automatic = d.detail;
            if (it == recommendations.end()) {
                recommendations.push_back(r);
            } else {
                *it = r;
            }
        }
        // Log a proposal the first time, and again whenever its verdict changes.
        bool repeat = false;
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->knob == p.knob && it->proposer == std::string(ToString(p.owner))) {
                repeat = it->to == p.value && it->decision.rule == d.rule;
                break;
            }
        }
        if (d.verdict == Verdict::Accepted) {
            started_one = StartEvaluation(p.knob, p.value, std::string(ToString(p.owner)), p.reason,
                                          false, now);
        } else if (!repeat) {
            Log(std::string(ToString(p.owner)), p.knob, cur, p.value, d, p.reason);
        }
    }
}

void CentralBrain::UpdateCalibration(std::uint64_t) {
    if (cal_done || (!experiment && !baseline_session)) {
        return;
    }
    const std::uint64_t idle = ToNs(config.idle_input_seconds);
    const std::uint64_t input = last_input_ns.load(std::memory_order_relaxed);
    for (const auto& s : window.SecondsSince(cal_last_second + 1)) {
        cal_last_second = s.start_ns;
        const bool warm =
            first_frame_ns != 0 && s.start_ns >= first_frame_ns + ToNs(config.warmup_min_seconds);
        const bool quiet = input == 0 || input + idle < s.start_ns;
        if (s.frames < 10 || !warm || !quiet || ShaderMsPerFrame(s) > 2.0 || evaluation ||
            (mode == Mode::Calibration && !learned_live_applied && CurrentProfile() &&
             !CurrentProfile()->validated.empty())) {
            continue;
        }
        cal_median.push_back(s.median_ms);
        cal_p99.push_back(s.p99_ms);
        cal_hitches += s.hitches;
    }
    cal_result = F("%.0f of %.0f idle seconds measured", static_cast<double>(cal_median.size()),
                   static_cast<double>(config.calibration_target_seconds));
    if (cal_median.size() < config.calibration_target_seconds) {
        return;
    }
    cal_done = true;
    Profile* p = CurrentProfile();
    if (!p) {
        return;
    }
    if (baseline_session) {
        p->baseline_seconds_ms.insert(p->baseline_seconds_ms.end(), cal_median.begin(),
                                      cal_median.end());
        p->baseline_seconds_p99_ms.insert(p->baseline_seconds_p99_ms.end(), cal_p99.begin(),
                                          cal_p99.end());
        while (p->baseline_seconds_ms.size() > Profile::MaxBaselineSeconds) {
            p->baseline_seconds_ms.erase(p->baseline_seconds_ms.begin());
            p->baseline_seconds_p99_ms.erase(p->baseline_seconds_p99_ms.begin());
        }
        cal_result = F("baseline recorded: %.0f idle seconds, median %.2f ms",
                       static_cast<double>(cal_median.size()), Stats::Percentile(cal_median, 0.5));
        Note("calibration: " + cal_result);
        SaveKnowledge();
        return;
    }
    // Judge the experiment against the most recent baseline seconds.
    const std::size_t take = std::min<std::size_t>(p->baseline_seconds_ms.size(), 300);
    const std::vector<double> base(p->baseline_seconds_ms.end() - static_cast<std::ptrdiff_t>(take),
                                   p->baseline_seconds_ms.end());
    const std::vector<double> base99(p->baseline_seconds_p99_ms.end() -
                                         static_cast<std::ptrdiff_t>(take),
                                     p->baseline_seconds_p99_ms.end());
    double pv = 1.0;
    const double rate = static_cast<double>(cal_hitches) * 60.0 / static_cast<double>(cal_median.size());
    const Outcome o = Judge(base, cal_median, base99, cal_p99, rate, rate, pv);
    const auto cmp = Stats::Compare(base, cal_median);
    const auto& e = *experiment;
    ExperimentRecord rec;
    rec.knob = e.knob;
    rec.baseline_value = e.baseline_value;
    rec.value = e.value;
    rec.outcome = o;
    rec.median_before_ms = cmp.median_a;
    rec.median_after_ms = cmp.median_b;
    rec.p99_before_ms = Stats::Percentile(base99, 0.5);
    rec.p99_after_ms = Stats::Percentile(cal_p99, 0.5);
    rec.seconds_before = static_cast<std::uint32_t>(base.size());
    rec.seconds_after = static_cast<std::uint32_t>(cal_median.size());
    rec.p_value = pv;
    rec.unix_time = UnixNow();
    rec.driver = fingerprint.driver;
    rec.method = "launch-ab";
    p->history.push_back(rec);
    const KnobState* st = knobs.Find(e.knob);
    const std::int64_t def = st ? st->spec.default_value : 0;
    if (o == Outcome::Improved) {
        if (e.value == def) {
            p->validated.erase(e.knob);
        } else {
            p->validated[e.knob] = e.value;
        }
        // The best configuration changed: the old baseline no longer describes it.
        p->baseline_seconds_ms.clear();
        p->baseline_seconds_p99_ms.clear();
    } else if (o == Outcome::Regressed) {
        p->rejected[e.knob][e.value] = Outcome::Regressed;
    }
    p->revalidate.erase(e.knob);
    knowledge.session_experiment.reset(); // judged: a later crash is not this value's fault
    cal_result = e.knob + "=" + std::to_string(e.value) + " " + std::string(ToString(o)) +
                 F(" (median %.2f -> %.2f ms, %+.1f%%, p = %.3f)", cmp.median_a, cmp.median_b,
                   cmp.relative_change * 100.0, pv);
    Note("calibration: " + cal_result);
    SaveKnowledge();
}

void CentralBrain::Log(const std::string& proposer, const std::string& knob, std::int64_t from,
                       std::int64_t to, const Decision& d, const std::string& reason) {
    DecisionLogEntry e;
    e.t_ns = clock();
    e.proposer = proposer;
    e.knob = knob;
    e.from = from;
    e.to = to;
    e.decision = d;
    e.reason = reason;
    log.push_back(std::move(e));
    while (log.size() > 2000) {
        log.pop_front();
    }
}

void CentralBrain::RequestMode(Mode m) {
    std::scoped_lock lk{command_mutex};
    commands.push_back([this, m] {
        if (m == mode) {
            return;
        }
        Note("mode changed: " + std::string(ToString(mode)) + " -> " + std::string(ToString(m)));
        mode = m;
        if (m == Mode::Observe || m == Mode::Manual) {
            if (evaluation) {
                FinishEvaluation(Outcome::Abandoned, "mode changed", clock());
            }
        }
        if (m == Mode::Observe) {
            knobs.ResetLiveToDefaults(clock());
        }
    });
}

void CentralBrain::RequestApply(const std::string& knob, std::int64_t value) {
    std::scoped_lock lk{command_mutex};
    commands.push_back([this, knob, value] {
        const KnobState* st = knobs.Find(knob);
        if (!st) {
            return;
        }
        if (st->spec.scope == Scope::Live) {
            if (evaluation) {
                Note("apply " + knob + ": another change is being measured; try again shortly");
                return;
            }
            // The user's approval stands in for Automatic mode; every other rule still applies.
            GateContext ctx;
            ctx.mode = Mode::Automatic;
            ctx.now_ns = clock();
            ctx.in_gameplay = true;
            ctx.memory_veto = memory->Veto();
            ctx.profile = CurrentProfile();
            const Decision d = gate.Evaluate(Proposal{knob, value, "user", 0, st->spec.domain}, knobs, ctx);
            if (d.verdict != Verdict::Accepted) {
                Log("user", knob, st->value, value, d, "applied from the overlay");
                return;
            }
            StartEvaluation(knob, value, "user", "applied from the overlay", false, clock());
        } else if (st->spec.scope == Scope::NextLaunch) {
            if (Profile* p = CurrentProfile()) {
                p->revalidate[knob] = value;
                SaveKnowledge();
            }
            Note(knob + "=" + std::to_string(value) +
                 " queued: the next launch in Calibration mode measures it");
        } else {
            Note(knob + " is an emulator setting the optimizer never changes; set it in the "
                        "emulator's settings");
        }
        std::erase_if(recommendations, [&](const Recommendation& r) { return r.knob == knob; });
    });
}

void CentralBrain::RequestRestoreDefaults() {
    std::scoped_lock lk{command_mutex};
    commands.push_back([this] {
        if (evaluation) {
            FinishEvaluation(Outcome::Abandoned, "user restored defaults", clock());
        }
        knobs.ResetLiveToDefaults(clock());
        learned_live_applied = true; // not reapplied this session
        Note("all live values restored to the emulator's defaults for this session");
    });
}

void CentralBrain::RequestForgetProfile() {
    std::scoped_lock lk{command_mutex};
    commands.push_back([this] {
        if (evaluation) {
            FinishEvaluation(Outcome::Abandoned, "user cleared learned data", clock());
        }
        knobs.ResetLiveToDefaults(clock());
        if (Profile* p = CurrentProfile()) {
            const Fingerprint fp = p->fingerprint;
            *p = Profile{};
            p->fingerprint = fp;
            p->sessions = 1;
        }
        experiment.reset();
        knowledge.session_experiment.reset();
        baseline_session = false;
        cal_done = true;
        learned_live_applied = true;
        SaveKnowledge();
        Note("learned data for this PC + game + emulator cleared; starting over");
    });
}

void CentralBrain::UpdateStatus(std::uint64_t) {
    StatusView v;
    v.mode = mode;
    v.snapshot = last_snapshot;
    for (const auto& b : brains) {
        v.brains.emplace_back(std::string(b->Name()), b->Status());
    }
    v.recommendations = recommendations;
    if (evaluation) {
        v.evaluation = EvaluationView{evaluation->knob,          evaluation->old_value,
                                      evaluation->new_value,     evaluation->block,
                                      evaluation->confirm_only ? 1u : config.eval_blocks,
                                      evaluation->a_median.size(), evaluation->b_median.size()};
    }
    v.notes = notes;
    const std::size_t n = std::min<std::size_t>(log.size(), 30);
    v.recent.assign(log.end() - static_cast<std::ptrdiff_t>(n), log.end());
    for (const auto& id : knobs.Ids()) {
        v.knob_values[id] = knobs.Find(id)->value;
    }
    if (experiment) {
        v.calibration = "testing " + experiment->knob + "=" + std::to_string(experiment->value) +
                        ": " + cal_result;
    } else if (baseline_session) {
        v.calibration = "baseline: " + cal_result;
    } else {
        v.calibration = mode == Mode::Calibration ? "nothing to measure this launch" : "off";
    }
    v.brain_ms_per_tick = ticks ? tick_ms_total / static_cast<double>(ticks) : 0.0;
    v.ticks = ticks;
    v.dropped_frames = dropped_frames;
    v.kill_switch = gate.KillSwitchTripped();
    std::scoped_lock lk{status_mutex};
    status = std::move(v);
}

StatusView CentralBrain::Status() const {
    std::scoped_lock lk{status_mutex};
    return status;
}

void CentralBrain::Start() {
    std::scoped_lock lk{thread_mutex};
    if (thread.joinable()) {
        return;
    }
    stop_requested = false;
    thread = std::thread([this] {
        if (config.on_thread_start) {
            config.on_thread_start();
        }
        std::unique_lock lock{thread_mutex};
        while (!stop_requested) {
            lock.unlock();
            Tick();
            lock.lock();
            thread_cv.wait_for(lock, std::chrono::milliseconds(config.tick_ms),
                               [this] { return stop_requested; });
        }
    });
}

void CentralBrain::Stop() {
    {
        std::scoped_lock lk{thread_mutex};
        stop_requested = true;
    }
    thread_cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

std::filesystem::path CentralBrain::Shutdown() {
    Stop();
    if (!started || shut_down) {
        return {};
    }
    shut_down = true;
    ProcessCommands();
    const std::uint64_t now = clock();
    if (evaluation) {
        FinishEvaluation(Outcome::Abandoned, "the game was closed during the measurement", now);
    }
    knobs.ResetLiveToDefaults(now);
    Profile* p = CurrentProfile();
    if (p && !cal_done && (experiment || baseline_session)) {
        if (experiment && cal_median.size() < config.calibration_min_seconds) {
            ExperimentRecord rec;
            rec.knob = experiment->knob;
            rec.baseline_value = experiment->baseline_value;
            rec.value = experiment->value;
            rec.outcome = Outcome::Abandoned;
            rec.seconds_after = static_cast<std::uint32_t>(cal_median.size());
            rec.unix_time = UnixNow();
            rec.driver = fingerprint.driver;
            rec.method = "launch-ab";
            p->history.push_back(rec);
            Note("calibration: only " + std::to_string(cal_median.size()) +
                 " idle seconds measured; the experiment is repeated next launch");
        } else if (cal_median.size() >= config.calibration_min_seconds) {
            config.calibration_target_seconds = static_cast<std::uint32_t>(cal_median.size());
            UpdateCalibration(now);
        }
    }
    if (p) {
        std::string top;
        std::uint32_t best = 0;
        for (const auto& [name, secs] : bottleneck_seconds) {
            if (secs > best) {
                best = secs;
                top = name;
            }
        }
        if (!top.empty()) {
            p->last_bottleneck = top;
        }
    }
    knowledge.session_open = false;
    knowledge.session_experiment.reset();
    knowledge.session_launch_values.clear();
    SaveKnowledge();
    UpdateStatus(now);

    std::error_code ec;
    const auto dir = config.data_dir / "reports";
    std::filesystem::create_directories(dir, ec);
    char name[64];
    std::snprintf(name, sizeof(name), "session-%lld.txt", static_cast<long long>(UnixNow()));
    const auto path = dir / name;
    const std::string text = BuildReport();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    std::ofstream latest(config.data_dir / "latest-report.txt", std::ios::binary | std::ios::trunc);
    latest << text;
    // Keep the 30 newest reports.
    std::vector<std::filesystem::path> reports;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        reports.push_back(e.path());
    }
    std::sort(reports.begin(), reports.end());
    while (reports.size() > 30) {
        std::filesystem::remove(reports.front(), ec);
        reports.erase(reports.begin());
    }
    return out ? path : std::filesystem::path{};
}

std::string CentralBrain::Report() const {
    return BuildReport();
}

std::string CentralBrain::BuildReport() const {
    std::ostringstream o;
    o << "Second Son Adaptive - session report\n";
    o << "Architecture v1.0, contract " << ContractVersionMajor << '.' << ContractVersionMinor
      << ", knowledge schema " << KnowledgeSchemaVersion << "\n\n";
    o << "== Mode: " << ToString(mode) << "\n\n";
    o << "== Hardware\n" << Describe(hardware) << '\n';
    o << "== Software\nemulator " << software.emulator_build << "\ngame " << software.game_serial
      << ' ' << software.game_version << " (eboot " << software.eboot_signature << ")\n\n";
    o << "== Profile " << profile_key << "\n";
    if (const auto it = knowledge.profiles.find(profile_key); it != knowledge.profiles.end()) {
        const Profile& p = it->second;
        o << "sessions " << p.sessions << ", unclean shutdowns " << p.unclean_sessions
          << ", baseline seconds " << p.baseline_seconds_ms.size() << '\n';
        for (const auto& [k, v] : p.validated) {
            o << "  learned  " << k << " = " << v << '\n';
        }
        for (const auto& [k, v] : p.revalidate) {
            o << "  re-check " << k << " = " << v << " (" << p.revalidation_reason << ")\n";
        }
        for (const auto& [k, vals] : p.rejected) {
            for (const auto& [v, out] : vals) {
                o << "  blocked  " << k << " = " << v << " (" << ToString(out) << ")\n";
            }
        }
        o << "\n== Experiments (newest last)\n";
        const std::size_t start = p.history.size() > 40 ? p.history.size() - 40 : 0;
        for (std::size_t i = start; i < p.history.size(); ++i) {
            const auto& e = p.history[i];
            o << "  " << e.method << ' ' << e.knob << ' ' << e.baseline_value << " -> " << e.value
              << ": " << ToString(e.outcome)
              << F(" | median %.2f -> %.2f ms, p99 %.2f -> %.2f ms", e.median_before_ms,
                   e.median_after_ms, e.p99_before_ms, e.p99_after_ms)
              << F(" | %.0f/%.0f s, p = %.3f", e.seconds_before, e.seconds_after, e.p_value)
              << '\n';
        }
    }
    o << "\n== This launch\n";
    for (const auto& id : knobs.Ids()) {
        const KnobState* st = knobs.Find(id);
        o << "  " << id << " = " << st->value << (st->value == st->spec.default_value ? "" : " *")
          << (st->user_pinned ? " (set by user)" : "") << "  [" << ToString(st->spec.domain)
          << ", "
          << (st->spec.scope == Scope::Live         ? "live"
              : st->spec.scope == Scope::NextLaunch ? "next launch"
                                                    : "user only")
          << "]\n";
    }
    o << "calibration: "
      << (experiment ? experiment->knob + "=" + std::to_string(experiment->value) + ", "
                     : std::string(baseline_session ? "baseline, " : ""))
      << cal_result << '\n';
    o << "\n== Last snapshot\n"
      << F("fps %.1f, frame p50 %.2f / p95 %.2f / p99 %.2f ms, ", last_snapshot.fps,
           last_snapshot.frame_ms_p50, last_snapshot.frame_ms_p95, last_snapshot.frame_ms_p99)
      << F("hitches %.1f/min, bottleneck ", last_snapshot.hitches_per_minute)
      << ToString(last_snapshot.bottleneck) << '\n';
    o << "time by bottleneck:";
    for (const auto& [name, secs] : bottleneck_seconds) {
        o << ' ' << name << ' ' << secs << " s;";
    }
    o << "\n\n== Mini-brains\n";
    for (const auto& b : brains) {
        o << "  " << b->Name() << ": " << b->Status() << '\n';
    }
    o << "\n== Recommendations\n";
    for (const auto& r : recommendations) {
        o << "  " << r.knob << ' ' << r.current << " -> " << r.value << ": " << r.reason << " ("
          << r.why_not_automatic << ")\n";
    }
    o << "\n== Decisions\n";
    for (const auto& e : log) {
        o << F("  %9.1f s ", static_cast<double>(e.t_ns) / 1e9) << e.proposer << ": " << e.knob
          << ' ' << e.from << " -> " << e.to << " | " << ToString(e.decision.verdict) << " ["
          << e.decision.rule << "] " << e.decision.detail << " | " << e.reason << '\n';
    }
    o << "\n== Notes\n";
    for (const auto& n : notes) {
        o << "  " << n << '\n';
    }
    o << "\n== Optimizer overhead\n"
      << F("%.0f ticks, %.3f ms per tick on the brain thread, ", static_cast<double>(ticks),
           ticks ? tick_ms_total / static_cast<double>(ticks) : 0.0)
      << dropped_frames << " frame samples dropped, emergency rollbacks "
      << gate.EmergencyRollbacks() << (gate.KillSwitchTripped() ? " (kill switch tripped)" : "")
      << '\n';
    return o.str();
}

} // namespace Adaptive
