"""Two monitors with independent cameras (canvas.linked_screens = false): a
window drawn on both at once must take clicks on either copy, X11 (XWayland)
and native Wayland alike.

usage: tests/multimonitor-nested.py [PLUGIN.so]   (default .build/dev/spatialoverview.so)
"""
import json, os, subprocess, sys, tempfile, time, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("nav", os.path.join(HERE, "navigator-nested.py"))
nav = importlib.util.module_from_spec(spec); spec.loader.exec_module(nav)
ROOT = os.path.dirname(HERE)
subprocess.run(["make", "-s", "-C", ROOT, "test-tools"], check=True)

logs = {name: tempfile.mktemp(prefix=f"{name}-", suffix=".log") for name in ("x11", "wayland")}
n = nav.Nested(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, ".build/dev/spatialoverview.so"), os.path.join(ROOT, ".build/shots-mm2"),
               extra_lua='\nhl.config({xwayland={force_zero_scaling=true}})\n'
                         # independent cameras: the same window can be drawn on both screens
                         'hl.config({plugin={spatialoverview={canvas={linked_screens=false}}}})\n'
                         'hl.monitor({ output = "WAYLAND-1", mode = "1280x720@60", position = "0x0", scale = 1 })\n'
                         'hl.monitor({ output = "SECOND", mode = "1280x720@60", position = "1280x0", scale = 1 })\n')
failures = []


def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg, flush=True)
    if not cond:
        failures.append(msg)


def mouse(*cmds):
    # buttons only; the cursor is placed with hl.dsp.cursor.move
    subprocess.run([os.path.join(ROOT, ".build/vpointer"), "1", "1", *map(str, cmds)], env=n.env(), check=True, timeout=20)


def count(name, word):
    return open(logs[name]).read().count(word) if os.path.exists(logs[name]) else 0


try:
    n.launch()
    # A second nested window as the second monitor (a headless output gets
    # no mode in a nested session).
    n.ctl("output", "create", "wayland", "SECOND")
    time.sleep(0.8)
    n.ctl("reload")
    time.sleep(1.0)
    print("monitors:", [(m["name"], m["x"], m["width"]) for m in json.loads(n.ctl("-j", "monitors"))])
    n.dispatch("hl.dsp.exec_cmd(" + json.dumps(f"sh -c '{ROOT}/.build/x11-menu x11-test > {logs['x11']}'") + ")")
    n.dispatch("hl.dsp.exec_cmd(" + json.dumps(f"sh -c 'python3 {HERE}/tools/wl-click.py wl-test > {logs['wayland']}'") + ")")
    for _ in range(100):
        titles = {c["title"] for c in n.clients()}
        if {"x11-test", "wl-test"} <= titles:
            break
        time.sleep(0.1)
    # Each monitor is its own camera on the one world; land on the same window
    # from both, so it is drawn on both monitors at once, then click each copy.
    n.dispatch('hl.plugin.spatialoverview.overview("toggle all")'); time.sleep(0.8)
    n.keys("-k", "Escape"); time.sleep(0.3)
    n.keys("-k", "Escape"); time.sleep(0.8)
    monitors = {m["name"]: m for m in json.loads(n.ctl("-j", "monitors"))}

    for name, title in (("x11", "x11-test"), ("wayland", "wl-test")):
        for monitor in ("SECOND", "WAYLAND-1"):
            n.dispatch(f'hl.dsp.focus({{monitor="{monitor}"}})'); time.sleep(0.3)
            n.dispatch(f'hl.plugin.spatialoverview.canvas("search {title}")'); time.sleep(0.4)
            n.keys("-k", "Return"); time.sleep(1.2)
        for monitor in ("WAYLAND-1", "SECOND"):
            m = monitors[monitor]
            # the window is centered on each monitor now
            cx, cy = m["x"] + m["width"] / m["scale"] / 2, m["y"] + m["height"] / m["scale"] / 2
            before = count(name, "press 1" if name == "x11" else "click")
            n.dispatch(f"hl.dsp.cursor.move({{x={cx:.0f}, y={cy:.0f}}})"); time.sleep(0.15)
            n.dispatch(f"hl.dsp.cursor.move({{x={cx + 3:.0f}, y={cy + 2:.0f}}})"); time.sleep(0.3)
            mouse("down", "sleep", 60, "up", "sleep", 400)
            check(count(name, "press 1" if name == "x11" else "click") > before, f"[{name}] a click on its copy on {monitor} reaches the window")
    os.makedirs(os.path.join(ROOT, ".build/shots-mm2"), exist_ok=True)
    subprocess.run(["grim", "-o", "WAYLAND-1", os.path.join(ROOT, ".build/shots-mm2", "WAYLAND-1.png")], env=n.env(), check=True)
    check(n.proc.poll() is None, "compositor alive")
finally:
    n.stop()
print("ALL PASSED" if not failures else f"{len(failures)} FAILED")
sys.exit(1 if failures else 0)
