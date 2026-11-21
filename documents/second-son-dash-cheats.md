<!--
SPDX-FileCopyrightText: 2026 infamous-pc contributors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Optional Second Son movement cheats (prototype)

Based on Liondarkz/issn_pc's GPL-2.0-or-later gameplay hook research
(`src/dash_hook.h`, `src/hero_hook.h`, and `gpu/shim/issn_hero.cpp`).
This implementation targets **CUSA00223 / APP_VER 01.07**, with independently
enabled cheats. It **does not** modify save files or include Sony game files.

## Activating the switches

From a Command Prompt in the unpacked SecondSonAdaptive release folder:

```cmd
set SHAD_SECOND_SON_INFINITE_NEON_DASH=1
set SHAD_SECOND_SON_INFINITE_VIDEO_DASH=1
set SHAD_SECOND_SON_SMOKE_DASH_SCALE=3
PLAY_SECOND_SON.cmd
```

These values are per process, not saved. Remove any setting or set it to
0 (Neon, Video) or 1 (Smoke) to disable the corresponding cheat on the next launch.

- **Neon:** Removes only the 2-second ability timeout branch. The normal
  held-button/release condition remains intact.
- **Video:** Removes only the 1.5-second timeout branch. Releasing dash still
  ends the flight normally.
- **Smoke:** While the game is running, scales the hero's
  `MAX_SMOKE_DASH_RATIO` field. Default is 1× (off), available 1–5×.
  3× matches the source port's cheat.

## Important compatibility and validation details

- Entire feature is disabled unless a setting is explicitly enabled.
- The game serial and application version must match.
- Neon and Video check complete expected instruction sequences and relevant
  RIP-relative references before changing conditional-jump bytes.
- Smoke checks the hero global and character getter signatures before starting
  its Windows-only trainer. Its character-memory accesses are checked through
  `VirtualQuery` each time; the trainer follows character changes.
- If the 1.07 executable signature differs, **the cheat is skipped**, not
  force-applied. Check the emulator log for the specific status.
- This is not a speed/FPS optimization. Very long dashes can create world
  streaming stress or make the character cross unloaded geometry.

## Current validation status

Isolated clang++ tests passed on Linux using synthetic game-memory images
including 100 deliberately damaged signatures. A separate Windows-code-path
mock test verified Smoke scaling from 1→3 and 2→6, but this did **not** run
inside the actual emulator. Full Windows build, real gameplay, and
save/mission-transition tests are still required before merging.

A draft PR exists so the working `second-son-adaptive` branch and its published
releases remain unchanged during review.
