# Phantomat

A zoomable, infinite-canvas window manager for Hyprland. Every window lives
on one endless plane instead of in workspaces. Press a key and the view pulls
back through a curved lens so you can see everything at once, then type a few
letters and fly straight to the window you want.

The name comes from Stanisław Lem's *Summa Technologiae*, where a phantomat
is a machine that builds a whole world around the person inside it.

<!-- Demo video: drag the recording into this spot in GitHub's editor. -->

Made on [Omarchy](https://omarchy.org) with Hyprland 0.56. Built on
[hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview)
by yayuuu (see [Credits](#credits)).

## What it does

- **One canvas for everything.** Windows float freely on a single 2D plane
  instead of in workspaces. Pan, zoom and place windows anywhere.
- **Your screens are one desk.** Side by side on the canvas as they stand on
  your desk, moving together: a window can sit across the seam, and dragging
  it from one screen to the other is one continuous move.
- **Type to find.** `SUPER + CTRL + G` zooms out with a search bar already
  listening. Type part of a title or app name and the camera glides to the
  best match; `Enter` lands on it at full size, `Esc` takes you back.
- **Keyboard first.** Jump to the nearest window in any direction, nudge
  windows around, bring a window to where you are, frame one or fit them all,
  tidy everything up by app, undo and redo.
- **Recent windows.** `ALT + TAB` flips between windows most recently used
  first; hold `ALT` to see the list.
- **Fill or go fullscreen.** `SUPER + T` makes a window fill its screen and puts
  it back. Fullscreen (`SUPER + F`, a video, a game) takes over just the screen
  the window is on; the other screens keep the canvas.
- **All your apps.** Wayland and X11 apps alike, games under Wine and Proton
  included: menus, drags, fullscreen and full frame rate work as on a normal
  desktop.
- **It remembers.** Windows return to their spots after a restart.
- **Tune it live.** `CTRL + ,` in the zoomed-out view opens a tuner for the
  lens, blur, grid, parallax, HUD size, type, colors and more, and you see
  each change as you make it.
- **Looks the part.** A barrel lens with optional edge blur, vignette and
  color fringe, a parallax wallpaper, a dotted grid, a minimap and a
  monospace HUD in the accent color of your choice.

## Install

You need Hyprland 0.56 or newer with its Lua config (`~/.config/hypr/hyprland.lua`,
which Omarchy uses), GCC 15 or newer, and Hyprland's development files. On
Arch or Omarchy:

```sh
sudo pacman -S --needed base-devel git hyprland hyprgraphics pango lua
git clone https://github.com/kaolti/phantomat.git
cd phantomat
scripts/install.sh
```

The installer builds the plugin, puts it in `~/.local/share/spatial-overview/`,
copies the default settings to `~/.config/hypr/spatialoverview.lua` (it never
overwrites yours), adds two marked lines to your `hyprland.lua` (after making a
backup), and loads it right away. Then press `SUPER + CTRL + G`.

Phantomat used to be called Spatial Overview, and inside it still is: the
settings file, the `hl.plugin.spatialoverview` actions and the
`hyprctl spatialoverview` command keep that name, so configs from before the
rename keep working.

**After a Hyprland update**, the plugin has to be rebuilt for the new version.
Hyprland shows a notification when that is the case; run the installer again:

```sh
cd phantomat && git pull && scripts/install.sh
```

Updating never touches your `~/.config/hypr/spatialoverview.lua`, so keys added
in a new version are not in it yet: compare it with
[examples/spatialoverview.lua](examples/spatialoverview.lua) and copy what you
want (or move yours aside and run the installer for a fresh one).

**To uninstall**, run `scripts/uninstall.sh`. Windows go back to your normal
layout, the lines come out of `hyprland.lua`, and the plugin is deleted. Your
settings stay, in case you come back; `scripts/uninstall.sh --purge` deletes
them too.

## Keys

### Anywhere

| Keys | Action |
| --- | --- |
| `SUPER + CTRL + G` | Zoom out to the canvas (and back). |
| `ALT + TAB`, `ALT + SHIFT + TAB` | Recent windows. A tap flips to the previous one; hold `ALT` for the list, release to go. |
| `SUPER` + arrows | Focus the nearest window in that direction; the camera follows. |
| `SUPER + SHIFT` + arrows | Nudge the focused window one grid step; hold to keep moving. |
| `SUPER + T`, `SUPER + ALT + F` | Make the focused window fill its screen, with the usual gaps; again puts it back. |
| `SUPER + F`, or an app going fullscreen | Fullscreen on the screen the window is on; the other screens keep the canvas. `SUPER + CTRL + G` takes the screen back, going back to the window makes it fullscreen again. |
| `SUPER + O` | Pin the window to the screen: it stays put while the canvas moves; again puts it back on the canvas. |
| `SUPER + 1` … `0`, `SUPER + TAB` and the other workspace keys | Nothing, on the canvas (with `canvas.places`, experimental: places on the canvas). |
| `SUPER + J`, `P`, `L`, `Home`, `G`, `SHIFT + ALT + SUPER` + arrows | Tiling and grouping keys: nothing, on the canvas (every window floats). |
| Middle-drag | Pan the canvas. |
| `CTRL` + wheel, pinch | Zoom. |
| `SUPER` + left-drag, right-drag | Move, resize a window. |

### In the zoomed-out canvas

| Keys | Action |
| --- | --- |
| type | Search window titles and app names. Every word must appear as typed, letters together (`fas` finds `fastfetch`); case and accents are ignored. The camera and focus follow the best match. |
| `↑` `↓`, `Tab` `SHIFT + Tab` | Move through the results. With no search, `Tab` walks recent windows. |
| `CTRL + 1` … `CTRL + 9` | Go straight to that result. |
| `Enter` | Go to the selected window at full size. |
| `SHIFT + Enter` | Bring the selected window to where you are, then go there. |
| `Esc` | Clear the search, then go back to where you started. |
| `←` `→` `↑` `↓` | Select the nearest window in that direction. |
| `SHIFT` + arrows | Nudge the selected window. |
| `CTRL` + arrows | Pan. |
| `CTRL + =`, `CTRL + -` | Zoom in, out. |
| `CTRL + 0` | Fit every window (every match while searching). |
| `CTRL + F` | Frame the selected window. |
| `CTRL + A` | Tidy all windows up on the grid, by app. |
| `CTRL + Z`, `CTRL + SHIFT + Z` | Undo, redo moves and tidying. |
| `CTRL + ,` | Tune the look. |
| `F1` | Every key, on screen. |

With the mouse: click a window to go to it, drag a window to move it, drag the
empty canvas to pan, scroll to zoom, click the minimap to jump.

## Tuning

`CTRL + ,` in the zoomed-out canvas turns the search bar into a list of
settings. Type to filter (`blur`, `grid dot`, `accent`), `↑` `↓` to pick, `←`
`→` to adjust while you watch (`SHIFT` for fine steps, `CTRL` for big ones),
`Delete` to undo a change, `Esc` when done. The footer explains the selected
setting.

Changes are saved as you make them to `~/.config/hypr/spatialoverview-tuning.lua`,
which is applied after `spatialoverview.lua`, so a tuned value wins until you
delete its line there.

Everything can also be set in `~/.config/hypr/spatialoverview.lua` (then
`hyprctl reload`). The main settings, all under `plugin.spatialoverview`:

| Setting | Effect |
| --- | --- |
| `canvas.desktop_mode` | Enable the shared infinite-window desktop |
| `canvas.linked_screens` | Screens show adjacent parts of the canvas and move together (default); off: each screen is its own camera |
| `canvas.places` | Experimental, off by default: the workspace keys go to places on the canvas and take windows there |
| `canvas.initial_zoom` | Camera zoom when desktop mode opens |
| `canvas.min_zoom` / `max_zoom` | Continuous camera zoom limits |
| `canvas.zoom_step` | Ctrl-wheel zoom strength |
| `canvas.space_pan` | Optional Space + left-drag panning; disabled by default so Space always reaches applications |
| `canvas.direct_input` | Forward input into transformed Wayland windows |
| `canvas.hover_focus` | Activate a transformed window when the pointer enters it |
| `canvas.persistent` | Keep the canvas renderer active at 100% zoom instead of returning to workspace rendering |
| `canvas.minimap_*` | Configure the undistorted navigation-mode minimap size, margin, and opacity |
| `canvas.arrange_context_grouping` | Prefer shared local title/app context before falling back to app categories |
| `canvas.arrange_size_similarity` | Relative threshold for treating window sizes as equivalent |
| `canvas.arrange_resize_limit` | Maximum relative resize applied to any window by smart arrangement |
| `canvas.auto_float` | Detach existing and new app windows from tiling |
| `canvas.auto_place` | Place newly managed windows near the active camera |
| `canvas.placement_gap` | Collision gap used by automatic placement |
| `input.pan_sensitivity` | Middle-drag camera sensitivity (and optional Space-drag sensitivity) |
| `input.drag_threshold` | Pixels before a click becomes a drag |
| `animation.enabled` | Enables overview camera animation |
| `animation.speed` | Camera animation speed |
| `animation.bezier` | Name of the Hyprland curve used for camera motion |
| `distortion.enabled` | The curved lens at all |
| `distortion.strength` | Signed lens curvature; `0` is flat |
| `distortion.shader_path` | A lens shader of your own; empty uses the one built into the plugin |
| `distortion.edge_scale` | Overscan that keeps curved corners filled |
| `distortion.feather` | Width of the soft screen-edge fade |
| `distortion.transition_power` | When the lens eases in during the zoom animation (1 follows the zoom) |
| `canvas.grid_*` | Shared snap/render spacing, width, opacity, line/dot style, and dot diameter |
| `canvas.background_dim` | Dark overlay behind the grid and windows in navigation mode |
| `canvas.snap_enabled` / `snap_size` | Snap free window placement to the canvas grid, and its step in pixels |
| `canvas.remember_layout` | Remember window positions and cameras across restarts and reopenings |
| `navigator.enabled` | Type-to-search palette in the zoomed-out canvas |
| `navigator.labels` | Window titles on the map |
| `navigator.dim_unmatched` | How far windows that do not match the search recede |
| `navigator.pointer` | Click lands, drag moves or pans, wheel zooms; off keeps direct app input while zoomed out |
| `navigator.accent` | HUD accent for the selected row, caret, key names and outlines: `#rrggbb`, a preset name (`orange`, `amber`, `yellow`, `lime`, `green`, `mint`, `teal`, `cyan`, `sky`, `blue`, `indigo`, `violet`, `purple`, `magenta`, `pink`, `red`, `white`), or `auto` to follow the active border |
| `navigator.mono_font` | The HUD's typeface (monospace, set in uppercase) |
| `wallpaper` / `blur` / `blur_strength` | Sharp-at-rest canvas backdrop and navigation-mode blur blend |
| `distortion.edge_blur` / `edge_blur_start` | Lens blur toward the screen edges, and where it begins (0 center, 1 corners) |
| `distortion.vignette` | Darkening toward the edges |
| `distortion.chromatic` | Red/blue fringe toward the edges |
| `parallax.enabled` / `strength` | The wallpaper follows the camera at this fraction of the windows' speed |
| `parallax.depth` | How much the wallpaper shrinks as you zoom out (0 keeps its size) |
| `parallax.desktop` | Also apply parallax when panning the full-size desktop |
| `navigator.hud_scale` | Size of the whole HUD |
| `navigator.width` / `top` / `rows` / `row_height` / `rounding` | Palette geometry |
| `navigator.panel_opacity` | Palette background opacity (lower is glassier) |
| `navigator.uppercase` / `letter_spacing` | Capitals or text as written, and tracking |
| `navigator.query_size` / `title_size` / `detail_size` / `corner_size` / `label_size` | Type sizes |
| `navigator.corner_labels` | Readouts in the screen corners |
| `navigator.search_height` / `search_border` / `search_border_opacity` / `search_border_color` / `search_glow` | The search field |
| `navigator.tuning_file` | Where the live tuner saves (default `$XDG_CONFIG_HOME/hypr/spatialoverview-tuning.lua`) |

`navigator.accent` takes `#rrggbb`, a preset name (`orange`, `amber`, `yellow`,
`lime`, `green`, `mint`, `teal`, `cyan`, `sky`, `blue`, `indigo`, `violet`,
`purple`, `magenta`, `pink`, `red`, `white`), or `auto` for your theme's border
color.

For scripts and bindings, `hl.plugin.spatialoverview.canvas(...)` takes
`search [text]`, `tune`, `fill`, `pin`, `go <place>|next|prev|back`,
`send <place> [stay]`, `switch next|prev`, `fit`, `summon`, `zoom in|out`,
`pan <dir>`, `nudge <dir>`, `undo`, `redo`, `arrange`, `frame`, `land`, `back`,
`noop` (for keys that do nothing on the canvas) and `refresh`.
`hyprctl spatialoverview` prints the canvases' state as JSON.

How each key behaves on the canvas, and what the tests check, is in
[docs/window-rules.md](docs/window-rules.md).

## Omarchy Integration

Phantomat includes a toggle script and integration snippets for Omarchy's status bar and application menu:

- **Quick toggle CLI**: `omarchy-phantomat-toggle` enables or disables Phantomat in real time, saving canvas window memory and restoring standard tiled layouts.
- **Status bar glyph**: add the snippet in [`examples/omarchy-bar-module.json`](examples/omarchy-bar-module.json) to `~/.config/omarchy/shell.json` to get a one-click toggle glyph on your bar (`󰊠` active / `󰊡` disabled).
- **`SUPER + Space` menu**: add the entry in [`examples/omarchy-menu.jsonc`](examples/omarchy-menu.jsonc) to `~/.config/omarchy/extensions/omarchy-menu.jsonc` to toggle Phantomat from the launcher with live active status checkmarks.

## Troubleshooting

- **"this build is for a different Hyprland version"**: Hyprland was updated.
  Run `scripts/install.sh` again.
- **Nothing happens on `SUPER + CTRL + G`**: check `hyprctl plugin list` and
  `hyprctl configerrors`. If you load the plugin in your own way, make sure
  `spatialoverview.lua` is loaded after it.
- **Something broke**: `scripts/uninstall.sh` puts your desktop back as it was.
  Please open an issue with your Hyprland version (`hyprctl version`).

This hooks deep into Hyprland internals, so a Hyprland update can break it
until it is updated too. It has been used daily on one setup (Omarchy,
Hyprland 0.56.2, NVIDIA, two monitors); other setups are less tested.

## Development

`make` builds `spatialoverview.so` in the checkout; `scripts/install-live.sh`
builds it and swaps it into the running session, keeping windows where they
are. The tests run a nested Hyprland in a window (`tests/*-nested.py`);
`make test-tools` builds the virtual mouse some of them use.

## Credits

Phantomat began as a fork of
[hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview)
by yayuuu (Daniel Skorupa) and its contributors, which grew out of the plugin
work of Vaxry and the Hypr Development team. Their code and history are
kept in this repository. Neither project endorses this one.

## License

BSD 3-Clause; see [LICENSE](LICENSE).
