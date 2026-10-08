#!/usr/bin/env python3
"""Drive the navigator in a disposable nested Hyprland.

Usage: tests/navigator-nested.py [--plugin PATH] [--keep] [--shots DIR]

Launches a nested compositor with the given plugin build, opens labelled
test windows, then exercises the navigator through the plugin's own
dispatchers and a virtual keyboard (wtype). Screenshots of each step land in
--shots. Only the child compositor is ever addressed.
"""
import argparse, json, os, shlex, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WINDOWS = [
    ("firefox", "Hacker News — Mozilla Firefox"),
    ("slack", "general | Omarchy crew — Slack"),
    ("code", "scrollOverview.cpp — spatial-overview — Visual Studio Code"),
    ("foot", "user@arch: ~/spatial-overview"),
    ("obsidian", "Napi jegyzet — értesítő — Obsidian"),
    ("spotify", "Spotify Premium"),
    ("foot", "btop"),
]


class Nested:
    def __init__(self, plugin, shots, state=None, extra="", extra_lua="", test_binds=True):
        self.plugin, self.shots = Path(plugin).resolve(), Path(shots)
        self.proc = self.sig = self.socket = None
        self.tmp = tempfile.TemporaryDirectory(prefix="navigator-")
        # Never let a test read or write the real ~/.local/state files.
        self.state = state or str(Path(self.tmp.name) / "state")
        self.extra = extra
        self.extra_lua = extra_lua
        self.test_binds = test_binds

    def ctl(self, *args, check=True):
        r = subprocess.run(["hyprctl", "-i", self.sig, *args], capture_output=True, text=True, timeout=8)
        if check and (r.returncode or r.stdout.startswith("error")):
            raise RuntimeError(f"hyprctl {' '.join(args)}: {r.stdout.strip()} {r.stderr.strip()}")
        return r.stdout.strip()

    def dispatch(self, expr):
        return self.ctl("dispatch", expr)

    def env(self):
        e = os.environ.copy()
        e["WAYLAND_DISPLAY"] = self.socket
        return e

    def keys(self, *args):
        subprocess.run(["wtype", *args], env=self.env(), check=True, timeout=10)
        time.sleep(0.35)

    def shot(self, name):
        self.shots.mkdir(parents=True, exist_ok=True)
        path = self.shots / f"{name}.png"
        for attempt in range(3):
            r = subprocess.run(["grim", str(path)], env=self.env(), capture_output=True, text=True, timeout=10)
            if r.returncode == 0:
                return path
            print(f"grim failed ({r.stderr.strip()}), clients={len(self.clients())}, alive={self.proc.poll() is None}", flush=True)
            time.sleep(0.5)
        return path

    def clients(self):
        return json.loads(self.ctl("-j", "clients"))

    def active(self):
        return json.loads(self.ctl("-j", "activewindow"))

    def launch(self):
        cfg = (ROOT / "tests/nested.lua").read_text()
        cfg = cfg.replace("@PLUGIN@", str(self.plugin))
        cfg = cfg.replace('mode = "1280x720@60",', f'mode = "{os.environ.get("NESTED_MODE", "1280x720@60")}",')
        cfg = cfg.replace("scale = 1,", f"scale = {os.environ.get('NESTED_SCALE', '1')},", 1)
        cfg += "\nif plugin_loaded then\n hl.config({plugin={spatialoverview={canvas={persistent=true,hover_focus=true,space_pan=false,initial_zoom=0.5,grid_size=46,background_dim=0.6,grid_opacity=0.3" + self.extra + "},distortion={strength=0.24}}}})\n"
        if self.test_binds:
            cfg += ' hl.bind("SUPER + CTRL + G", hl.plugin.spatialoverview.overview("toggle all"))\n'
            cfg += ' hl.bind("ALT + TAB", hl.plugin.spatialoverview.canvas("switch next"), { repeating = true })\n'
            cfg += ' hl.bind("ALT + SHIFT + TAB", hl.plugin.spatialoverview.canvas("switch prev"), { repeating = true })\n'
        cfg += 'end\n'
        cfg += self.extra_lua
        path = Path(self.tmp.name) / "nested.lua"
        path.write_text(cfg)
        env = os.environ.copy()
        env["AQ_BACKEND"] = "wayland"
        env["XDG_STATE_HOME"] = self.state
        self.log = open(Path(self.tmp.name) / "hyprland.log", "w")
        self.proc = subprocess.Popen(["Hyprland", "--config", str(path)], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(80):
            if self.proc.poll() is not None:
                raise RuntimeError("nested compositor exited; see " + self.log.name)
            try:
                inst = json.loads(subprocess.check_output(["hyprctl", "instances", "-j"], text=True, timeout=3))
                m = next((x for x in inst if x["pid"] == self.proc.pid), None)
                if m:
                    self.sig, self.socket = m["instance"], m["wl_socket"]
                    break
            except (subprocess.SubprocessError, json.JSONDecodeError):
                pass
            time.sleep(0.1)
        if not self.sig:
            raise RuntimeError("nested compositor did not register")
        # A nested compositor is itself a window in the host. A tiled host can
        # resize it below the requested mode, invalidating the popup/pixel tests.
        # Only float/resize this test process's host window, never user windows.
        host_sig = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
        if host_sig and host_sig != self.sig:
            for _ in range(50):
                result = subprocess.run(["hyprctl", "-i", host_sig, "clients", "-j"], capture_output=True, text=True, timeout=3)
                hosts = json.loads(result.stdout) if result.returncode == 0 else []
                host = next((c for c in hosts if c.get("pid") == self.proc.pid), None)
                if host:
                    selector = json.dumps("address:" + host["address"])
                    mode = os.environ.get("NESTED_MODE", "1280x720@60").split("@")[0]
                    width, height = map(int, mode.split("x"))
                    for expr in (
                        f'hl.dsp.window.float({{action="set", window={selector}}})',
                        f'hl.dsp.window.resize({{x={width}, y={height}, window={selector}}})',
                    ):
                        subprocess.run(["hyprctl", "-i", host_sig, "dispatch", expr], check=True, capture_output=True, text=True, timeout=3)
                    time.sleep(0.3)
                    break
                time.sleep(0.1)
        errors = self.ctl("configerrors", check=False)
        if errors:
            raise RuntimeError("config errors: " + errors)
        # Windows are started through the child compositor itself, one at a
        # time, so they always inherit its WAYLAND_DISPLAY and map in order.
        for app, title in WINDOWS:
            before = len(self.clients())
            cmd = f"foot --app-id={shlex.quote(app)} --title={shlex.quote(title)} /usr/bin/cat"
            self.dispatch(f"hl.dsp.exec_cmd({json.dumps(cmd, ensure_ascii=False)})")
            for _ in range(50):
                if len(self.clients()) > before:
                    break
                time.sleep(0.1)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.dispatch("hl.dsp.exit()")
                self.proc.wait(timeout=8)
            except Exception:
                self.proc.terminate()
        if getattr(self, "log", None):
            self.log.close()


def restored_x(restored, n, address):
    for c in n.clients():
        if c["address"] == address:
            return restored.get(c["class"], c["at"])[0]
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", default=str(ROOT / ".build/dev/spatialoverview.so"))
    ap.add_argument("--shots", default=str(ROOT / ".build/shots"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--memory-only", action="store_true")
    a = ap.parse_args()
    if a.memory_only:
        failures = []
        memory_test(a, lambda cond, msg: (print(("PASS " if cond else "FAIL ") + msg, flush=True), failures.append(msg) if not cond else None))
        print("FAILED: " + "; ".join(failures) if failures else "ALL PASSED", flush=True)
        return 1 if failures else 0
    n = Nested(a.plugin, a.shots)
    failures = []

    def check(cond, msg):
        print(("PASS " if cond else "FAIL ") + msg, flush=True)
        if not cond:
            failures.append(msg)

    try:
        n.launch()
        check(len(n.clients()) >= len(WINDOWS), f"{len(n.clients())} test windows mapped")
        # The first toggle creates the canvas and goes straight to the map,
        # so typing already searches.
        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.8)
        n.keys("spot")
        time.sleep(0.4)
        check(n.active().get("class") == "spotify", "first Super+Ctrl+G opens the navigator directly")
        n.keys("-k", "Escape", "-k", "Escape")
        time.sleep(0.6)
        n.dispatch('hl.plugin.spatialoverview.canvas("arrange")')
        time.sleep(0.4)
        n.shot("00-canvas")

        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.8)
        n.shot("01-navigator-open")

        n.keys("slack")
        time.sleep(0.5)
        check(n.active().get("class") == "slack", "typing 'slack' focuses Slack")
        n.shot("02-typed-slack")

        n.keys("-k", "BackSpace", "-k", "BackSpace", "-k", "BackSpace", "-k", "BackSpace", "-k", "BackSpace")
        n.keys("code")
        time.sleep(0.4)
        check(n.active().get("class") == "code", "typing 'code' focuses the editor")

        n.keys("-k", "Escape")
        n.keys("ertes")
        time.sleep(0.4)
        check(n.active().get("class") == "obsidian", "accent-insensitive 'ertes' finds 'értesítő'")
        n.shot("03-unicode")

        # Letters must appear together: p-t-f-y occurs in order inside
        # "Spotify", but not as a run, so it matches nothing.
        n.keys("-k", "Escape")
        before = n.active().get("class")
        # The whole query at once: typed letter by letter, "p" alone would
        # match Spotify first.
        n.dispatch('hl.plugin.spatialoverview.canvas("search ptfy")')
        time.sleep(0.4)
        after = n.active().get("class")
        print(f"  ptfy: focus {before} -> {after}")
        check(after != "spotify", "'ptfy' is not a match for Spotify (letters must be adjacent)")
        n.shot("03b-no-scattered-match")

        n.keys("-k", "Escape")
        n.keys("-k", "Tab")
        time.sleep(0.4)
        n.shot("04-tab-recent")
        n.keys("-k", "F1")
        time.sleep(0.3)
        n.shot("05-help")
        n.keys("-k", "F1")

        n.keys("-k", "Escape")  # hides the recent list, stays in the navigator
        n.keys("btop")
        time.sleep(0.3)
        n.keys("-k", "Return")
        time.sleep(0.8)
        check(n.active().get("title") == "btop", "Enter lands on btop")
        n.shot("06-landed")

        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.6)
        n.keys("-M", "ctrl", "0", "-m", "ctrl")
        time.sleep(0.6)
        n.shot("07-fit-all")
        n.keys("spot")
        time.sleep(0.4)
        check(n.active().get("class") == "spotify", "typing 'spot' focuses Spotify while browsing")
        n.keys("-k", "Escape", "-k", "Escape")
        time.sleep(0.6)
        check(n.active().get("title") == "btop", "Escape returns focus to where the session began")
        n.shot("08-escaped")

        before = {c["class"]: c["at"] for c in n.clients()}
        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.6)
        n.keys("spot")
        n.keys("-M", "shift", "-k", "Return", "-m", "shift")
        time.sleep(0.9)
        after = {c["class"]: c["at"] for c in n.clients()}
        check(n.active().get("class") == "spotify" and after["spotify"] != before["spotify"], "Shift+Enter brings Spotify to the current view")
        n.shot("09-summoned")

        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.6)
        n.keys("-M", "ctrl", "a", "-m", "ctrl")
        time.sleep(0.5)
        n.shot("10-tidied")
        n.keys("-M", "ctrl", "z", "-m", "ctrl")
        time.sleep(0.5)
        restored = {c["class"]: c["at"] for c in n.clients()}
        check(restored["spotify"] == after["spotify"], "Ctrl+Z undoes the tidy")
        start = n.active().get("address")
        n.keys("-k", "Right")
        time.sleep(0.4)
        moved = n.active().get("address")
        check(moved != start, "Right arrow moves the selection spatially")
        n.keys("-M", "shift", "-k", "Right", "-m", "shift")
        time.sleep(0.3)
        nudged = {c["address"]: c["at"] for c in n.clients()}
        check(nudged[moved][0] > restored_x(restored, n, moved), "Shift+Right nudges the selected window")
        n.keys("-k", "Escape")
        time.sleep(0.6)
        check(n.active().get("address") == start, "Escape restores the original focus after arrow browsing")
        # Alt+Tab. Virtual keyboards cannot trigger Hyprland binds, so hold
        # Alt from one wtype process and invoke the bound dispatcher directly.
        def with_alt(hold_ms, *steps):
            holder = subprocess.Popen(["wtype", "-M", "alt", "-s", str(hold_ms), "-m", "alt"], env=n.env())
            time.sleep(0.08)
            for step in steps:
                step()
            holder.wait(timeout=10)
            time.sleep(0.6)

        first = n.active().get("address")
        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.6)
        n.keys("-k", "Right")
        n.keys("-k", "Return")
        time.sleep(0.8)
        second = n.active().get("address")
        with_alt(120, lambda: n.dispatch('hl.plugin.spatialoverview.canvas("switch next")'))
        check(n.active().get("address") == first and first != second, "quick Alt+Tab returns to the previous window")
        n.shot("11-quick-alttab")

        def held():
            n.dispatch('hl.plugin.spatialoverview.canvas("switch next")')
            time.sleep(0.5)
            n.shot("12-held-alttab")
            n.dispatch('hl.plugin.spatialoverview.canvas("switch next")')
        with_alt(900, held)
        landed = n.active()
        check(landed.get("address") not in (first, second), "held Alt+Tab shows the navigator and lands on the third most recent window")
        n.shot("13-after-held-alttab")
        check(not n.ctl("configerrors", check=False), "no config errors")
    finally:
        if not a.keep:
            n.stop()
        else:
            print("kept nested instance", n.sig, n.tmp.name, flush=True)
            n.tmp._finalizer.detach()
    if not a.keep and not failures:
        memory_test(a, check)
    print("FAILED: " + "; ".join(failures) if failures else "ALL PASSED", flush=True)
    return 1 if failures else 0


