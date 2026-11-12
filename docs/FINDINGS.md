<sub>[← back to README](../README.md)</sub>

# Technical findings

Notes on what TimeSplitters 2 needed beyond a stock PS2Recomp setup. Each item was confirmed by
measurement against PCSX2 (savestates, GS dumps, memory comparisons), not guessed.

## Executable layout
- One monolithic, stripped ELF (`.text` 0x200000–0x499f50, entry 0x200008). No EE overlays.
- VU1 microcode lives in `.vutext` as 19 MPG blocks. These sections are hidden from the EE
  recompiler and built into a 16 KB VU1 image,
  bit-identical to a live capture. The MPG load address field is **11 bits** (`& 0x7FF`), not 10.
- Function discovery: data scan for code pointers + `lui/addiu` and `lui/ori` pairs
  (1368 pointers, found by an in-house tool). Switch tables in `.rodata` are filtered out.

## Recompiler fixes (`recomp/ps2xRecomp`)
- A function may start in the delay slot of `jr`/`j`/`b` (e.g. `sceSifInitRpc`); only conditional
  branches, `jal` and `jalr` rule a delay-slot start out.
- Tables of numbers that happen to lie in code range are not pointer tables when neighbours are unaligned.
- `LQ`/`SQ`/`LQC2`/`SQC2` ignore the low 4 address bits, as on hardware.

## Runtime fixes (`recomp/ps2xRuntime`)
- **VIF1 GS download** (`DIR=0`, `sceGsExecStoreImage`): implemented via the GS bridge FIFO read,
  with completion delivered on the following VBlank events.
- **DMAC interrupts while masked** are kept pending and delivered when `EnableDmac` is called.
- **Scratchpad DMA addresses** (bit 31 on MADR/TADR/ASR/tags) are mapped to scratchpad memory.
- **Alarm syscalls** use Sony's numbering (`0xFD` = ReleaseAlarm, `-0xFE` = iSetAlarm) alongside ps2sdk's.
- `sceCdSearchFile` resolves real LSNs from the ISO9660 directory (`src/ts2_override.cpp`).

## The HLE layout trap (`sceGsSetDefLoadImage`)
The generic HLE stub wrote a 12-byte private struct, but TS2 builds the real 0x60-byte
`sceGsLoadImage` GIF packet and reads `NLOOP` at +0x50. The upload size was read from leftover text
("me" = 0x656D), which desynchronised the VIF stream and crashed the GS renderer. Fix: let the game's
original libgraph code run. Lesson: HLE stubs for libraries whose
structs the game reads back must match Sony's exact memory layout, or not be used.

The same applied to the display/draw environment functions (`sceGsSetDefDispEnv`, `sceGsPutDispEnv`,
`sceGsSetDefDrawEnv`, `sceGsPutDrawEnv`, `sceGsSetDefDBuff`, `sceGsSwapDBuff`, `sceGsSetDefClear`,
`sceGszbufaddr`): the HLE versions produced `DISPLAY2 = {MAGH=0, DW=511, DH=223}` instead of the
hardware values `{MAGH=4, DW=2559, DH=447}` read from a PCSX2 savestate, so only part of the frame was
shown. With the original code the privileged GS registers (PMODE, SMODE2, DISPFB2, DISPLAY2) match PCSX2
exactly, including the double-buffer flip.

## Ruled out by measurement
CD pseudo-LBN overlap, asset decompression (100 % identical to PCSX2), the render-timeout alarm.

## Input
- DualShock 2 pressure bytes follow PCSX2's `PadDualshock2.cpp` order: right, left, up, down, △, ○, ×, □,
  **L1, R1, L2, R2** (the runtime had L2/R1 swapped).

## More HLE traps found by comparing with PCSX2
The generic HLE stubs carried over from other ports were the most common source of bugs. Each of these now runs
as the game's original code:

- **`fread`**: newlib `__svfscanf` (`sscanf` with `%c`) reads through `fread` from a string `FILE` on the stack.
  The host-file HLE rejected it.
- **libsdr `sceSdRemote*`**: the HLE reported `sceSdVoiceTrans` as finished without moving any data into SPU2.
  The real `SDRDRV.IRX` already runs in the IOP emulator against PCSX2's SPU2.
