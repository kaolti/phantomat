#!/usr/bin/env bash
# omarchy-phantomat-toggle: Enable / disable Phantomat canvas window manager
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." 2>/dev/null && pwd || echo "$HOME/Projects/phantomat")"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/spatial-overview"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/spatial-overview"
PLUGIN="$DATA_DIR/spatialoverview.so"
HELPER="$DATA_DIR/safe-unload.so"
DISABLED_FILE="$STATE_DIR/disabled"

mkdir -p "$DATA_DIR" "$STATE_DIR"

is_loaded() {
  hyprctl plugin list 2>/dev/null | grep -q '^Plugin spatialoverview '
}

remember_layout() {
  python3 - "$STATE_DIR/canvas-memory.tsv" <<'EOF' 2>/dev/null || true
import json, subprocess, sys, time

try:
    clients = json.loads(subprocess.check_output(["hyprctl", "clients", "-j"]))
    monitors = json.loads(subprocess.check_output(["hyprctl", "monitors", "-j"]))
    now = int(time.time())
    clean = lambda s: s.replace("\t", " ").replace("\n", " ").replace("\r", " ")

    lines = ["# spatial-overview canvas memory v1"]
    for monitor in monitors:
        owned = [c for c in clients if c.get("monitor") == monitor["id"] and c.get("mapped") and c["workspace"]["id"] > 0]
        if owned:
            c = min(owned, key=lambda c: c.get("focusHistoryID", 1 << 30))
            w, h = monitor["width"] / monitor["scale"], monitor["height"] / monitor["scale"]
            cx, cy = c["at"][0] + c["size"][0] / 2, c["at"][1] + c["size"][1] / 2
            lines.append(f"camera\t{monitor['name']}\t{cx - monitor['x'] - w / 2:.1f}\t{cy - monitor['y'] - h / 2:.1f}")
    for c in clients:
        if not c.get("mapped") or not c.get("floating") or c["workspace"]["id"] <= 0 or c.get("pinned"):
            continue
        klass = clean(c.get("class") or c.get("initialClass") or "")
        title = clean(c.get("title") or c.get("initialTitle") or klass)
        if klass:
            x, y = c["at"]
            w, h = c["size"]
            lines.append(f"window\t{klass}\t{title}\t{x:.1f}\t{y:.1f}\t{w:.1f}\t{h:.1f}\t{now}")
    open(sys.argv[1], "w").write("\n".join(lines) + "\n")
except Exception:
    pass
EOF
}

safe_unload() {
  if [[ ! -f "$HELPER" ]] && [[ -f "$project_dir/.build/safe-unload.so" ]]; then
    cp -p "$project_dir/.build/safe-unload.so" "$HELPER"
  fi
  if [[ ! -f "$HELPER" ]] && [[ -d "$project_dir" ]]; then
    make -C "$project_dir" --no-print-directory -s safe-unload 2>/dev/null || true
    [[ -f "$project_dir/.build/safe-unload.so" ]] && cp -p "$project_dir/.build/safe-unload.so" "$HELPER"
  fi

  if [[ -f "$HELPER" ]]; then
    local report="${XDG_RUNTIME_DIR:-/tmp}/spatialoverview-safe-unload.${HYPRLAND_INSTANCE_SIGNATURE:-default}"
    rm -f "$report"
    hyprctl plugin load "$HELPER" >/dev/null 2>&1 || true
    for _ in $(seq 30); do
      grep -q '^unloaded ' "$report" 2>/dev/null && break
      sleep 0.05
    done
    hyprctl plugin unload "$HELPER" >/dev/null 2>&1 || true
    rm -f "$report"
  else
    hyprctl plugin unload "$PLUGIN" >/dev/null 2>&1 || true
  fi
}

notify() {
  local title="$1"
  local msg="$2"
  if command -v omarchy-notification-send >/dev/null 2>&1; then
    omarchy-notification-send "$title" "$msg" 2>/dev/null || true
  elif command -v notify-send >/dev/null 2>&1; then
    notify-send "$title" "$msg" 2>/dev/null || true
  fi
}

do_disable() {
  if ! is_loaded; then
    touch "$DISABLED_FILE"
    return 0
  fi
  remember_layout
  touch "$DISABLED_FILE"
  safe_unload
  hyprctl reload >/dev/null 2>&1 || true
  notify "Phantomat" "Disabled (Standard Tiled Layout)"
}

do_enable() {
  rm -f "$DISABLED_FILE"
  if is_loaded; then
    return 0
  fi
  if [[ -f "$PLUGIN" ]]; then
    hyprctl plugin load "$PLUGIN" >/dev/null 2>&1 || true
    hyprctl reload >/dev/null 2>&1 || true
    hyprctl dispatch 'hl.plugin.spatialoverview.overview("on all")' >/dev/null 2>&1 || true
  fi
  notify "Phantomat" "Enabled (Infinite Canvas Active)"
}

do_status() {
  if is_loaded; then
    printf '{"text":"󰊠","tooltip":"Phantomat: Active (Infinite Canvas)\\nLeft click: disable\\nRight click: toggle HUD","class":"active"}\n'
  else
    printf '{"text":"󰊡","tooltip":"Phantomat: Disabled (Tiled Layout)\\nLeft click: enable","class":"disabled"}\n'
  fi
}

case "${1:-toggle}" in
  --status|-s|status)
    do_status
    ;;
  enable|on)
    do_enable
    ;;
  disable|off)
    do_disable
    ;;
  toggle)
    if is_loaded; then
      do_disable
    else
      do_enable
    fi
    ;;
  *)
    echo "Usage: $0 {toggle|enable|disable|--status}" >&2
    exit 1
    ;;
esac