def memory_test(a, check):
    """Windows come back to their remembered spots in a new compositor."""
    state = tempfile.mkdtemp(prefix="navigator-state-")
    n = Nested(a.plugin, a.shots, state=state, extra=",remember_layout=true")
    try:
        n.launch()
        if not n.clients():
            n.log.flush()
            print(open(n.log.name).read()[-3000:], flush=True)
        n.dispatch('hl.plugin.spatialoverview.overview("on all")')
        time.sleep(0.6)
        n.dispatch('hl.plugin.spatialoverview.canvas("arrange")')
        time.sleep(0.3)
        n.dispatch('hl.plugin.spatialoverview.overview("toggle all")')
        time.sleep(0.6)
        n.keys("spot")
        for _ in range(6):
            n.keys("-M", "shift", "-k", "Right", "-m", "shift")
        n.keys("-k", "Return")
        time.sleep(1.2)
        saved = {c["title"]: c["at"] for c in n.clients()}
        memory = Path(state) / "spatial-overview" / "canvas-memory.tsv"
        check(memory.exists() and "Spotify Premium" in memory.read_text(), "layout memory is written")
    finally:
        n.stop()
    time.sleep(1.0)
    n = Nested(a.plugin, a.shots, state=state, extra=",remember_layout=true")
    try:
        n.launch()
        n.dispatch('hl.plugin.spatialoverview.overview("on all")')
        time.sleep(0.8)
        restored = {c["title"]: c["at"] for c in n.clients()}
        same = [t for t in saved if restored.get(t) == saved[t]]
        check(len(same) == len(saved), f"{len(same)}/{len(saved)} windows restored to their remembered spots")
        n.shot("14-restored")
    finally:
        n.stop()


if __name__ == "__main__":
    sys.exit(main())
