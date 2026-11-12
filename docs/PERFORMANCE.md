<sub>[← back to README](../README.md)</sub>

# Performance

AMD Ryzen (32 threads) + Radeon RX 7900 XT, CachyOS, PCSX2 GS (Vulkan) via the bridge.

## Frame rate (`pc033`–`pc035`)

Measured with `PS2X_FPSLOG=1`: every 250 VBlanks the port prints VBlanks per second and how many of them showed a
new frame (the game flips `DISPFB2` once per frame).

| Scene | VBlanks/s | Visible FPS | Notes |
|---|---:|---:|---|
| Story level 1, in-game (fly-over, tunnel, moving, shooting) | 59.9 | **59.9** | 250/250 flips |
| Menus, synchronous GS read-back (`pc034`) | 46.4 | 46.4 | |
| Menus, asynchronous GS read-back (`pc035`) | 48.6 | 48.6 | pixel-identical to the synchronous path |
| Menus, profile-guided build (`pc036`) | **59.9** | **59.9** | |
| Arcade deathmatch with bots (`pc036`) | 59.9 | 59.9 | |

## History

| Build | Change | Effect |
|---|---|---|
| `pc009` | first image, VU1 interpreter | 22.5 FPS |
| `pc011` | VU1 microcode translated to native code (`vu1rec.py`, 2 programs, 3,730 instruction pairs) | 50.6 FPS |
| `pc036` | profile-guided build (GCC `-fprofile-generate` → training run through menus and Siberia → `-fprofile-use`), as in the SotC port | menus 48.6 → 59.9 FPS; whole route 163 → 147 s |
| `pc045` | VU1 translation stage 5 (SSE FMAC helper, dead flag removal, clip ring, cycle counter in a register) with TS2 support for `FCSET` and EFU results | Story co-op 30–40 → **59.9 FPS**, pixel-identical |
| `pc035` | VIF1 GS download asynchronous: the request is queued, the EE keeps running, the data is waited for at the VBlank that completes the DMA | menus +5 % |

## CPU profile in-game (`pc033`)

Sampling profiler (`SOTC_PROFILE`), last minute of the level run:

| Thread | Share | Top |
|---|---:|---|
| Game (EE + IOP emulator) | 47 % | SPU2 mix, scheduler, IOP timers, game code spread out |
| VU1 (MTVU) | 25 % | compiled VU1 program 62 % |
| Window/present | 14 % | timer polling |
| GS | 10 % | texture cache read-back |

There is plenty of headroom in-game. In the menus (`pc035`) the **VU1 thread was saturated (76 % of all samples)**
by the animated hex background, not the GS read-back; the profile-guided build removed that limit.

## Split-screen profile (`pc041`)

Story co-op, 2 players, last minute of the run:

| Thread | Share | Top |
|---|---:|---|
| VU1 (MTVU) | 69 % | compiled VU1 program 59 %, pipeline bookkeeping 19 % |
| Game (EE + IOP emulator) | 20 % | SPU2 mix, scheduler, game code |
| GS | 6 % | texture cache read-back |

Split-screen renders the world twice, so the VU1 thread became the limit (one core at 100 %). Stage 4 of the
translator changed nothing; stage 5 removed the limit (`pc045`).
