---
title: Bug fixes and updates - 1.1 is on the way
date: 2026-11-14
tag: Update
image: img/smoke-before-after.jpg
summary: Thanks for the reports! The German launcher on Windows, the black bars, the grey smoke wall and a lobby freeze are fixed. The update goes up as soon as the last round of testing is through.
---

# Bug fixes and updates - 1.1 is on the way

Thanks to everyone who reported problems here and on Reddit. This is what's fixed and what's new. The update goes up as soon as the last round of testing is through, above all on Windows.

✅ fixed and tested · 🧪 fixed, final test on Windows still running

![The smoke when bullets hit a surface, before and after](img/smoke-before-after.jpg)

## Fixed

| | |
|---|:-:|
| **Launcher always in German on Windows**, no matter which language you picked ([#1](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues/1), [#2](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues/2)) | 🧪 |
| **Black bars at 16:9** on 16:10 screens. The picture now fits any screen: 16:9, 16:10, ultrawide ([#2](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues/2)) | ✅ |
| **Big grey, flickering smoke wall** when bullets hit a surface ([#3](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues/3)). Checked against the original running in PCSX2 | ✅ |
| **Slowdowns on weaker graphics cards**: the "Auto" resolution now steps down by itself when the frame rate stays low ([#2](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues/2)) | 🧪 |
| **Online+ lobby could freeze** after a few seconds | ✅ |
| Texture packs could not be imported on Windows | 🧪 |

## New

Everything here is optional. The game as it was on the PS2 stays the default, and each extra can be switched on by itself.

- **Texture packs** in PCSX2 format: put the archive into the `texture_packs` folder and switch it on in the settings menu under Textures.
- **Modern gamepad aiming**: the right stick turns the view directly and the crosshair stays in the centre. Off by default, the original controls stay as they are.
- **Unlock everything**: opens all levels, characters and modes while it is switched on. Your own progress stays untouched.
- **Everything in English**: launcher, settings file, folders and log messages. German stays available as a menu language.

## What's next

- An auto-aim switch for the original controls
- The new options inside the game's own menus
- More comparisons against the original, scene by scene

Found something that still looks wrong? [Open an issue](https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/issues) and tell us your system, graphics card and the internal resolution you use.
