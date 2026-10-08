// Who drives a wl_data_device drag on the canvas (CanvasDndRoute.hpp), checked
// without a compositor. The table covers each decision; the drag scripts replay
// the cases from the PR review against a small model of Hyprland's drag
// handling (one target at a time: leave the old, enter the new; its motion
// listener measures from the target's real position) and check what every
// target receives.
//
//   make test-dnd-route        (or: g++ -std=c++2b -I. tests/canvas-dnd-route-test.cpp)
#include "CanvasDndRoute.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace SpatialOverview::CanvasDnd;

static int g_failures = 0;
static void check(bool cond, const std::string& msg) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", msg.c_str());
    if (!cond)
        ++g_failures;
}

static void routeTable() {
    const SPointerProbe DRAG{.dragActive = true, .buttonHeld = true, .pointerOnThisCanvas = true};
    auto                with = [&](auto edit) {
        auto p = DRAG;
        edit(p);
        return route(p);
    };

    check(with([](auto& p) { p.dragActive = false; }) == ERoute::NOT_DRAGGING, "no drag: the normal canvas pointer path");
    check(with([](auto& p) { p.canvasWindowBelow = true; }) == ERoute::CANVAS_TARGET, "over a canvas window: the canvas picks the target");
    check(with([](auto&) {}) == ERoute::CANVAS_EMPTY, "over empty canvas: no target");
    check(with([](auto& p) { p.layerAbove = true; }) == ERoute::HYPRLAND, "over a panel: Hyprland");
    check(with([](auto& p) { p.layerAbove = p.canvasWindowBelow = true; }) == ERoute::HYPRLAND, "a panel above a canvas window wins");
    check(with([](auto& p) { p.pointerOnThisCanvas = false; p.canvasWindowBelow = true; }) == ERoute::HYPRLAND,
          "another monitor / fullscreen (no canvas there): Hyprland");
    check(with([](auto& p) { p.closing = true; p.canvasWindowBelow = true; }) == ERoute::HYPRLAND, "a closing canvas hands the drag to Hyprland");
    check(with([](auto& p) { p.buttonHeld = false; p.canvasWindowBelow = true; }) == ERoute::DROPPED, "after the drop the canvas holds still");
    check(with([](auto& p) { p.buttonHeld = false; p.pointerOnThisCanvas = false; }) == ERoute::HYPRLAND, "after the drop, other monitors stay Hyprland's");
    check(holdBackHyprlandMotion(true, true), "Hyprland's motion is held back for the canvas-picked target");
    check(!holdBackHyprlandMotion(true, false), "...but not once Hyprland has moved the target elsewhere");
    check(!holdBackHyprlandMotion(false, false), "...nor when the canvas picked nothing");
}

// ---- drag model ---------------------------------------------------------

enum class EWhere { CANVAS_WINDOW, EMPTY_CANVAS, PANEL, PANEL_OVER_WINDOW, OTHER_MONITOR_FULLSCREEN };

struct SSpot {
    EWhere      where;
    std::string surface; // what is under the pointer ("" for nothing)
};

struct SEvent {
    std::string kind; // enter, motion, leave, drop, cancelled
    bool        wrongCoords = false;
};

class CModel {
  public:
    std::map<std::string, std::vector<SEvent>> got;
    int                                        cancelled = 0;

    void begin(const std::string& origin) {
        m_active = m_held = true;
        m_canvasPick.forget();
        m_target = origin; // Hyprland enters the origin when the drag starts
        send(m_target, {"enter"});
    }

    void move(const SSpot& s) {
        const bool    ONCANVAS = s.where != EWhere::OTHER_MONITOR_FULLSCREEN;
        SPointerProbe p{.dragActive          = m_active,
                        .buttonHeld          = m_held,
                        .pointerOnThisCanvas = ONCANVAS,
                        .layerAbove          = s.where == EWhere::PANEL || s.where == EWhere::PANEL_OVER_WINDOW,
                        .canvasWindowBelow   = s.where == EWhere::CANVAS_WINDOW || s.where == EWhere::PANEL_OVER_WINDOW};
        const auto    R = route(p);

        // 1. the canvas listener (registered first)
        bool cancelEvent = true;
        switch (R) {
            case ERoute::NOT_DRAGGING:
            case ERoute::HYPRLAND: cancelEvent = false; break;
            case ERoute::DROPPED: break;
            case ERoute::CANVAS_TARGET:
                focus(s.surface);
                m_canvasPick.pick(id(s.surface));
                send(m_target, {"motion"});
                break;
            case ERoute::CANVAS_EMPTY:
                m_canvasPick.forget();
                focus("");
                break;
        }

        // 2. Hyprland's drag-motion listener, same event: measures from the
        // real position, which is wrong for a canvas-drawn window.
        if (m_active && m_held && !m_target.empty() && !m_canvasPick.holdBack(id(m_target)))
            send(m_target, {"motion", isCanvasWindow(m_target)});

        // 3. Hyprland's own hit test, when the event was not cancelled.
        // Over the canvas it would find windows at their real position, so
        // only panels and other monitors are modelled here.
        if (!cancelEvent && m_active && m_held)
            focus(s.where == EWhere::PANEL_OVER_WINDOW ? std::string("panel") : s.surface);
    }

    void release() {
        if (!m_active)
            return;
        m_held = false;
        if (m_target.empty()) {
            ++cancelled; // dropDrag without a target: abortDrag
            end();
            return;
        }
        send(m_target, {"drop"});
        send(m_target, {"leave"});
    }

