// Standard headers first: they must not see the access override below.
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// Hyprland's drag state and the signal listener's handler are private.
#define private public
#define protected public
#include <hyprutils/signal/Listener.hpp>
#include <hyprland/src/protocols/core/DataDevice.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#undef private
#undef protected

#include "CanvasDnd.hpp"

namespace SpatialOverview::CanvasDnd {
    namespace {
        // The surface the canvas picked as the drag target, if it still is.
        WP<CWLSurfaceResource> g_canvasTarget;
        SCanvasPick            g_pick;
        // Hyprland's drag-motion listener of the current drag, and its own
        // handler, kept so it can be put back as it was.
        WP<Hyprutils::Signal::CSignalListener> g_wrappedListener;
        std::function<void(void*)>             g_hyprlandHandler;

        void forgetPick() {
            g_canvasTarget.reset();
            g_pick.forget();
        }

        bool holdBackHyprlandMotionNow() {
            if (g_canvasTarget.expired())
                forgetPick();
            const auto TARGET = g_pSeatManager ? g_pSeatManager->m_state.dndPointerFocus.lock() : SP<CWLSurfaceResource>{};
            const bool HOLD   = g_pick.holdBack(TARGET.get());
            if (!g_pick.surface)
                g_canvasTarget.reset();
            return HOLD;
        }

        void forgetDrag() {
            forgetPick();
            g_wrappedListener.reset();
            g_hyprlandHandler = nullptr;
        }
    }

    bool active() {
        return PROTO::data && PROTO::data->dndActive();
    }

    bool buttonHeld() {
        // Armed by initiateDrag, dropped together with the drop.
        return active() && !!PROTO::data->m_dnd.mouseButton;
    }

    void sync() {
        if (!active()) {
            forgetDrag();
            return;
        }

        const auto& LISTENER = PROTO::data->m_dnd.mouseMove;
        if (!LISTENER || (!g_wrappedListener.expired() && LISTENER.get() == g_wrappedListener.get()))
            return;

        // A new drag. Hyprland's motion listener measures from the target's
        // real position. Keep it, and only skip it while the target is a
        // canvas-drawn window (the canvas sends that motion itself); for any
        // other target it runs unchanged.
        forgetPick();
        g_wrappedListener = LISTENER;
        g_hyprlandHandler = LISTENER->m_fHandler;
        LISTENER->m_fHandler = [hyprland = g_hyprlandHandler](void* args) {
            if (holdBackHyprlandMotionNow())
                return;
            hyprland(args);
        };
    }

    void focusCanvasTarget(SP<CWLSurfaceResource> surface, const Vector2D& surfaceLocal) {
        if (!active() || !surface)
            return;

        g_canvasTarget = surface;
        g_pick.pick(surface.get());
        if (g_pSeatManager->m_state.dndPointerFocus.lock() != surface) {
            g_pSeatManager->m_state.dndPointerFocus = surface;
            g_pSeatManager->m_events.dndPointerFocusChange.emit(); // Hyprland's updateDrag: leave the old target, enter this one
        }

        if (const auto DEVICE = PROTO::data->m_dnd.focusedDevice.lock())
            DEVICE->sendMotion(Time::millis(Time::steadyNow()), surfaceLocal);
    }

    void clearTarget() {
        forgetPick();
        if (!active() || !g_pSeatManager->m_state.dndPointerFocus)
            return;

        g_pSeatManager->m_state.dndPointerFocus.reset();
        g_pSeatManager->m_events.dndPointerFocusChange.emit(); // sends leave; no new target
        PROTO::data->m_dnd.focusedDevice.reset();               // so a drop here cancels the drag
    }

    void canvasGone() {
        forgetPick();
    }

    void shutdown() {
        if (const auto LISTENER = g_wrappedListener.lock(); LISTENER && g_hyprlandHandler)
            LISTENER->m_fHandler = g_hyprlandHandler;
        forgetDrag();
    }
}
