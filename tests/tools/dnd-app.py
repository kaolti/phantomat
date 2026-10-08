"""A GTK4 window (or top-layer panel) for drag-and-drop tests. It is a drag
source (drag anywhere: text "dnd-payload TITLE") and a drop target, and prints
what wl_data_device and wl_pointer give it, in surface coordinates:

  hover X Y            pointer motion (no drag)
  dnd-enter X Y        a drag entered
  dnd-motion X Y       drag motion
  dnd-leave            the drag left (also after a drop)
  drop TEXT X Y        dropped here
  drag-begin / drag-end / drag-cancel REASON   (as the source)

usage: dnd-app.py TITLE [--size WxH] [--fullscreen OUTPUT] [--layer OUTPUT]
(--layer needs LD_PRELOAD=/usr/lib/libgtk4-layer-shell.so)
"""
import argparse
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, GObject, Gtk

ap = argparse.ArgumentParser()
ap.add_argument("title")
ap.add_argument("--size", default="360x240")
ap.add_argument("--fullscreen")
ap.add_argument("--layer")
args = ap.parse_args()
W, H = map(int, args.size.split("x"))


def out(*parts):
    print(" ".join(str(p) for p in parts), flush=True)


def monitor_named(name):
    monitors = Gdk.Display.get_default().get_monitors()
    for i in range(monitors.get_n_items()):
        if monitors.get_item(i).get_connector() == name:
            return monitors.get_item(i)
    return None


def activate(app):
    win = Gtk.ApplicationWindow(application=app, title=args.title)
    area = Gtk.DrawingArea()
    area.set_size_request(W, H)
    area.set_draw_func(lambda _a, cr, w, h: (cr.set_source_rgb(0.2, 0.35, 0.6), cr.paint()))

    hover = Gtk.EventControllerMotion()
    hover.connect("motion", lambda _c, x, y: out("hover", round(x), round(y)))
    area.add_controller(hover)

    source = Gtk.DragSource(actions=Gdk.DragAction.COPY)
    source.connect("prepare", lambda _s, _x, _y: Gdk.ContentProvider.new_for_value(f"dnd-payload {args.title}"))
    source.connect("drag-begin", lambda _s, _d: out("drag-begin"))
    source.connect("drag-end", lambda _s, _d, delete: out("drag-end"))
    source.connect("drag-cancel", lambda _s, _d, reason: out("drag-cancel", reason.value_nick) or False)
    area.add_controller(source)

    motion = Gtk.DropControllerMotion()
    motion.connect("enter", lambda _c, x, y: out("dnd-enter", round(x), round(y)))
    motion.connect("motion", lambda _c, x, y: out("dnd-motion", round(x), round(y)))
    motion.connect("leave", lambda _c: out("dnd-leave"))
    area.add_controller(motion)

    target = Gtk.DropTarget.new(GObject.TYPE_STRING, Gdk.DragAction.COPY)

    def dropped(_t, value, x, y):
        out("drop", value.replace(" ", "_"), round(x), round(y))
        return True

    target.connect("drop", dropped)
    area.add_controller(target)
    win.set_child(area)

    if args.layer:
        gi.require_version("Gtk4LayerShell", "1.0")
        from gi.repository import Gtk4LayerShell as LayerShell

        LayerShell.init_for_window(win)
        LayerShell.set_layer(win, LayerShell.Layer.TOP)
        LayerShell.set_namespace(win, "dnd-test-panel")
        for edge in (LayerShell.Edge.TOP, LayerShell.Edge.LEFT, LayerShell.Edge.RIGHT):
            LayerShell.set_anchor(win, edge, True)
        LayerShell.auto_exclusive_zone_enable(win)
        if monitor := monitor_named(args.layer):
            LayerShell.set_monitor(win, monitor)
        area.set_size_request(100, H)
    win.set_default_size(W, H)
    win.present()
    if args.fullscreen:
        monitor = monitor_named(args.fullscreen)
        GLib.timeout_add(300, lambda: (win.fullscreen_on_monitor(monitor) if monitor else win.fullscreen()) and False)
    out("ready")


app = Gtk.Application(application_id=f"dev.spatialoverview.dnd.{args.title.replace('-', '')}", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
app.run(None)
