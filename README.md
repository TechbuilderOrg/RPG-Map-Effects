## RPG Map Effects – OBS battlemap helper

`RPG Map Effects` is an OBS Studio **frontend plugin** that adds a tool window under  
**Tools → RPG Map Effects** for running tactical battle maps with:

- **Battlemap scene picker** linked to Program output
- **Grid overlay** (preview + optional on-stream image overlay)
- **FX template scenes** (named `FX: Something`) you can spawn onto the map
- **Sequenced FX** using `[250] name`–style delays
- **Per‑template defaults** (fade, lifetime, label, sequencing)
- **Staged FX**: place, move, rotate and resize effects while they are hidden from players, then trigger them
- **Fog of war**: paint fog away (or back) per battlemap; players see solid fog, the GM sees through it

Everything lives inside this plugin; it uses only public OBS APIs and does **not** require
patching OBS core.


## Click this image (below) for video instructions
[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/U_O7mSwl27I/0.jpg)](https://www.youtube.com/watch?v=U_O7mSwl27I)

https://www.youtube.com/watch?v=U_O7mSwl27I

---
![SCR-20260305-mpfe](https://github.com/user-attachments/assets/924e8cc2-3766-4287-9349-76c99455cb3d)


## Features (current)

<img width="305" height="175" alt="SCR-20260305-mpoq" src="https://github.com/user-attachments/assets/9425cd11-d786-47fd-ad1d-342629b50261" />

- **Battlemap view**
  - Choose a battlemap scene from a dropdown; the plugin window and Program scene follow it.
  - Battlemap scenes are any scenes whose name starts with `Map:` (case‑insensitive). Only those appear in the dropdown.
  - Click on the map display to drive FX placement and selection.
  - 
<img width="446" height="48" alt="SCR-20260305-mpyt" src="https://github.com/user-attachments/assets/4f2a1d6f-36ba-4487-b8aa-25fdb3a39c4b" />

- **Grid overlay**
  - Preview-only grid (in the plugin window) with:
    - Cell size (px)
    - Line width (px)
    - Color
    - **Opacity** (applies to the preview and the on-output grid)
    - **Align**: X/Y offset, or click **Click a square corner on the map** and then click any square
      corner on your battlemap to line the grid up with the map's own squares
  - Only whole squares are drawn, every line at full width (including the edges); a strip at the right or
    bottom edge narrower than one square is left without grid. When the squares fill the canvas exactly,
    the closing line sits just inside the edge.
  - Snap to grid, the fog rectangle tool and the cell readout use the same layout.
  - Grid settings are remembered between sessions.
  - **Show grid on output**:
    - Creates/ensures an image source named `RPG Map Grid`.
    - Renders a PNG at the base canvas size and adds it as a source to the selected battlemap scene.
    - Places the grid source on top of other sources.
    - Checkbox on → grid visible; off → grid hidden (item stays in the scene).
    - 
<img width="273" height="315" alt="SCR-20260305-mqik" src="https://github.com/user-attachments/assets/004b7fba-ed1f-4fe9-be2b-efc5d3706004" />

- **FX template scenes**
  - Any scene named like `FX: Something` (case‑insensitive `fx:` prefix) is treated as an **effect template**.
  - You can pick an FX template, then:
    - Click on the map to **spawn** an instance into the current battlemap.
    - Configure:
      - **Sequence [ms]** using `[250] name` prefixes in the template to stage child items.
      - **Label text** (optional text inside the FX scene, moves with the effect; label text and color are configurable, and labels start hidden until you show them).
      - **Fade‑out** duration (ms) for a hide transition when clearing.
      - **Lifetime** (seconds, `0` = infinite) to auto‑clear.
  - Per‑template defaults (sequence, label, fade, lifetime) are saved per template UUID.

- **Active effects list**
  - Shows all currently active FX instances on the map.
  - Lets you:
    - Select FX (also via click‑nearest on the map).
    - Clear selected / last / all, with configured fade‑out where applicable.
  - Double‑click / edit labels in the list to rename instances; labels update the in‑scene text.

<img width="490" height="389" alt="SCR-20260305-mral" src="https://github.com/user-attachments/assets/7b86cdb9-7fa0-4bb0-9ebb-4daa1787824f" />

- **Set direction**
  - Right click and select  **Rotate**, then set the direction and click to complete. 
  - Useful for characters or directional effects; rotation is applied to the FX scene item.
- **Move**
  - Right-click and select **Move**, the item will be locked to the mouse. Click to release and place the item. 

-<img width="159" height="172" alt="SCR-20260305-mrxa" src="https://github.com/user-attachments/assets/7ec9264e-a90d-4a2d-870d-f9d9a5d8fd70" />

- **Staged FX (set up a scene ahead of time)**
  - Check **Stage hidden (place now, trigger later)** before spawning. The FX is added to the battlemap
    but stays hidden on Program/stream output.
  - In the plugin window, staged FX are drawn as translucent ghosts with an amber marker and are tagged
    **STAGED** in the Active Effects list. Move, Rotate, Resize, labels, Lock and Clear all work as usual.
  - Reveal them with **Trigger selected**, **Trigger all staged**, or **Trigger now** in the right‑click
    menu (map or list). Sequencing (`[250] name`) and lifetime start at trigger time, not at spawn.
  - OBS hotkeys (Settings → Hotkeys): **RPG Map Effects: Trigger all staged FX** and
    **RPG Map Effects: Trigger next staged FX** (handy for a Stream Deck). Bindings are saved with the
    scene collection.

- **Fog of war**
  - Check **Fog on this map** in the **Fog of War** box. A fog source named `RPG Fog: <map name>` is added
    to the battlemap (locked, on top of the map and FX, below the cursor) and covers the whole map.
  - Players see solid fog; the plugin window draws it see‑through (**GM view** %) so you can still read
    the map and see FX/tokens under it.
  - **Brush**: drag on the map to reveal (or **Hide** to paint fog back; hold **Alt** to flip
    temporarily). Brush radius via the spin box or **[** / **]**.
  - **Rectangle**: drag a rectangle to reveal a room; snaps to the grid when **Snap to grid** is on.
  - **Undo** (Ctrl+Z / Cmd+Z), **Clear all** (reveal everything), **Cover all**.
  - Fog color can be changed from the panel; the fog source's OBS properties also offer an optional fog
    image, output opacity and GM view opacity. The revealed area is saved in the scene collection, so it
    survives restarts. Each battlemap has its own fog.

 **Cursor overlay**
  - Right‑click on the map preview to access **Show cursor / Hide cursor** for the current battlemap.
  - When shown, a cursor image (from a toolbar **Cursor** dropdown) is added as an `image_source` named `RPG Map Cursor`, centered on the map and locked to the mouse position while the pointer is over the preview.
  - The cursor is managed **per battlemap**: each `Map:` scene can have its own cursor item. On plugin open/refresh, any existing cursors in battlemap scenes are hidden so the preview starts clean.

---

## Installing on Windows

1. Download the `rpg-map-effects-<version>-windows-x64.zip` build (from the repository's **Actions** tab:
   open the latest successful run and download the `...-windows-x64-...` artifact, or from **Releases**).
2. Close OBS.
3. Extract the zip into `C:\ProgramData\obs-studio\plugins\` so that you end up with
   `C:\ProgramData\obs-studio\plugins\rpg-map-effects\bin\64bit\rpg-map-effects.dll`.
4. Start OBS and open **Tools → RPG Map Effects**.

The plugin is built against OBS 31.1, so use OBS Studio 31.1 or newer.

### Building on Windows

Requires Visual Studio 2022 (Desktop development with C++) and CMake 3.28+. From a Developer PowerShell:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The first configure downloads OBS sources, obs-deps and Qt per `buildspec.json`. The built plugin is under
`build_x64\RelWithDebInfo`.

## Building (macOS)

This repo uses the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) for **standalone** builds. You do not need to clone OBS Studio.

### Standalone build (recommended)

1. **Clone this repo**:

   ```bash
   git clone https://github.com/adiastra/RPG-Map-Effects.git
   cd RPG-Map-Effects
   ```

2. **Configure and build** (Xcode generator; first run downloads OBS and deps per `buildspec.json`):

   ```bash
   cmake -S . -B build -G Xcode
   cmake --build build --config Release
   ```

3. **Install** the built plugin into OBS (or copy it manually):

   ```bash
   cmake --install build --config Release
   ```

   Default install prefix is `~/Library/Application Support/obs-studio/plugins`. Copy `rpg-map-effects.plugin` from there into your OBS app’s plugin folder if needed (e.g. `OBS.app/Contents/PlugIns/`). To install **inside OBS.app**, copy the whole bundle:  
   `cp -R build/Release/rpg-map-effects.plugin "/Applications/OBS.app/Contents/PlugIns/"`  
   (folder name is **PlugIns**; the plugin will use OBS’s frameworks from the app).

4. **Run OBS** and open **Tools → RPG Map Effects**.

**Architecture (macOS):** The plugin must match your OBS build: **arm64** (Apple Silicon) or **x86_64** (Intel). By default the build uses your Mac’s native arch (arm64 on M-series). If OBS reports “incompatible architecture (have 'arm64', need 'x86_64')”, you’re running Intel OBS—either switch to an arm64 OBS build or build the plugin for Intel:
   ```bash
   cmake -S . -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=x86_64
   cmake --build build --config Release
   ```

### Building inside the OBS source tree (alternative)

You can still build the plugin as part of the OBS Studio tree:

1. Clone OBS Studio (with submodules), then clone this repo into `obs-studio/plugins/rpg-map-effects`.
2. In `obs-studio/plugins/CMakeLists.txt` add: `add_obs_plugin(rpg-map-effects PLATFORMS MACOS)`.
3. Use the **in-tree** `CMakeLists.txt`: copy `CMakeLists.in-tree.txt` to `CMakeLists.txt` in this repo (see that file for details).

---

## Releases

Release artifacts are built by [GitHub Actions](https://github.com/adiastra/RPG-Map-Effects/actions). To create a release:

1. Set `version` in `buildspec.json` (e.g. `1.0.0`), commit and push to `main`.
2. Create and push a **version tag**:  
   `git tag -a 1.0.0 -m "Release 1.0.0"` then `git push origin 1.0.0`
3. After the workflow finishes, open **Releases** on GitHub, edit the **draft** release, add notes, and click **Publish release**.

See **[RELEASING.md](RELEASING.md)** for full steps, optional code signing/notarization, and troubleshooting.

**Installing from a release:** Download the latest release (macOS `.pkg` and `.tar.xz`; other platforms not built in CI) and install the plugin into OBS (e.g. on macOS, run the `.pkg`, or copy `rpg-map-effects.plugin` into `~/Library/Application Support/obs-studio/plugins` or `OBS.app/Contents/PlugIns/`).

---

## Usage overview

1. **Select your battlemap scene**
   - Use the scene dropdown at the top of the window.
   - The plugin preview switches, and OBS Program is set to that scene.

2. **Configure and show the grid**
   - Adjust cell size, line width, and color.
   - Check **Show grid** to see the preview grid in the plugin window.
   - Check **Show grid on output** to add the `RPG Map Grid` image source to your battlemap
     scene and show it on stream/recording.

3. **Create FX template scenes**
   - In OBS, make scenes named `FX: Something` and build your effect using normal sources.
   - Optional sequencing: prefix child source names with `[250]`, `[1000]`, etc. to delay their
     reveal in milliseconds.

4. **Spawn FX onto the map**
   - Choose a template in the **Template Scene** combo.
   - Configure sequence / label / fade / lifetime as desired.
   - Click on the map display and use **Spawn at last click** to drop the effect.

5. **Manage active effects**
   - Use the **Active Effects** list to select, rename, and clear effects.
   - Clear buttons support per‑effect fade‑out when configured.
   - Editing an effect’s label in the list updates the in‑scene label text (and keeps the configured label color). Labels start hidden by default; use context menus (FX list or map) to show/hide them.

6. **Stage effects ahead of time (optional)**
   - Check **Stage hidden**, spawn FX, then arrange them with Move / Rotate / Resize while players see nothing.
   - Press **Trigger all staged** (or the hotkey) when the moment comes.

7. **Use fog of war (optional)**
   - Check **Fog on this map**, pick **Brush** or **Rectangle**, and drag to reveal areas as players explore.

8. **Show or hide the cursor overlay**
   - Use the toolbar **Cursor** dropdown to choose a cursor style.
   - Right‑click on the map preview and choose **Show cursor** to add the cursor overlay to the current `Map:` scene; move the mouse over the preview to reposition it.
   - Right‑click again and choose **Hide cursor** to hide the cursor in that battlemap.

---

## Notes

- This is a **frontend plugin** that uses `obs-frontend-api.h` and Qt widgets.
- The UI is a **floating window**, not a dock, to maximize map real estate.
- All scene and source manipulation uses the public OBS API; no OBS core patches are required.

## TODO

- [x] **Reset FX size** – Be able to reset the size of an FX (in case we want it back to standard size).
- [x] **Organize FX context menu** – Organize the FX context menu logically with possible separators and maybe submenus if logical.

## License

RPG Map Effects is licensed under the GNU General Public License v2.0. See [LICENSE](LICENSE) for details.

