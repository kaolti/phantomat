#!/usr/bin/env bash
# Build Phantomat, install it, and load it into Hyprland.
#
#   scripts/install.sh            build, install, hook into hyprland.lua, load now
#   scripts/install.sh --no-load  the same, but load it only at the next login
#
# Run it again after a Hyprland update: it rebuilds against the new version
# and swaps the running copy, keeping every window where it is. Your settings
# (spatialoverview.lua and spatialoverview-tuning.lua) are never overwritten.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$project_dir/scripts/common.sh"

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/spatial-overview"
plugin="$data_dir/spatialoverview.so"
hyprland_lua="$config_dir/hyprland.lua"
settings="$config_dir/spatialoverview.lua"
marker_begin="-- >>> spatial-overview >>>"
marker_end="-- <<< spatial-overview <<<"

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() {
  printf 'install: %s\n' "$*" >&2
  exit 1
}

load_now=1
for arg in "$@"; do
  case $arg in
  --no-load) load_now=0 ;;
  -h | --help)
    sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *) fail "unknown option: $arg (see --help)" ;;
  esac
done

# ---- requirements -------------------------------------------------------------

[[ -f $hyprland_lua ]] || fail "$hyprland_lua not found. Phantomat needs Hyprland 0.56 or newer with its Lua config (Omarchy uses it)."

for tool in make pkg-config g++ python3; do
  command -v "$tool" >/dev/null || fail "$tool is not installed."
done
(($(g++ -dumpversion | cut -d. -f1) >= 15)) || fail "g++ 15 or newer is needed (found $(g++ -dumpversion))."

missing=()
for pkg in hyprland hyprgraphics pangocairo pixman-1 libdrm libinput libudev wayland-server xkbcommon; do
  pkg-config --exists "$pkg" || missing+=("$pkg")
