<sub>[← back to README](../README.md)</sub>

# Building

## How the game is built

You don't build anything by hand. The release package contains the complete setup: it checks your disc image,
extracts it, recompiles the game code with the bundled recompiler and builds the executable with the bundled
compiler (`./TS2-Recomp.sh`, see the [README](../README.md#play-it)).

The source of the recompiler and runtime ([`recomp/`](../recomp), a fork of PS2Recomp) and of the PCSX2 GS/SPU2 bridge
([`gsbridge/`](../gsbridge)) is in this repository under GPL-3.0, as required for the binaries shipped in the package.
The bridge can be rebuilt with the [CI workflow](../.github/workflows/linux-bridge.yml). The in-house development,
test and reference tooling is not published.

## Settings

Most settings live in the in-game menu (ESC / F1 / Guide, or Select + Start on a gamepad) and are saved to
`~/.config/ts2-recomp/einstellungen.ini` (`sprache=-1|0|1` = menu language auto / English / German). A few are launch options (environment variables):

| Setting | Default | Effect |
|---|---|---|
| `TS2_WIDESCREEN` | `1` | real 16:9; `0` = original 4:3 |
| `TS2_AUFLOESUNG` | `3` | internal resolution multiplier (1–8) |
| `TS2_HUD43` | `1` | HUD and menus in 4:3 proportion at 16:9; `0` = stretched like the original |
| `TS2_LETTERBOX` | `0` at 16:9, `1` at 4:3 | black bars in cutscenes |
| `TS2_HUD_RAND` | `1` | corner HUD elements at the 16:9 screen edges; `0` = whole HUD in a centred 4:3 area |
| `TS2_ALLE_LEVEL` | `0` | `1` = all story levels and challenges unlocked (saved into the profile) |
| `TS2_MITSCHNITT` | `0` | `1` = screenshot every 30 s + sound recording + frame-rate log while playing |
| `TS2_HAENGERBERICHT` | `1` | `0` = no automatic hang report in `build/spielen.log` |
| `TS2_EINGABE_WIEDERGABE` | — | replay a recorded session (`build/eingaben_letzte_sitzung.txt`) |
| texture packs | — | put a PCSX2 pack (`.zip/.7z/.rar`, `SLUS-20314`) into `texturpakete/`, toggle it in the menu |

## Recording and the free camera

- **F8** starts and stops a video recording (internal resolution, 59.94 fps, with sound, H.264 `crf 16`
  `veryfast`) into the screenshot folder; `PS2XGS_CAPTURE=<file.mp4>` starts one at launch. The FFmpeg 8 libraries
  ship in the `gs/` folder. Up to 4× internal resolution records in real time on a desktop CPU; above that the game
  waits for the encoder (the file is still 60 fps, only the session slows down). `PS2XGS_CAPTURE_PARAMS` overrides
  the x264 parameters (for example `crf=14:preset=slower` for an offline-quality render),
  `PS2XGS_CAPTURE_CODEC` the codec (`h264_vaapi`, `h264_nvenc` when your FFmpeg has them).
- **F7** toggles the **free camera** (dev mode): the mouse looks, `W A S D` or the left stick move along the view
  direction, Shift = 4× speed; the game's own input is frozen, so the character neither walks nor fires. At the
  moment the game still snaps the height back to the floor every frame, so it is a ground-level dolly rather than a
  flight: good for tracking shots, not yet for aerials. `TS2_FREECAM=1` / `TS2_FREECAM_AB=<tick>` /
  `TS2_FREECAM_TEMPO=<units per frame>` drive it in scripted runs.
