"""Drag and drop (wl_data_device) from a canvas window, two monitors. The
canvas draws windows away from their real position, so it picks the drag
target itself over its windows; everywhere else Hyprland's own drag handling
must keep working, motion included. Checks, with a GTK source and targets
that print what they get (tests/tools/dnd-app.py):
- a canvas window: enter, motion in the coordinates it is drawn at, leave;
- a top-layer panel above the canvas (a bar): enter, motion, leave;
- a fullscreen window on the other monitor (its canvas steps aside): enter,
  motion, drop;
- a drop on empty canvas cancels the drag; the next drag works again;
- the source going away mid-drag cancels cleanly; the compositor lives on;
- after each drag the pointer reaches the windows again (no grab left).

usage: tests/dnd-nested.py [PLUGIN.so]   (default .build/dev/spatialoverview.so)
"""
import json, os, signal, subprocess, sys, tempfile, time, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("nav", os.path.join(HERE, "navigator-nested.py"))
nav = importlib.util.module_from_spec(spec); spec.loader.exec_module(nav)
ROOT = os.path.dirname(HERE)
subprocess.run(["make", "-s", "-C", ROOT, "test-tools"], check=True)

tmp = tempfile.mkdtemp(prefix="dnd-")
logs = {name: os.path.join(tmp, f"{name}.log") for name in ("source", "target", "panel", "fullscreen")}
PANEL_H = 48
n = nav.Nested(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, ".build/dev/spatialoverview.so"), os.path.join(ROOT, ".build/shots-dnd"),
               extra_lua='\nhl.monitor({ output = "WAYLAND-1", mode = "1280x720@60", position = "0x0", scale = 1 })\n'
                         'hl.monitor({ output = "SECOND", mode = "1280x720@60", position = "1280x0", scale = 1 })\n')
failures = []


def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg, flush=True)
    if not cond:
        failures.append(msg)


def lines(name, since=0):
    if not os.path.exists(logs[name]):
        return []
    return open(logs[name]).read().splitlines()[since:]


def mark():
    return {name: len(lines(name)) for name in logs}


def events(name, m, kind):
    return [l.split() for l in lines(name, m[name]) if l.split()[0] == kind]


def point(name, m, kind):
    got = events(name, m, kind)
    return (int(got[-1][-2]), int(got[-1][-1])) if got else None


def near(p, q, tol=12):
    return p is not None and q is not None and abs(p[0] - q[0]) <= tol and abs(p[1] - q[1]) <= tol


def client(title):
    return next((c for c in n.clients() if c["title"] == title), None)


def app(name, title, extra=""):
    pre = "LD_PRELOAD=/usr/lib/libgtk4-layer-shell.so " if "--layer" in extra else ""
    n.dispatch("hl.dsp.exec_cmd(" + json.dumps(f"sh -c '{pre}python3 {HERE}/tools/dnd-app.py {title} {extra} > {logs[name]}'") + ")")
    for _ in range(100):
        if "ready" in lines(name):
            return
        time.sleep(0.1)
    raise RuntimeError(f"{title} did not start")


def pointer(*cmds):
    # One connection, so the button stays held between commands.
    return subprocess.Popen([os.path.join(ROOT, ".build/vpointer"), "2560", "720", *map(str, cmds)], env=n.env())


def path(a, b, steps=10, pause=30):
    """Small moves from a to b, so each target gets several motion events."""
    out = []
    for i in range(1, steps + 1):
        out += ["abs", round(a[0] + (b[0] - a[0]) * i / steps), round(a[1] + (b[1] - a[1]) * i / steps), "sleep", pause]
    return out


def start_drag(at):
    # GTK starts a drag after a few pixels with the button held.
    return ["abs", at[0], at[1], "sleep", 200, "down", "sleep", 150] + sum([["rel", 4, 3, "sleep", 30] for _ in range(6)], []) + ["sleep", 200]


def hovered_by(p):
    """Which app gets the pointer at p (no button held)."""
    m = mark()
    pointer("abs", p[0], p[1], "sleep", 60, "rel", 1, 0, "sleep", 60, "rel", -1, 0, "sleep", 200).wait(timeout=10)
    return [name for name in ("source", "target") if events(name, m, "hover")]


