// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <initializer_list>
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


std::string F(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
    return buf;
}

} // namespace

CentralBrain::CentralBrain(BrainConfig config_, Clock clock_)
    : config{std::move(config_)}, clock{clock_ ? std::move(clock_) : Clock{SteadyNs}},
      store{config.data_dir / "knowledge.txt"} {
    auto mem = std::make_unique<GpuMemoryBrain>();
    memory = mem.get();
    brains.push_back(std::move(mem));
}

CentralBrain::~CentralBrain() {
    Stop();
}

bool CentralBrain::RegisterMiniBrain(std::unique_ptr<MiniBrain> brain) {
    if (!brain || brain->ContractMajor() != ContractVersionMajor) {
        Note(brain ? "refused mini-brain '" + std::string(brain->Name()) +
                         "': contract version " + std::to_string(brain->ContractMajor()) +
                         ", the analyser speaks " + std::to_string(ContractVersionMajor)
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
    const auto undo = [&] {
        for (const auto& id : registered) {
            knobs.Remove(id);
        }
    };
    for (auto& spec : brain->Knobs()) {
        if (spec.domain != brain->GetDomain()) {
            undo();
            Note("refused mini-brain '" + std::string(brain->Name()) + "': knob " + spec.id +
                 " belongs to another domain");
            return false;
        }
        const std::string id = spec.id;
        // Nothing is changed live any more (build 55), so no knob has an actuator.
        if (spec.scope == Scope::Live || !knobs.Register(std::move(spec), std::nullopt, 0)) {
            undo();
            Note("refused mini-brain '" + std::string(brain->Name()) + "': knob " + id +
                 " failed registration (live knobs are no longer supported)");
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

void CentralBrain::SetCheatsNote(std::string note) {
    std::scoped_lock lk{renderer_gpu_mutex};
    cheats_note = std::move(note);
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
            ++it->second.unclean_sessions;
            Note("the previous launch ended without a clean shutdown");
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

    // Nothing learned is set any more; this only notes the values the player set themselves.
    knobs.ApplyLaunchValues({}, env_get, env_set);
    for (const auto& id : knobs.Ids()) {
        const KnobState* st = knobs.Find(id);
        if (st->user_pinned) {
            Note("knob " + id + " is set by the user (" + std::to_string(st->value) + ")");
        }
    }
    knowledge.session_open = true;
    knowledge.session_profile = profile_key;
    SaveKnowledge();
    r.notes = notes;
    UpdateStatus(clock());
    return r;
}

void CentralBrain::SaveKnowledge() {
    if (!store.Save(knowledge)) {
        Note("could not save the knowledge base (disk full or no permission?); the frame-rate "
             "target's choice of this session may be lost");
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
    // Seconds are therefore classified with a three-second delay, so the first seconds of a
    // cutscene are never taken for gameplay just because the last key press was a moment ago.
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
    CollectRecommendations();

    // A tick that spans a sleep of the PC (or a debugger pause) is not analyser work: the field
    // report showed 10678 ms per tick after one tick lasted hours.
    const double tick_ms = static_cast<double>(SteadyNs() - t0) / 1e6;
    if (tick_ms < 2000.0) {
        ++ticks;
        tick_ms_total += tick_ms;
    }
    UpdateStatus(now);
}

void CentralBrain::CollectRecommendations() {
    // Every area describes what it measures (its status); only settings that are the player's
    // to change are passed on, as text. Nothing is applied.
    const Snapshot& s = last_snapshot;
    for (std::size_t i = 1; i < brains.size(); ++i) {
        const auto& b = brains[i];
        std::vector<std::int64_t> values;
        for (const auto& spec : b->Knobs()) {
            const KnobState* st = knobs.Find(spec.id);
            values.push_back(st ? st->value : spec.default_value);
        }
        for (const auto& p : b->Think(s, values)) {
            const KnobState* st = knobs.Find(p.knob);
            if (!st || st->spec.scope != Scope::Never || st->spec.domain != b->GetDomain()) {
                continue;
            }
            Recommendation r;
            r.knob = p.knob;
            r.value = p.value;
            r.current = st->value;
            r.owner = std::string(ToString(b->GetDomain()));
            r.reason = p.reason;
            const auto it = std::find_if(recommendations.begin(), recommendations.end(),
                                         [&](const Recommendation& x) { return x.knob == p.knob; });
            if (it == recommendations.end()) {
                recommendations.push_back(std::move(r));
            } else {
                *it = std::move(r);
            }
        }
    }
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

void CentralBrain::RequestForgetProfile() {
    std::scoped_lock lk{command_mutex};
    commands.push_back([this] {
        if (Profile* p = CurrentProfile()) {
            const Fingerprint fp = p->fingerprint;
            *p = Profile{};
            p->fingerprint = fp;
            p->sessions = 1;
        }
        SaveKnowledge();
        Note("what the frame-rate target learned on this PC + game + emulator was cleared");
    });
}

void CentralBrain::UpdateStatus(std::uint64_t) {
    StatusView v;
    v.snapshot = last_snapshot;
    for (const auto& b : brains) {
        v.brains.emplace_back(std::string(b->Name()), b->Status());
    }
    v.recommendations = recommendations;
    v.notes = notes;
    for (const auto& id : knobs.Ids()) {
        v.knob_values[id] = knobs.Find(id)->value;
    }
    v.brain_ms_per_tick = ticks ? tick_ms_total / static_cast<double>(ticks) : 0.0;
    v.ticks = ticks;
    v.dropped_frames = dropped_frames;
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
    Profile* p = CurrentProfile();
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
        // Left open on purpose: the next launch counts this one as unclean, exactly as if the
        // process had vanished.
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
    o << "Second Son Adaptive - performance analyser report\n";
    o << "Contract " << ContractVersionMajor << '.' << ContractVersionMinor
      << ", knowledge schema " << KnowledgeSchemaVersion
      << ". Measures only; the frame-rate target is the one thing it chooses.\n\n";
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
        if (!cheats_note.empty()) {
            o << "== Cheats\n" << cheats_note << "\n\n";
        }
    }
    o << "== Software\nemulator " << software.emulator_build << "\ngame " << software.game_serial
      << ' ' << software.game_version << " (eboot " << software.eboot_signature << ")\n\n";
    o << "== Profile " << profile_key << "\n";
    if (const auto it = knowledge.profiles.find(profile_key); it != knowledge.profiles.end()) {
        const Profile& p = it->second;
        o << "sessions " << p.sessions << ", unclean shutdowns " << p.unclean_sessions << '\n';
    }
    o << "\n== Settings set by the player\n";
    bool pinned = false;
    for (const auto& id : knobs.Ids()) {
        const KnobState* st = knobs.Find(id);
        if (st->user_pinned || st->spec.scope == Scope::Never) {
            pinned = true;
            o << "  " << id << " = " << st->value
              << (st->value == st->spec.default_value ? "" : " *") << "  ["
              << ToString(st->spec.domain) << "]\n";
        }
    }
    if (!pinned) {
        o << "  none\n";
    }
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
    o << "\n\n== Measurements by area\n";
    for (const auto& b : brains) {
        o << "  " << b->Name() << ": " << b->Status() << '\n';
    }
    o << "\n== Recommendations (settings that are yours to change)\n";
    if (recommendations.empty()) {
        o << "  none\n";
    }
    for (const auto& r : recommendations) {
        o << "  " << r.knob << ' ' << r.current << " -> " << r.value << ": " << r.reason << '\n';
    }
    o << "\n== Notes\n";
    for (const auto& n : notes) {
        o << "  " << n << '\n';
    }
    o << "\n== Analyser overhead\n"
      << F("%.0f ticks, %.3f ms per tick on the analyser thread, ", static_cast<double>(ticks),
           ticks ? tick_ms_total / static_cast<double>(ticks) : 0.0)
      << dropped_frames << " frame samples dropped\n";
    return o.str();
}

} // namespace Adaptive
