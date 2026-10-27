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
- Game Switcher and power-off saves: the game continues where you left it, with its screenshot in the Game Switcher
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
| Menu | Game Switcher / OnionOS menu (see below) |

To save, open the passport in the inventory and choose "Save Game?" (its "Current Position" is on the Load Game page). The quick save
(Menu + R1) has its own slot: it is loaded only with Menu + L1 and does not show up on the Load Game page, so
the two never overwrite each other. The buttons can be changed in the controls options (the gamepad in the
inventory); the menus always use Y to select and A to go back.

### Menu button, Game Switcher and power off

The Menu button does what it is set to do in OnionOS (the Menu button options in the Tweaks app): by default,
a tap opens the Game Switcher and holding it goes back to the OnionOS menu; a double tap can switch to the
previous game. Leaving the game this way, or turning the console off with the power button, saves your
position in a slot of its own, and the next time the game opens (from the Game Switcher, or by itself when the
console is turned on again) it continues from there; hold Menu while turning the console on to skip that, as
with any OnionOS game. On the title screen, in the menus and during cutscenes and videos there is nothing to
save, so the game starts again from the title. Menu pressed together with another button (Menu + R1, Menu + L1,
the OnionOS Menu + Power screenshot) does none of this. The game can also be closed from the passport: "Exit
to Title" and then "Exit Game".

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

- **v1.2.0** - OnionOS integration: the Menu button works as set for it in OnionOS (Game Switcher, back to the menu, previous game), the Game Switcher shows the game's screenshot and the game continues where it was left, and turning the console off saves the position and continues from it when the console is turned on again. Wall textures no longer jump as they pass the edges of the screen (faces that are not flat are always drawn as two triangles), and the lighting no longer pops there either. Smoother motion: the game steps with the panel's real refresh rate (just under 60 Hz), measured while it runs. Loading a save in the drained channel of the Lost Valley rapids no longer leaves Lara stuck turning in place (the flipped rooms are set before Lara, as in the original). The game no longer freezes when opened from the Game Switcher, and buttons pressed before it opened no longer stay held. At start the screen is set to 640x480 with vsync whatever mode the previous program left, and a change of the system time no longer freezes the game.
- **v1.1.0** - Textures no longer bend (warp) when the camera is right up against a wall or floor: where the perspective steps would be off by more than half a texel, they are taken more often; everywhere else the image is the same as before, at almost the same speed.
- **v1.0.9** - Dynamic resolution: only in the heaviest moments, the 3D is drawn a step smaller (576x432 or 512x384) and the chip's 2D engine scales it back to 640x480, smoothing it, to keep the frame rate; it goes back to 640x480 as soon as there is room again, and menus, the inventory and videos always stay at 640x480. Plants and rocks no longer lose parts as the camera turns: like in the original game, a room seen through a doorway is drawn whole, not cut to the doorway (only rooms that share space with another one still are), and a plant or rock that leans into the next room is drawn with that room too, so it no longer vanishes when only that room is in view (as TRX does). The see-through parts of plants and fences cost less to draw (the same image).
- **v1.0.8** - Faster drawing, about 25% over v1.0.7 with the same look: quads are drawn whole as in the original game, faces hidden behind what is already on screen are skipped, and the pixel loop is shorter; most noticeable in large open areas and underwater. The waterfall splashes (and the other effects: smoke, blood, sparks, bubbles) no longer flicker between dark and tinted: they are drawn at the original game's neutral brightness.
- **v1.0.7** - Lara no longer sticks to the sides of the Lost Valley rapids: like in the original, the current slides her along the banks all the way to the waterfall. Natla's Mines: grabbing a crate no longer throws Lara inside it (she took the crate behind the one she faced), so the crates that open the way to the mine cart lever can be moved again; reaching for crates, levers and items is reliable everywhere.
- **v1.0.6** - Enemies find their way to Lara like in the original: the bear in the caves comes out to attack, and wolves and other animals chase her properly instead of running in circles. Swimming no longer snags on the bottom. The Lost Valley rapids carry Lara as in the original: along the surface, where she can breathe, without getting stuck in mid-water or thrown onto the rocks.
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
- **`src/savegame.h`**, **`src/game.h`** - separate save slots for the quick save and the passport's Save Game,
  and the hidden slot the game continues from after the Game Switcher or a power off
- **`src/lara.h`** - after a save is loaded, Lara's state (on land, swimming) follows the flipped rooms, as in
  the original
- **`src/lang.h`** - the help screen text and the gamepad button names of the Miyoo Mini Plus
- **`src/gameflow.h`** - TR1 music mapped onto the 9 PC CD tracks, GOG file names accepted
- **`src/sound.h`** - MP3/OGG decoder fixes (buffered PCM, stereo output)
- **`src/format.h`**, **`src/collision.h`**, **`src/utils.h`**, **`src/ui.h`**, **`src/inventory.h`** - case-insensitive file
  lookup, video frames, palette handling and fixes for later games' data
- **`src/platform/bittboy/`** - the Miyoo Mini platform: SDL 1.2 video with the 180-degree panel rotation,
  MI_AO audio, controls, crash handler (`crash_handler.cc`), first-run extraction of `DATA`/`FMV` from the
  GOG CD image with a progress screen (`cdextract.h`), the OnionOS integration (`onion.h`: the Menu button
  actions, the Game Switcher image, the save at power off) and the build script

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