- **libm (`__ieee754_rem_pio2f`, `__kernel_sinf/cosf`, `sin`, `floor`, `ceil`, `fabs`)**: the HLE used host IEEE
  math. The EE FPU has no Inf/NaN (`x/0 = ±Fmax`). Level 1 calls `acosf(-10.2)`, which gives +Fmax on the
  hardware. Host `sinf`/`cosf` of that returned NaN/-Inf, an object angle became 1.2e36, and the game's
  normalisation loop (`while (w >= 1) w -= 1`) never ended. The original code under the runtime's exact EE-FPU
  emulation gives bounded results.

How it was found: an in-house tool drives a muted, hidden PCSX2 to the same point. IOP logs and
savestates were compared with the port. An in-house tool stops the port mid-hang (native stack = guest call
stack), and a RAM dump showed the one object that differed from the PCSX2 savestate.

## libvu0: invisible objects
The generic HLE replacements for Sony's `libvu0` (28 functions: matrix setup, `sceVu0RotTransPers`, `sceVu0ClipScreen`,
`sceVu0ClipAll`, `_sceVu0ecossin` …) were written for another game. In TS2 they produced different results from the
library's own VU0 macro code: fences, crates, trees, weapons and the first guard were culled. With the original code
(translated VU0 macro instructions with PS2 semantics) the scene matches PCSX2, at the same frame rate.
See `docs/screenshots/compare_vu0_hle.jpg`.

## Half-HLE libraries
In a stripped ELF the analyzer only names part of a library. For `libmpeg` that left `sceMpegCreate`/`GetPicture`/…
as HLE but `sceMpegIsEnd`, `IsRefBuffEmpty`, `SetDecodeMode`, `GetPictureRAW8(xy)`, `Set/ResetDefaultPtsGap` and
`DispWidth/Height` as original code. Those read `mpeg->sys` fields that the HLE never fills, so the trailer
"ended" immediately. Rule: a library is either completely HLE or completely original. The missing entries were
identified by their code (all access `*(a0+0x40)`) and listed for the stub filter.

## Split-screen: every pad port saw player 1 (pc040–pc041)

The pad HLE fed the input of port 0 (script / first gamepad) to all ports, and port 2 always reported "connected"
in script runs. Now every port has its own source (port 1: keyboard/first gamepad, port 2: second gamepad through the
GS bridge `ps2xgs_pad_n`, ports 3–4 likewise), `scePadGetState` reports a port as disconnected when nothing is
plugged in, and test runs drive player 2 with `TS2_INPUT2`. Checked against PCSX2 with a second virtual gamepad
(`drueck2`/`halte2` in the remote): the title offers "2 Players" with two pads, and the port does the same.
The game probes the multitap through the real `MTAPMAN.IRX` in the IOP emulator (slot 1 queries return "no pad").

## Split-screen speed: VU1 is the bottleneck (pc042–pc045)

