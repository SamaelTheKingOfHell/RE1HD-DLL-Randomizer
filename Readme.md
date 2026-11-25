# RE1HD DLL Randomizer (BhdTool Fork)

An open-source, runtime `dinput8.dll` Randomizer and practice/utility overlay for **Resident Evil HD Remaster** (Steam), forked from [eleval/BhdTool](https://github.com/eleval/BhdTool).

Instead of patching `.arc` archives on disk or writing memory from an external trainer (which breaks against Steam DRM / decrypted `.text` sections or crashes when items lack floor `.mod` meshes), this fork hooks item spawning and room triggers directly inside game memory via `dinput8.dll`.

## Randomizer Features
Supported game build for randomizer hooks: **Steam September 2026 build (`V_6699874012183562949`, `bhd.exe` SHA-256 `39b840663e4b1a6835c60cbd176e4ee6c21b9197fc2c2dd733f2a7c36a0b429a`)**. Legacy builds supported by upstream `BhdTool` retain the standard trainer/utility features.

- **Item Randomizer (153 Vanilla Pickups)**
  - Hooks room item spawning (`0x004819e0`, `0x00481830`) and pickup model instantiation (`0x004e77f0`) using a curated 153-entry pickup table (`REmakeHook/src/Tools/VanillaItemTable.h`).
  - Modes: **Off (Log Only)**, **Safe Shuffle (Keep Key Items)**, and **Full Shuffle (All Items)**.
  - **Safe 3D Floor Visual Handling**: Keeps the room's already-mounted vanilla `uItem` `.mod` mesh on the floor while replacing the pickup ID, quantity, and granted reward—eliminating `Failed open file ... i004p.mod` fatal crashes for items without standalone floor models (such as Sword Key, Serum, or Wooden Mount).
- **Bidirectional Door Randomizer**
  - `Randomize Doors (Bidirectional)` hooks `GAID_CHECK_FOR_TRIGGERS` and pairs reciprocal portals parsed from `rooms.json` into a connected 1-to-1 bidirectional involution (pairing single-door leaf rooms with multi-door hubs).
- **Character & Difficulty Awareness**
  - **Play As**: `Auto-Detect`, `Chris`, or `Jill` (automatically excludes `SmallKey` from Jill's item pool).
  - **Difficulty**: `Auto-Detect`, `Normal`, or `Hard`.
- **Pool Filtering & Modifiers**
  - **Remove from pool (replaces removed pickups with Junk: Wooden Mount, Slide Filter, X-Ray 1, X-Ray 2)**:
    - `Healing Items`
    - `Ink Ribbons`
    - `Defence Items`
  - **Swap all non-key items with Self-Defense Gun**
  - **Fuel Supply Capsule armed on pickup** (`111 -> 31`)
- **Seeds, Spoiler Log & Content Map**
  - Deterministic **Seed** input + **New Seed** generator.
  - **Export Spoiler** (`RandomizerSpoiler.txt`).
  - Live **Recent Room Spawns** log (resolved with room names from `rooms.json`) and searchable **Content Map (All 153 Pickups)**.

## Upstream BhdTool Features (Preserved)
- Runtime **Door Skip** toggle
- **Room Jump** tool (using `rooms.json`)
- **Inventory** editor
- **Save Anywhere** tool
- **Cheats** menu

## Known Limitations
- **Enemies & Locks are not randomized** (currently unhooked).
- Randomizer hooks activate only on the Steam September 2026 build (`V_6699874012183562949`).
- Randomized floor pickups reuse the vanilla pedestal/floor 3D mesh for stability; the prompt, inventory icon, and received item reflect the randomized item.

## How to Run
1. Download the latest release zip (or build `dinput8.dll` yourself).
2. Copy `dinput8.dll` and `rooms.json` into your *Resident Evil HD Remaster* installation folder next to `bhd.exe`.
3. Launch the game normally through Steam.
4. Press **`F5`** in-game to open the overlay, expand **`Randomizer (dinput8.dll)`**, configure your options/seed, and start or reload a run.

Upon first use, closing the game generates `bhdtool.ini`. To temporarily disable the hook without deleting `dinput8.dll`, set `EnableBhdTool=0` inside `bhdtool.ini`.

> Note: Keep `rooms.json` next to `dinput8.dll`. Both **Room Jump**, **Bidirectional Door Randomizer**, and **Recent Room Spawns** room-name lookup rely on it.

## How to Build
Built with **Visual Studio** (`v143` toolset, `Win32` / `x86`, `Release`). Visual Studio 2019 or newer with C++ x86 build tools is supported.

1. Clone recursively so `imgui` and `json11` submodules are initialized:
   ```sh
   git clone --recursive https://github.com/SamaelTheKingOfHell/RE1HD-DLL-Randomizer.git
   ```
2. Build `Release|x86` via Visual Studio or MSBuild:
   ```bat
   MSBuild.exe BhdTool.sln /p:Configuration=Release /p:Platform=x86
   ```
3. Output binary: `Release\dinput8.dll`.

## Credits & Attribution
- **Eleval** — Creator of [eleval/BhdTool](https://github.com/eleval/BhdTool) ([Ko-Fi](https://ko-fi.com/eleval777)).
- **Benn Powell** — Randomizer option set inspiration (character/difficulty pool controls, junk replacements, Self-Defense Gun & Fuel Capsule modifiers).
- **Third-Party Dependencies**: [Dear ImGui](https://github.com/ocornut/imgui), [json11](https://github.com/dropbox/json11), and `picosha2`.

## License
Distributed under the **GNU General Public License v3.0 (GNU GPLv3)**. See [LICENSE.txt](LICENSE.txt) for full terms.
