// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <initializer_list>
#include <cmath>
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

void CentralBrain::SetPowerNote(std::string note) {
    std::scoped_lock lk{renderer_gpu_mutex};
    power_note = std::move(note);
}

void CentralBrain::SetRendererGpu(const GpuInfo& gpu) {
    std::scoped_lock lk{renderer_gpu_mutex};
    renderer_gpu = gpu;
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

    // Start-up tests: one experiment per launch, judged against this profile's baseline. In
    // Calibration mode on idle seconds (the player leaves the title screen running); in
    // Automatic mode on still pictures (pause, menus) that also appear in the baseline.
    const bool auto_tests = mode == Mode::Automatic && config.automatic_startup_tests;
    const std::size_t baseline_have =
        auto_tests ? static_cast<std::size_t>(std::count_if(
                         p.baseline_signatures.begin(), p.baseline_signatures.end(),
                         [](std::uint64_t sig) { return sig != 0; }))
                   : p.baseline_seconds_ms.size();
    if (auto_tests && baseline_have < p.baseline_seconds_ms.size()) {
        // Idle seconds from Calibration mode cannot be matched to a scene: start over.
        p.baseline_seconds_ms.clear();
        p.baseline_seconds_p99_ms.clear();
        p.baseline_signatures.clear();
    }
    if (mode == Mode::Calibration || auto_tests) {
        if (hw.on_battery && !config.calibrate_on_battery) {
            Note("start-up tests skipped: on battery power, measurements would not be "
                 "comparable");
        } else if (const auto last = std::find_if(
                       p.history.rbegin(), p.history.rend(),
                       [](const ExperimentRecord& e) {
                           return e.method == "launch-ab-still" || e.method == "launch-baseline";
                       });
                   auto_tests && last != p.history.rend() && last->method == "launch-ab-still" &&
                   last->outcome == Outcome::Abandoned) {
            // The last test found too few pause screens it could compare: record the pause
            // screens of this launch instead, so the next test has more to match.
            baseline_session = true;
            Note("start-up tests: the last test found too few known pause screens; this launch "
                 "records more of them");
        } else if (baseline_have < config.baseline_min_seconds) {
            baseline_session = true;
            Note(auto_tests
                     ? "start-up tests: this launch records how pause and menu screens run (" +
                           std::to_string(baseline_have) + "/" +
                           std::to_string(config.baseline_min_seconds) +
                           " seconds so far); tests start once enough are known"
                     : "calibration: this launch measures the baseline (" +
                           std::to_string(baseline_have) + "/" +
                           std::to_string(config.baseline_min_seconds) +
                           " idle seconds collected so far)");
        } else {
            const auto best = [&](const KnobState& st) {
                const auto it = chosen.find(st.spec.id);
                return it == chosen.end() ? st.spec.default_value : it->second;
            };
            const auto tried = [&](const std::string& knob, std::int64_t base, std::int64_t v) {
                std::uint32_t abandoned = 0;
                const bool judged =
                    std::any_of(p.history.begin(), p.history.end(), [&](const auto& e) {
                        const bool same = e.knob == knob && e.value == v &&
                                          e.baseline_value == base &&
                                          e.method.rfind("launch-ab", 0) == 0;
                        abandoned += same && e.outcome == Outcome::Abandoned ? 1 : 0;
                        return same &&
                               (e.outcome == Outcome::Neutral || e.outcome == Outcome::Regressed ||
                                e.outcome == Outcome::Improved);
                    });
                // Three launches without enough comparable seconds: give the next one a turn.
                return judged || abandoned >= 3;
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
                Note(auto_tests
                         ? "start-up test: this launch tests " + experiment->knob + "=" +
                               std::to_string(experiment->value) + " against " +
                               std::to_string(experiment->baseline_value) +
                               ", judged on pause and menu screens seen in earlier launches"
                         : "calibration: this launch tests " + experiment->knob + "=" +
                               std::to_string(experiment->value) + " against " +
                               std::to_string(experiment->baseline_value) +
                               " (leave the game idle on a steady scene, e.g. the title screen, "
                               "for 2 minutes after it loads)");
            } else {
                baseline_session = true;
                Note("start-up tests: every candidate has been measured on this PC; this launch "
                     "refreshes the baseline");
            }
        }
    }

    // Frame-rate target: the game's quality for this launch, a step on the ladder between the
    // player's own choice and the lowest quality they allow (performance mode and render size).
    if (config.fps_target > 0) {
        quality_ladder = BuildLadder({config.perf_mode_floor, config.perf_mode_max,
                                      config.render_height, config.render_height_min});
        if (quality_ladder.size() > 1) {
            launch_step = 0;
            if (p.auto_perf_mode >= 0) {
                const std::uint32_t own = NormalizeRenderHeight(config.render_height);
                launch_step = FindStep(quality_ladder,
                                       {static_cast<std::uint32_t>(p.auto_perf_mode),
                                        p.auto_render_height ? p.auto_render_height : own});
                if (p.auto_render_height == 0 && p.auto_perf_mode == 2) {
                    // Chosen by a build without render sizes: office was the only step below
                    // low. Smaller sizes at low come first now.
                    const std::size_t low = FindStep(quality_ladder, {1, own});
                    if (quality_ladder[low] == QualityStep{1, own} &&
                        low + 1 < quality_ladder.size()) {
                        launch_step = low + 1;
                    }
                }
            }
            const QualityStep& s = quality_ladder[launch_step];
            launch_perf_mode = static_cast<std::int32_t>(s.perf_mode);
            r.perf_mode = launch_perf_mode;
            r.render_height = s.render_height;
            Note("frame-rate target " + std::to_string(config.fps_target) +
                 " fps: this launch uses " + Describe(s) + " (step " +
                 std::to_string(launch_step + 1) + " of " +
                 std::to_string(quality_ladder.size()) + ", down to " +
                 Describe(quality_ladder.back()) + ")");
        } else {
            quality_ladder.clear();
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
    // The graphics card itself: when it is busy for most of every frame, nothing on the CPU side
    // can make frames faster, whatever else stalls (field: GTX 1650 busy 27-33 ms of ~31 ms
    // frames, which the stall shares alone labelled "VRAM pressure").
    if (s.gpu_busy_ms_p50 > 0.0 && s.gpu_busy_ms_p50 >= 0.85 * frame) {
        confidence = std::min(1.0, s.gpu_busy_ms_p50 / frame);
        return Bottleneck::HostGpuBound;
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

Scene CentralBrain::ClassifySecond(const SecondSummary& s, bool input_recent,
                                   std::uint32_t still_run) {
    if (s.draws_median == 0 && s.dispatches_median == 0) {
        return Scene::Unknown; // the emulator reported no draw counts
    }
    if (s.draws_median < 60) {
        return Scene::Loading;
    }
    if (still_run >= 3) {
        return Scene::Menu;
    }
    return input_recent ? Scene::Gameplay : Scene::Cutscene;
}

void CentralBrain::ClassifySeconds() {
    // A second is gameplay when the player gave input within two seconds of it, before or after.
    // Seconds are therefore classified with a three-second delay; nothing that compares seconds
    // looks at one before it is classified, so the first seconds of a cutscene are never taken
    // for gameplay just because the last key press was a moment ago.
    constexpr std::uint64_t Margin = 2'000'000'000ull;
    constexpr std::uint64_t Delay = 3'000'000'000ull;
    const std::uint64_t input = last_input_ns.load(std::memory_order_relaxed);
    if (input != 0 && (input_times.empty() || input_times.back() != input)) {
        input_times.push_back(input);
        while (input_times.size() > 4096) {
            input_times.pop_front();
        }
    }
    const std::uint64_t now = clock();
    for (const auto& s : window.SecondsSince(scene_last_second + 1)) {
        if (s.start_ns + 1'000'000'000ull + Delay > now) {
            break;
        }
        scene_last_second = s.start_ns;
        if (s.signature != 0 && s.signature == still_signature) {
            ++still_run;
        } else {
            still_run = s.signature != 0 ? 1 : 0;
            still_signature = s.signature;
        }
        const std::uint64_t lo = s.start_ns > Margin ? s.start_ns - Margin : 0;
        const std::uint64_t hi = s.start_ns + 1'000'000'000ull + Margin;
        const auto it = std::lower_bound(input_times.begin(), input_times.end(), lo);
        const bool input_recent = it != input_times.end() && *it <= hi;
        const Scene scene = ClassifySecond(s, input_recent, still_run);
        scene_seconds.push_back({s.start_ns, scene, scene == Scene::Menu ? s.signature : 0});
        while (scene_seconds.size() > 1800) {
            scene_seconds.pop_front();
        }
        auto& st = scene_stats[static_cast<std::size_t>(scene)];
        ++st.seconds;
        if (s.freezes > 0) {
            st.freezes += s.freezes;
            st.frozen_ms += s.frozen_ms;
            for (std::size_t k = 0; k < NumStallKinds; ++k) {
                st.freeze_stall_ms[k] += s.stall_ms[k];
            }
        }
        if (s.frames >= 10 && st.medians.size() < 7200) {
            st.medians.push_back(s.median_ms);
        }
        if (scene == Scene::Gameplay && s.frames >= 5) {
            auto& limit = gameplay_limits[s.frames + 1 < LimitTargetFps() ? 0 : 1];
            ++limit.seconds;
            limit.frames += s.frames;
            limit.card_ms += s.gpu_busy_median_ms * s.frames;
            for (std::size_t k = 0; k < NumStallKinds; ++k) {
                limit.stall_ms[k] += s.stall_ms[k];
            }
        }
        if (scene == Scene::Gameplay && s.gpu_busy_median_ms > 0.0 &&
            gameplay_gpu_busy.size() < 7200) {
            gameplay_gpu_busy.push_back(s.gpu_busy_median_ms);
        }
        // Scene switches: the second of the switch and the three after it.
        for (auto& sw : switches) {
            if (sw.seconds_left == 0) {
                continue;
            }
            --sw.seconds_left;
            sw.worst_ms = std::max(sw.worst_ms, s.max_ms);
            for (std::size_t i = 0; i < NumStallKinds; ++i) {
                sw.stall_ms[i] += s.stall_ms[i];
            }
            sw.frames_over_100ms += s.max_ms >= 100.0 ? 1 : 0;
        }
        if (scene != Scene::Unknown && last_scene != Scene::Unknown && scene != last_scene) {
            SceneSwitch sw;
            sw.t_ns = s.start_ns;
            sw.from = last_scene;
            sw.to = scene;
            sw.worst_ms = s.max_ms;
            sw.stall_ms = s.stall_ms;
            sw.frames_over_100ms = s.max_ms >= 100.0 ? 1 : 0;
            sw.seconds_left = 3;
            switches.push_back(sw);
            while (switches.size() > 200) {
                switches.pop_front();
            }
        }
        if (scene != Scene::Unknown) {
            last_scene = scene;
        }
    }
}

Scene CentralBrain::SceneOf(std::uint64_t second_start) const {
    for (auto it = scene_seconds.rbegin(); it != scene_seconds.rend(); ++it) {
        if (it->start_ns == second_start) {
            return it->scene;
        }
        if (it->start_ns < second_start) {
            break;
        }
    }
    return Scene::Unknown;
}

bool CentralBrain::StillPictureSecond(std::uint64_t second_start, std::uint64_t& signature) const {
    for (auto it = scene_seconds.rbegin(); it != scene_seconds.rend(); ++it) {
        if (it->start_ns == second_start) {
            signature = it->signature;
            return it->scene == Scene::Menu;
        }
        if (it->start_ns < second_start) {
            break;
        }
    }
    signature = 0;
    return false;
}

std::vector<SceneSwitch> CentralBrain::SceneSwitches() const {
    return {switches.begin(), switches.end()};
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
    ClassifySeconds();
    Snapshot s = window.Build(now);
    s.scene = last_scene;
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
        const bool playing = s.scene == Scene::Gameplay || s.scene == Scene::Unknown;
        if ((mode == Mode::Automatic || mode == Mode::Calibration) && !learned_live_applied &&
            !evaluation && s.in_gameplay && playing && !warmup && !gate.KillSwitchTripped()) {
            ApplyLearnedLiveValues(now);
        }
        if (mode == Mode::Automatic && learned_live_applied && !evaluation && s.in_gameplay &&
            playing && !warmup && !gate.KillSwitchTripped()) {
            ResumePartialEvaluation(now);
        }
        ConsiderProposals(now);
    }
    UpdateCalibration(now);

    // A tick that spans a sleep of the PC (or a debugger pause) is not optimizer work: the field
    // report showed 10678 ms per tick after one tick lasted hours.
    const double tick_ms = static_cast<double>(SteadyNs() - t0) / 1e6;
    if (tick_ms < 2000.0) {
        ++ticks;
        tick_ms_total += tick_ms;
    }
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
    // Values learned before an emulator or driver update: a full A/B test, not just a watch.
    for (const auto& [knob, v] : p->revalidate) {
        const KnobState* st = knobs.Find(knob);
        if (st && st->spec.scope == Scope::Live && st->value != v && !st->user_pinned &&
            !session_tried.contains({knob, v})) {
            StartEvaluation(knob, v, "Knowledge Base",
                            "re-check of a value learned before: " + p->revalidation_reason, false,
                            now);
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
    ev.scene = last_scene;
    ev.settle_left = static_cast<std::uint32_t>(config.settle_seconds);
    // Continue a test an earlier session could not finish (same knob, same two values).
    bool resumed = false;
    if (Profile* p = CurrentProfile(); p && p->partial && !confirm_only) {
        const PartialEvaluation& pe = *p->partial;
        if (pe.knob == knob && pe.old_value == ev.old_value && pe.new_value == value) {
            ev.a_median = pe.a_median;
            ev.b_median = pe.b_median;
            ev.a_p99 = pe.a_p99;
            ev.b_p99 = pe.b_p99;
            ev.a_hitches = pe.a_hitches;
            ev.b_hitches = pe.b_hitches;
            ev.resumed_sessions = pe.sessions;
            const std::uint32_t per_block = std::max<std::uint32_t>(
                1, static_cast<std::uint32_t>(config.eval_block_seconds - config.settle_seconds));
            const auto blocks_b = static_cast<std::uint32_t>(ev.b_median.size()) / per_block;
            const auto blocks_a = static_cast<std::uint32_t>(ev.a_median.size()) / per_block;
            // Blocks alternate B, A, B, A...: resume at the first block not yet complete.
            ev.block = std::min(blocks_b * 2, blocks_a * 2 + 1);
            ev.block = std::min(ev.block, config.eval_blocks - 1);
            resumed = true;
        }
        p->partial.reset();
    }
    const std::int64_t first_value = ev.block % 2 == 0 ? value : ev.old_value;
    if (!knobs.Set(knob, first_value, now)) {
        Log(proposer, knob, ev.old_value, value,
            Decision{Verdict::Rejected, "actuator", "the emulator did not take the value"}, reason);
        return false;
    }
    gate.NoteApplied(knob, now);
    Log(proposer, knob, ev.old_value, value,
        Decision{Verdict::Accepted, confirm_only ? "learned-value" : "accepted",
                 confirm_only ? "reapplied; watched for 60 s of gameplay"
                 : resumed    ? F("A/B measurement continued (%.0f + %.0f seconds from earlier "
                                  "sessions)",
                                  static_cast<double>(ev.a_median.size()),
                                  static_cast<double>(ev.b_median.size()))
                              : std::string("A/B measurement started")},
        reason);
    evaluation = std::move(ev);
    return true;
}

void CentralBrain::AdvanceEvaluation(std::uint64_t now) {
    Evaluation& ev = *evaluation;
    if (now - ev.start_ns > ToNs(config.eval_timeout_seconds)) {
        if (ev.confirm_only) {
            FinishEvaluation(Outcome::Abandoned, "no gameplay to watch for 30 minutes", now);
        } else {
            KeepPartialEvaluation("too little gameplay in 30 minutes", now);
        }
        return;
    }
    const std::uint32_t per_block = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(config.eval_block_seconds - config.settle_seconds));
    for (const auto& s : window.SecondsSince(ev.last_second_seen + 1)) {
        if (s.start_ns > scene_last_second) {
            break; // not classified yet
        }
        ev.last_second_seen = s.start_ns;
        if (s.frames < 10) {
            continue;
        }
        // Compare like with like: a cutscene, menu or loading screen pauses the test, with the
        // knob back on its old value meanwhile (so other measurements see the usual setup).
        if (SceneOf(s.start_ns) != ev.scene) {
            ++ev.other_scene_seconds;
            if (!ev.paused && !ev.confirm_only) {
                ev.paused = true;
                knobs.Set(ev.knob, ev.old_value, now);
            }
            continue;
        }
        if (ev.paused) {
            ev.paused = false;
            knobs.Set(ev.knob, ev.block % 2 == 0 ? ev.new_value : ev.old_value, now);
            ev.settle_left = static_cast<std::uint32_t>(config.settle_seconds);
            continue;
        }
        if (ev.settle_left > 0) {
            --ev.settle_left;
            continue;
        }
        // Seconds spent compiling shaders measure the shader cache, not the setting: they count
        // neither as evidence for an emergency nor as samples (field data: a fresh cache gave
        // 0.6-0.9 s compile stalls that rolled back a value measuring 6% faster).
        if (ShaderMsPerFrame(s) > 2.0) {
            ++ev.compile_seconds_skipped;
            continue;
        }
        const bool new_value_active = ev.confirm_only || ev.block % 2 == 0;
        if (!new_value_active) {
            ev.slow_streak = 0;
        } else {
            ev.slow_streak = gate.IsSlowSecond(s.median_ms, ev.ref_median) ? ev.slow_streak + 1 : 0;
            ev.recent_spikes.push_back(gate.IsSpikeSecond(s.p99_ms, ev.ref_p99));
            while (ev.recent_spikes.size() > gate.Limits().emergency_spike_window) {
                ev.recent_spikes.pop_front();
            }
        }
        if (new_value_active && gate.SustainedEmergency(ev.slow_streak, ev.recent_spikes)) {
            const auto key = std::make_pair(ev.knob, ev.new_value);
            const std::uint32_t n = ++emergency_counts[key];
            gate.NoteEmergencyRollback();
            FinishEvaluation(n >= 2 ? Outcome::Regressed : Outcome::Abandoned,
                             F("emergency rollback: sustained slowdown (latest second %.1f ms "
                               "median / %.1f ms p99 against %.1f / %.1f before the change)",
                               s.median_ms, s.p99_ms, ev.ref_median, ev.ref_p99),
                             now);
            if (n < 2) {
                // A bad stretch can be a scene change: allow one more try before blocking it.
                session_tried.erase(key);
            }
            return;
        }
        if (new_value_active) {
            ev.b_median.push_back(s.median_ms);
            ev.b_p99.push_back(s.p99_ms);
            ev.b_hitches += s.hitches > 0 ? 1 : 0; // hitch seconds: a burst is one event
        } else {
            ev.a_median.push_back(s.median_ms);
            ev.a_p99.push_back(s.p99_ms);
            ev.a_hitches += s.hitches > 0 ? 1 : 0;
        }
        if (ev.confirm_only) {
            if (ev.b_median.size() >= 60) {
                FinishEvaluation(Outcome::Improved, "learned value held for 60 s of gameplay",
                                 now);
                return;
            }
            continue;
        }
        if (++ev.block_counted < per_block) {
            continue;
        }
        ev.block_counted = 0;
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
            std::string extra;
            if (ev.compile_seconds_skipped) {
                extra += ", " + std::to_string(ev.compile_seconds_skipped) +
                         " shader-compile seconds left out";
            }
            if (ev.other_scene_seconds) {
                extra += ", " + std::to_string(ev.other_scene_seconds) +
                         " seconds of other scenes left out";
            }
            if (ev.resumed_sessions) {
                extra += ", measured over " + std::to_string(ev.resumed_sessions + 1) + " sessions";
            }
            FinishEvaluation(o,
                             F("median %.2f -> %.2f ms (%+.1f%%), p = %.3f", cmp.median_a,
                               cmp.median_b, cmp.relative_change * 100.0, p) +
                                 extra,
                             now);
            return;
        }
        knobs.Set(ev.knob, ev.block % 2 == 0 ? ev.new_value : ev.old_value, now);
        ev.block_start_ns = now;
        ev.settle_left = static_cast<std::uint32_t>(config.settle_seconds);
    }
}

void CentralBrain::KeepPartialEvaluation(const std::string& why, std::uint64_t now) {
    if (!evaluation) {
        return;
    }
    Evaluation ev = std::move(*evaluation);
    evaluation.reset();
    knobs.Set(ev.knob, ev.old_value, now);
    Profile* p = CurrentProfile();
    if (p && (ev.a_median.size() + ev.b_median.size()) > 0) {
        PartialEvaluation pe;
        pe.knob = ev.knob;
        pe.old_value = ev.old_value;
        pe.new_value = ev.new_value;
        pe.a_median = ev.a_median;
        pe.b_median = ev.b_median;
        pe.a_p99 = ev.a_p99;
        pe.b_p99 = ev.b_p99;
        pe.a_hitches = ev.a_hitches;
        pe.b_hitches = ev.b_hitches;
        pe.started_unix = UnixNow();
        pe.sessions = ev.resumed_sessions + 1;
        p->partial = std::move(pe);
    }
    Log(ev.proposer, ev.knob, ev.new_value, ev.old_value,
        Decision{Verdict::Deferred, "test-paused",
                 why + F("; %.0f + %.0f seconds kept, the test continues in a later session",
                         static_cast<double>(ev.a_median.size()),
                         static_cast<double>(ev.b_median.size()))},
        ev.reason);
    SaveKnowledge();
}

void CentralBrain::ResumePartialEvaluation(std::uint64_t now) {
    Profile* p = CurrentProfile();
    if (!p || !p->partial) {
        return;
    }
    const PartialEvaluation pe = *p->partial;
    const KnobState* st = knobs.Find(pe.knob);
    const auto blocked = p->rejected.find(pe.knob);
    if (!st || st->spec.scope != Scope::Live || st->user_pinned || st->value != pe.old_value ||
        (blocked != p->rejected.end() && blocked->second.contains(pe.new_value)) ||
        memory->Veto()) {
        if (!st || st->user_pinned || st->spec.scope != Scope::Live) {
            p->partial.reset(); // cannot be continued here
        }
        return;
    }
    StartEvaluation(pe.knob, pe.new_value, "Knowledge Base",
                    "continuing a test from an earlier session", false, now);
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
    // Hitch rates are hitch-seconds per minute; one event is worth 60 / n per minute, so the
    // tolerances are counted in events: regressing needs about three more hitch-seconds than the
    // other side, a winner may have one more (a scene-streaming hitch is the game, not the knob).
    const double one_event = 60.0 / static_cast<double>(std::max<std::size_t>(cmp.n_b, 1));
    const bool regressed =
        (cmp.p_value < config.significance && cmp.relative_change >= config.regress_threshold) ||
        (p99.p_value < config.significance && p99.relative_change > 2.0 * config.p99_tolerance) ||
        b_hitch_rate > a_hitch_rate * 2.0 + 3.0 * one_event;
    if (regressed) {
        return Outcome::Regressed;
    }
    const bool improved = cmp.p_value < config.significance &&
                          cmp.relative_change <= -config.improve_threshold &&
                          p99.relative_change <= config.p99_tolerance &&
                          b_hitch_rate <= a_hitch_rate * 1.25 + one_event;
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
        // A re-check settles the value either way (abandoned ones are tried again later).
        if (outcome != Outcome::Abandoned) {
            const auto rv = p->revalidate.find(ev.knob);
            if (rv != p->revalidate.end() && rv->second == ev.new_value) {
                p->revalidate.erase(rv);
            }
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
    // Tests start only while the player plays: cutscenes, menus and loading are other scenes.
    ctx.in_gameplay = s.in_gameplay && (s.scene == Scene::Gameplay || s.scene == Scene::Unknown);
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

namespace {
/// Start-up test seconds compared with baseline seconds of the same scene. Each second is divided
/// by the median of the baseline seconds that showed the same still picture, so seconds of a
/// light menu and a heavy paused city can be pooled. Signature 0 (an idle moving scene, only
/// used in Calibration mode, where the player idles on the title screen) matches signature 0.
struct MatchedSets {
    std::vector<double> base, test, base99, test99;
    std::vector<double> base_ms, test_ms; ///< unnormalized, for the record
};
MatchedSets MatchScenes(const std::vector<double>& base_ms, const std::vector<double>& base_p99,
                        const std::vector<std::uint64_t>& base_sig,
                        const std::vector<double>& test_ms, const std::vector<double>& test_p99,
                        const std::vector<std::uint64_t>& test_sig) {
    std::map<std::uint64_t, std::vector<std::size_t>> by_sig;
    for (std::size_t i = 0; i < base_ms.size(); ++i) {
        by_sig[i < base_sig.size() ? base_sig[i] : 0].push_back(i);
    }
    std::map<std::uint64_t, std::pair<double, double>> ref; // sig -> median, p99 median
    for (const auto& [sig, idx] : by_sig) {
        if (idx.size() < 5) {
            continue; // too few baseline seconds of this scene to stand for it
        }
        std::vector<double> m, q;
        for (const auto i : idx) {
            m.push_back(base_ms[i]);
            q.push_back(i < base_p99.size() ? base_p99[i] : base_ms[i]);
        }
        ref[sig] = {Stats::Percentile(m, 0.5), Stats::Percentile(q, 0.5)};
    }
    std::set<std::uint64_t> used;
    MatchedSets out;
    for (std::size_t i = 0; i < test_ms.size(); ++i) {
        const std::uint64_t sig = i < test_sig.size() ? test_sig[i] : 0;
        const auto r = ref.find(sig);
        if (r == ref.end() || r->second.first <= 0.0) {
            continue;
        }
        used.insert(sig);
        out.test.push_back(test_ms[i] / r->second.first);
        out.test99.push_back((i < test_p99.size() ? test_p99[i] : test_ms[i]) /
                             std::max(r->second.second, 1e-6));
        out.test_ms.push_back(test_ms[i]);
    }
    for (const auto sig : used) {
        const auto& r = ref.at(sig);
        for (const auto i : by_sig.at(sig)) {
            out.base.push_back(base_ms[i] / r.first);
            out.base99.push_back((i < base_p99.size() ? base_p99[i] : base_ms[i]) /
                                 std::max(r.second, 1e-6));
            out.base_ms.push_back(base_ms[i]);
        }
    }
    return out;
}
} // namespace

void CentralBrain::UpdateCalibration(std::uint64_t, bool finishing) {
    if (cal_done || (!experiment && !baseline_session)) {
        return;
    }
    const bool automatic = mode == Mode::Automatic;
    const std::uint64_t idle = ToNs(config.idle_input_seconds);
    const std::uint64_t input = last_input_ns.load(std::memory_order_relaxed);
    for (const auto& s : window.SecondsSince(cal_last_second + 1)) {
        if (s.start_ns > scene_last_second) {
            break; // not classified yet
        }
        cal_last_second = s.start_ns;
        const bool warm =
            first_frame_ns != 0 && s.start_ns >= first_frame_ns + ToNs(config.warmup_min_seconds);
        const bool quiet = input == 0 || input + idle < s.start_ns;
        std::uint64_t sig = 0;
        const bool still = StillPictureSecond(s.start_ns, sig);
        // Automatic mode measures only still pictures (pause, menus): the same scene can then be
        // found again in another launch. Calibration mode measures any idle second, as the
        // player was asked to leave the title screen running.
        const bool usable = automatic ? still : quiet;
        // A live test paused by this scene does not matter while its knob is on the old value.
        const bool live_test_differs =
            evaluation && knobs.Find(evaluation->knob) &&
            knobs.Find(evaluation->knob)->value != evaluation->old_value;
        if (s.frames < 10 || !warm || !usable || SceneOf(s.start_ns) == Scene::Loading ||
            ShaderMsPerFrame(s) > 2.0 || live_test_differs ||
            (mode == Mode::Calibration && !learned_live_applied && CurrentProfile() &&
             !CurrentProfile()->validated.empty())) {
            continue;
        }
        cal_median.push_back(s.median_ms);
        cal_p99.push_back(s.p99_ms);
        cal_signatures.push_back(automatic ? sig : 0);
        cal_hitches += s.hitches;
    }
    Profile* p = CurrentProfile();
    if (!p) {
        return;
    }
    const std::size_t target =
        finishing ? 0 : automatic && baseline_session ? 300 : config.calibration_target_seconds;
    std::size_t have = cal_median.size();
    MatchedSets m;
    if (experiment) {
        m = MatchScenes(p->baseline_seconds_ms, p->baseline_seconds_p99_ms, p->baseline_signatures,
                        cal_median, cal_p99, cal_signatures);
        have = m.test.size();
    }
    cal_result = F(experiment ? "%.0f of %.0f matching idle seconds measured"
                              : "%.0f of %.0f idle seconds measured",
                   static_cast<double>(have), static_cast<double>(target));
    if (have < target) {
        return;
    }
    cal_done = true;
    if (baseline_session) {
        p->baseline_seconds_ms.insert(p->baseline_seconds_ms.end(), cal_median.begin(),
                                      cal_median.end());
        p->baseline_seconds_p99_ms.insert(p->baseline_seconds_p99_ms.end(), cal_p99.begin(),
                                          cal_p99.end());
        p->baseline_signatures.resize(p->baseline_seconds_ms.size() - cal_median.size(), 0);
        p->baseline_signatures.insert(p->baseline_signatures.end(), cal_signatures.begin(),
                                      cal_signatures.end());
        while (p->baseline_seconds_ms.size() > Profile::MaxBaselineSeconds) {
            p->baseline_seconds_ms.erase(p->baseline_seconds_ms.begin());
            p->baseline_seconds_p99_ms.erase(p->baseline_seconds_p99_ms.begin());
            p->baseline_signatures.erase(p->baseline_signatures.begin());
        }
        cal_result = F("baseline recorded: %.0f idle seconds, median %.2f ms",
                       static_cast<double>(cal_median.size()), Stats::Percentile(cal_median, 0.5));
        Note("calibration: " + cal_result);
        if (automatic) {
            ExperimentRecord rec; // marks this launch, so tests and recordings take turns
            rec.knob = "pause-screens";
            rec.outcome = Outcome::Neutral;
            rec.median_after_ms = Stats::Percentile(cal_median, 0.5);
            rec.seconds_after = static_cast<std::uint32_t>(cal_median.size());
            rec.unix_time = UnixNow();
            rec.driver = fingerprint.driver;
            rec.method = "launch-baseline";
            p->history.push_back(rec);
            if (p->history.size() > Profile::MaxHistory) {
                p->history.erase(p->history.begin());
            }
        }
        SaveKnowledge();
        return;
    }
    // Judge the experiment against baseline seconds of the same scenes.
    double pv = 1.0;
    const double rate = 0.0;
    const Outcome o = Judge(m.base, m.test, m.base99, m.test99, rate, rate, pv);
    const auto cmp = Stats::Compare(m.base, m.test);
    const auto& e = *experiment;
    ExperimentRecord rec;
    rec.knob = e.knob;
    rec.baseline_value = e.baseline_value;
    rec.value = e.value;
    rec.outcome = o;
    rec.median_before_ms = Stats::Percentile(m.base_ms, 0.5);
    rec.median_after_ms = Stats::Percentile(m.test_ms, 0.5);
    rec.p99_before_ms = Stats::Percentile(m.base99, 0.5) * rec.median_before_ms;
    rec.p99_after_ms = Stats::Percentile(m.test99, 0.5) * rec.median_before_ms;
    rec.seconds_before = static_cast<std::uint32_t>(m.base.size());
    rec.seconds_after = static_cast<std::uint32_t>(m.test.size());
    rec.p_value = pv;
    rec.unix_time = UnixNow();
    rec.driver = fingerprint.driver;
    rec.method = automatic ? "launch-ab-still" : "launch-ab";
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
        p->baseline_signatures.clear();
    } else if (o == Outcome::Regressed) {
        p->rejected[e.knob][e.value] = Outcome::Regressed;
    }
    p->revalidate.erase(e.knob);
    knowledge.session_experiment.reset(); // judged: a later crash is not this value's fault
    cal_result = e.knob + "=" + std::to_string(e.value) + " " + std::string(ToString(o)) +
                 F(" (same-scene median %.2f -> %.2f ms, %+.1f%%, p = %.3f)",
                   rec.median_before_ms, rec.median_after_ms, cmp.relative_change * 100.0, pv);
    Note("start-up test: " + cal_result);
    SaveKnowledge();
}

void CentralBrain::DecideNextPerfMode() {
    Profile* p = CurrentProfile();
    if (!p || launch_perf_mode < 0 || config.fps_target == 0 || quality_ladder.empty()) {
        return;
    }
    const auto& play = scene_stats[static_cast<std::size_t>(Scene::Gameplay)].medians;
    std::size_t next = launch_step;
    if (play.size() < 120) {
        perf_mode_decision = F("only %.0f s of gameplay this launch (120 needed); it stays at ",
                               static_cast<double>(play.size())) +
                             Describe(quality_ladder[launch_step]);
    } else {
        const double med = Stats::Percentile(play, 0.5);
        const double busy =
            gameplay_gpu_busy.size() >= 60 ? Stats::Percentile(gameplay_gpu_busy, 0.5) : 0.0;
        LadderMemory mem;
        mem.prev_busy_ms = p->auto_prev_busy_x10 / 10.0;
        mem.prev = {p->auto_prev_perf, p->auto_prev_height ? p->auto_prev_height : 1080u};
        mem.has_stop = p->auto_stop_perf >= 0;
        mem.stop = {static_cast<std::uint32_t>(std::max(p->auto_stop_perf, 0)),
                    p->auto_stop_height ? p->auto_stop_height : 1080u};
        const LadderDecision d = DecideStep(quality_ladder, launch_step, med, busy,
                                            static_cast<double>(config.fps_target), mem);
        next = d.next;
        perf_mode_decision = d.reason;
        p->auto_prev_busy_x10 =
            static_cast<std::uint32_t>(std::lround(std::max(0.0, d.memory.prev_busy_ms) * 10.0));
        p->auto_prev_perf = d.memory.prev.perf_mode;
        p->auto_prev_height = d.memory.prev.render_height;
        p->auto_stop_perf = d.memory.has_stop ? static_cast<std::int32_t>(d.memory.stop.perf_mode)
                                              : -1;
        p->auto_stop_height = d.memory.has_stop ? d.memory.stop.render_height : 0;
    }
    p->auto_perf_mode = static_cast<std::int32_t>(quality_ladder[next].perf_mode);
    p->auto_render_height = quality_ladder[next].render_height;
    Note("frame-rate target: " + perf_mode_decision);
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

void CentralBrain::RequestNoteUserSetting(const std::string& knob, std::int64_t value) {
    std::scoped_lock lk{command_mutex};
    commands.push_back([this, knob, value] {
        const KnobState* st = knobs.Find(knob);
        if (!st || st->spec.scope != Scope::Never) {
            return;
        }
        knobs.Set(knob, value, clock());
        std::erase_if(recommendations, [&](const Recommendation& r) { return r.knob == knob; });
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
                if (p->revalidation_reason.empty()) {
                    p->revalidation_reason = "you asked to test it";
                }
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
    if (launch_perf_mode >= 0 && !quality_ladder.empty()) {
        v.fps_target = std::to_string(config.fps_target) + " fps target: " +
                       Describe(quality_ladder[launch_step]) + " this launch" +
                       (perf_mode_decision.empty() ? "" : "; " + perf_mode_decision);
    }
    v.scene_switches = switches.size();
    for (const auto& sw : switches) {
        v.worst_switch_ms = std::max(v.worst_switch_ms, sw.worst_ms);
    }
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

std::filesystem::path CentralBrain::Shutdown(bool crashed) {
    Stop();
    if (!started || shut_down) {
        return {};
    }
    shut_down = true;
    ProcessCommands();
    const std::uint64_t now = clock();
    if (evaluation) {
        if (evaluation->confirm_only || crashed) {
            FinishEvaluation(Outcome::Abandoned, "the game was closed during the measurement",
                             now);
        } else {
            KeepPartialEvaluation("the game was closed during the measurement", now);
        }
    }
    knobs.ResetLiveToDefaults(now);
    Profile* p = CurrentProfile();
    std::size_t measured = cal_median.size();
    if (p && experiment) {
        measured = MatchScenes(p->baseline_seconds_ms, p->baseline_seconds_p99_ms,
                               p->baseline_signatures, cal_median, cal_p99, cal_signatures)
                       .test.size();
    }
    if (p && !cal_done && (experiment || baseline_session)) {
        if (experiment && measured < config.calibration_min_seconds) {
            ExperimentRecord rec;
            rec.knob = experiment->knob;
            rec.baseline_value = experiment->baseline_value;
            rec.value = experiment->value;
            rec.outcome = Outcome::Abandoned;
            rec.seconds_after = static_cast<std::uint32_t>(measured);
            rec.unix_time = UnixNow();
            rec.driver = fingerprint.driver;
            rec.method = mode == Mode::Automatic ? "launch-ab-still" : "launch-ab";
            p->history.push_back(rec);
            Note("start-up test: only " + std::to_string(measured) +
                 (mode == Mode::Automatic ? " seconds of pause / menu screens seen before"
                                          : " idle seconds") +
                 " measured; the test is repeated in a later launch");
        } else if (experiment ||
                   cal_median.size() >= (mode == Mode::Automatic ? 20u
                                                                 : config.calibration_min_seconds)) {
            // Judge / record with what was measured (Automatic mode collects still pictures for
            // as long as the session runs).
            UpdateCalibration(now, true);
        }
    }
    if (p && launch_perf_mode >= 0) {
        DecideNextPerfMode();
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
    if (crashed) {
        // Left open on purpose: the next launch treats this one as unclean and blames what was
        // under test, exactly as if the process had vanished.
        Note("this launch ended in a crash (emulator assertion)");
    } else {
        knowledge.session_open = false;
        knowledge.session_experiment.reset();
        knowledge.session_launch_values.clear();
    }
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

namespace {
// Where the time of freezes went, in groups a player can act on.
struct FreezeGroup {
    const char* name;
    std::vector<StallKind> kinds;
};
const std::vector<FreezeGroup>& FreezeGroups() {
    static const std::vector<FreezeGroup> groups = {
        {"shader compiling",
         {StallKind::ShaderTranslate, StallKind::PipelineCreate, StallKind::PipelineWait}},
        {"loading data to the graphics card",
         {StallKind::BufferUpload, StallKind::TextureUpload, StallKind::UploadWait,
          StallKind::UploadThread, StallKind::Residency, StallKind::SparseBind}},
        {"memory shared with the game (page faults, read-backs)",
         {StallKind::PageFault, StallKind::PageProtect, StallKind::PageProtectGpu,
          StallKind::ReadbackWait, StallKind::BufferDownload}},
        {"graphics card busy", {StallKind::GpuWait}},
    };
    return groups;
}

std::string FreezeCauses(const std::array<double, NumStallKinds>& stall_ms) {
    const auto& groups = FreezeGroups();
    std::vector<double> group_ms(groups.size(), 0.0);
    double total = 0.0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        for (const auto k : groups[g].kinds) {
            group_ms[g] += stall_ms[static_cast<std::size_t>(k)];
        }
        total += group_ms[g];
    }
    std::string out;
    for (std::size_t g = 0; total > 0.0 && g < groups.size(); ++g) {
        if (group_ms[g] > 0.0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), " %s %.0f%%;", groups[g].name,
                          100.0 * group_ms[g] / total);
            out += buf;
        }
    }
    return out;
}
} // namespace

/// What held gameplay seconds under the frame-rate target: the graphics card, the emulator's GPU
/// thread, or the game's own threads, waiting on memory it shares with the card or on shaders.
/// A lower quality only helps in the first case; the player's sessions were in the last one, with
/// the card busy half to three quarters of the frame (build 35).
static std::string DescribeLimits(const std::array<LimitStats, 2>& limits, std::uint32_t target) {
    std::ostringstream o;
    o << "== What holds the frame rate back (gameplay, this launch)\n";
    const std::uint32_t total = limits[0].seconds + limits[1].seconds;
    if (total == 0) {
        o << "  no gameplay measured\n";
        return o.str();
    }
    o << "  " << limits[0].seconds << " of " << total << " seconds under " << target << " fps\n";
    const auto stalls = [](const LimitStats& l, std::initializer_list<StallKind> kinds) {
        double ms = 0.0;
        for (const auto k : kinds) {
            ms += l.stall_ms[static_cast<std::size_t>(k)];
        }
        return ms / std::max(l.frames, 1.0);
    };
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& l = limits[i];
        if (l.seconds == 0) {
            continue;
        }
        const double frame = 1000.0 * l.seconds / std::max(l.frames, 1.0);
        const double card = l.card_ms / std::max(l.frames, 1.0);
        const double thread =
            std::max(stalls(l, {StallKind::GpuThread}) -
                         stalls(l, {StallKind::GuestWait, StallKind::FrameWait}),
                     0.0);
        const double shared = stalls(l, {StallKind::PageFault, StallKind::ReadbackWait,
                                         StallKind::BufferDownload, StallKind::PageProtect});
        const double shaders = stalls(l, {StallKind::ShaderTranslate, StallKind::PipelineCreate,
                                          StallKind::PipelineWait});
        o << (i == 0 ? "  under " : "  at ") << target
          << F(" fps: %.1f fps, %.1f ms a frame: ", 1000.0 / frame, frame);
        if (card > 0.0) {
            o << F("graphics card busy %.1f ms (%.0f%%), ", card, 100.0 * card / frame);
        } else {
            o << "graphics card not measured, ";
        }
        o << F("emulator GPU thread busy %.1f ms (%.0f%%); game threads waited %.1f ms on memory "
               "shared with the card (page faults, copies back) and %.1f ms on shaders\n",
               thread, 100.0 * thread / frame, shared, shaders);
        if (i == 0) {
            o << "  so the limit there is ";
            if (card >= 0.9 * frame) {
                o << "the graphics card: a smaller render size or lower quality helps\n";
            } else if (thread >= 0.9 * frame) {
                o << "the emulator's GPU thread: lower quality (fewer draws) helps a little\n";
            } else {
                o << "the game's own threads: the card and the emulator's GPU thread had time to "
                     "spare, so lower quality does not help; the waits above are what to cut\n";
            }
        }
    }
    return o.str();
}

/// Freezes (frames of 100 ms or more) per scene: how many, the time lost per 10 minutes, what
/// it went to. Shows whether they shrink as the shader cache fills (build 34).
static std::string DescribeFreezes(const std::array<SceneStats, NumScenes>& scenes) {
    std::ostringstream o;
    o << "== Freezes (frames of 100 ms or more, this launch)\n";
    bool any = false;
    std::array<double, NumStallKinds> all{};
    for (std::size_t i = 0; i < NumScenes; ++i) {
        const auto& st = scenes[i];
        if (st.seconds == 0 || static_cast<Scene>(i) == Scene::Loading) {
            continue;
        }
        any = true;
        o << "  " << ToString(static_cast<Scene>(i)) << ": " << st.freezes
          << F(" freezes, %.1f s lost (%.1f s per 10 minutes)\n", st.frozen_ms / 1000.0,
               st.frozen_ms / 1000.0 * 600.0 / st.seconds);
        for (std::size_t k = 0; k < NumStallKinds; ++k) {
            all[k] += st.freeze_stall_ms[k];
        }
    }
    if (!any) {
        o << "  none measured\n";
    }
    const std::string causes = FreezeCauses(all);
    if (!causes.empty()) {
        o << "  the time went to:" << causes << '\n';
    }
    return o.str();
}

static std::string DescribeSwitches(const std::deque<SceneSwitch>& switches) {
    std::ostringstream o;
    o << "== Scene switches\n";
    if (switches.empty()) {
        o << "  none\n";
        return o.str();
    }
    const auto& groups = FreezeGroups();
    std::size_t freezes = 0;
    double worst = 0.0;
    std::vector<double> group_ms(groups.size(), 0.0);
    std::vector<const SceneSwitch*> bad;
    for (const auto& sw : switches) {
        worst = std::max(worst, sw.worst_ms);
        if (sw.worst_ms < 150.0) {
            continue;
        }
        ++freezes;
        bad.push_back(&sw);
        for (std::size_t g = 0; g < groups.size(); ++g) {
            for (const auto k : groups[g].kinds) {
                group_ms[g] += sw.stall_ms[static_cast<std::size_t>(k)];
            }
        }
    }
    o << "  " << switches.size() << " switches, " << freezes
      << F(" with a freeze over 150 ms (worst %.0f ms)\n", worst);
    double total = 0.0;
    for (const double v : group_ms) {
        total += v;
    }
    if (total > 0.0) {
        o << "  in those freezes the time went to:";
        for (std::size_t g = 0; g < groups.size(); ++g) {
            if (group_ms[g] > 0.0) {
                o << ' ' << groups[g].name << F(" %.0f%%;", 100.0 * group_ms[g] / total);
            }
        }
        o << '\n';
    }
    std::sort(bad.begin(), bad.end(),
              [](const SceneSwitch* a, const SceneSwitch* b) { return a->worst_ms > b->worst_ms; });
    for (std::size_t i = 0; i < std::min<std::size_t>(bad.size(), 5); ++i) {
        const auto& sw = *bad[i];
        o << F("  %8.1f s ", static_cast<double>(sw.t_ns) / 1e9) << ToString(sw.from) << " -> "
          << ToString(sw.to) << F(": worst frame %.0f ms", sw.worst_ms) << '\n';
    }
    return o.str();
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
    {
        HardwareInfo shown = hardware;
        std::scoped_lock lk{renderer_gpu_mutex};
        if (renderer_gpu) {
            const std::string windows_driver = shown.gpu.driver_version;
            shown.gpu = *renderer_gpu;
            shown.gpu.driver_version += " (Windows " + windows_driver + ")";
        }
        o << "== Hardware\n" << Describe(shown) << '\n';
    }
    {
        std::scoped_lock lk{renderer_gpu_mutex};
        if (!power_note.empty()) {
            o << "== Power\n" << power_note << "\n\n";
        }
    }
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
    o << "\n\n== Scenes (this launch)\n";
    for (std::size_t i = 0; i < NumScenes; ++i) {
        const auto& st = scene_stats[i];
        if (st.seconds == 0) {
            continue;
        }
        const double med = Stats::Percentile(st.medians, 0.5);
        o << "  " << ToString(static_cast<Scene>(i)) << ": " << st.seconds << " s"
          << (med > 0.0 ? F(", median frame %.1f ms (%.1f fps)", med, 1000.0 / med) : "") << '\n';
    }
    if (!gameplay_gpu_busy.empty()) {
        o << F("  graphics card busy in gameplay: %.1f ms a frame (median)\n",
               Stats::Percentile(gameplay_gpu_busy, 0.5));
    }
    o << '\n'
      << DescribeLimits(gameplay_limits, LimitTargetFps()) << '\n'
      << DescribeFreezes(scene_stats) << '\n'
      << DescribeSwitches(switches);
    if (!perf_mode_decision.empty()) {
        o << "\n== Frame-rate target\n  " << perf_mode_decision << '\n';
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