def find(name, guess):
    if name in hovered_by(guess):
        return guess
    for y in range(PANEL_H + 40, 720, 60):
        for x in range(40, 1280, 80):
            if name in hovered_by((x, y)):
                return (x, y)
    return None


try:
    n.launch()
    n.ctl("output", "create", "wayland", "SECOND")
    time.sleep(0.8)
    n.ctl("reload")
    time.sleep(1.0)
    print("monitors:", [(m["name"], m["x"], m["width"]) for m in json.loads(n.ctl("-j", "monitors"))])

    app("panel", "dnd-panel", f"--size 1280x{PANEL_H} --layer WAYLAND-1")
    n.dispatch('hl.dsp.focus({monitor="WAYLAND-1"})'); time.sleep(0.3)
    app("target", "dnd-target")
    app("source", "dnd-source")
    n.dispatch('hl.dsp.focus({monitor="SECOND"})'); time.sleep(0.3)
    app("fullscreen", "dnd-fullscreen", "--size 640x360 --fullscreen SECOND")
    for _ in range(50):
        c = client("dnd-fullscreen")
        if c and c["fullscreen"] and c["at"][0] == 1280:
            break
        time.sleep(0.1)
    time.sleep(1.0)
    fs = client("dnd-fullscreen")
    check(fs and fs["fullscreen"] != 0 and fs["at"][0] == 1280, f"the fullscreen target covers SECOND ({fs and fs['at']} {fs and fs['size']})")

    n.dispatch('hl.plugin.spatialoverview.overview("toggle all")'); time.sleep(0.8)
    n.keys("-k", "Escape"); time.sleep(0.3)
    n.keys("-k", "Escape"); time.sleep(0.8)
    n.dispatch('hl.dsp.focus({monitor="WAYLAND-1"})'); time.sleep(0.3)
    n.dispatch('hl.plugin.spatialoverview.canvas("search dnd-source")'); time.sleep(0.4)
    n.keys("-k", "Return"); time.sleep(1.2)
    n.shot("0-landed")

    src, tgt = client("dnd-source"), client("dnd-target")
    print("  real: source", src["at"], src["size"], " target", tgt["at"], tgt["size"])
    # Landing centers the source; at 100% the target is drawn relative to it.
    src_at = find("source", (640, 360))
    guess = (round(640 + tgt["at"][0] + tgt["size"][0] / 2 - src["at"][0] - src["size"][0] / 2),
             round(360 + tgt["at"][1] + tgt["size"][1] / 2 - src["at"][1] - src["size"][1] / 2))
    tgt_at = find("target", guess)
    empty = next((p for p in ((40, 700), (1240, 700), (40, PANEL_H + 40), (1240, PANEL_H + 40)) if not hovered_by(p)), None)
    print("  drawn: source", src_at, " target", tgt_at, "(guessed", guess, ")  empty canvas", empty)
    check(src_at and tgt_at and empty, "found the source, the target and some empty canvas on screen")
    if not (src_at and tgt_at and empty):
        raise SystemExit
    # Where the pointer hits the target, in its own (drawn) coordinates.
    m = mark()
    pointer("abs", tgt_at[0], tgt_at[1], "sleep", 60, "rel", 1, 0, "sleep", 60, "rel", -1, 0, "sleep", 200).wait(timeout=10)
    tgt_local = point("target", m, "hover")
    panel_at, fs_at = (tgt_at[0], PANEL_H // 2), (1920, 360)

    # -- 1. one drag over everything, dropped on the fullscreen window
    m = mark()
    pointer(*start_drag(src_at), *path(src_at, tgt_at), "sleep", 300, *path(tgt_at, panel_at), "sleep", 300,
            *path(panel_at, fs_at, 16), "sleep", 400, "up", "sleep", 800).wait(timeout=30)
    time.sleep(0.5)
    n.shot("1-dropped-fullscreen")
    print("  target:", lines("target", m["target"])[:3], "...", len(events("target", m, "dnd-motion")), "motions")
    print("  panel:", len(events("panel", m, "dnd-motion")), "motions, fullscreen:", lines("fullscreen", m["fullscreen"])[-3:])
    check(events("source", m, "drag-begin"), "[drag] the drag started from the canvas window")
    check(events("target", m, "dnd-enter") and len(events("target", m, "dnd-motion")) >= 3 and events("target", m, "dnd-leave"),
          "[canvas window] enter, motion, leave")
    motions = [(int(e[1]), int(e[2])) for e in events("target", m, "dnd-motion")]
    w, h = tgt["size"]
    check(tgt_local and any(near(p, tgt_local, 6) for p in motions) and all(0 <= x <= w and 0 <= y <= h for x, y in motions),
          f"[canvas window] drag motion is in the coordinates it is drawn at (pointer {tgt_local}, motion {motions[:4]}...)")
    check(events("panel", m, "dnd-enter") and len(events("panel", m, "dnd-motion")) >= 2 and events("panel", m, "dnd-leave"),
          "[panel] a top-layer panel above the canvas gets enter, motion, leave")
    check(near(point("panel", m, "dnd-motion"), (panel_at[0], panel_at[1]), 40), f"[panel] its motion is where the pointer is ({point('panel', m, 'dnd-motion')})")
    check(events("fullscreen", m, "dnd-enter") and len(events("fullscreen", m, "dnd-motion")) >= 3,
          "[fullscreen, other monitor] enter and motion (Hyprland's drag motion still runs)")
    check(near(point("fullscreen", m, "dnd-motion"), (640, 360), 12), f"[fullscreen] its motion is where the pointer is ({point('fullscreen', m, 'dnd-motion')})")
    check([d for d in events("fullscreen", m, "drop") if d[1] == "dnd-payload_dnd-source"], "[fullscreen] the drop lands there")
    check(not events("source", m, "drag-cancel") and events("source", m, "drag-end"), "[drag] the source sees a completed drag")

    # -- 2. dropped on empty canvas: cancelled
    m = mark()
    pointer(*start_drag(src_at), *path(src_at, tgt_at), "sleep", 200, *path(tgt_at, empty), "sleep", 300, "up", "sleep", 800).wait(timeout=30)
    time.sleep(0.5)
    check(events("source", m, "drag-cancel"), f"[cancel] a drop on empty canvas cancels the drag ({events('source', m, 'drag-cancel')})")
    check(events("target", m, "dnd-leave") and not events("target", m, "drop"), "[cancel] the window it crossed got leave, no drop")

    # -- 3. right after: a drag onto the canvas window, then the fullscreen one
    m = mark()
    pointer(*start_drag(src_at), *path(src_at, tgt_at), "sleep", 400, "up", "sleep", 800).wait(timeout=30)
    time.sleep(0.5)
    drop = events("target", m, "drop")
    check(drop and near((int(drop[-1][2]), int(drop[-1][3])), tgt_local, 40), f"[repeat] the next drag drops on the canvas window where the pointer is ({drop})")
    m = mark()
    pointer(*start_drag(src_at), *path(src_at, fs_at, 16), "sleep", 400, "up", "sleep", 800).wait(timeout=30)
    time.sleep(0.5)
    check(len(events("fullscreen", m, "dnd-motion")) >= 3 and events("fullscreen", m, "drop"), "[repeat] and again on the fullscreen window, with motion")

    # -- 4. the pointer is the apps' again
    check("target" in hovered_by(tgt_at), "[after] the canvas window gets the pointer again (no grab left behind)")

    # -- 5. the source goes away mid-drag
    m = mark()
    held = pointer(*start_drag(src_at), *path(src_at, fs_at, 16), "sleep", 1500, "up", "sleep", 500)
    time.sleep(0.2 + 0.15 + 0.18 + 0.2 + 16 * 0.035 + 0.5)
    pid = client("dnd-source")["pid"]
    os.kill(pid, signal.SIGKILL)
    held.wait(timeout=30)
    time.sleep(0.5)
    check(events("fullscreen", m, "dnd-enter") and events("fullscreen", m, "dnd-leave") and not events("fullscreen", m, "drop"),
          "[source gone] the fullscreen window got leave, no drop")
    check("target" in hovered_by(tgt_at), "[source gone] the pointer reaches the canvas again")
    check(n.proc.poll() is None, "compositor alive")
except SystemExit:
    pass
finally:
    n.stop()
print("ALL PASSED" if not failures else f"{len(failures)} FAILED")
sys.exit(1 if failures else 0)
