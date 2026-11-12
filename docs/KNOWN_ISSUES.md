<sub>[← back to README](../README.md)</sub>

# Known issues

An honest list, as of 11 November 2026 (release 1.0.1).

## Not a bug

- **Siberia on Easy:** there is no "Restore power" objective and the dam elevators stay without power. The
  original game does the same; the objective exists on Normal and Hard only.
- **Siberia: the snow haze looks busier than on a PS2 at high resolutions.** Measured against PCSX2 (software
  renderer as the reference): the game logic, the wind and the haze speed match the original. At 3× and above the
  haze texture is rendered much sharper than on the console, so its movement stands out more. A "softer haze"
  option for high resolutions is planned for an update.
- **Free camera (F7) cannot gain height.** The game recomputes the eye height from the floor under the player
  every frame; writing the position (and its copies at `0x784708`, `0x7E82B0`) is not enough. The write-watch tool
  did not catch the writer on this build yet. Horizontal movement and pitch work.
- **Mouse play is not finished** – the open points (aim-mode walking speed, vehicles, sensitivity curve, Windows
  hands-on) are listed in [MOUSE](MOUSE.md).
- **Mouse play: walking is a bit slower than with the stick.** The mouse holds the game's aim mode (that is where the
  original crosshair comes from), and TS2 walks about 16 % slower in aim mode. Release the mouse (ESC) or untick
  "original crosshair" in Controls to walk at full speed without the reticle.
- **Mouse play: an old key layout is gone after updating.** Bindings are versioned; a layout saved by a build before
  `pc113` is replaced by the PC-shooter defaults once. Rebind in the Controls tab, it is saved live.

## Open

- **Some Arcade setup menus** (3D character preview) run at 49–56 FPS; main menus and gameplay run at 59.9 FPS.
- **Sound:** three bugs in the real-time sound path fixed (`pc069`–`pc073`, see FINDINGS); being verified by
  playing. `TS2_MITSCHNITT=1` records the sound (`ton.wav`) and a screenshot every 30 s for checking.
- **Tested so far:** Story level 1 played through on PC and Steam Deck (also as 2-player co-op), Arcade League, Challenge, MapMaker, options, the bonus
  trailer. Later story levels, Arcade Custom matches with 2 players and 3–4 players (multitap) are untested.
- **3–4 players:** the multitap is reported to the game and all four slots are read; joining players 3/4 with
  scripted input has not worked yet, real gamepads still need a test.
- **Story levels 2–10:** scripted runs could not reach the level list reliably (menu timing varies); they are checked
  while playing (`TS2_ALLE_LEVEL=1` unlocks all levels). Level 1 runs at 59.9 FPS.
- **HLE audit done:** of 198 generic stubs only 65 are ever called; libc, libdma, `sceGsResetPath/SyncPath` now run
  as original code. Still HLE: the host interfaces (SIF, pad, memory card, CD, file I/O), `sceGsSyncV` (frame pacer)
  and `sceGsResetGraph` (the original one costs 30 FPS until VU1/GS timing matches the PS2; see FINDINGS).
- **HD extras:** in-game settings menu (ESC/F1), texture packs (`texturpakete/`), FXAA/CAS/anisotropic filtering and
  vkBasalt effects are in; no texture pack is shipped (third-party content).
- **VU1 per-call comparison:** the CLIP register at the end of the program at `0x18d8` still differs from the
  interpreter (the compiled program ends ~90 cycles earlier). No frame is affected; kept under observation.

## Fixed (kept for the record)

- **Windows: black window / "VM not ready" on real PCs** (1.0.1): the graphics hand-over file pointed to `C:\tmp`,
  which does not exist on Windows; it is in the system temp folder now. Setup without OpenGL (VMs, Remote Desktop)
  works too. Windows starts with Direct3D 11 (Vulkan as fallback). See [WINDOWS_PORT](WINDOWS_PORT.md).
- **Windows: exit at start on real PCs (`0xC0000409`)** (1.0.1): background threads of the graphics DLL were still
  joinable while the process shut down / the DLL unloaded, and the C++ runtime aborted. The threads are detached and
  the process ends without running those destructors; PCSX2 data now lives in `%APPDATA%\ts2-recomp\pcsx2`.
  `build\bridge.log` records every start step and, if it ever crashes again, the exact location.
- **16:9 HUD and sniper scope** (1.0.1): corner HUD elements (ammo, timers) sit at the 16:9 edges by default
  (`TS2_HUD_RAND=0` keeps the centred 4:3 layout); menus, loading screens and centred messages stay centred. The
  scope frame, crosshair and health bars (untextured sprites, not adjusted before) line up in the zoom.
