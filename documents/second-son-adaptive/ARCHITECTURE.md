# Second Son Adaptive — Architecture v1.0 as implemented

Base: MeWeGo/infamous-pc **v1.1.0** (`2c599e026f2b69166fd71eac4343f14152a34d38`), the 4 GB-capable
inFAMOUS Second Son build. Native keyboard/mouse ported from **shadPS4-second-son v1.1.1**.
The renderer, caches, uploads, motion keys and touchpad features of the base are unchanged;
every addition is either off-path (telemetry, one relaxed atomic per frame) or a single atomic
store into a value the emulator already had.

## Hierarchy

```
                 Central brain (src/adaptive/brain.*) - 1 low-priority thread, 1 tick/s
   objectives - bottleneck diagnosis - arbitration - A/B evaluation - calibration - learning
        |                    |                        |
   Telemetry            Knowledge Base            Safety Gate        <- shared services,
 (telemetry.*)          (knowledge.*)             (safety.*)            owned by no mini-brain
        |
  Mini-brains (mini_brains.*), one per domain, propose changes to their own knobs only
  GPU Memory (budget authority) | Buffer & Geometry | Texture & Cache | Transfer & Sync |
  Shader & Pipeline | Rendering & Frame Pacing | CPU Workload
        |
  Workers = the emulator's existing fast paths, reached through knobs (knobs.*)
        |
  Emulation core + hardware (unchanged infamous-pc v1.1.0)
```

* **Contracts** (`contracts.h`): every type the modules exchange. Mini-brains declare the contract
  major version; a mismatch is refused at registration. A brain may own knobs of its domain only;
  a proposal for another domain's knob is rejected by Safety Gate rule R2.
* **Telemetry**: `Common::Perf::OnFlip` publishes one `FrameSample` (frame time, 22 stall
  categories, draws/dispatches/submits) per presented frame into a lock-free SPSC ring. The brain
  drains it, builds 10-second snapshots and one-second summaries (the unit of all comparisons).
  VRAM use and budget come from `VK_EXT_memory_budget`, sampled by the brain thread.
* **Bottleneck diagnosis**: warm-up, shader compilation, VRAM pressure, presentation,
  transfers, page faults, host GPU, game CPU, GPU command thread - from stall shares.
* **Knobs**:

| Knob | Domain | Scope | Default | Candidates | What changes |
|---|---|---|---|---|---|
| `SS_TEXCACHE_GC_HEADROOM_MB` | Texture & Cache | live | 0 | 0, 256, 512 | texture GC starts earlier; its own age/GPU-completion rules unchanged |
| `SS_UPLOAD_WAKE_JOBS` | Transfer & Sync | live | 8 | 4, 8, 16 | when the upload worker thread is woken |
| `SHAD_READBACK_WINDOW_KB` | Transfer & Sync | start-up | 128 | 64, 128, 256 | readback widening window (existing tunable) |
| `SHAD_PREFETCH_BACKOFF_MAX` | Buffer & Geometry | start-up | 1 | 1, 2, 4 | stale readback-prefetch skipping (existing) |
| `SHAD_HOT_WINDOW_SECS` | Buffer & Geometry | start-up | 5 | 3, 5, 8 | how long read-back memory is copied ahead (existing) |
| `SHAD_AHEAD_MAX` | CPU Workload | start-up | 16 | 8, 16, 32 | page-invalidation lookahead (existing) |
| `SS_PIPELINE_WORKERS` | Shader & Pipeline | start-up | built-in | built-in, +2, -1 | pipeline compiler threads |
| `SS_PRESENT_MODE` | Frame Pacing | user only | setting | - | recommended, never changed |
| `SHADPS4_ASYNC_COMPUTE` | Transfer & Sync | user only | 0 | - | can hang the GPU on this game: never automatic |

Start-up knobs are environment variables the emulator already reads once; the brain sets them
before any renderer code runs, unless the user set them (then the knob is *pinned* and never
touched). Defaults are the emulator's own built-in values.

## Modes (user-controlled, `user/adaptive/settings.ini` or the F1 window)

| Mode | Changes anything? |
|---|---|
| observe | never |
| manual | only what the user clicks; still measured A/B and reverted if worse |
| automatic (default, the player's choice) | live knobs during steady gameplay; reuses learned values |
| calibration | automatic + one start-up experiment per launch, measured on idle seconds |

## How a change is judged

* **Live, during gameplay**: the new and old values alternate in 15-second blocks (4 + 4), the
  first 2 seconds of each block are discarded, and per-second medians are compared with a
  Mann-Whitney U test. Kept only if the median frame time is >= 2 % lower at p < 0.05, the p99
  is not more than 5 % worse and hitches did not rise. Interleaving cancels scene drift.
* **Start-up, across launches**: the brain collects >= 90 *idle* seconds (no input for 5 s,
  after warm-up, low shader activity) with the current best values (baseline), then one
  launch per candidate; same statistics against the baseline.
* **Emergency rollback**: any second during a test with median > 1.35x or p99 > 2x (and > 50 ms)
  of the pre-change reference reverts at once. Two emergencies for one value block it; two in a
  session trip the kill switch (no more automatic changes until the next launch).
* A neutral result keeps the default. A value tried without a win is not retried in the session.

## Knowledge Base (`user/adaptive/knowledge.txt`)

Profiles keyed by hardware (CPU, cores, RAM class, GPU vendor/device) + game build (serial,
APP_VER, eboot hash) + emulator build. Stored per profile: learned values, values that
regressed or crashed (blocked), every experiment with its evidence, baseline seconds, last
dominant bottleneck, session counts. Text with an FNV-1a checksum line, written to a temporary
file, verified, then renamed; the previous valid file is kept as `.bak` and loaded if the main
file is torn. A schema change keeps history, drops learned values for re-measurement.

Invalidation: other GPU/CPU/RAM class, game build or emulator build -> separate profile (old
results never applied). Driver change -> learned values move to "re-check" and are not used
until calibration confirms them; regressions may be retried, crashes stay blocked. A session
that ends without a clean shutdown blames the running experiment (blocked for good) or, if
learned values were in use, moves them to "re-check".

## Native mouse camera

`core/mouse_aim.*` (from the v1.1.1 fork) adds the mouse's yaw/pitch at the game's single
pan/tilt store through a code cave. `input/camera_hook_signature.*` decides whether that is
safe: exact 27-byte match at the measured offset of a known build -> install with that build's
data offsets; the same bytes elsewhere -> reported as a relocated build, **not** installed;
otherwise not installed. Unreadable gaps between eboot segments are skipped (VirtualQuery).
Without the hook, the mouse drives the right stick. Active only for CUSA00223 / CUSA00004.

## Overhead

One relaxed atomic load per frame when disabled; when enabled, one ~200-byte copy into the ring
per frame on the flip thread, and the brain's tick (reported per session as ms per tick).