Profile of Story co-op: the VU1 thread runs at 100 % of a core (69 % of all samples), the EE waits for it. Stage 4 of
the VU1 translator is bit-identical but not faster. Stage 5 (SSE FMAC helper, dead MAC/status flag removal, clip ring,
cycle counter in a register) brings co-op to 59.9 FPS, but needed two TS2-specific additions in an in-house tool
(SotC's microcode uses neither):

1. **`FCSET`**: the SotC translator refused stage 5 for programs with `FCSET`. Now `FCSET` sets the working clip value
   and becomes an entry of the clip ring with its real cycle, exactly like `queueFcset` (a first attempt that handed it
   to the interpreter pipeline was wrong: that entry was committed only at program end and overwrote later CLIP results).
2. **`EATAN` → `MFP`**: the lighting code computes angles with the EFU. With the cycle counter held locally, the P
   register was never published (`commitReadyPipelines` does not run per pair at stage 5), so `MFP` read the previous
   result: `vf15.x` 1.672 instead of 1.654, a visible brightness change in the level fly-over. Pairs with an interpreter
   fallback now get the real cycle and publish ready EFU results first, like Q.

Found with `PS2X_VU1_DIFF=2` (each VU1 call also run on a copy through the interpreter and compared). After the fixes:
single-player route and 12 co-op frames pixel-identical; only the CLIP register at the end of the program at `0x18d8`
still differs in the per-call comparison (the compiled program ends ~90 cycles earlier), with no effect on any frame.

## Widescreen HUD (pc047–pc052)

The community widescreen patch (No.47, in `pcsx2_patches`) changes the eight aspect constants of the 3D cameras; there
is no community fix for the HUD. Findings:

- None of the seven other `7/6` constants in `.lit4` touches HUD or weapons (`TS2_WS_EXTRA`, one run per constant).
- `PS2X_GIFSTAT`: everything, HUD included, reaches the GS through VU1 (PATH1). The 3D world uses `STQ` texture
  coordinates; HUD, text and menu graphics use fixed `UV` coordinates (`PRIM.FST=1`).
- So the runtime squeezes FST packets by 0.75 around the screen centre. Two exceptions are needed: packets at least
  560 px wide (menu backgrounds) and packets at least 360 px high: the game copies the finished frame back in vertical
  strips for its effects, and squeezing those produced ghost images in the first attempt (`pc049`).
- Two of the seven callers of the projection function `0x33d3d0` computed their aspect as `(H/3)/(W/4)*(W/H) = 4/3`
  in code. An in-house tool patches `lui at,0x4040; mtc1` into `nop; lwc1 f,-0x1800(gp)`, a padding word between
  `.sdata` and `.sbss` that no code references; the runtime writes 3.0 or 2.25 there every frame. A temporary log of
  all projection calls (`pc052`) shows every camera at 16:9 in widescreen; 4:3 mode stays bit-identical.
- The first-person weapons were not stretched: with all cameras at 16:9 they keep their 4:3 shape.

## Cutscene letterbox (pc053)

The PCSX2 community patch "No Cutscene Letterbox" (Muzzarino, `pcsx2_patches`) rewrites seven 16-bit immediates in
the cutscene drawing code (`0x3C7030` … `0x3C7684`). PCSX2 applies it permanently; here it is switchable without
recompiling: each `addiu reg,zero,imm` becomes `lhu reg,X(gp)` reading from a halfword table in the same unused padding
(`0x59e9d0`), and the runtime writes either the original values or the patch values every frame (`TS2_LETTERBOX`).
With the original values the output is bit-identical to `pc040`. The level-intro fly-over has no letterbox; the
in-level cutscenes still need a test run.

## HLE audit (pc055–pc064)

The 198 generic HLE stubs carried over from other ports were counted at run time (an in-house tool
instruments every `ps2_stubs::X(...)` call, `TS2_STUBCOUNT=1` prints a table every 20 s) across boot, menus,
story, co-op, Arcade, Challenge, MapMaker, memory card and the trailer. Result: **137 have call sites in the
game, 65 are actually called**. Each of the 65 was then switched to the game's original code one library at a
time and compared with PCSX2 (screenshots, RAM, GS registers):

| Group | Now | Result |
|---|---|---|
| libc `memcpy memset memmove memcmp strcmp strncmp strlen strncpy strcpy strcat strchr strstr _mbtowc_r` | original | identical frames, same speed |
| libdma (8 functions: `sceDmaGetChan`, `sceDmaSend`, `sceDmaSync`, …) | original | identical, 59.9 FPS |
| `sceGsResetPath`, `sceGsSyncPath` | original | identical, 59.9 FPS |
| `sceGsResetGraph` | **HLE kept** | original sets `IMR=0xFE00` like PCSX2 (`0x7E00`), but the end-of-frame `SIGNAL` (id `87654321`) arrives one VBlank late because VU1/GS run asynchronously here → 30 FPS. Documented as an open architecture item |
| `sceGsSyncV` | **HLE kept** | it is the port's frame pacer (host VSync); reads interlace/field mode from the game's own `GParam` (`0x56A250`, same bytes as in PCSX2) |
| SIF/RPC, pad, memory card, CD, file I/O | HLE by design | these are the interfaces to the host (gamepads, save folder, disc image) |

Side fix found on the way: the GS `SIGNAL`/`FINISH` interrupt was only checked at the next VBlank. Setting either
bit now wakes the EE scheduler at once (`gs_frontend.cpp`, `PS2X_GS_EVENT_NO_WAKE` to compare), and reads of the
DMA/GIF/VIF status registers flush pending transfers first (`PS2X_POLL_NO_FLUSH`).

## Co-op ghosting at 16:9 (pc068)

In split-screen the game copies each half of the finished frame back in **half-height** vertical strips (blur
effect). The HUD correction only excluded copies taller than 360 px, so at 16:9 these strips were squeezed like HUD
and every frame left a shifted copy behind (`docs/screenshots/koop_ghosting.jpg`, left `pc065`, right `pc068`).
Each strip points its texture base at its own page of the frame buffer, so `U` starts at 0 for every strip and the
"UV = screen position" test missed them; a shape-based guess (`pc067`: 1:1 sprites) did not catch them either.
The reliable criterion: the runtime remembers every render target (`FRAME_1/2.FBP`, `FBW`, on all GIF paths) and a
2D packet whose `TEX0.TBP0` lies inside one of them is a screen copy and stays untouched. Co-op and single-player
both 59.9 FPS, HUD in proportion, no ghosting.

## 30 instead of 60 FPS on screen, and the original's frame blending (v0.9.5)

**Only every second frame was shown.** The game ran at 59.94 FPS internally, but PCSX2's on-screen counter said
29.97 and that was true. In live mode the bridge called `MTGS::PostVsyncStart(false)`, so PCSX2 never learned that
the game had written the display registers. It fell back to counting frame-buffer blits, saw one every second
VSync and, with `SkipDuplicateFrames` (on by default), skipped presenting the other frame. The bridge now reports a
new frame whenever `PMODE`, `DISPFB1/2` or `DISPLAY1/2` changed since the last VSync
(`GSDumpReplayer.cpp`). Title screen: before 29.97 FPS `[B]`, 33.3 ms per presented frame; after 59.94 FPS `[P]`,
16.7 ms (`PS2XGS_PERFLOG=1` logs the overlay values).

**Ghost images when turning are the original game's effect.** A PCSX2 reference run of the same Siberia scene
(keys shifted by ~3000 ticks because PCSX2 plays the intro and loads slower) shows the same
translucent copies next to moving edges. With `TS2_BLENDLOG=1` the runtime lists every textured primitive that
reads from a render target. Each frame has two kinds:
- a 512×448 sprite (`PRIM 0x156`) whose texture is the frame it draws into, a no-op;
- triangle strips (`PRIM 0x15C`, `ALPHA 0x44` = (Cs−Cd)·As+Cd) whose texture is the **other** frame buffer, i.e.
  the previous frame, blended at about 50 %. This is the frame blending.

The menu option **Bild & Shader → "Nachzieheffekt beim Drehen (wie Original)"** (picture & shaders → motion trail when turning) (`nachzieheffekt` in
`einstellungen.ini`, default off; the "Original look" preset turns it on) keeps those strips. When it is off the
runtime collapses their vertices onto the first one, so they cover no pixels (`ps2xNachziehFix` in `ps2_memory.cpp`,
`TS2_NACHZIEH=1` forces the effect in test runs). Menus are unaffected.

## Recompiler: sign branches only looked at 32 bits (v0.9.9)

Found while proving the turret fix: with `TS2_DTOA_TEST=1` the runtime calls the game's own `malloc` and `sprintf` once
in the running game and compares the result with the host. `malloc` worked, but `sprintf("%f")` printed garbage for
most values (`1234.5678` → `0.00000012…6,78`, digits like `:`), and the game's double-precision soft-float routines
(libgcc fp-bit `dpsub`, PS2 has no double FPU) computed `1.5 − 2.25 = 15.25`.

Cause in the recompiler (`ps2xRecomp/src/lib/control_flow_emitter.cpp`): `blez`, `bgtz`, `bltz`, `bgez` and their
likely/link variants were emitted as `GPR_S32(ctx, rs) < 0` etc. The R5900 is a 64-bit MIPS: these branches test bit 63
of the whole register. For ordinary sign-extended 32-bit values both agree, which is why the game looked fine; for
64-bit values (soft-float mantissas, `long long` arithmetic) the branch went the wrong way. Fixed to `GPR_S64`;
1,691 branch sites in TS2 changed. After the fix all nine `printf("%f")` test values match the host exactly and the
gross soft-float errors are gone. Some double results still differ from the host in the last bit, which matches
fp-bit's own rounding; to be confirmed against PCSX2.

## Siberia turret hang: the C library had no heap (v0.9.7–v0.9.8)

Mounting the gun turret on top of the dam (right before the helicopter) froze the game about a second later: VBlanks,
sound and pad polling went on, no new frame came. The built-in hang report (v0.9.6) showed EE thread 3 **running** at
`0x497688`, every other thread waiting normally. That address is newlib's `__lshift` inside `_dtoa_r`, i.e. the game was
formatting a float as text (`printf("%f")`), apparently for the first time at the turret. `__lshift` grows a Bigint by
doubling its capacity until it is large enough (`do k <<= 1; while (k < need)`); with capacity 0 that never ends.

Capacity 0 came from `_Balloc` getting NULL: **every** libc `malloc` failed in the port. Two layers:

1. The recompiler replaced newlib's allocator (`malloc`, `free`, `_malloc_r`, `_free_r`, `_calloc_r`, `_realloc_r`,
   `malloc_extend_top`, `_malloc_trim_r`) with runtime stubs that allocate from the runtime's own guest heap. For TS2
   that heap is empty, so they always returned NULL. **v0.9.8:** an in-house tool keeps these functions as the
   game's original code.