- **PCSX2 start messages** "patches.zip" / "Controller 1 has no input bindings" no longer show (1.0.1).

- **Siberia gun turret froze the game** (v0.9.8): the C library had no heap (allocator stubs + `EndOfHeap()` returned 0), so
  `printf("%f")` looped forever. See FINDINGS.
- **Only 30 of 60 frames shown** (v0.9.5): PCSX2 skipped every second frame as a duplicate. See FINDINGS.
- **Ghosting when turning** (v0.9.5): the original game's frame blending; now switchable, off by default.
- **Steam Deck: random weapon switches, visible mouse cursor, low FPS** (v0.9.4).

| Build | Problem | Cause and fix |
|---|---|---|
| `pc009` | garbage geometry, GS crash | HLE `sceGsSetDefLoadImage` wrote a 12-byte private struct; TS2 reads Sony's 0x60-byte GIF packet (NLOOP at +0x50). Original code now runs |
| `pc011` | 22.5 FPS | VU1 translated to native code: 50.6 FPS |
| `pc023` | GS download hang | VIF1 `DIR=0` (GS→EE) implemented via the bridge FIFO read, completion on the next VBlank, masked DMAC interrupts kept pending until `EnableDmac` |
| `pc025` | only part of the frame shown | HLE display/draw environment functions produced `DISPLAY2 = {MAGH=0, DW=511, DH=223}` instead of `{4, 2559, 447}`; original libgraph code now runs, registers identical to PCSX2 |
| `pc026` | L1/R1/L2/R2 pressure swapped | DualShock 2 pressure order fixed (PCSX2 `PadDualshock2.cpp`) |
| `pc026` | test runs not reproducible | every run gets a fresh memory card (`TS2_MCROOT`) |
| `pc027` | `sscanf("%c")` failing | HLE `fread` rejected newlib's string `FILE`; original code now runs |
| `pc029` | level sound banks never reached SPU2 | HLE `sceSdRemote*` faked `sceSdVoiceTrans`; the real `SDRDRV.IRX` now does the transfer |
| `pc032` | level start hung forever | host-IEEE libm HLE (`sinf/cosf` helpers) returned NaN/-Inf for `acosf(-10.2) = +max`; an object angle became 1.2e36 and `while (w >= 1) w -= 1` never ended. Original libm under exact EE-FPU emulation |
| `pc035` | menus 46.4 FPS | GS read-back asynchronous (EE keeps running until the VBlank that needs the data): 48.6 FPS |
| `pc036` | memory card in slot 2 that PCSX2 doesn't have | HLE reported a formatted card in both ports; now only slot 1 (`TS2_MC_SLOT2=1` for two) |
| `pc036` | menus 48.6 FPS, VU1 thread saturated | profile-guided build (GCC PGO, training run through menus and level): 59.9 FPS everywhere |
| `pc037` | fences, crates, trees, weapons and the first guard invisible | generic HLE of `libvu0` (`sceVu0ClipScreen`, `sceVu0RotTransPers`, matrices, `_sceVu0ecossin`) computed differently from the VU0 macro code; the original code now runs. Same speed |
| `pc038` | bonus trailer stopped after half a second | half of `libmpeg` was HLE, the other half (not named by the analyzer) ran as original code and read fields the HLE never set (`sceMpegIsEnd`); the 9 missing functions were identified in the code and added as HLE |
| `pc039` | no widescreen in the NTSC-U menus | PCSX2 community patch `SLUS-20314_12532F1C.pnach` (No.47): 8 aspect constants in `.lit4` (0x3f955555 → 0x3fc71c71), written every VSync like PCSX2; renderer 16:9 |
| `pc041` | every pad port read player 1's input, port 2 always "connected" | input per port, presence from the actual device; "2 Players" and Story co-op work |
| `pc045` | split-screen 30–40 FPS | VU1 stage 5 with `FCSET` and `EATAN`→`MFP` support for TS2: 59.9 FPS, pixel-identical |
| `pc050` | HUD and menus stretched in 16:9 | 2D graphics (fixed texture coordinates on PATH1) put back into 4:3 proportions; full-screen backgrounds and screen-copy strips excluded |
| `pc051` | two camera projections with a hard-coded 4:3 ratio | instruction patch at recompile time: the divisor 3.0 is read from a free data word, 3.0 (4:3, bit-identical) or 2.25 (16:9) |
