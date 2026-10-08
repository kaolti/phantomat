#pragma once

#include "CanvasDndRoute.hpp"

#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

class CWLSurfaceResource;

// wl_data_device drags (file drag, browser tab drag) on the canvas. The
// canvas only takes over while the pointer is over a canvas-drawn window; see
// CanvasDndRoute.hpp for who drives the drag where.
namespace SpatialOverview::CanvasDnd {
    // A drag is in progress (also after the drop, until the target finishes).
    bool active();
    // The drag has not been dropped yet.
    bool buttonHeld();

    // Call on every pointer motion before routing it: forgets the last drag
    // once it has ended, and arms the hold-back on Hyprland's drag-motion
    // listener when a new drag has started.
    void sync();

    // The canvas picked this surface as the target: focus it (leave/enter as
    // needed) and send it surface-local motion. Hyprland's motion is held back
    // while this surface stays the target; anything else (a panel, a
    // fullscreen window, another monitor) Hyprland targets and moves itself.
    void focusCanvasTarget(SP<CWLSurfaceResource> surface, const Vector2D& surfaceLocal);
    // Over empty canvas: leave the current target, so a drop here is cancelled.
    void clearTarget();
    // A canvas went away: what it picked is not drawn by it any more, so
    // Hyprland's drag motion is no longer held back for it.
    void canvasGone();

    // Plugin unload: put Hyprland's own drag-motion handler back.
    void shutdown();
}