    void finish() { end(); } // the target calls wl_data_offer.finish
    void escape() {          // abortDrag
        if (!m_active)
            return;
        if (!m_target.empty())
            send(m_target, {"leave"});
        ++cancelled;
        end();
    }

    bool active() const { return m_active; }

  private:
    bool        m_active = false, m_held = false;
    std::string m_target;
    SCanvasPick m_canvasPick;

    static bool isCanvasWindow(const std::string& s) { return s == "source" || s == "canvas-target"; }

    void        send(const std::string& surface, SEvent e) {
        if (!surface.empty())
            got[surface].push_back(e);
    }
    void focus(const std::string& surface) {
        if (surface == m_target)
            return;
        send(m_target, {"leave"});
        m_target = surface;
        send(m_target, {"enter"});
    }
    void end() {
        m_active = m_held = false;
        m_target.clear();
        m_canvasPick.forget();
    }
    // a stable identity per surface name, like a surface pointer
    const void* id(const std::string& surface) {
        return surface.empty() ? nullptr : &m_ids[surface];
    }
    std::map<std::string, char> m_ids;
};

static int count(const std::vector<SEvent>& v, const std::string& kind) {
    int n = 0;
    for (const auto& e : v)
        n += e.kind == kind;
    return n;
}

static bool anyWrong(const std::vector<SEvent>& v) {
    for (const auto& e : v)
        if (e.wrongCoords)
            return true;
    return false;
}

// enter, then motion, then leave, in that order, as many times as entered
static bool wellFormed(const std::vector<SEvent>& v) {
    bool inside = false;
    for (const auto& e : v) {
        if (e.kind == "enter") {
            if (inside)
                return false;
            inside = true;
        } else if (e.kind == "leave") {
            if (!inside)
                return false;
            inside = false;
        } else if (!inside)
            return false;
    }
    return !inside;
}

static void dragScripts() {
    const SSpot SOURCE{EWhere::CANVAS_WINDOW, "source"}, CANVAS{EWhere::CANVAS_WINDOW, "canvas-target"}, EMPTY{EWhere::EMPTY_CANVAS, ""},
        PANEL{EWhere::PANEL, "panel"}, PANEL_OVER{EWhere::PANEL_OVER_WINDOW, "canvas-target"}, FULLSCREEN{EWhere::OTHER_MONITOR_FULLSCREEN, "fullscreen"};

    {
        CModel m;
        m.begin("source");
        for (const auto& s : {SOURCE, CANVAS, CANVAS, PANEL, PANEL, PANEL_OVER, CANVAS, FULLSCREEN, FULLSCREEN, FULLSCREEN})
            m.move(s);
        m.release();
        m.finish();
        for (const auto* t : {"source", "canvas-target", "panel", "fullscreen"})
            check(wellFormed(m.got[t]), std::string("[tour] ") + t + ": enter / motion / leave in order");
        check(count(m.got["canvas-target"], "motion") >= 3 && !anyWrong(m.got["canvas-target"]), "[tour] canvas window: motion, all in its drawn coordinates");
        check(count(m.got["panel"], "enter") == 1 && count(m.got["panel"], "motion") >= 2, "[tour] panel above the canvas: entered, moved, kept over a window under it");
        check(count(m.got["fullscreen"], "motion") >= 2, "[tour] fullscreen window on the other monitor gets motion (the v3 bug)");
        check(count(m.got["fullscreen"], "drop") == 1 && m.cancelled == 0, "[tour] the drop lands on the fullscreen window");
    }
    {
        CModel m;
        m.begin("source");
        m.move(CANVAS);
        m.move(EMPTY);
        m.release();
        check(m.cancelled == 1 && !m.active(), "[empty] a drop on empty canvas cancels the drag");
        check(wellFormed(m.got["canvas-target"]) && count(m.got["canvas-target"], "drop") == 0, "[empty] the window it passed got leave, no drop");
    }
    {
        CModel m;
        m.begin("source");
        m.move(FULLSCREEN);
        m.move(FULLSCREEN);
        m.escape();
        check(m.cancelled == 1 && !m.active() && wellFormed(m.got["fullscreen"]), "[escape] Escape over the fullscreen window cancels with a leave");
        m.got.clear();
        m.begin("source");
        m.move(CANVAS);
        m.move(FULLSCREEN);
        m.move(FULLSCREEN);
        m.release();
        m.finish();
        check(count(m.got["fullscreen"], "motion") >= 1 && count(m.got["fullscreen"], "drop") == 1, "[repeat] the next drag reaches the fullscreen window again");
        check(!anyWrong(m.got["canvas-target"]) && wellFormed(m.got["canvas-target"]), "[repeat] and the canvas window again, no stale target");
    }
    {
        CModel m;
        m.begin("source");
        m.move(CANVAS);
        m.release(); // dropped on the canvas window, target not finished yet
        const auto BEFORE = m.got;
        m.move(PANEL_OVER);
        m.move(EMPTY);
        check(m.got["panel"].empty() && m.got["canvas-target"].size() == BEFORE.at("canvas-target").size(),
              "[dropped] moving before the target finishes sends nothing new on the canvas");
        m.finish();
        m.begin("source");
        m.move(PANEL);
        m.release();
        m.finish();
        check(count(m.got["panel"], "drop") == 1, "[dropped] after finishing, the next drag drops on the panel");
    }
}

int main() {
    routeTable();
    dragScripts();
    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
