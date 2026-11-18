# Second Son Adaptive — completion matrix

Status as of 2026-11-18, after the player's runs of builds 10 and 14 (diagnostics before and after an NVIDIA shader-cache cleanup). **Implemented** = code exists in branch `second-son-adaptive`.
**Tested** = an automated check passed (named). **Unverified** = needs the player's PC, GPU or
game files. **Blocked** = cannot proceed without something missing. No benchmark or gameplay
result below was measured on the target laptop beyond what the session reports show; none is claimed.

| # | Requirement | Implemented | Tested (evidence) | Unverified on target PC | Blocked |
|---|---|---|---|---|---|
| 1 | Pinned base infamous-pc v1.1.0 `2c599e0`, 4 GB renderer preserved | yes: only additive hooks in render code, defaults unchanged | unmodified v1.1.0 built on Windows CI (run for `122b8a84`, release `ssa-…-1`); patched tree reproduced from pinned commit + patches, tree hash checked | runs like plain infamous-pc on GTX 1650 (checklist A) | – |
| 2 | Native mouse camera (1:1), signature-gated, never forced | yes | 7 signature unit tests; **build 14 on the player's PC (CUSA00004 01.07): hook installed on the exact match and live in gameplay ("gameplay camera live (mouse look)"); player: "mouse works"** | other game versions (refused by design, falls back to right-stick mouse) | – |
| 3 | Comprehensive KB/M controls + on-screen key hints | PC layout, R2/L2 mouse buttons, `-`/`=` speed, F7/Delete/F6, G/H/T kept; mouse = right stick + motion sensor whenever the camera hook is idle (spray can); layout used even with the shared-config setting; F1 overlay; key panel (Home) | build 10 issues (F1, layout, inert mouse) fixed in build 11/14; **build 14: player confirms "mouse works, keyboard works"** | – | in-place keyboard glyphs inside the game's own prompts need the v1.1.1 fork's picture pack, which is not in its source |
| 4 | Central brain, 7 mini-brains, workers, versioned contracts | yes: `adaptive/brain`, `mini_brains`, `knobs`, `contracts` | contract mismatch refused; one brain per domain; cross-domain proposal rejected (R2) | – | – |
| 5 | Independent Safety Gate | yes: rules R1–R12, emergency rollback on **sustained** evidence (2 slow seconds in a row, or 3 spike seconds of the last 5), kill switch | 11 gate tests + emergency end-to-end test (bad value lives ≤ 8 s, blocked, kill switch) + single-hitch test (one 130 ms second during a faster value is not an emergency) | – | – |
| 6 | Hardware discovery, capability-aware | yes: hwinfo (CPU/cores/AVX2/RAM/OS/GPU/driver), Vulkan device + budget, battery state; fingerprint | fingerprint tests (RAM class tolerance, GPU change, NVIDIA driver decoding) | discovery on the laptop (checklist C3); Optimus GPU pick | – |
| 7 | GPU memory / textures / buffers / transfers / shaders / CPU / frame pacing subsystems | yes: live knobs texture-GC headroom, upload wake; start-up knobs readback window, prefetch backoff, hot window, page lookahead, compiler threads; frame pacing + async compute recommend-only | mini-brain tests; Windows build | whether any knob helps on GTX 1650 — **unknown until measured** | – |
| 8 | Persistent learning, invalidation on hardware / driver / game / emulator change | yes: `knowledge.txt`, checksummed, atomic, `.bak` recovery, schema change, driver → re-check, crash → blocked; schema 2 forgives v1 "regressed" blocks (v1 could block after one hitch), keeps crash blocks | 12 knowledge tests incl. every-flipped-byte detection, torn-file recovery, 100 save/load cycles, v1→v2 migration; end-to-end reuse next launch | – | – |
| 9 | Observation-first calibration, measured optimization, rollback, user-controlled autonomy | yes: modes observe/manual(default)/automatic/calibration; A/B live (interleaved blocks, Mann-Whitney), launch A/B on idle seconds | simulated-emulator tests: improvement kept + reused, regression reverted + blocked, neutral not kept, calibration learns a start-up value across launches, crash blames experiment, driver change, user env wins, observe/manual change nothing | real calibration sessions (checklist D, E) | – |
| 10 | Never silently change unsafe/unvalidated settings | yes: graphics, vblank, present mode, async compute are user-only; user env pins knobs; every decision logged with rule + evidence | gate + brain tests | – | – |
| 11 | Optimizer overhead bounded and reported | yes: per-tick cost, dropped samples in report and F1 | ring push+drain 37 ns/frame (test build); **on the i7-9750H: 0.187 ms per 1 s tick (build 10), 0.24–0.32 ms (build 14), 0 samples dropped** | – | – |
| 12 | Windows x64 executable | yes | built on GitHub Windows runner, tests passed there, PE x64 verified, SHA-256 matches; builds 10 and 14 delivered to `E:\Games\MY Own MOD\Hybrid Emulator\`; **build 14 runs on the laptop** | – | – |
| 13 | Reproducible Windows build without local tools | GitHub Actions on the fork: pinned v1.1.0 + branch commits, VS-bundled clang-cl, tests, smoke test, pre-release zip | build 10 published and verified (PE x64, SHA-256); the local one-click builder was dropped at the player's request (downloads from Releases instead) | – | – |
| 14 | Tests: modular, integration, regression, performance, diagnostics | 67 unit/integration tests; CI on GCC+ASan/UBSan, Clang, clang-cl; session reports; COLLECT_DIAGNOSTICS | all pass (CI `unit-tests-linux`, Windows test step) | – | – |
| 15 | Optimization validated against the baseline on target hardware | method implemented (calibration baseline, A/B) + manual F-section of checklist | build 14 field run: ~30 fps in gameplay, VRAM at 137–142 % of budget (the main limit); one live test (texture-GC headroom 256) measured faster medians (29.1 → 27.5 ms) but was rolled back by a single hitch — fixed above, to be re-measured | **all** performance claims | needs more gameplay sessions on the player's GPU |
| 16 | Documentation | README_FIRST, VALIDATION_CHECKLIST, ARCHITECTURE.md, this file | – | – | – |

Known limits, stated plainly:
* The optimizer's levers are the tunables this emulator build already has plus two new live
  knobs; it cannot make the GPU faster than the 4 GB build's renderer allows. On a GTX 1650 it
  may well find that the defaults are best — then it keeps them, which is the correct outcome.
* Start-up experiments compare different launches; they are only comparable on a steady idle
  scene (title screen). Gameplay-wide claims need the manual F-section measurement.
* Architecture v1.0's text and the ChatGPT Phase 1 code were not available; the architecture
  was reconstructed from the exported conversation (messages 27/29/31) and implemented fresh.
