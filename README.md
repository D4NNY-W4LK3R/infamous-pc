<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<div align="center">

# Second Son Adaptive

### inFAMOUS Second Son on PC with native mouse and keyboard, built for 4 GB graphics cards

[**⬇ Download (Windows x64)**](https://github.com/D4NNY-W4LK3R/infamous-pc/releases) ·
[What it adds](#what-it-adds) ·
[Set up](#set-up) ·
[Reporting problems](#reporting-problems) ·
[Credits](#credits)

![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![Graphics](https://img.shields.io/badge/graphics-Vulkan-red)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-green)

</div>

---

## What is this?

**Second Son Adaptive** is a fork of [infamous-pc](https://github.com/MeWeGo/infamous-pc), the
[shadPS4](https://github.com/shadps4-emu/shadPS4) build tuned for **inFAMOUS Second Son**. It
keeps infamous-pc's speed work and adds what playing it on a PC needs: true mouse look and
keyboard controls, the touchpad and motion sensor from the mouse, and a lot of work to keep the
game smooth on a card with only 4 GB of video memory (developed on a GTX 1650 laptop).

It runs update **01.07** of the EU (CUSA00004) and US (CUSA00223) releases. Every change to the
game's code is checked byte for byte first and skipped on any other version.

> [!NOTE]
> No game files are included. You need your own dump of the game (base + update 1.07 merged
> in one folder, the one with `eboot.bin`) and the system modules (`sys_modules`) from your own
> PS4.

## What it adds

**Native mouse and keyboard**
- True 1:1 mouse look in gameplay through a hook on the game's own camera (only installed on a
  measured game build); everywhere else the mouse drives the right stick and the motion sensor.
- An on-screen key panel next to the PlayStation button prompts, read from your real bindings
  (Home shows / hides it).
- The touchpad from the mouse: hold the middle (or back side) button and the mouse is a finger
  on the pad - tap, hold, swipe, a second finger with the right button.
- The motion sensor from keys and mouse for the spray can: tilt (G / H), shake (hold T), and
  flip keys for stencils that paint the wrong way (P, N, M).

**Performance analyser and frame-rate target (F1)**
- Measures every second of play: frame times, what holds the frame rate back (the graphics
  card, the emulator's GPU thread, or the game's own threads waiting on shared memory), freezes,
  scene switches, video memory, power. Each session writes a plain-text report.
- An optional frame-rate target picks the game's performance mode and render size once per
  launch, never below the lowest quality you allow. It changes nothing while you play.

**The 4 GB video memory work**
- Empty buffer memory left out of video memory (on by default): about 1.4 GB of the memory the
  game reads holds only zeros and is not kept on the card. Seconds under 30 fps went from 65% to
  about 21% on the GTX 1650.
- A GPU hang record and "VRAM:" / "Copy-backs:" log lines for finding what is left, and an
  experimental fault-ahead switch for the game's write faults on shared memory.

**Second Son game patches (F3 > Graphics, next launch)**
- The game's own lower quality levels (Low / Office), smaller render sizes with the HUD kept at
  its size, office-level textures, and the particle patches of shadPS4-second-son v1.1.1.

**Cheats (F1 > Cheats)**
- Infinite energy, bombs, shards, finisher, Neon / Video dash and a longer Smoke dash, each
  byte-checked and switched on at once; "keep on" remembers them.

**Diagnostics**
- `COLLECT_DIAGNOSTICS.cmd` packs the logs, the session reports and the crash records into one
  zip to send with a problem report.

## Set up

1. Download the newest `SecondSonAdaptive-windows-x64-build<N>.zip` from
   [Releases](https://github.com/D4NNY-W4LK3R/infamous-pc/releases) and extract it to a folder
   of its own, for example `E:\SecondSonAdaptive\Game`.
2. If Windows says `VCRUNTIME140.dll` is missing, install the
   [Visual C++ Redistributable 2015-2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
3. Put your PS4 system modules in `user\sys_modules`.
4. Run **`PLAY_SECOND_SON.cmd`** and paste the path of your game folder the first time.
5. The first start compiles shaders: expect stutter once. Keep the `user\cache` folder.

`README_FIRST.txt` in the zip has the full key list, the F1 / F3 options and what every log line
means. Saves, settings and the shader cache live in a `SecondSonAdaptive-data` folder next to
your build folders, so a new build picks them up.

## Reporting problems

Close the game, run `COLLECT_DIAGNOSTICS.cmd` and attach the `Build<N>_Diagnostics.zip` it makes
to an [issue](https://github.com/D4NNY-W4LK3R/infamous-pc/issues), with what you did just before
the problem. Your GPU, driver and the build number are in the zip.

## Build from source

Every push to a `second-son-adaptive*` branch is built and packaged by GitHub Actions
(`.github/workflows/second-son-adaptive.yml`) and published under Releases. To build yourself:

```sh
git clone --recursive -b second-son-adaptive https://github.com/D4NNY-W4LK3R/infamous-pc.git
cd infamous-pc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The performance analyser, game patches, cheats' byte checks and input helpers have stand-alone
unit tests:

```sh
cmake -S tests/adaptive -B build-adaptive
cmake --build build-adaptive && build-adaptive/adaptive_tests
```

## Credits

- **Emulator:** [shadPS4](https://github.com/shadps4-emu/shadPS4) and all its contributors
- **Game-tuned build:** [infamous-pc](https://github.com/MeWeGo/infamous-pc) (Eye Of The Ruins,
  0xDeftones) and its parent [shadPS4-Infamous-FixBuild](https://github.com/rayhlancaner-png/shadPS4-Infamous-FixBuild)
  (4 GB OOM fix by TOXIC-KID)
- **Native keyboard / mouse groundwork and game patches:** shadPS4-second-son v1.1.1
- **Cheats:** the GoldHEN cheat database authors (CUSA00004 01.07: Weysincha, 2B) and the dash
  cheats of Liondarkz/issn_pc
- **Shader fixes** ported by infamous-pc from [HayatoG/iss_pc](https://github.com/HayatoG/iss_pc)

## License

[GPL-2.0-or-later](./LICENSE). No game files are included. Not affiliated with Sony Interactive
Entertainment or Sucker Punch Productions; *inFAMOUS* is their trademark.
