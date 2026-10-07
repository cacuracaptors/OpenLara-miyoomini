# Tomb Raider (OpenLara) - Miyoo Mini Plus / OnionOS Port

This is a port of [**OpenLara**](https://github.com/XProger/OpenLara) - the open-source reimplementation of the
classic Tomb Raider engine by Timur "XProger" Gagiev - to the **Miyoo Mini Plus** handheld running **OnionOS**.

The Miyoo Mini Plus has no 3D GPU, so this port runs on OpenLara's software renderer, which was largely
rebuilt for it: precise, perspective-correct and spread over both CPU cores. This release plays the original
**Tomb Raider (1996)**; the engine and folder layout are ready for more games in future updates.

**[Download the latest release](https://github.com/cacuracaptors/OpenLara-miyoomini/releases)**

## OnionOS Exclusive Features

- Native 640x480, no overclock needed
- A rebuilt software renderer:
  - perspective-correct textures and sub-pixel precise triangles: no PS1-style warping textures, wobbling
    vertices or cracks between polygons
  - a proper depth buffer, so objects no longer show through each other
  - rendering pipelined over both CPU cores
  - effects that were invisible in OpenLara's software renderers: bubbles, bullet sparks, blood, splashes,
    smoke, flames and lava sparks
  - shadows under Lara and the enemies
  - translucent menus and working health and air bars
- Easy installation: just copy the files from the GOG version. The game data is extracted from `GAME.GOG`
  automatically on the first start, with a progress screen
- Music with the GOG file names, as they come (no renaming), including the cutscene audio
- A full PS1-style control scheme adapted to the Miyoo Mini Plus buttons
- Save anywhere from the passport (Save Game), as in the PC game, plus a separate quick save slot
- An in-game help screen listing the Miyoo Mini Plus buttons
- Set Controls with the Miyoo Mini Plus button names (the L2/R2 sidesteps follow the Walk and Left/Right buttons)
- Lara's Home narration, as in the PC game
- 45 to 60 FPS

## Installation

1. Download the release zip and extract it to the root of your OnionOS SD card. This places the `OpenLara`
   folder in `Roms/PORTS/Games/`, and the shortcut and its image in `Roms/PORTS/Shortcuts/` and
   `Roms/PORTS/Imgs/`.
2. You need your own legitimate copy of Tomb Raider, in the **GOG version**: the
   [Tomb Raider 1+2+3](https://www.gog.com/en/game/tomb_raider_123) pack. From its `Tomb Raider 1`
   installation folder, copy into `Roms/PORTS/Games/OpenLara/Tomb Raider 1/`:
   - `GAME.GOG` (required - the game data)
   - `02.mp3` to `10.mp3` (optional - the music)

   To find that folder in GOG Galaxy: select the game, click the settings button next to "Play", then
   "Manage installation" > "Show folder".
3. On OnionOS, open **Tomb Raider** from the Ports list (use "Refresh roms" at the bottom of the list if it
   doesn't show up).

The first start copies the game data out of `GAME.GOG`. It takes about a minute, shows a progress screen, and
only happens once: `GAME.GOG` is removed at the end. It needs about 170 MB of free space on the SD card (on top
of `GAME.GOG` itself); if there isn't enough, the screen says how much is needed.

**Original CD:** copy the `DATA` folder (required) and the `FMV` folder (optional - the videos) into the same
folder instead of `GAME.GOG`.

## Controls

| Button | Action |
|---|---|
| D-pad | Move |
| B | Jump |
| Y | Action (shoot, grab, pick up, use) / confirm in menus |
| A | Roll |
| X | Draw / holster weapons |
| L1 (hold) | Look around with the D-pad |
| R1 (hold) | Walk |
| L2 / R2 | Sidestep left / right |
| Start | Inventory (pause) |
| Select | Show / hide the help screen |
| Menu + R1 | Quick save |
| Menu + L1 | Quick load |

To save, open the passport in the inventory and choose "Save Game?" (its "Current Position" is on the Load Game page). The quick save
(Menu + R1) has its own slot: it is loaded only with Menu + L1 and does not show up on the Load Game page, so
the two never overwrite each other. The buttons can be changed in the controls options (the gamepad in the
inventory); the menus always use Y to select and A to go back. To exit, choose "Exit to Title" and then "Exit Game". The Menu key alone does nothing,
so the OnionOS Menu+Power screenshot combo is safe to use at any time.

## Cheats

OpenLara's built-in cheats work during gameplay. Press the buttons one at a time, without touching the D-pad
in between:

| Sequence | Effect |
|---|---|
| X, L1, X, L1, X, L1, X, L1 | All weapons with ammo (Lara screams) |
| B, L1, B, L1, B, L1, B, L1 | Skip to the next level |
| R1, L1, R1, L1, R1, L1, R1, L1 | Fly mode: Lara swims through the air and takes no damage. Press R1 to land (careful: from high up she falls!) |

They don't work in Lara's Home, in cutscenes or on the title screen.

## Known issues

- The PC version of Tomb Raider has 9 music tracks (the menu theme, the ambient themes and the cutscenes); the
  in-level music of the PlayStation version was never part of it.
- Large, busy areas may run below 60 FPS.
- If the game crashes, a `crash_log.txt` file is created in the `Tomb Raider 1` folder - please attach it to
  your report. The game's log is in `log.txt`, in the same folder.

## Changelog

- **v1.0.5** - Faster in big open areas and around lots of pillars or objects (up to ~25% more FPS in the heaviest scenes), with the very same image: each room is drawn once, near rooms and faces first, hidden stretches skipped, adaptive perspective, and drawing straight into the screen. The in-game menu now shows the paused game darkened behind it, like the original, and opens smoothly. No reverberation (the original game has none, and it cost CPU). The alligator in the water is less persistent, as in the original.
- **v1.0.4** - Vsync: no more screen tearing, the panel itself paces the game. Up to 60 FPS where the scene allows (it was held at 50), and a faster renderer with the very same image (about 12-17% less drawing time; videos, menus and loading screens much lighter). More precise depth: surfaces almost touching each other no longer flicker.
- **v1.0.3** - The quick save (Menu + R1) and the passport's Save Game now use separate slots: the quick save is loaded only with Menu + L1, and the passport save only from the Load Game page. Select now shows the help screen, rewritten with the Miyoo Mini Plus buttons (Start still opens the inventory). The Set Controls page and the menu hints now show the Miyoo Mini Plus buttons instead of the keyboard keys, and choosing a button already in use swaps it between the two actions. Enemies are as aggressive as in the original game again: on the Miyoo Mini Plus their random choices almost never came out (a wounded animal nearly always ran away instead of attacking), and the creatures that never give up a chase in the original (bears, lions, crocodiles, raptors, the T-Rex and others) could give up and wander off.
- **v1.0.2** - Fixed enemies getting stuck running in place near some edges (the bears in City of Vilcabamba): they are now kept away from walls the way the original game does it.
- **v1.0.1** - Shadows under Lara and the enemies. Lara's Home narration now plays. Save the game from the passport, as in the PC game (the PlayStation save crystals are gone). Picking up items fixed: only the nearest one, from close by, with no more teleports (and keys stay in their keyholes). The picked-up item spinning in the corner is drawn correctly. Pushable blocks can only be grabbed from their own level (standing on top of one, Lara was pulled down to grab it).
- **v1.0.0** - Initial release: Tomb Raider 1.

## Building from source

This port cross-compiles for ARMv7 hard-float with a Docker-based toolchain. Tested on Windows + WSL2 + Docker
Desktop.

### Prerequisites

- WSL2 with Ubuntu, and Docker Desktop with WSL integration enabled
- The [union-miyoomini-toolchain](https://github.com/shauninman/union-miyoomini-toolchain) container
- A Miyoo Mini buildroot sysroot providing SDL 1.2, expected at
  `/root/workspace/mini/arm-buildroot-linux-gnueabihf/sysroot` inside the container
- [steward-fu/sdl2](https://github.com/steward-fu/sdl2) at `/root/workspace/sdl2-miyoo`, for the Miyoo Mini
  audio (MI_AO) headers and libraries in `mini/inc` and `mini/lib`

### Steps

Inside the toolchain container, with this repository at `/root/workspace/OpenLara`:

```bash
cd /root/workspace/OpenLara/src/platform/bittboy
./build_miyoomini.sh
```

This produces two binaries in `bin/`: `OpenLara-sw-debug` (with symbols, for `addr2line` on crash reports)
and `OpenLara-sw` (stripped: the one shipped as `OpenLara` in the release). The release also ships
`libSDL-1.2.so.0` from the sysroot.

### What this fork changes (compared to upstream XProger/OpenLara)

- **`src/gapi/sw.h`** - the software renderer: exact sub-pixel triangle rasterizer (top-left fill rule, plane
  equations), 16-bit depth buffer, perspective-correct texturing, near-plane clipping, an exact backface
  test, a frame recorder that rasterizes on the second core while the first prepares the next frame, 2D UI
  with translucency, and the per-room underwater palette and light shimmer
- **`src/level.h`**, **`src/controller.h`** - effect sprites placed correctly in fixed-function renderers,
  per-room water palette, sky backdrop for the software renderer
- **`src/savegame.h`**, **`src/game.h`** - separate save slots for the quick save and the passport's Save Game
- **`src/lang.h`** - the help screen text and the gamepad button names of the Miyoo Mini Plus
- **`src/gameflow.h`** - TR1 music mapped onto the 9 PC CD tracks, GOG file names accepted
- **`src/sound.h`** - MP3/OGG decoder fixes (buffered PCM, stereo output)
- **`src/format.h`**, **`src/collision.h`**, **`src/utils.h`**, **`src/ui.h`**, **`src/inventory.h`** - case-insensitive file
  lookup, video frames, palette handling and fixes for later games' data
- **`src/platform/bittboy/`** - the Miyoo Mini platform: SDL 1.2 video with the 180-degree panel rotation,
  MI_AO audio, controls, crash handler (`crash_handler.cc`), first-run extraction of `DATA`/`FMV` from the
  GOG CD image with a progress screen (`cdextract.h`), and the build script

## Credits

- OpenLara by Timur "XProger" Gagiev - https://github.com/XProger/OpenLara
- Miyoo Mini audio output (MI_AO) based on Steward Fu's SDL2 port - https://github.com/steward-fu/sdl2
- Miyoo Mini toolchain by Shaun Inman - https://github.com/shauninman/union-miyoomini-toolchain
- Setup screen text in DejaVu Sans Mono - https://dejavu-fonts.github.io
- Original Tomb Raider (1996) by Core Design / Eidos Interactive
- Miyoo Mini Plus port by [cacuracaptors](https://github.com/cacuracaptors)

## License

OpenLara is distributed under the BSD 2-Clause License (see [LICENSE](LICENSE)). Tomb Raider and Lara Croft
are trademarks of their respective owners; no game data is included - you need your own copy of the game.