2. The original allocator gets memory through `_sbrk` (`0x443EA0`), which compares against `EndOfHeap()`. TS2's startup
   calls `SetupThread(gp, 0x01FF0000, 0x10000, …)` and `SetupHeap(0x01FEDF30 /* end of bss */, -1)`: on the PS2 the libc
   heap is the ~8 KB up to the stack and `EndOfHeap()` returns `0x01FF0000`. The runtime returned **0**. **v0.9.7:**
   `SetupThread` remembers the stack bottom, `SetupHeap(start, -1)` sets the kernel heap end to it, `EndOfHeap()`
   returns that (`Kernel/Syscalls/System.cpp`, `PS2X_HEAPLOG=1` logs it).

v0.9.7 only had the second fix, so the turret still froze on the Steam Deck; the first one was found by logging
`EndOfHeap()` and seeing it was never called. The game mostly uses its own allocator, which is why nothing else broke
visibly.

## Sound in real time (pc069–pc073)

All earlier test runs were muted and unpaced, so the sound path had only been checked for "data reaches the SPU2".
The first real play session showed: menu music fine, silence from the first cutscene on, then random noises and no
gunshots. Found with real-time WAV recordings (`PS2X_WAVREC`), the SPU2 voice dump (`PS2X_SPULOG`) and a new
`sceSdRemote` trace (`PS2X_SDRLOG`). Three separate bugs:

