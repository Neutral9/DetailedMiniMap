# Detailed MiniMap

An SKSE plugin for Skyrim Special Edition / Anniversary Edition: a detailed 3D minimap in the HUD, built from the game's own meshes.

- Real geometry of the loaded cells: ground, houses, stairs, rocks, furniture, water, roads (from the navmesh).
- Tilted view that turns with the camera (or stays north-up), square or round, in any corner of the screen at any offset.
- Replaces the game's local map (in the map menu) with a large version of the same map: drag with the mouse, zoom with the wheel.
- Cutaway: inside, what is above the character's head (ceilings, upper floors) is removed.
- Two styles: colour (all colours configurable) and vanilla (the game's local map palette and frame).
- Icons for enemies, guards, residents, followers, animals, doors, food, potions, chests, weapons, armour, loot, plants to pick, dead bodies (until you have looked into them) and quest targets (on the rim when off the map); each kind can be switched off. Merchants' hidden chests are left out.
- A beam from the character to the nearest quest target, shown and hidden by its own key.
- A key shows / hides the minimap with a fade.
- Settings menu in nine languages (Russian, English, French, Italian, German, Spanish, Polish, Chinese (traditional), Japanese); the texts are plain files in `SKSE/Plugins/DetailedMiniMap/Translations`.

## Requirements

- SKSE64
- Address Library for SKSE Plugins
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/search/?gsearch=SKSE%20Menu%20Framework) (draws the minimap and the settings menu)

## How it works

- **Geometry.** Three times a second the plugin reads the meshes of the cells the game has already loaded around the player. Most meshes are copied from the CPU-side copy the game keeps in RAM; landscape and anything without that copy are read back from GPU buffers asynchronously, without waiting for the GPU. Everything is transformed to world space and uploaded to the GPU once per cell.
- **Rendering.** A small custom D3D11 renderer draws a tilted orthographic view into a small texture (4x MSAA). The shader is simple: lighting, colour by kind (ground, objects, water), roads from the navmesh, a cutaway above the player.
- **Display.** The texture is drawn as one image in the HUD through SKSE Menu Framework, together with the icons and the frame.

Why it barely costs FPS:

- Only a cell that changed is copied again; large cells are copied in chunks of 30k triangles per frame.
- Only what falls inside the minimap is drawn; objects smaller than a pixel are skipped.
- If the player and camera are still and nothing is fading in, the frame is not redrawn: the last image is reused.
- The shaders compile on a background thread while you are still in the main menu.
- When the minimap is hidden with its key, nothing is drawn (with the local map replacement on, the loaded cells are still read, cheaply, so the map menu has them).
- A cell is rebuilt only when something new appears in it; shapes that vanish for a moment (faded out, culled) do not make it rebuild, and the landscape of a cell is never dropped by a failed read.
- No disk cache, no hooks into the game's renderer: its own texture and one image in the HUD.

## Settings

`SKSE/Plugins/DetailedMiniMap.ini` holds the defaults; the in-game menu (SKSE Menu Framework → Detailed MiniMap) saves into `DetailedMiniMap_User.ini`, which overrides it key by key.

For stutter or flicker reports: turn on the detailed log (menu → Diagnostics, or `DebugLog=1`). `DetailedMiniMap.log` then gets every 10 seconds what the map cost (update, harvest and drawing times), which cells were built again and which shapes came or went, plus the game version, Community Shaders / ENB, uGridsToLoad.

## Building

Requirements: Visual Studio 2022 (C++ desktop development), [xmake](https://xmake.io).

```
git clone --recursive https://github.com/Neutral9/DetailedMiniMap.git
cd DetailedMiniMap
xmake f -p windows -m releasedbg
xmake build
```

Build from PowerShell or a VS developer prompt (from Git Bash xmake may pick MinGW). `xmake install` copies the plugin, the ini, the translations and the icons into `%XSE_TES5_MODS_PATH%/DetailedMiniMap` (a Mod Organizer 2 mod folder).

Layout:

- `src/` – the plugin: `MapMesh` (geometry and renderer), `MiniMap` (HUD element, icons, input), `Menu`, `Settings`, `Lang`, `Icons`
- `dist/` – files shipped with the plugin: the default ini, translations, icon textures
- `extern/SKSEMenuFramework.h` – the SKSE Menu Framework API header
- `tools/MakeIcons.java` – generates the icon textures
- `lib/commonlibsse-ng` – CommonLibSSE-NG (submodule)

## License

GPL-3.0, see [LICENSE](LICENSE).

Third-party code:

- [CommonLibSSE-NG / CommonLibVR](https://github.com/alandtse/CommonLibVR) – GPL-3.0, included as a submodule.
- `extern/SKSEMenuFramework.h` – the API header of SKSE Menu Framework, included unmodified for building; it belongs to its author and is covered by that project's terms.