done
pkg-config --exists 'lua5.4 >= 5.4' || pkg-config --exists 'lua >= 5.4' || missing+=("lua 5.4")
((${#missing[@]} == 0)) || fail "missing development files for: ${missing[*]}. On Arch: sudo pacman -S --needed base-devel hyprland hyprgraphics pango lua"

headers=$(pkg-config --modversion hyprland)
if hyprland_session; then
  running=$(hyprctl version -j | python3 -c 'import json, sys; print(json.load(sys.stdin).get("version", ""))')
  [[ -z $running || $running == "$headers" ]] ||
    fail "Hyprland $running is running, but the installed headers are for $headers. Log out and back in after updating, then run this again."
fi

# ---- build and install ----------------------------------------------------------

say "Building Phantomat for Hyprland $headers"
make -C "$project_dir" -j"$(nproc)" all
make -C "$project_dir" --no-print-directory -s safe-unload
mkdir -p "$data_dir" "${XDG_BIN_HOME:-$HOME/.local/bin}"
install -m 0755 "$project_dir/spatialoverview.so" "$plugin.next"
mv -f "$plugin.next" "$plugin"
say "Installed $plugin"
install -m 0755 "$project_dir/.build/safe-unload.so" "$data_dir/safe-unload.so"
install -m 0755 "$project_dir/scripts/omarchy-phantomat-toggle.sh" "${XDG_BIN_HOME:-$HOME/.local/bin}/omarchy-phantomat-toggle"
say "Installed omarchy-phantomat-toggle"

if [[ -e $settings ]]; then
  say "Keeping your settings in $settings"
else
  install -m 0644 "$project_dir/examples/spatialoverview.lua" "$settings"
  say "Settings: $settings"
fi

hooked=1
if grep -qF -e "$marker_begin" "$hyprland_lua"; then
  :
elif grep -q 'spatialoverview\.so' "$hyprland_lua"; then
  # Somebody loads a build of their own; leave that alone.
  say "Your hyprland.lua already loads a spatialoverview.so; it is left as it is."
  hooked=0
else
  cp -p "$hyprland_lua" "$hyprland_lua.bak.$(date +%s)-spatial-overview"
  cat >>"$hyprland_lua" <<EOF

$marker_begin
-- Phantomat: added by its scripts/install.sh; scripts/uninstall.sh removes it.
do
  local disabled_file = io.open(os.getenv("HOME") .. "/.local/state/spatial-overview/disabled", "r")
  if disabled_file then
    disabled_file:close()
  else
    hl.plugin.load("$plugin")
    dofile("$settings")
  end
end
$marker_end
EOF
  say "Added Phantomat to $hyprland_lua (backup next to it)"
fi

# ---- Omarchy integration --------------------------------------------------------

omarchy_config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy"
if [[ -d "$omarchy_config_dir" ]] || command -v omarchy >/dev/null 2>&1; then
  # 1. Omarchy Super+Space menu extension
  omarchy_menu_ext="$omarchy_config_dir/extensions/omarchy-menu.jsonc"
  mkdir -p "$(dirname "$omarchy_menu_ext")"
  python3 - "$omarchy_menu_ext" <<'EOF' 2>/dev/null || true
import os, sys

path = sys.argv[1]
entry = '''  "phantomat": {
    "icon": "󰊠",
    "label": "Phantomat",
    "description": "Switch between Phantomat canvas and standard tiled layout",
    "action": "omarchy-phantomat-toggle",
    "checked": "hyprctl plugin list 2>/dev/null | grep -q \'^Plugin spatialoverview \'"
  },
'''
content = ""
if os.path.exists(path):
    try:
        content = open(path, "r", encoding="utf-8").read()
    except Exception:
        content = ""

if '"phantomat"' not in content:
    if not content.strip():
        content = "{\n" + entry + "}\n"
    else:
        brace = content.find('{')
        if brace != -1:
            content = content[:brace+1] + "\n" + entry + content[brace+1:]
        else:
            content = "{\n" + entry + content + "\n}\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
EOF
  say "Configured Omarchy menu entry in $omarchy_menu_ext"
  omarchy menu refresh >/dev/null 2>&1 || true

  # 2. Omarchy status bar glyph
  omarchy_shell_json="$omarchy_config_dir/shell.json"
  if [[ -f "$omarchy_shell_json" ]] || [[ -f "/usr/share/omarchy/config/omarchy/shell.json" ]]; then
    if [[ ! -f "$omarchy_shell_json" ]]; then
      mkdir -p "$omarchy_config_dir"
      cp -p "/usr/share/omarchy/config/omarchy/shell.json" "$omarchy_shell_json" 2>/dev/null || true
    fi
    if [[ -f "$omarchy_shell_json" ]]; then
      python3 - "$omarchy_shell_json" <<'EOF' 2>/dev/null || true
import json, sys

path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    bar = data.get("bar", {})
    layout = bar.get("layout", {})
    left = layout.get("left", [])
    all_ids = [m.get("id") for sec in layout.values() if isinstance(sec, list) for m in sec if isinstance(m, dict)]
    if "phantomat" not in all_ids:
        widget = {
            "id": "phantomat",
            "type": "command",
            "exec": "omarchy-phantomat-toggle --status",
            "interval": 1,
            "onClick": "omarchy-phantomat-toggle",
            "onRightClick": "hyprctl dispatch spatialoverview:overview toggle"
        }
        idx = -1
        for i, m in enumerate(left):
            if isinstance(m, dict) and m.get("id") in ("omarchy.workspaces", "workspaces"):
                idx = i
                break
        if idx >= 0:
            left.insert(idx + 1, widget)
        else:
            left.append(widget)
        layout["left"] = left
        bar["layout"] = layout
        data["bar"] = bar
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
except Exception:
    pass
EOF
      say "Configured Omarchy status bar glyph in $omarchy_shell_json"
    fi
  fi
fi

# ---- load it --------------------------------------------------------------------

if ((!load_now)) || ! hyprland_session; then
  say "Done. Phantomat loads at your next login."
  exit 0
fi
if ((!hooked)); then
  say "Done. Reload your own setup to use the new build."
  exit 0
fi

was_running=0
if spatialoverview_loaded; then
  was_running=1
  remember_canvas_layout
  safe_unload || fail "the new build is installed and loads at your next login."
  hyprctl plugin load "$plugin" >/dev/null
fi
hyprctl reload >/dev/null
sleep 0.5

if ! spatialoverview_loaded; then
  fail "Hyprland did not load the plugin; its notification says why. Nothing else was changed: remove it again with scripts/uninstall.sh."
fi
config_errors=$(hyprctl configerrors)
if [[ -n $config_errors ]]; then
  printf 'Hyprland reports config errors:\n%s\n' "$config_errors" >&2
  exit 1
fi

# An update brings the canvas straight back, windows where they were.
((was_running)) && hyprctl dispatch 'hl.plugin.spatialoverview.overview("on all")' >/dev/null

say "Phantomat is running. Press SUPER + CTRL + G, type to find a window, Enter to go there."
say "In the zoomed-out canvas, CTRL + , tunes the look and F1 lists every key."