1. **Wrong sectors for streamed audio.** `STREAM.IRX` (music, speech, cutscene audio) looks up its files with the
   IOP's own `sceCdSearchFile`. That returned LSNs of a *virtual* disc built from the extracted folder, while
   `sceCdRead` read from the real ISO, so the stream buffers in SPU RAM were all zeros (`nz=1` in the voice dump).
   The IOP lookup now walks the ISO9660 directory of the image itself, like the EE side already did.
2. **Sound bank uploads rejected.** The game uploads the level bank (`S_SPU_<n>.PAK`, ~1 MB) in ~100 pieces, one
   `sceSdVoiceTrans` per piece. Only the first succeeded; the rest returned `-210` (busy): libsd checks the DMA mode
   bits in `ATTR`, which its DMA interrupt clears, but the SPU2 only reported the transfer as finished after its
   transfer time in IOP cycles, and during an EE RPC burst almost no IOP time passes. The bridge now reports a normal
   SPU DMA as finished at once (the port side schedules the interrupt like the non-bridge path and drops the late
   SPU2 report).
3. **Data copied too late.** PCSX2's SPU2 copies a DMA in 0x100-word steps over the transfer time, reading the IOP
   buffer the whole time, but the game already writes the next piece into that same buffer. The bridge now copies a
   normal (non-auto) DMA completely when it starts; auto-DMA streams are unchanged.

Result: 234 of 234 uploads accepted, the effect voices play real sample data, gunshots audible.
