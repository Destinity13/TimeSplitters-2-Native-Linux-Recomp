<sub>[← back to README](../README.md)</sub>

# Verification

> The in-house test and reference tooling described here is not published.

Nothing is fixed by guessing. Each bug was found by putting the port next to a reference and finding the first
difference.

## PCSX2 as the hardware reference

- **Remote control**: PCSX2 runs hidden (Xvfb) and muted. A command file drives a virtual
  gamepad step by step (`drueck`, `halte`, `warte`, `bild`, `lade`); `drueck2`/`halte2` drive a second gamepad for split-screen. Every `bild` writes a savestate plus a screenshot.
- **Savestates compared field by field:** GS privileged registers (PMODE, SMODE2, DISPFB, DISPLAY) from
  `PCSX2 Internal Structures.dat`, EE RAM (`eeMemory.bin`), object tables.
- **IOP console logs** (PCSX2 `emulog.txt` vs. the port's `[IOP printf]`) side by side. For example, `buffer %x not
  allocated` appeared only in the port and led to the libsdr bug.

## Inside the port

- **Headless test runs**: scripted input (`SOTC_INPUT`), screenshots at fixed VBlanks, a
  watchdog with thread states, a fresh memory card per run. The story-level sweep selects each of the ten levels
  through the real menus (the level list appears a fixed 300 VBlanks after the ✕ that skips the intro) and plays
  ~6 minutes of scripted movement, shooting and aiming per level; latest sweep with `pc114`: 10/10 levels, 59.94 FPS,
  no hangs.
- **gdb mid-hang**: stops the running port after N seconds. Because
  translated functions call each other natively, the native stack is the game's own call stack. This found the
  endless `sub_00312B18` loop.
- **RAM dumps** (`SOTC_RAMDUMP=<vsync>`) compared against PCSX2's `eeMemory.bin`.
- **Self-tests**: `TS2_FPUTEST=1` calls the game's translated `acosf/sinf/cosf` with edge values (±max, Inf/NaN bit
  patterns) and checks that the results are PS2-plausible.
- **Diagnostic logs**: `PS2X_DISPLOG` (display registers), `PS2X_RPCLOG` (every SIF RPC with both ABI readings),
  `PS2X_DLLOG` (GS read-back rhythm), `PS2X_STREAMLOG` (SIF/SPU DMA, IOP CD reads), `SOTC_TRACE_VIF` (VIF/XGKICK
  statistics), `PS2X_FPSLOG` (real-time frame rate).

## VU1

The translated VU1 programs and the cycle-accurate interpreter produce identical frames on the same route
(`PS2X_VU1_REC=1` vs. `0`). The VU1 image built from the ELF is bit-identical to a live
capture.

Since `pc045` there is also a per-call check: `PS2X_VU1_DIFF=2` runs every VU1 call a second time on a copy through
the interpreter and compares all registers, the clip value (including entries still in the pipeline), the VU data
memory and the GS packets. The first difference is printed with register and both values (for example
`vf15.x uebersetzt 3fd5f482 Interpreter 3fd3be6a`, which led to the `EATAN`→`MFP` fix). 10.9 million calls on the
reference route: all VF/VI/ACC/Q/P/I values and GS output identical.

## Multiplayer

Test runs drive player 2 with `TS2_INPUT2` (same format as `SOTC_INPUT`). The reference is PCSX2 with two virtual
gamepads: with two pads its title screen offers "2 Players", and so does the port.
