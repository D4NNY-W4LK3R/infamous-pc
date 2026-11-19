SECOND SON ADAPTIVE - inFAMOUS Second Son on PC
4 GB VRAM build (infamous-pc 1.1.0) + native mouse and keyboard + self-tuning optimizer
==========================================================================================

You need your own dump of the game (base + update 1.07 merged, the folder with eboot.bin) and
your PS4 system modules, exactly as for infamous-pc. No game files are included.

INSTALL
  1. Extract this zip to a folder on E:, for example  E:\SecondSonAdaptive\Game
     Everything the emulator writes (saves, settings, shader cache, logs, what the optimizer
     learns) goes into the "user" folder next to shadPS4.exe - nothing on C:.
  2. If Windows says VCRUNTIME140.dll is missing, install the Visual C++ Redistributable
     2015-2022 x64 once: https://aka.ms/vs/17/release/vc_redist.x64.exe
  3. Put your PS4 system modules in user\sys_modules (create the folders if needed).
     Moving over from infamous-pc? Copy the contents of its data folder (its "user" folder, or
     %APPDATA%\shadPS4) into the new "user" folder instead: that brings sys_modules, saves and
     the shader cache along.
  4. Double-click PLAY_SECOND_SON.cmd and paste your game folder the first time.
  5. The first start compiles shaders: expect stutter once. Keep user\cache.

CONTROLS (keyboard and mouse)
  The game keeps its PlayStation button prompts; the panel at the bottom left shows the key
  for each of them (Home shows / hides it). It reads your real bindings, so it stays right
  after you change keys (F8 reloads the bindings).
  Mouse                in gameplay: true 1:1 mouse look. Wherever the game's normal camera is
                       not running (spray painting, menus, special scenes) the mouse drives the
                       right stick AND the motion sensor, so the spray can's pointer follows it.
                       F7 captures / releases the mouse.
  P                    stencil art: the spray can is held on its side, which swaps up/down
                       and left/right of the mouse. P = sideways, P again = sideways the
                       other way (if up/down is reversed), P again = normal (graffiti).
                       Where you aim up or down stays put for 5 s after you last moved
                       the mouse, also on ledges and rooftop edges where the game pulls
                       the camera back to level (SHADPS4_MOUSE_AIM_TILT_HOLD=0: off).
  Left / right button  shoot (R2) / aim (L2)          - and =   mouse speed down / up
  W A S D              move          Space  jump (X)        Left Shift  dash (O)
  C                    melee (Square)                 F       Karmic / interact (Triangle)
  Q / E                special (L1) / heavy (R1)       U / O   L3 / R3
  R / Y                touchpad left / right           Tab     touchpad press
  R + Y together       a thumb on each side (prompts with two fingerprints, e.g. "Move car")
  Z / X  or wheel      swipe the touchpad up / down    V / B   swipe left / right
                       (arrow prompts such as lifting wreckage). Holding the key keeps the
                       finger down at the end ("swipe up and hold": Mobile Command cores).
  Middle mouse button  TOUCHPAD ANYWHERE: hold it and the mouse is a finger on the touchpad,
  (or back side button) starting in the middle; a small touchpad at the bottom shows it.
                         click            = press the touchpad (Core Relays)
                         hold still       = keep it pressed (draining smoke / neon / video)
                         hold and move    = swipe any way (doors, wreckage), or slide to the
                                            mark on the D.U.P. hand scanner
                         move, keep holding = swipe and hold (Mobile Command core: keep
                                            clicking the left button, it stays R2)
                         + right button   = a second finger, one on each half ("Move car")
                       The camera pauses while it is held; the left button still shoots.
  Enter or Esc         pause (Options)                 Arrows  D-pad
  G / H                tilt the controller left / right (spray can)     T  shake
  Delete               mouse = touchpad (fingerprint scanners); press again to go back
  F6                   mouse = motion sensor only      F1      optimizer window
  Home                 show / hide the key panel
  F10 FPS counter      F11 fullscreen      F12 screenshot      F9 pause the emulator
  A DualSense / DualShock 4 / Xbox pad works at the same time.
  The layout lives in user\input_config\CUSA00004.ini (EU) or CUSA00223.ini (US) - edit freely;
  it is only written when it does not exist yet (the swipe keys are added to older copies). Second Son always uses this file, even if the
  emulator is set to share one input config between games.

  Mouse look needs a recognized game executable. The log says either
     "Mouse aim: camera hook at ..."          -> true mouse look is available; "gameplay camera
                                                 live" appears once you walk around freely
     "Mouse aim: camera hook NOT installed"   -> your eboot is a build whose code was never
                                                 measured; the mouse then moves the right stick
  The F1 window shows the same status. The hook is never forced onto an unknown build.

