# GS/SPU2 bridge for PCSX2

`libps2xgs.so` is PCSX2's GS renderer (Vulkan/OpenGL, internal upscaling, texture replacements)
and SPU2, built as a shared library that the native port loads in-process. None of PCSX2's CPU, VU,
DMA or IOP emulation runs: the port pushes GIF packets and GS register writes (`ps2xgs_transfer`),
privileged register writes, VSync/field events and SPU2 register/DMA traffic into it, and gets back
the presented frame, a small settings menu overlay and the audio output.

- `pcsx2_ps2x.patch` — changes to PCSX2 at commit `PCSX2_COMMIT` (GS dump replayer "live" mode,
  MTGS hooks, SPU2 entry points, build option `PS2X_GSBRIDGE`).
- `ps2x-gsbridge/Bridge.cpp` — the library's public C API, settings (`PS2XGS_CFG`), the in-game menu,
  texture replacement folder (`PS2XGS_TEXDIR`, `PS2XGS_TEXDUMP=1`), input capture in the game window.
- `ps2x_deps.sh` — builds PCSX2's pinned third-party dependencies.

PCSX2 (GS and SPU2) is GPL-3.0-or-later; the patch and bridge are published under the same terms. See `docs/BUILDING.md`
for the build steps.
