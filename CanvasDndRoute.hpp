#pragma once

// Who drives a wl_data_device drag (file drag, browser tab drag) while the
// pointer moves. Kept free of Hyprland types so tests/canvas-dnd-route-test.cpp
// can check it on its own.
//
// On a canvas monitor, windows are drawn away from their real (world)
// position, so Hyprland's hit test and its drag-motion listener (which
// measures from the real position) are wrong for them. Everything else (a
// panel on the top/overlay layer, a fullscreen window, a monitor without a
// canvas) is where Hyprland thinks it is, so Hyprland keeps driving those.
namespace SpatialOverview::CanvasDnd {
    enum class ERoute {
        NOT_DRAGGING,  // no drag: the normal canvas pointer path
        HYPRLAND,      // Hyprland's own hit test, focus and drag motion
        CANVAS_TARGET, // a canvas-drawn window: the canvas picks it and sends motion
        CANVAS_EMPTY,  // empty canvas: no target, so a drop here is cancelled
        DROPPED,       // dropped, waiting for the target to finish: hold still
    };

    struct SPointerProbe {
        bool dragActive          = false; // PROTO::data->dndActive()
        bool buttonHeld          = false; // not yet dropped (Hyprland's button listener is armed)
        bool pointerOnThisCanvas = false; // the pointer is on a canvas-desktop monitor and this canvas owns it
        bool closing             = false; // the canvas is closing
        bool layerAbove          = false; // a top/overlay layer surface (panel, launcher) is under the pointer
        bool canvasWindowBelow   = false; // a canvas-drawn window with a surface is under the pointer
    };

    constexpr ERoute route(const SPointerProbe& p) {
        if (!p.dragActive)
            return ERoute::NOT_DRAGGING;
        if (!p.pointerOnThisCanvas || p.closing)
            return ERoute::HYPRLAND;
        if (!p.buttonHeld)
            return ERoute::DROPPED;
        // Panels sit above the canvas, at their real position: they win.
        if (p.layerAbove)
            return ERoute::HYPRLAND;
        if (p.canvasWindowBelow)
            return ERoute::CANVAS_TARGET;
        return ERoute::CANVAS_EMPTY;
    }

    // Hyprland's drag-motion listener stays installed for the whole drag. It is
    // held back only while the drag target is the window the canvas picked:
    // for that surface it would measure from the wrong place. Hyprland runs it
    // before its own hit test, so when the pointer leaves the canvas window
    // for a panel or another monitor, that one event still goes to the canvas
    // window and is held back too; once Hyprland has moved the target
    // elsewhere the pick is forgotten and every motion goes through.
    constexpr bool holdBackHyprlandMotion(bool canvasPickedTarget, bool targetIsCanvasPick) {
        return canvasPickedTarget && targetIsCanvasPick;
    }

    // The canvas pick, by surface identity (only compared, never dereferenced).
    struct SCanvasPick {
        const void* surface = nullptr;

        void        pick(const void* s) {
            surface = s;
        }
        void forget() {
            surface = nullptr;
        }
        // From Hyprland's drag-motion listener, with the current target.
        bool holdBack(const void* target) {
            if (surface && surface != target)
                surface = nullptr;
            return holdBackHyprlandMotion(!!surface, surface && surface == target);
        }
    };

    constexpr const char* routeName(ERoute r) {
        switch (r) {
            case ERoute::NOT_DRAGGING: return "not-dragging";
            case ERoute::HYPRLAND: return "hyprland";
            case ERoute::CANVAS_TARGET: return "canvas-target";
            case ERoute::CANVAS_EMPTY: return "canvas-empty";
            case ERoute::DROPPED: return "dropped";
        }
        return "?";
    }
}