THE OPTIMIZER (F1)
  F1 opens and closes the window. While it is open the mouse is a normal pointer for its
  buttons; closing it gives the mouse back to the camera.
  Modes (user\adaptive\settings.ini, or the F1 window):
    observe      measures only, never changes anything
    manual       measures and recommends; you click "Try it" to test a change
    automatic    (default) applies safe live changes during gameplay, measures each one in alternating
                 10-second blocks, keeps only real improvements, undoes regressions at once,
                 and reuses what it learned in earlier sessions. It also tests ONE start-up
                 setting per launch, judged only on pause / menu screens it has seen before
                 (pausing for a minute now and then gives it the seconds it needs).
    calibration  as automatic, but start-up tests use the title screen: start the game, let it
                 reach the title screen, then do not touch anything for 2 minutes.
  It knows what is on screen: gameplay (you are pressing keys), cutscene / watching, menu /
  pause (a still picture), loading / video. A test compares gameplay with gameplay only; a
  cutscene or the menu pauses it, and quitting keeps its seconds for the next session.
  Scene switches (gameplay <-> cutscene, loading) are measured: the report lists the freezes
  and what they were spent on (shader compiling, loading data, shared memory, the card).
  SS_FAST_FIRST_COMPILE=1 builds an effect shader the game needs right now without the
  driver's optimizations first (the optimized one replaces it). Off by default: on a GTX 1650
  those builds were slower, not quicker.
  Frame-rate target (F3 > Graphics, next launch): "Optimizer: Frame-Rate Target" 30 and
  "Lowest Quality It May Use" Low or Office. After each launch it checks your gameplay frame
  rate and picks the performance mode for the next one: lower quality when you were under the
  target, back up when the card had plenty of room. It never goes below your limit.
  What it never does: change graphics settings, the frame-rate / vblank setting, async compute
  or present mode on its own (those are only recommended), override a value you set yourself
  in the environment, or keep a change it could not measure as a win.
  It re-checks learned values by itself when the GPU driver or the emulator build changes, and
  starts a new profile when the game build or the PC changes. "Forget what was learned" in F1 starts over.
  Every session writes user\adaptive\latest-report.txt: hardware, decisions, evidence, cost.
  Turn it off completely: enabled = 0 in user\adaptive\settings.ini (then the emulator behaves
  exactly like the plain 4 GB build).
  With "Smallest Render Size It May Use" set as well, the target goes: your choice -> Low ->
    smaller render sizes (down to your limit) -> Office (if allowed). It moves one step per
    launch (two when far below the target) and back up when the card has room. Since build 33
    it only lowers quality when the graphics card is the limit (busy 85% of the frame or more),
    and it checks every step down: one that saved less than 8% of the card's time is undone and
    not taken again - no less detail for nothing.

FRAME RATE AND SETTINGS (F3)
  F3 opens the emulator settings; the mouse pointer is free while it is open and mouse look
  comes back when it closes. Graphics > "Frame Rate Limit": Off / 30 / 40 / 45 / 60. On a
  GTX 1650 the game runs at about 30-35 fps; a 30 limit shows every frame for the same time,
  which feels smoother than an uneven 30-35. It applies at once and is kept in
  user\adaptive\settings.ini (frame_limit). Keep Present Mode on Mailbox: vsync (Fifo) at 60 Hz
  makes ~30 fps frames alternate between one and two refreshes.

SECOND SON GAME PATCHES (F3 > Graphics, apply at the next launch; game update 01.07 only)
  Ported from the shadPS4-second-son v1.1.1 fork. Each checks the game's code byte for byte
  first and is skipped if anything differs; F1 > "Second Son game patches" shows what is on.
  Particle Timing Fix (off): the game reads its particle results one frame later instead of
    waiting for the graphics card in the middle of every frame. Off by default since build 25:
    with it on there were white flashes, and the emulator stopped twice. If it stops with it
    on, it is turned off for the next launch automatically.
  Particle Lights in One Submit (off, experimental): fewer GPU submissions in big fights.
  Performance Mode: Off / Low / Office - the game's own lower quality levels.
    Low:    less detail far away, one sun-shadow layer less, particles (smoke!) at half
            resolution, fewer high-resolution textures (saves video memory).
    Office: even less detail, particles at quarter resolution, no high-resolution textures,
            half the pedestrians.
  The optimizer recommends Low when the game stays well under 30 fps because of the graphics
  card ("Use next launch" in F1). If anything looks or behaves wrong, turn the patch off in F3.
  Render Size (build 30): the game renders fewer pixels and the picture is scaled to the
    window - 1920x1080 (its own), 1792x1008, 1664x936, 1536x864, 1408x792, 1280x720,
    1152x648. The HUD keeps its size. Much lighter for the graphics card and its memory;
    turn on "Enable FSR" in F3 > Graphics for a sharper picture at the smaller sizes.

UPDATING TO A NEW BUILD - ONE DATA FOLDER FOR EVERY BUILD
  From this build on, saves, shader cache, settings, logs and what the optimizer learned live in
  "SecondSonAdaptive-data\user", next to your build folders (for example
  E:\Games\...\PhoenixDownloads\SecondSonAdaptive-data\user). Extract each new build in its own
  folder or over an old one: every build next to that folder uses the same data.
  The first start of this build creates it by COPYING the "user" folder of the build folder you
  played most recently (newest save); the old folders are not changed or deleted. The log says
  which one was copied ("Adaptive: data folder ...").
  To keep one build separate, put an empty file named portable.txt next to its shadPS4.exe.
  What the optimizer learned also survives emulator updates now: learned values are re-checked
  once instead of being forgotten.

TROUBLESHOOTING
  Run COLLECT_DIAGNOSTICS.cmd and look at / send diagnostics.zip.
  Back to the plain 4 GB behaviour for one run: set SS_ADAPTIVE=0 and SHADPS4_MOUSE_AIM=0
  before starting, or use the original infamous-pc build - this build never touches its files.

Built from pinned sources (SOURCE_PINS.txt); SHA256.txt has the executable's hash.
GPL-2.0-or-later. Not affiliated with Sony Interactive Entertainment or Sucker Punch.
