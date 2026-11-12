<sub>[← back to README](../README.md)</sub>

# Mouse and keyboard play

> Status: **works, not finished.** Mouse look, the original crosshair and free key binding are in and play well in
> Siberia and Arcade. The list of what is still open is at the end — it is honest, and it is the next thing we work on
> after 1.0.

TimeSplitters 2 is a PS2 game with no idea what a mouse is. There is no hidden PC code path to switch on; everything
here is done from the outside, on top of the unchanged game logic.

## What is done

| Part | How it works |
|---|---|
| **Mouse look** | Each VBlank the runtime reads the mouse delta from the window and writes yaw and pitch **straight into the game's player object** (player pointer at `0x59DC0C`, yaw at `+0x148` in degrees, pitch at `+0x14C`, pitch limit at `0x59DC1C`). The addresses came from RAM diffs of a stick-turn replay; the layout is the same the GameCube Mouse Injector uses for its version. The game's own camera code then does everything else, so recoil, death cam, vehicles and cutscenes behave as on the PS2. |
| **Sensitivity** | `yaw += dx * 0.022 * sensitivity`, the same scale as the classic PC shooters, so a value that feels right elsewhere feels right here. Zoom has its own sensitivity (percentage of the normal one), vertical can be inverted. |
| **Original crosshair** | While the mouse is captured the port holds the game's **aim mode** (L2). That is where the game draws its own reticle. The reticle's "sway" offset (`+0x8B8/+0x8BC`) is pinned to 0 every frame, so it sits exactly where the bullets go. We tried a drawn-on crosshair first; it was rejected because it changes the feel of the original. |
| **No "magnet"** | TS2's *auto lookahead* pulls the pitch back towards the horizon while you walk (up to ~1° per frame). With a stick you never notice; with a mouse it feels like a magnet. The port now owns the camera while the mouse is captured: small drifts the game applies are undone, big changes (recoil, being hit) are kept. |
| **Bindings** | Every PS2 button and both sticks can be put on any key or mouse button, in the **Controls** tab while playing (click a field, press a key). Two slots per action. Mouse wheel and side buttons work. Defaults are a PC shooter layout: `W A S D`, left = fire, right = zoom (hold), middle/`Q` = secondary fire, `E`/Space = use, `R` = reload, `C`/Ctrl = crouch, Shift = aim mode, wheel/arrows = weapons, `P`/Enter = pause, Tab = select. Layouts are versioned, so an old saved layout is replaced once when the defaults change. |
| **Zoom on hold** | The PS2 zooms in steps (D-pad up/down). Holding the zoom button zooms in; releasing it sends the same number of steps back out. |
| **Capture** | Click into the game to capture the mouse, ESC / F1 (or opening the menu) releases it. Linux: X11 pointer grab with relative motion; Windows: Raw Input (`WM_INPUT`) with the cursor clipped to the window, absolute-motion devices (VMs, Remote Desktop, tablets) are converted to deltas. |
| **Keyboard with an idle gamepad** | A plugged-in but idle gamepad used to make the port ignore the keyboard. While the mouse is captured, the keyboard always counts. |

Verified with input replays: a scripted 66° turn and 24° pitch land exactly; the pitch holds at −35.5° through
10 seconds of walking (it used to be pulled to 0°); a bullet hole appears in the centre of the reticle.

## What is not done yet

- **Walking speed in aim mode.** The original crosshair lives in aim mode, and TS2 walks about 16 % slower there. So
  with the mouse you walk slightly slower than a stick player. Options being looked at: writing the walk speed back,
  or finding the game's own "crosshair visible" flag so aim mode is not needed. Until then: untick "Original
  crosshair" in Controls to walk at full speed without a reticle.
- **Real-Windows Raw Input** has been tested under Wine and in code review only; the first hands-on round on a
  Windows PC is running. The Linux/X11 path is tested daily.
- **Vehicles and mounted guns** (the Siberia turret, the Wild West horse section) take the mouse through the same
  yaw/pitch, but their feel has not been tuned.
- **Sensitivity curve.** Linear only; no acceleration option, no separate horizontal/vertical scale.
- **Menus with the mouse.** The game's own menus are still gamepad/keyboard-driven (arrows + Enter). The port's
  settings menu is mouse-driven.
- **Split-screen.** The mouse always controls player 1.
- **Wayland native.** The game window runs through XWayland; that works everywhere, but a native Wayland pointer lock
  would avoid the cursor warp near the edges.

## Settings and keys

All in the in-game menu (ESC / F1 / Guide) → **Controls**. The same values live in `einstellungen.ini`
(`maus_zielen`, `maus_empfindlichkeit`, `maus_invertiert`, `maus_zielmodus`, `zoom_empfindlichkeit`, `taste_<n>`,
`belegung_version`), see [BUILDING](BUILDING.md).

## Steam Deck and gamepads

On a Steam Deck mouse aiming is off by default (a tap on the touchscreen or the trackpad would otherwise capture the
mouse). On every system the mouse is released as soon as the gamepad is used, so the game never stays in mouse
aiming mode while you play with sticks. Turn it on in the menu (Controls) if you use a mouse on the Deck.

