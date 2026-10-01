#!/usr/bin/env bash
# Remove Phantomat: unload it (windows go back to your normal layout),
# take it out of hyprland.lua, and delete the installed plugin.
#
#   scripts/uninstall.sh          keep your settings, in case you come back
#   scripts/uninstall.sh --purge  also delete settings, tuning and window memory
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$project_dir/scripts/common.sh"

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}/spatial-overview"
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/spatial-overview"
hyprland_lua="$config_dir/hyprland.lua"
marker_begin="-- >>> spatial-overview >>>"
marker_end="-- <<< spatial-overview <<<"

say() { printf '\033[1m%s\033[0m\n' "$*"; }
fail() {
  printf 'uninstall: %s\n' "$*" >&2
  exit 1
}

purge=0
for arg in "$@"; do
  case $arg in
  --purge) purge=1 ;;
  -h | --help)
    sed -n '2,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *) fail "unknown option: $arg (see --help)" ;;
  esac
done

# Unload first: that is what puts windows back into your normal layout.
if hyprland_session && spatialoverview_loaded; then
  safe_unload || fail "nothing was removed; log out and back in, then run this again."
fi

if [[ -f $hyprland_lua ]] && grep -qF -e "$marker_begin" "$hyprland_lua"; then
  cp -p "$hyprland_lua" "$hyprland_lua.bak.$(date +%s)-spatial-overview"
  python3 - "$hyprland_lua" "$marker_begin" "$marker_end" <<'PY'
import sys
path, begin, end = sys.argv[1:]
lines = open(path).read().split("\n")
out, skipping = [], False
for line in lines:
    if line.strip() == begin:
        skipping = True
        # drop the blank line the installer put before the block
        if out and out[-1].strip() == "":
            out.pop()
        continue
    if skipping:
        skipping = line.strip() != end
        continue
    out.append(line)
open(path, "w").write("\n".join(out))
PY
  say "Removed Phantomat from $hyprland_lua (backup next to it)"
elif [[ -f $hyprland_lua ]] && grep -q 'spatialoverview' "$hyprland_lua"; then
  say "Your hyprland.lua loads Phantomat in a way the installer did not write; remove those lines yourself:"
  grep -n 'spatialoverview' "$hyprland_lua" | sed 's/^/  /'
fi

rm -rf "$data_dir"
rm -f "${XDG_BIN_HOME:-$HOME/.local/bin}/omarchy-phantomat-toggle"
say "Deleted $data_dir and omarchy-phantomat-toggle"

omarchy_config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy"
if [[ -d "$omarchy_config_dir" ]] || command -v omarchy >/dev/null 2>&1; then
  omarchy_menu_ext="$omarchy_config_dir/extensions/omarchy-menu.jsonc"
  if [[ -f "$omarchy_menu_ext" ]]; then
    python3 - "$omarchy_menu_ext" <<'EOF' 2>/dev/null || true
import re, sys
path = sys.argv[1]
try:
    content = open(path, "r", encoding="utf-8").read()
    clean = re.sub(r'([ \t]*"phantomat":\s*\{[^}]*\},?\s*)', '', content)
    with open(path, "w", encoding="utf-8") as f:
        f.write(clean)
except Exception:
    pass
EOF
    omarchy menu refresh >/dev/null 2>&1 || true
    say "Removed Phantomat from $omarchy_menu_ext"
  fi

  omarchy_shell_json="$omarchy_config_dir/shell.json"
  if [[ -f "$omarchy_shell_json" ]]; then
    python3 - "$omarchy_shell_json" <<'EOF' 2>/dev/null || true
import json, sys
path = sys.argv[1]
try:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    layout = data.get("bar", {}).get("layout", {})
    modified = False
    for sec in ("left", "center", "right"):
        if sec in layout:
            items = layout[sec]
            new_items = [m for m in items if not (isinstance(m, dict) and m.get("id") == "phantomat")]
            if len(new_items) != len(items):
                layout[sec] = new_items
                modified = True
    if modified:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
except Exception:
    pass
EOF
    say "Removed Phantomat widget from $omarchy_shell_json"
  fi
fi

if ((purge)); then
  rm -f "$config_dir/spatialoverview.lua" "$config_dir/spatialoverview-tuning.lua"
  rm -rf "$state_dir"
  say "Deleted your Phantomat settings, tuning and window memory"
else
  say "Kept your settings: $config_dir/spatialoverview.lua and spatialoverview-tuning.lua (--purge deletes them)"
fi

if hyprland_session; then
  hyprctl reload >/dev/null
fi
say "Phantomat is uninstalled."
