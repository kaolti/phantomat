#include "scrollOverview.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_set>
#include <linux/input-event-codes.h>
#include <state/MonitorState.hpp>
#include <state/WorkspaceState.hpp>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/managers/XWaylandManager.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/PreBlurElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private
#include "Config.hpp"
#include "Experiments.hpp"
#include "BarrelShader.hpp"
#include "Hud.hpp"
#include "Icons.hpp"
#include "Memory.hpp"
#include "Navigator.hpp"
#include "Tuning.hpp"
#include "DropIndicator.hpp"
#include "OverviewPassElement.hpp"
#include "OverviewRender.hpp"
#include "Window.hpp"

static PHLWINDOW getOverviewFullscreenVisibilityWindow(const PHLWORKSPACE& workspace, const PHLWINDOW& fallback = {});
static constexpr const char* OVERVIEW_SUBMAP = "spatialoverview";
static constexpr auto        POST_DROP_PSEUDO_FOCUS_DURATION = std::chrono::milliseconds(100);
static CScrollOverview*             g_pointerGrabOverview = nullptr;
static std::unordered_set<uint32_t> g_topLayerPointerButtons;
static PHLWINDOWREF                 g_pseudoFocusedWindow;
static Time::steady_tp              g_pseudoFocusUntil = {};
static std::unordered_set<const void*> g_canvasManagedTargets;
static PHLMONITORREF                g_canvasKeyboardNavigationMonitor;
static PHLMONITORREF                g_canvasActiveCameraMonitor;
static int                          g_canvasInternalFocusDepth = 0;
// Native and spatial geometry are distinct, session-scoped representations.
// Weak references prevent a closed client from being resurrected by restore.
struct SCanvasSavedWindow {
    PHLWINDOWREF window;
    PHLWORKSPACEREF workspace;
    CBox box;
    bool floating;
    Fullscreen::SFullscreenMode fullscreen;
};
static std::unordered_map<uint64_t, SCanvasSavedWindow> g_canvasNativeLayout;
static std::unordered_map<uint64_t, CBox> g_canvasSpatialLayout;
static std::vector<std::vector<SCanvasSavedWindow>> g_canvasUndo;
static std::vector<std::vector<SCanvasSavedWindow>> g_canvasRedo;
static bool g_canvasRestoring = false;
static std::unordered_map<std::string, Vector2D> g_canvasCameraBookmarks;
static std::string g_canvasCursorShape; // the cursor override the canvases set; empty: none
static const CScrollOverview* g_linkedLeader = nullptr; // linked screens: whose camera the others take
static Time::steady_tp        g_linkedLeaderMovedAt;    // when the leader last moved its own camera
static int g_userFollowMouse = 1; // input:follow_mouse as set before the canvas turned it off
// Places (see canvasPlaceAction), defined with them at the end.
static void notePlaceWindow(const PHLWINDOW& window, bool opened);
static int  placeAt(const PHLMONITOR& monitor, const Vector2D& world);
static void removePlace(int place);
static void notePlaceFullscreen(const PHLWINDOW& window, const Fullscreen::SFullscreenMode& modes);
static void forgetPlaceFullscreen(const PHLWINDOW& window);
static bool placeTiling(int place);
static bool canvasSnapDoubleClick(const PHLWINDOW& window, uint32_t timeMs);
// Going to a place at 100% shows the minimap for a moment (see canvasPlaceAction).
static Time::steady_tp g_minimapFlashStart, g_minimapFlashUntil;
static float minimapFlashAlpha(const Time::steady_tp& now) {
    if (now >= g_minimapFlashUntil || now < g_minimapFlashStart)
        return 0.F;
    const float IN  = std::chrono::duration<float>(now - g_minimapFlashStart).count() / 0.15F;
    const float OUT = std::chrono::duration<float>(g_minimapFlashUntil - now).count() / 0.4F;
    return std::clamp(std::min(IN, OUT), 0.F, 1.F);
}
// Windows fullscreen on a screen whose canvas stepped aside for them (see
// "Fullscreen on the canvas").
struct SCanvasFullscreen {
    PHLWINDOWREF        window;
    PHLMONITORREF       monitor;
    std::optional<CBox> request; // what an X11 app last asked for meanwhile (X11 coordinates)
    std::optional<CBox> before;  // where it was before, as the canvas last drew it
};
static std::vector<SCanvasFullscreen> g_canvasFullscreen;
void unconstrainCanvasWindows();
static bool canvasFullscreenWindow(const PHLWINDOW& window) {
    return window && std::ranges::any_of(g_canvasFullscreen, [&window](const auto& entry) { return entry.window.lock() == window; });
}
// Each window's box when the canvas last drew it outside fullscreen. After
// fullscreen Hyprland centers a floating window on its monitor; the canvas
// puts it back there instead.
struct SCanvasWindowedBox {
    PHLWINDOWREF window;
    CBox         box;
};
static std::unordered_map<const Desktop::View::CWindow*, SCanvasWindowedBox> g_canvasWindowedBox;
static void noteCanvasWindowedBox(const PHLWINDOW& window) {
    if (Fullscreen::controller()->isFullscreen(window))
        return;
    if (g_canvasWindowedBox.size() > 256)
        std::erase_if(g_canvasWindowedBox, [](const auto& entry) { return !entry.second.window.lock(); });
    g_canvasWindowedBox[window.get()] = {.window = window, .box = CBox{window->m_realPosition->goal(), window->m_realSize->goal()}};
}
// The first canvas of a compositor session restores remembered homes for a
// short while, so apps started at login find their places as they appear.
static bool            g_canvasMemoryStarted     = false;
static Time::steady_tp g_canvasMemoryRestoreUntil = {};
static wl_event_source* g_canvasMemoryTimer       = nullptr;
extern void requestFlightDeckNative(PHLWINDOW window, Fullscreen::eFullscreenMode mode);
void        canvasReleaseX11Windows();

static SCanvasSavedWindow saveCanvasWindow(PHLWINDOW w) {
    return {w, w->m_workspace, w->layoutTarget()->position(), w->layoutTarget()->floating(), Fullscreen::controller()->getFullscreenModes(w)};
}

// A window and its layout target can disagree about where they live: the
// window holds its workspace, the target holds a space that only weakly holds
// its workspace. If they drift apart and the old workspace is destroyed,
// tiling the window makes Hyprland's layout dereference a null workspace and
// the compositor dies. Re-home such a target before touching its mode.
static bool attachCanvasLayoutTarget(const PHLWINDOW& w) {
    const auto TARGET    = w ? w->layoutTarget() : nullptr;
    const auto WORKSPACE = w ? w->m_workspace : nullptr;
    if (!TARGET || !valid(WORKSPACE) || !WORKSPACE->m_space)
        return false;
    if (TARGET->space() != WORKSPACE->m_space || !TARGET->space()->workspace()) {
        Log::logger->log(Log::WARN, "[spatialoverview] re-attaching window {:x} to the layout of workspace {}", rc<uintptr_t>(w.get()), WORKSPACE->m_id);
        g_layoutManager->newTarget(TARGET, WORKSPACE->m_space);
    }
    const auto SPACE = TARGET->space();
    return SPACE == WORKSPACE->m_space && SPACE->workspace() == WORKSPACE;
}

// Every floating/tiling change the canvas makes goes through here. Tiling
// additionally needs a monitor to tile on; without one the window stays put.
static Config::Actions::ActionResult setCanvasFloating(const PHLWINDOW& w, bool floating) {
    if (!attachCanvasLayoutTarget(w))
        return Config::Actions::actionError("window is not attached to a live workspace");
    if (!floating && (!w->m_workspace->m_monitor || !Desktop::focusState()->monitor()))
        return Config::Actions::actionError("no monitor to tile the window on");
    return Config::Actions::floatWindow(floating ? Config::Actions::TOGGLE_ACTION_ENABLE : Config::Actions::TOGGLE_ACTION_DISABLE, w);
}

// CWindow::moveToWorkspace alone leaves the layout target behind (see above);
// this is the path Hyprland's own movetoworkspace takes.
static void moveCanvasWindowToWorkspace(const PHLWINDOW& w, const PHLWORKSPACE& ws) {
    if (w && valid(ws) && w->m_workspace != ws)
        Desktop::globalWindowController()->moveWindowToWorkspace(w, ws);
    attachCanvasLayoutTarget(w);
}

static void restoreCanvasNativeLayout() {
    if (g_canvasNativeLayout.empty()) return;
    g_canvasRestoring = true;
    for (auto& [id, saved] : g_canvasNativeLayout) {
        const auto w = saved.window.lock();
        if (!validMapped(w) || !w->layoutTarget()) continue;
        g_canvasSpatialLayout[id] = w->layoutTarget()->position();
        Fullscreen::controller()->setFullscreenMode(w, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        if (const auto ws = saved.workspace.lock(); ws && w->m_workspace != ws) moveCanvasWindowToWorkspace(w, ws);
        if (!setCanvasFloating(w, saved.floating)) continue;
        if (saved.floating && w->layoutTarget()) {
            // Clamp recovery geometry to its current output if topology changed.
            auto box = saved.box;
            if (const auto m = w->m_monitor.lock()) {
                box.w = std::min(box.w, m->m_size.x);
                box.h = std::min(box.h, m->m_size.y);
                box.x = std::clamp(box.x, m->m_position.x, m->m_position.x + m->m_size.x - box.w);
                box.y = std::clamp(box.y, m->m_position.y, m->m_position.y + m->m_size.y - box.h);
            }
            w->layoutTarget()->rememberFloatingSize(box.size());
            w->layoutTarget()->setPositionGlobal(box);
            w->layoutTarget()->warpPositionSize();
        }
        Fullscreen::controller()->setFullscreenMode(w, saved.fullscreen.internal, saved.fullscreen.client);
    }
    g_canvasNativeLayout.clear();
    g_canvasManagedTargets.clear();
    g_canvasRestoring = false;
}

// Hyprland still owns the actual workspaces, but Canvas mode owns their sparse
// 2D placement. Empty cells have no workspace and therefore cost nothing.
static std::unordered_map<WORKSPACEID, Vector2D> g_canvasWorkspaceCells;

static std::string canvasLowercase(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
    return value;
}

static std::string canvasCategoryKey(ECanvasWindowCategory category) {
    switch (category) {
        case ECanvasWindowCategory::TERMINAL: return "10-terminals";
        case ECanvasWindowCategory::BROWSER: return "20-browsers";
        case ECanvasWindowCategory::COMMUNICATION: return "30-communication";
        case ECanvasWindowCategory::DEVELOPMENT: return "40-development";
        case ECanvasWindowCategory::FILES_NOTES: return "50-files-notes";
        case ECanvasWindowCategory::MEDIA: return "60-media";
        default: return "90-other";
    }
}

static std::set<std::string> canvasContextTokens(const PHLWINDOW& window) {
    // Words that say nothing about what a window is for, including the user
    // and host names every terminal title carries ("user@host: ~").
    static const std::unordered_set<std::string> STOPWORDS = [] {
        std::unordered_set<std::string> words{
            "about", "agent", "application", "browser", "chrome", "chromium", "claude", "codex", "default", "desktop", "file", "files", "firefox", "foot",
            "google", "home", "https", "inbox", "manager", "mozilla", "new", "notes", "omarchy", "project", "terminal", "untitled", "window", "with",
        };
        if (const char* user = std::getenv("USER"); user && *user)
            words.emplace(canvasLowercase(user));
        if (char host[256] = {}; gethostname(host, sizeof(host) - 1) == 0 && *host)
            words.emplace(canvasLowercase(host));
        return words;
    }();

    std::set<std::string> result;
    const auto TEXT = canvasLowercase(window ? window->m_title + " " + window->m_class : std::string{});
    std::string token;
    const auto commit = [&] {
        if (token.size() >= 4 && !STOPWORDS.contains(token))
            result.emplace(token);
        token.clear();
    };

    for (const auto c : TEXT) {
        if (std::isalnum(sc<unsigned char>(c)))
            token += c;
        else
            commit();
    }
    commit();
    return result;
}

static bool sameCanvasCell(const Vector2D& lhs, const Vector2D& rhs) {
    return sc<int>(std::round(lhs.x)) == sc<int>(std::round(rhs.x)) && sc<int>(std::round(lhs.y)) == sc<int>(std::round(rhs.y));
}

static void restoreActiveWorkspaceVisibility() {
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;

        for (const auto& workspace : {monitor->m_activeWorkspace, monitor->m_activeSpecialWorkspace}) {
            if (!workspace)
                continue;

            workspace->m_visible = true;
            workspace->m_alpha->setValueAndWarp(1.F);
            workspace->m_renderOffset->setValueAndWarp(Vector2D{});
        }

        g_pHyprRenderer->damageMonitor(monitor);
    }
}

static void releaseTopLayerPointerButtons(uint32_t timeMs) {
    if (g_topLayerPointerButtons.empty())
        return;

    for (const auto button : g_topLayerPointerButtons)
        g_pSeatManager->sendPointerButton(timeMs, button, WL_POINTER_BUTTON_STATE_RELEASED);

    g_topLayerPointerButtons.clear();
    g_pSeatManager->sendPointerFrame();
}

static void removeOverview(CScrollOverview* overview) {
    const auto PMONITOR = overview ? overview->pMonitor.lock() : PHLMONITOR{};
    unregisterScrollOverview(overview);
    if (scrollOverviews().empty())
        disableScrollOverviewHooks();

    if (PMONITOR) {
        PMONITOR->recheckSolitary();
        g_pHyprRenderer->damageMonitor(PMONITOR);
    }
}

static xkb_keysym_t getOverviewKeysym(const IKeyboard::SKeyEvent& event) {
    const auto PKEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (!PKEYBOARD)
        return XKB_KEY_NoSymbol;

    xkb_state* const STATE = PKEYBOARD->m_resolveBindsBySym && PKEYBOARD->m_xkbSymState ? PKEYBOARD->m_xkbSymState : PKEYBOARD->m_xkbState;

    if (!STATE)
        return XKB_KEY_NoSymbol;

    return xkb_state_key_get_one_sym(STATE, event.keycode + 8);
}

static bool hasOverviewSubmap() {
    return g_pKeybindManager && std::ranges::any_of(g_pKeybindManager->m_keybinds, [](const auto& keybind) { return keybind && keybind->submap.name == OVERVIEW_SUBMAP; });
}

static bool isOverviewSubmapActive() {
    return g_pKeybindManager && g_pKeybindManager->getCurrentSubmap().name == OVERVIEW_SUBMAP;
}

static bool hasApplicableScrollKeybind(const IPointer::SAxisEvent& event) {
    if (!g_pKeybindManager || !g_pInputManager || event.source != WL_POINTER_AXIS_SOURCE_WHEEL || event.delta == 0.0)
        return false;

    std::string key;
    if (event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        key = event.delta > 0 ? "mouse_down" : "mouse_up";
    else if (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        key = event.delta < 0 ? "mouse_left" : "mouse_right";
    else
        return false;

    const auto MODS   = g_pInputManager->getModsFromAllKBs();
    const auto SUBMAP = g_pKeybindManager->getCurrentSubmap();
    return std::ranges::any_of(g_pKeybindManager->m_keybinds, [&](const auto& keybind) {
        return keybind && keybind->enabled && !keybind->shadowed && keybind->key == key && (keybind->modmask == MODS || keybind->ignoreMods) &&
            (keybind->submap.name == SUBMAP.name || keybind->submapUniversal);
    });
}

static bool scrollKeybindIsThrottled() {
    if (!g_pKeybindManager)
        return false;

    return g_pKeybindManager->m_scrollTimer.getMillis() < ScrollOverview::Config::getValue<int>("binds:scroll_event_delay");
}

static bool isTopLayerFocused(PHLMONITOR monitor) {
    const auto FOCUSEDSURFACE = g_pSeatManager->m_state.keyboardFocus.lock();

    if (!FOCUSEDSURFACE)
        return false;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(FOCUSEDSURFACE);
    if (!HLSURFACE)
        return false;

    const auto VIEW = HLSURFACE->view();
    if (!VIEW)
        return false;

    auto layerOwner = Desktop::View::CLayerSurface::fromView(VIEW);

    if (!layerOwner) {
        const auto POPUP = Desktop::View::CPopup::fromView(VIEW);
        if (POPUP) {
            const auto T1OWNER = POPUP->getT1Owner();
            if (T1OWNER)
                layerOwner = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
        }
    }

    return layerOwner && Desktop::View::validMapped(layerOwner) && layerOwner->m_monitor == monitor && layerOwner->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

static constexpr const char* OVERVIEW_INSERT_FADE_BEZIER = "spatialoverviewWorkspaceInsertFade";
static constexpr const char* OVERVIEW_REMOVE_FADE_BEZIER = "spatialoverviewWorkspaceRemoveFade";

static bool isPointerOnTopLayer(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
    Vector2D   surfaceCoords;
    PHLLS      layerSurface;

    const auto isAnimatedChrome = [&monitor](const PHLLS& surface) {
        const auto overview = scrollOverviewForMonitor(monitor);
        const auto spatial  = overview ? dynamic_cast<CScrollOverview*>(overview.get()) : nullptr;
        if (!surface || !spatial || !spatial->isCanvasDesktop() || !ScrollOverview::Config::getChromeAnimationEnabled() || spatial->overviewProgress() <= 0.001F)
            return false;

        const auto TOPNS    = ScrollOverview::Config::getChromeTopNamespace();
        const auto BOTTOMNS = ScrollOverview::Config::getChromeBottomNamespace();
        return (!TOPNS.empty() && surface->m_namespace == TOPNS) || (!BOTTOMNS.empty() && surface->m_namespace == BOTTOMNS);
    };

    if (Desktop::viewState()->hitTest().layerSurfaceAt(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &layerSurface) &&
        !isAnimatedChrome(layerSurface))
        return true;

    layerSurface.reset();
    return Desktop::viewState()->hitTest().layerSurfaceAt(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &layerSurface) &&
        !isAnimatedChrome(layerSurface);
}

static PHLWINDOW getOverviewWindowToShow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    if (window->m_group)
        return window->m_group->current();

    return window;
}

static bool shouldShowOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if (WINDOW->m_workspace && WINDOW->m_workspace->m_isSpecialWorkspace)
        return false;

    if (WINDOW->m_pinned && WINDOW->m_isFloating)
        return false;

    // X11 menus and tooltips belong to the screen, not the canvas.
    if (WINDOW->m_isX11 && WINDOW->isX11OverrideRedirect())
        return false;

    // Fullscreen on a screen of its own: Hyprland shows it there, natively.
    if (canvasFullscreenWindow(WINDOW))
        return false;

    return true;
}

// X11 menus and tooltips (override-redirect windows): the app places them
// itself, in screen coordinates, so they are drawn and hit fixed to the
// screen rather than moved with the camera.
// Pinned floating windows too: they stay put on the screen while the canvas
// moves under them (SUPER + O).
static bool canvasScreenFixedWindow(const PHLWINDOW& window) {
    if (!validMapped(window) || window->isHidden())
        return false;
    if (window->m_isX11 && window->isX11OverrideRedirect())
        return true;
    return window->m_pinned && window->m_isFloating && !(window->m_workspace && window->m_workspace->m_isSpecialWorkspace);
}

static bool shouldShowPinnedFloatingOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if (!WINDOW->m_pinned || !WINDOW->m_isFloating)
        return false;

    return true;
}

static bool surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return false;

    bool hasCallbacks = false;
    surface->breadthfirst(
        [&hasCallbacks](SP<CWLSurfaceResource> child, const Vector2D&, void*) {
            if (!child || child->m_current.callbacks.empty())
                return;

            hasCallbacks = true;
        },
        nullptr);

    return hasCallbacks;
}

static void surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!surface)
        return;

    std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};
    surface->breadthfirst([](SP<CWLSurfaceResource> child, const Vector2D&, void* data) {
        if (!child)
            return;

        const auto [MONITOR, NOW] = *sc<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
        child->presentFeedback(NOW, MONITOR, false);
    }, &data);
}

// Popups are surfaces of their own, outside the window's surface tree. Apps
// animate menus on frame callbacks (Chromium fades them in step by step), so
// a popup that never gets frames stays stuck at its first, faint step.
static void popupTreePresent(const PHLWINDOW& window, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!window || window->m_isX11 || !window->m_popupHead)
        return;
    window->m_popupHead->breadthfirst(
        [&monitor, &now](WP<Desktop::View::CPopup> popup, void*) {
            if (popup && popup->aliveAndVisible() && popup->wlSurface() && popup->wlSurface()->resource())
                surfaceTreePresent(popup->wlSurface()->resource(), monitor, now);
        },
        nullptr);
}

static bool windowHasOverviewAnimation(const PHLWINDOW& window) {
    if (!window)
        return false;

    return window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated() || window->m_alpha.isBeingAnimated() ||
        window->m_borderFadeAnimationProgress->isBeingAnimated() || window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_dimPercent->isBeingAnimated() ||
        window->m_shadowFadeAnimationProgress->isBeingAnimated();
}

static bool layerHasOverviewAnimation(const PHLLS& layer) {
    if (!Desktop::View::validMapped(layer))
        return false;

    return layer->positionAnimation()->isBeingAnimated() || layer->sizeAnimation()->isBeingAnimated() || layer->m_alpha.isBeingAnimated();
}

static Vector2D axisOffsetVector(float offset, ScrollOverview::Config::ELayout layout);

static CBox getOverviewBox(CBox box, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const Vector2D& offset, bool round = true) {
    const auto MONITORSCALE    = monitor->m_scale;
    const auto VIEWPORT_CENTER = CBox{{}, monitor->m_size * MONITORSCALE}.middle();

    box = {box.pos() * MONITORSCALE, box.size() * MONITORSCALE};
    box.translate(-VIEWPORT_CENTER).scale(scale).translate(VIEWPORT_CENTER).translate(-viewOffset * scale * MONITORSCALE).translate(offset);
    if (round)
        box.round();

    return box;
}

static CBox getOverviewBox(CBox box, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                           bool round = true) {
    return getOverviewBox(box, monitor, scale, viewOffset, axisOffsetVector(offset, layout), round);
}

static CBox getOverviewBox(CBox box, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const Vector2D& offset,
                           ScrollOverview::Config::ELayout, bool round = true) {
    return getOverviewBox(box, monitor, scale, viewOffset, offset, round);
}

static CBox getOverviewGlobalBox(CBox globalBox, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                 bool round = true) {
    return getOverviewBox({globalBox.pos() - monitor->m_position, globalBox.size()}, monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewGlobalBox(CBox globalBox, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const Vector2D& offset,
                                 ScrollOverview::Config::ELayout layout, bool round = true) {
    return getOverviewBox({globalBox.pos() - monitor->m_position, globalBox.size()}, monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                 bool round = true) {
    if (!window)
        return {};

    return getOverviewGlobalBox(window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT), monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const Vector2D& offset,
                                 ScrollOverview::Config::ELayout layout, bool round = true) {
    if (!window)
        return {};

    return getOverviewGlobalBox(window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT), monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewDragWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                     bool round = true) {
    if (!window)
        return {};

    const auto TARGET = window->layoutTarget();
    if (window->m_group && TARGET)
        return getOverviewGlobalBox(TARGET->position(), monitor, scale, viewOffset, offset, layout, round);

    return getOverviewWindowBox(window, monitor, scale, viewOffset, offset, layout, round);
}

static CBox getOverviewDragWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const Vector2D& offset,
                                     ScrollOverview::Config::ELayout layout, bool round = true) {
    if (!window)
        return {};

    const auto TARGET = window->layoutTarget();
    if (window->m_group && TARGET)
        return getOverviewGlobalBox(TARGET->position(), monitor, scale, viewOffset, offset, layout, round);

    return getOverviewWindowBox(window, monitor, scale, viewOffset, offset, layout, round);
}

static CBox expandOverviewWindowHitbox(CBox box, float scale, float monitorScale) {
    const auto GAPS = ScrollOverview::Config::getCssGapData("general:gaps_in");
    constexpr float GAP_MULTIPLIER = 2.F;
    const float     TOTALSCALE     = scale * monitorScale;

    box.x -= sc<float>(std::max<int64_t>(0, GAPS.m_left)) * TOTALSCALE * GAP_MULTIPLIER;
    box.y -= sc<float>(std::max<int64_t>(0, GAPS.m_top)) * TOTALSCALE * GAP_MULTIPLIER;
    box.width += sc<float>(std::max<int64_t>(0, GAPS.m_left) + std::max<int64_t>(0, GAPS.m_right)) * TOTALSCALE * GAP_MULTIPLIER;
    box.height += sc<float>(std::max<int64_t>(0, GAPS.m_top) + std::max<int64_t>(0, GAPS.m_bottom)) * TOTALSCALE * GAP_MULTIPLIER;

    return box;
}

static float overviewPointDistanceSqToBox(const Vector2D& point, const CBox& box) {
    const float dx = point.x < box.x ? box.x - point.x : point.x > box.x + box.width ? point.x - (box.x + box.width) : 0.F;
    const float dy = point.y < box.y ? box.y - point.y : point.y > box.y + box.height ? point.y - (box.y + box.height) : 0.F;

    return dx * dx + dy * dy;
}

static Vector2D getOverviewMousePosLocal(PHLMONITOR monitor) {
    if (!monitor)
        return {};

    auto point = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    const auto OVERVIEW = scrollOverviewForMonitor(monitor);
    const auto SPATIAL  = OVERVIEW ? dc<CScrollOverview*>(OVERVIEW.get()) : nullptr;
    const float PROGRESS = SPATIAL ? SPATIAL->distortionProgress() : 0.F;
    if (!ScrollOverview::Config::getBarrelEnabled() || SpatialOverview::BarrelShader::shaderPath().empty() || PROGRESS <= 0.F)
        return point;

    const auto SIZE = monitor->m_size * monitor->m_scale;
    if (SIZE.x <= 0.F || SIZE.y <= 0.F)
        return point;

    Vector2D centered{point.x / SIZE.x * 2.F - 1.F, point.y / SIZE.y * 2.F - 1.F};
    const float RADIUS2 = centered.x * centered.x + centered.y * centered.y;
    const float STRENGTH  = ScrollOverview::Config::getBarrelStrength() * PROGRESS;
    const float EDGESCALE = 1.F + (ScrollOverview::Config::getBarrelEdgeScale() - 1.F) * PROGRESS;
    const float FACTOR    = (1.F + STRENGTH * RADIUS2) / EDGESCALE;
    centered = centered * FACTOR;
    return {(centered.x * 0.5F + 0.5F) * SIZE.x, (centered.y * 0.5F + 0.5F) * SIZE.y};
}

static bool isOverviewPointerOnMonitor(PHLMONITOR monitor) {
    const auto overview = monitor ? scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()) : SP<IOverview>{};
    return overview && overview->pMonitor.lock() == monitor;
}

static void markCanvasCameraActive(PHLMONITOR monitor) {
    if (monitor && scrollOverviewForMonitor(monitor))
        g_canvasActiveCameraMonitor = monitor;
}

SP<IOverview> canvasNavigationOverview() {
    const bool METAHELD = g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META);
    if (!METAHELD)
        g_canvasKeyboardNavigationMonitor.reset();

    if (METAHELD) {
        if (const auto LOCKEDMONITOR = g_canvasKeyboardNavigationMonitor.lock()) {
            if (const auto LOCKEDOVERVIEW = scrollOverviewForMonitor(LOCKEDMONITOR))
                return LOCKEDOVERVIEW;
            g_canvasKeyboardNavigationMonitor.reset();
        }
    }

    SP<IOverview> overview;
    if (const auto ACTIVECAMERA = g_canvasActiveCameraMonitor.lock())
        overview = scrollOverviewForMonitor(ACTIVECAMERA);

    if (!overview && g_pInputManager)
        overview = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());

    if (!overview)
        overview = activeScrollOverview();

    if (METAHELD && overview)
        g_canvasKeyboardNavigationMonitor = overview->pMonitor;
    return overview;
}

static CScrollOverview* canvasOf(const SP<IOverview>& overview) {
    return overview ? dynamic_cast<CScrollOverview*>(overview.get()) : nullptr;
}

static CScrollOverview* activeCanvasOverview() {
    return canvasOf(activeScrollOverview());
}

static void beginNavigatorSession() {
    if (SpatialOverview::Navigator::isOpen() || !ScrollOverview::Config::getNavigatorEnabled())
        return;
    SpatialOverview::Navigator::begin(getOverviewWindowToShow(Desktop::focusState()->window()));
    for (const auto& window : Desktop::windowState()->windows())
        SpatialOverview::Icons::resolve(getOverviewWindowToShow(window));
}

// Every monitor enters and leaves navigation together, but one at a time;
// the session ends when the last camera is back at 100%.
static void canvasFullscreenReturnCheck();

static void endNavigatorSessionIfIdle() {
    if (!SpatialOverview::Navigator::isOpen())
        return;
    for (const auto& overview : scrollOverviews()) {
        const auto* canvas = canvasOf(overview);
        if (canvas && !canvas->isClosing() && canvas->isCanvasNavigationActive())
            return;
    }
    SpatialOverview::Navigator::end();
    canvasFullscreenReturnCheck();
}

// A quick Alt+Tab switches straight away; holding Alt past this delay opens
// the navigator so you can see where you are going.
static constexpr int SWITCHER_REVEAL_MS = 170;
static wl_event_source* g_switcherTimer = nullptr;

static bool altHeld() {
    return g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_ALT);
}

static int switcherTimerCallback(void*) {
    auto* canvas = activeCanvasOverview();
    if (!canvas || !SpatialOverview::Navigator::isOpen() || !SpatialOverview::Navigator::state().switcher)
        return 0;
    if (!altHeld()) {
        canvas->finishSwitcher();
        return 0;
    }
    canvas->revealSwitcher();
    // Keep watching the modifier: a release can be missed while a panel
    // holds keyboard focus, and the switcher must never be left hanging.
    if (g_switcherTimer)
        wl_event_source_timer_update(g_switcherTimer, 90);
    return 0;
}

void disarmCanvasSwitcher() {
    if (g_switcherTimer)
        wl_event_source_remove(g_switcherTimer);
    g_switcherTimer = nullptr;
}

// While the session is locked every key and pointer event belongs to the
// lock screen; the canvas must neither read nor redirect them.
static bool sessionLocked() {
    return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
}

static bool keyStillHeld(uint32_t keycode) {
    return g_pInputManager && std::ranges::any_of(g_pInputManager->m_keyboards, [keycode](const auto& keyboard) { return keyboard && keyboard->getPressed(keycode); });
}

// Keys the palette has no use for still stay away from the window it has
// selected (Delete or Ctrl+D would act on whatever the search landed on).
// Media and function keys, and Alt+Tab, keep working.
static bool navigatorPassesThrough(uint32_t keysym, uint32_t mods) {
    if (keysym >= 0x1008FF00 && keysym <= 0x1008FFFF)
        return true;
    if (keysym == XKB_KEY_Print || keysym == XKB_KEY_Sys_Req || (keysym >= XKB_KEY_F2 && keysym <= XKB_KEY_F35))
        return true;
    if ((mods & HL_MODIFIER_ALT) && (keysym == XKB_KEY_Tab || keysym == XKB_KEY_ISO_Left_Tab))
        return true;
    return (mods & HL_MODIFIER_CTRL) && (mods & HL_MODIFIER_ALT);
}

static bool isModifierKeysym(uint32_t keysym) {
    return (keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R) || keysym == XKB_KEY_ISO_Level3_Shift || keysym == XKB_KEY_ISO_Level5_Shift ||
        keysym == XKB_KEY_Mode_switch || keysym == XKB_KEY_Num_Lock || keysym == XKB_KEY_ISO_Next_Group || keysym == XKB_KEY_ISO_Prev_Group ||
        (keysym >= XKB_KEY_dead_grave && keysym <= XKB_KEY_dead_greek);
}

static std::string navigatorKeyText(const IKeyboard::SKeyEvent& event) {
    const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();
    if (!KEYBOARD || !KEYBOARD->m_xkbState)
        return {};

    char      buffer[64];
    const int LENGTH = xkb_state_key_get_utf8(KEYBOARD->m_xkbState, event.keycode + 8, buffer, sizeof(buffer));
    if (LENGTH <= 0 || LENGTH >= sc<int>(sizeof(buffer)))
        return {};

    std::string text{buffer, sc<size_t>(LENGTH)};
    if (std::ranges::any_of(text, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        return {};
    return text;
}

static Vector2D axisOffsetVector(float offset, ScrollOverview::Config::ELayout layout) {
    return layout == ScrollOverview::Config::ELayout::HORIZONTAL ? Vector2D{offset, 0.F} : Vector2D{0.F, offset};
}

static float axisValue(const Vector2D& vector, ScrollOverview::Config::ELayout layout) {
    return layout == ScrollOverview::Config::ELayout::HORIZONTAL ? vector.x : vector.y;
}

static float axisSize(const Vector2D& size, ScrollOverview::Config::ELayout layout) {
    return layout == ScrollOverview::Config::ELayout::HORIZONTAL ? size.x : size.y;
}

static CBox getOverviewWorkspaceBox(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout) {
    return getOverviewBox({{}, monitor->m_size}, monitor, scale, viewOffset, offset, layout);
}

static CBox getOverviewWorkspaceBox(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const Vector2D& offset, ScrollOverview::Config::ELayout layout) {
    return getOverviewBox({{}, monitor->m_size}, monitor, scale, viewOffset, offset, layout);
}

static CBox getWorkspaceGlobalBox(PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : fallbackMonitor;
    if (!MONITOR)
        return {};

    return {MONITOR->m_position, MONITOR->m_size};
}

static CBox centerBoxInWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    const auto WORKSPACEBOX = getWorkspaceGlobalBox(workspace, fallbackMonitor);
    if (WORKSPACEBOX.width <= 0 || WORKSPACEBOX.height <= 0)
        return box;

    box.x = WORKSPACEBOX.x + std::max(0.F, sc<float>(WORKSPACEBOX.width - box.width)) / 2.F;
    box.y = WORKSPACEBOX.y + std::max(0.F, sc<float>(WORKSPACEBOX.height - box.height)) / 2.F;

    return box;
}

static CBox clampBoxToWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor, float margin = 0.F) {
    const auto WORKSPACEBOX = getWorkspaceGlobalBox(workspace, fallbackMonitor);
    if (WORKSPACEBOX.width <= 0 || WORKSPACEBOX.height <= 0)
        return box;

    const float CLAMPMARGIN = std::max(0.F, margin);
    const float MINX        = WORKSPACEBOX.x + CLAMPMARGIN;
    const float MINY        = WORKSPACEBOX.y + CLAMPMARGIN;
    const float MAXX        = WORKSPACEBOX.x + std::max(0.F, sc<float>(WORKSPACEBOX.width - box.width - 2.F * CLAMPMARGIN)) + CLAMPMARGIN;
    const float MAXY        = WORKSPACEBOX.y + std::max(0.F, sc<float>(WORKSPACEBOX.height - box.height - 2.F * CLAMPMARGIN)) + CLAMPMARGIN;

    box.x = std::clamp(sc<float>(box.x), MINX, std::max(MINX, MAXX));
    box.y = std::clamp(sc<float>(box.y), MINY, std::max(MINY, MAXY));

    return box;
}

static CBox resizedOverviewBoxFromCorner(const CBox& originalBox, const Vector2D& delta, Layout::eRectCorner corner, const Vector2D& minSizePx,
                                         const std::optional<Vector2D>& maxSizePx) {
    float left   = originalBox.x;
    float top    = originalBox.y;
    float right  = originalBox.x + originalBox.width;
    float bottom = originalBox.y + originalBox.height;

    switch (corner) {
        case Layout::CORNER_TOPLEFT:
            left += delta.x;
            top += delta.y;
            break;
        case Layout::CORNER_TOPRIGHT:
            right += delta.x;
            top += delta.y;
            break;
        case Layout::CORNER_BOTTOMLEFT:
            left += delta.x;
            bottom += delta.y;
            break;
        case Layout::CORNER_BOTTOMRIGHT:
        default:
            right += delta.x;
            bottom += delta.y;
            break;
    }

    float width  = right - left;
    float height = bottom - top;

    const float minWidth  = sc<float>(std::max(1.0, minSizePx.x));
    const float minHeight = sc<float>(std::max(1.0, minSizePx.y));
    const float maxWidth  = maxSizePx ? sc<float>(std::max(sc<double>(minWidth), maxSizePx->x)) : std::numeric_limits<float>::max();
    const float maxHeight = maxSizePx ? sc<float>(std::max(sc<double>(minHeight), maxSizePx->y)) : std::numeric_limits<float>::max();

    width  = std::clamp(width, minWidth, maxWidth);
    height = std::clamp(height, minHeight, maxHeight);

    switch (corner) {
        case Layout::CORNER_TOPLEFT:
            left = right - width;
            top  = bottom - height;
            break;
        case Layout::CORNER_TOPRIGHT:
            right = left + width;
            top   = bottom - height;
            break;
        case Layout::CORNER_BOTTOMLEFT:
            left   = right - width;
            bottom = top + height;
            break;
        case Layout::CORNER_BOTTOMRIGHT:
        default:
            right  = left + width;
            bottom = top + height;
            break;
    }

    return CBox{{left, top}, {right - left, bottom - top}};
}

static CBox clampResizedOverviewBoxToWorkspace(const CBox& box, const CBox& workspaceBox, Layout::eRectCorner corner, float marginPx) {
    if (workspaceBox.width <= 0 || workspaceBox.height <= 0)
        return box;

    const float minX = workspaceBox.x + std::max(0.F, marginPx);
    const float minY = workspaceBox.y + std::max(0.F, marginPx);
    const float maxX = workspaceBox.x + workspaceBox.width - std::max(0.F, marginPx);
    const float maxY = workspaceBox.y + workspaceBox.height - std::max(0.F, marginPx);

    float left   = box.x;
    float top    = box.y;
    float right  = box.x + box.width;
    float bottom = box.y + box.height;

    switch (corner) {
        case Layout::CORNER_TOPLEFT:
            left = std::max(left, minX);
            top  = std::max(top, minY);
            break;
        case Layout::CORNER_TOPRIGHT:
            right = std::min(right, maxX);
            top   = std::max(top, minY);
            break;
        case Layout::CORNER_BOTTOMLEFT:
            left   = std::max(left, minX);
            bottom = std::min(bottom, maxY);
            break;
        case Layout::CORNER_BOTTOMRIGHT:
        default:
            right  = std::min(right, maxX);
            bottom = std::min(bottom, maxY);
            break;
    }

    return CBox{{left, top}, {right - left, bottom - top}};
}

static bool overviewBoxIntersectsMonitor(const CBox& box, PHLMONITOR monitor) {
    if (!monitor || box.width <= 0 || box.height <= 0)
        return false;

    const auto RENDERSIZE = monitor->m_size * monitor->m_scale;

    return box.x < RENDERSIZE.x && box.x + box.width > 0 && box.y < RENDERSIZE.y && box.y + box.height > 0;
}

static bool overviewBoxFullyVisibleOnMonitor(const CBox& box, PHLMONITOR monitor) {
    if (!monitor || box.empty())
        return false;

    const CBox VIEWPORT = {{}, monitor->m_size * monitor->m_scale};
    return box.x >= VIEWPORT.x && box.y >= VIEWPORT.y && box.x + box.width <= VIEWPORT.x + VIEWPORT.width && box.y + box.height <= VIEWPORT.y + VIEWPORT.height;
}

static double overviewBoxIntersectionArea(const CBox& a, const CBox& b) {
    const auto INTERSECTION = a.intersection(b);
    return std::max(0.0, INTERSECTION.width) * std::max(0.0, INTERSECTION.height);
}

static double overviewBoxArea(const CBox& box) {
    return std::max(0.0, box.width) * std::max(0.0, box.height);
}

static double overviewBoxCenterDistanceSquared(const CBox& a, const CBox& b) {
    const auto ACENTER = a.middle();
    const auto BCENTER = b.middle();
    const auto DX      = ACENTER.x - BCENTER.x;
    const auto DY      = ACENTER.y - BCENTER.y;

    return DX * DX + DY * DY;
}

static CBox getPinnedFloatingOverviewWindowBox(PHLMONITOR monitor, const PHLWINDOW& window, float targetOverviewScale, float animationProgress, float* renderScale) {
    if (!monitor || !window) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const auto MONITORSCALE = monitor->m_scale;
    const auto WINDOWSIZE   = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) * MONITORSCALE;
    if (WINDOWSIZE.x <= 0 || WINDOWSIZE.y <= 0) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const CBox WINDOWBOX = {(window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - monitor->m_position) * MONITORSCALE, WINDOWSIZE};
    const auto MONITORW  = sc<float>(monitor->m_size.x * MONITORSCALE);
    const auto MONITORH  = sc<float>(monitor->m_size.y * MONITORSCALE);

    const std::array<CBox, 4> QUADRANTS = {
        CBox{0.F, 0.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{MONITORW / 2.F, 0.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{0.F, MONITORH / 2.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{MONITORW / 2.F, MONITORH / 2.F, MONITORW / 2.F, MONITORH / 2.F},
    };

    size_t bestQuadrant = 0;
    double bestArea     = -1.0;
    for (size_t i = 0; i < QUADRANTS.size(); ++i) {
        const auto AREA = overviewBoxIntersectionArea(WINDOWBOX, QUADRANTS[i]);
        if (AREA <= bestArea)
            continue;

        bestQuadrant = i;
        bestArea     = AREA;
    }

    const bool RIGHT  = bestQuadrant == 1 || bestQuadrant == 3;
    const bool BOTTOM = bestQuadrant == 2 || bestQuadrant == 3;

    const auto FULLBOX = monitor->logicalBox();
    const auto WORKBOX = monitor->logicalBoxMinusReserved();

    const float RESERVEDLEFT   = std::max(0.F, sc<float>(WORKBOX.x - FULLBOX.x)) * MONITORSCALE;
    const float RESERVEDTOP    = std::max(0.F, sc<float>(WORKBOX.y - FULLBOX.y)) * MONITORSCALE;
    const float RESERVEDRIGHT  = std::max(0.F, sc<float>((FULLBOX.x + FULLBOX.width) - (WORKBOX.x + WORKBOX.width))) * MONITORSCALE;
    const float RESERVEDBOTTOM = std::max(0.F, sc<float>((FULLBOX.y + FULLBOX.height) - (WORKBOX.y + WORKBOX.height))) * MONITORSCALE;

    const auto WORKSPACEGAP       = sc<float>(ScrollOverview::Config::getWorkspaceGap()) * MONITORSCALE;
    const auto RESERVEDWIDTH      = RIGHT ? RESERVEDRIGHT : RESERVEDLEFT;
    const auto CALCULATEDWIDTH    = std::max(1.F, sc<float>((MONITORW - MONITORW * targetOverviewScale) / 2.F - 2.F * WORKSPACEGAP - RESERVEDWIDTH));
    const auto CALCULATEDSCALE    = CALCULATEDWIDTH / sc<float>(WINDOWSIZE.x);
    const auto WINDOWRENDERSCALE  = std::min(1.F, std::max(CALCULATEDSCALE, targetOverviewScale));
    const auto PROGRESS           = std::clamp(animationProgress, 0.F, 1.F);
    const auto CURRENTRENDERSCALE = 1.F + (WINDOWRENDERSCALE - 1.F) * PROGRESS;
    const auto TARGETWIDTH       = sc<float>(WINDOWSIZE.x) * CURRENTRENDERSCALE;
    const auto TARGETHEIGHT      = sc<float>(WINDOWSIZE.y) * CURRENTRENDERSCALE;

    if (renderScale)
        *renderScale = CURRENTRENDERSCALE;

    const float X = RIGHT ? MONITORW - TARGETWIDTH - WORKSPACEGAP - RESERVEDRIGHT : WORKSPACEGAP + RESERVEDLEFT;
    const float Y = BOTTOM ? MONITORH - TARGETHEIGHT - WORKSPACEGAP - RESERVEDBOTTOM : WORKSPACEGAP + RESERVEDTOP;

    CBox box = {{X, Y}, {TARGETWIDTH, TARGETHEIGHT}};

    box.x = WINDOWBOX.x + (box.x - WINDOWBOX.x) * PROGRESS;
    box.y = WINDOWBOX.y + (box.y - WINDOWBOX.y) * PROGRESS;
    box.round();

    return box;
}

static std::chrono::milliseconds getOverviewIdleFrameInterval() {
    const int fps = std::clamp<int>(ScrollOverview::Config::getValue<int>("misc:render_unfocused_fps"), 1, 240);
    return std::chrono::milliseconds(std::max(1, 1000 / fps));
}

static constexpr std::chrono::milliseconds OVERVIEW_WINDOW_FRAME_INTERVAL = std::chrono::milliseconds(33);

struct SOverviewShadowConfig {
    bool       enabled     = false;
    int        range       = 0;
    int        renderPower = 1;
    Config::CGradientValueData color;
};

static SOverviewShadowConfig getOverviewShadowConfig() {
    const auto enabled     = ScrollOverview::Config::getShadowEnabled();
    const auto range       = ScrollOverview::Config::getShadowRange();
    const auto renderPower = ScrollOverview::Config::getShadowRenderPower();
    const auto color       = ScrollOverview::Config::getShadowColor();

    const auto globalRange       = ScrollOverview::Config::getValue<int>("decoration:shadow:range");
    const auto globalRenderPower = ScrollOverview::Config::getValue<int>("decoration:shadow:render_power");
    const auto globalColor       = ScrollOverview::Config::getValue<::Config::CGradientValueData>("decoration:shadow:color");

    Config::CGradientValueData shadowColor;
    if (color && !color->m_colors.empty())
        shadowColor = *color;
    else if (!globalColor.m_colors.empty())
        shadowColor = globalColor;

    return {
        .enabled      = !!enabled,
        .range        = std::max(0, range >= 0 ? range : globalRange),
        .renderPower  = std::clamp(renderPower >= 0 ? renderPower : globalRenderPower, 1, 4),
        .color        = shadowColor,
    };
}

static void renderOverviewWorkspaceShadow(PHLMONITOR monitor, const CBox& workspaceBox, float overviewScale, bool cutoutCenter, float alpha = 1.F) {
    if (!monitor)
        return;

    const auto SHADOW = getOverviewShadowConfig();
    const bool HASVISIBLECOLOR = std::ranges::any_of(SHADOW.color.m_colors, [](const CHyprColor& color) { return color.a > 0.F; });
    if (!SHADOW.enabled || SHADOW.range <= 0 || !HASVISIBLECOLOR || alpha <= 0.F)
        return;

    const int RANGE = sc<int>(std::round(SHADOW.range * monitor->m_scale * overviewScale));
    if (RANGE <= 0)
        return;

    auto baseBox = workspaceBox.copy().round();
    if (baseBox.width < 1 || baseBox.height < 1)
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewShadowPassElement>(COverviewShadowPassElement::SData{
        .monitor       = monitor,
        .fullBox       = baseBox.copy().expand(RANGE).round(),
        .cutoutBox     = baseBox,
        .rounding      = 0,
        .roundingPower = 2.F,
        .range         = RANGE,
        .renderPower   = SHADOW.renderPower,
        .color         = SHADOW.color,
        .alpha         = alpha,
        .ignoreWindow  = cutoutCenter,
    }));
}

static float getWorkspaceRenderedPitch(PHLMONITOR monitor, float scale, ScrollOverview::Config::ELayout layout) {
    return (axisSize(monitor->m_size, layout) * scale + sc<float>(ScrollOverview::Config::getWorkspaceGap())) * monitor->m_scale;
}

static float getWorkspaceLogicalPitch(PHLMONITOR monitor, float scale, ScrollOverview::Config::ELayout layout) {
    const auto safeScale = std::max(scale, 0.01F);
    return axisSize(monitor->m_size, layout) + sc<float>(ScrollOverview::Config::getWorkspaceGap()) / safeScale;
}

static float getWindowVerticalOverlap(const PHLWINDOW& a, const PHLWINDOW& b) {
    if (!a || !b)
        return 0.F;

    const auto APOS  = a->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto ASIZE = a->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BPOS  = b->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BSIZE = b->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    const double overlap = std::min(APOS.y + ASIZE.y, BPOS.y + BSIZE.y) - std::max(APOS.y, BPOS.y);

    return std::max(0.F, sc<float>(overlap));
}

static float getWindowHorizontalOverlap(const PHLWINDOW& a, const PHLWINDOW& b) {
    if (!a || !b)
        return 0.F;

    const auto APOS  = a->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto ASIZE = a->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BPOS  = b->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto BSIZE = b->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    const double overlap = std::min(APOS.x + ASIZE.x, BPOS.x + BSIZE.x) - std::max(APOS.x, BPOS.x);

    return std::max(0.F, sc<float>(overlap));
}

static bool overviewBoxesEqual(const CBox& a, const CBox& b) {
    return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
}

static bool moveOverviewScrollingTargetToWorkspaceEdge(const SP<Layout::ITarget>& target, int side);
static bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction);

static double overviewTargetDirectionDelta(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || !anchor->layoutTarget())
        return 0.0;

    const auto TARGETCENTER = target->position().middle();
    const auto ANCHORCENTER = anchor->layoutTarget()->position().middle();

    if (direction == "l" || direction == "r")
        return TARGETCENTER.x - ANCHORCENTER.x;

    return TARGETCENTER.y - ANCHORCENTER.y;
}

static void moveOverviewTargetOneStep(const SP<Layout::ITarget>& target, const std::string& direction) {
    if (!target || direction.empty())
        return;

    g_layoutManager->moveInDirection(target, direction, true);
}

static Layout::Tiled::CScrollingAlgorithm* overviewScrollingAlgorithmForTarget(const SP<Layout::ITarget>& target) {
    if (!target || !target->space() || !target->space()->algorithm())
        return nullptr;

    return dc<Layout::Tiled::CScrollingAlgorithm*>(target->space()->algorithm()->m_tiled.get());
}

static Layout::Tiled::CScrollingAlgorithm* overviewScrollingAlgorithmForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space || !workspace->m_space->algorithm())
        return nullptr;

    return dc<Layout::Tiled::CScrollingAlgorithm*>(workspace->m_space->algorithm()->m_tiled.get());
}

static bool isWorkspaceScrolling(const PHLWORKSPACE& workspace) {
    return overviewScrollingAlgorithmForWorkspace(workspace) != nullptr;
}

static Vector2D overviewScrollingCameraTranslation(Layout::Tiled::CScrollingAlgorithm* algorithm) {
    if (!algorithm || !algorithm->m_scrollingData || !algorithm->m_scrollingData->controller)
        return {};

    const auto& CONTROLLER = algorithm->m_scrollingData->controller;
    const auto TRANSLATION = CONTROLLER->isReversed() ? CONTROLLER->getOffset() : -CONTROLLER->getOffset();

    return CONTROLLER->isPrimaryHorizontal() ? Vector2D{TRANSLATION, 0.F} : Vector2D{0.F, TRANSLATION};
}

template <typename TOffset>
static CBox getOverviewWorkspaceUsableBox(const PHLWORKSPACE& workspace, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, const TOffset& offset,
                                          ScrollOverview::Config::ELayout layout) {
    if (!workspace || !monitor)
        return {};

    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller) {
        const auto USABLE = ALGO->usableArea();
        return getOverviewBox(USABLE, monitor, scale, viewOffset, offset, layout);
    }

    if (!workspace->m_space)
        return getOverviewWorkspaceBox(monitor, scale, viewOffset, offset, layout);

    auto USABLE = workspace->m_space->workArea();
    USABLE.translate(-monitor->m_position);
    USABLE.w = std::max(USABLE.w, 1.0);
    USABLE.h = std::max(USABLE.h, 1.0);

    return getOverviewBox(USABLE, monitor, scale, viewOffset, offset, layout);
}

static double clampOverviewScrollingOffset(Layout::Tiled::CScrollingAlgorithm* algo, double offset) {
    if (!algo || !algo->m_scrollingData)
        return offset;

    const double MAXOFFSET = std::max(0.0, algo->m_scrollingData->maxWidth() - algo->primaryViewportSize());
    return std::clamp(offset, 0.0, MAXOFFSET);
}

static bool moveOverviewScrollingTargetToWorkspaceEdge(const SP<Layout::ITarget>& target, int side) {
    if (!target || side == 0)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForTarget(target);
    if (!ALGO || !ALGO->m_scrollingData)
        return false;

    const auto TDATA = ALGO->dataFor(target);
    if (!TDATA)
        return false;

    const auto SRC_COL = TDATA->column.lock();
    if (!SRC_COL)
        return false;

    const auto SRC_COL_WIDTH = SRC_COL->getColumnWidth();
    SRC_COL->remove(target);

    const int64_t INSERT_AFTER = side < 0 ? -1 : sc<int64_t>(ALGO->m_scrollingData->columns.size()) - 1;
    const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER, SRC_COL_WIDTH);
    NEW_COL->add(TDATA);
    ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
    ALGO->m_scrollingData->recalculate();
    ALGO->focusTargetUpdate(target);

    return true;
}

static bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || !anchor->layoutTarget() || direction.empty())
        return false;

    const auto ALGO = overviewScrollingAlgorithmForTarget(target);
    if (!ALGO || !ALGO->m_scrollingData)
        return false;

    const auto TDATA      = ALGO->dataFor(target);
    const auto ANCHORDATA = ALGO->dataFor(anchor->layoutTarget());
    if (!TDATA || !ANCHORDATA)
        return false;

    const auto SRC_COL    = TDATA->column.lock();
    const auto ANCHOR_COL = ANCHORDATA->column.lock();
    if (!SRC_COL || !ANCHOR_COL)
        return false;

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const bool MOVINGCOLUMN      = PRIMARYHORIZONTAL ? direction == "l" || direction == "r" : direction == "u" || direction == "d";
    const bool STACKINGCOLUMN    = PRIMARYHORIZONTAL ? direction == "u" || direction == "d" : direction == "l" || direction == "r";

    if (MOVINGCOLUMN) {
        const auto SRC_COL_WIDTH = SRC_COL->getColumnWidth();
        SRC_COL->remove(target);

        const auto ANCHOR_COL_IDX = ALGO->m_scrollingData->idx(ANCHOR_COL);
        if (ANCHOR_COL_IDX < 0)
            return false;

        const bool    INSERT_BEFORE = direction == "l" || direction == "u";
        const int64_t INSERT_AFTER  = INSERT_BEFORE ? ANCHOR_COL_IDX - 1 : ANCHOR_COL_IDX;
        const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER, SRC_COL_WIDTH);
        NEW_COL->add(TDATA);
        ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
        ALGO->m_scrollingData->recalculate();
        ALGO->focusTargetUpdate(target);

        return true;
    }

    if (!STACKINGCOLUMN)
        return false;

    SRC_COL->remove(target);

    const auto ANCHOR_IDX    = ANCHOR_COL->idx(anchor->layoutTarget());
    const bool INSERT_BEFORE = direction == "l" || direction == "u";
    const int  INSERT_AFTER  = INSERT_BEFORE ? sc<int>(ANCHOR_IDX) - 1 : sc<int>(ANCHOR_IDX);
    ANCHOR_COL->add(TDATA, INSERT_AFTER);
    ALGO->m_scrollingData->centerOrFitCol(ANCHOR_COL);
    ALGO->m_scrollingData->recalculate();
    ALGO->focusTargetUpdate(target);

    return true;
}

static void moveOverviewTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || direction.empty())
        return;

    if (moveOverviewScrollingTargetNextToWindow(target, anchor, direction))
        return;

    const auto PREVFALLBACK = ScrollOverview::Config::getValue<int>("binds:window_direction_monitor_fallback");
    ScrollOverview::Config::setValue("binds:window_direction_monitor_fallback", 0);
    auto restoreFallback   = Hyprutils::Utils::CScopeGuard([PREVFALLBACK] {
        ScrollOverview::Config::setValue("binds:window_direction_monitor_fallback", PREVFALLBACK);
    });

    const bool WANT_NEGATIVE = direction == "l" || direction == "u";
    const auto FORWARD       = direction;
    const auto BACKWARD      = direction == "l" ? "r" : direction == "r" ? "l" : direction == "u" ? "d" : "u";

    auto isDesiredSide = [&] {
        const auto DELTA = overviewTargetDirectionDelta(target, anchor, direction);
        return WANT_NEGATIVE ? DELTA < 0.0 : DELTA > 0.0;
    };

    for (size_t i = 0; i < 64 && isDesiredSide(); ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        moveOverviewTargetOneStep(target, BACKWARD);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;

        if (!isDesiredSide()) {
            moveOverviewTargetOneStep(target, FORWARD);
            return;
        }
    }

    for (size_t i = 0; i < 64 && !isDesiredSide(); ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        moveOverviewTargetOneStep(target, FORWARD);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;
    }
}

CScrollOverview::~CScrollOverview() {
    if (g_pointerGrabOverview == this)
        g_pointerGrabOverview = nullptr;
    transferSharedStateOwnership();
    restoreSubmapIfActive();
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    if (realtimePreviewTimer) {
        wl_event_source_remove(realtimePreviewTimer);
        realtimePreviewTimer = nullptr;
    }
    if (backdropBlurFB)
        backdropBlurFB->release();
    backdropBlurFB.reset();
    if (backdropSharpFB)
        backdropSharpFB->release();
    backdropSharpFB.reset();
    const auto MONITOR = pMonitor.lock();
    if (g_canvasKeyboardNavigationMonitor.lock() == MONITOR)
        g_canvasKeyboardNavigationMonitor.reset();
    if (g_canvasActiveCameraMonitor.lock() == MONITOR)
        g_canvasActiveCameraMonitor.reset();
    // Leading, it may be going away mid-move (a screen stepping aside for a
    // fullscreen window right after landing): the others go on to where it
    // was heading, not stop where it had got to.
    if (g_linkedLeader == this) {
        g_linkedLeader = nullptr;
        for (const auto& overview : scrollOverviews()) {
            auto* canvas = canvasOf(overview);
            if (canvas && canvas != this && !canvas->isClosing())
                canvas->inheritLinkedCamera(this);
        }
    }
    const auto WORKSPACE = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(WORKSPACE, Desktop::focusState()->window()), false);
    restoreWorkspaceAnimationOverrides();
    restoreInputConfigOverrides();
    restoreForcedSurfaceVisibility();
    restoreForcedWindowVisibility();
    restoreForcedLayerVisibility();
    images.clear(); // otherwise we get a vram leak
    // Where the camera was heading: a canvas can close mid-glide (a window going
    // fullscreen right after landing on it).
    if (isCanvasDesktop() && MONITOR) g_canvasCameraBookmarks[MONITOR->m_name] = viewOffset->goal();
    endNavigatorSessionIfIdle();
    if (scrollOverviews().empty()) {
        // Re-tiling while an output is being torn down trips the layout's
        // invariants. The windows stay floating; reopening the canvas or
        // closing it normally later restores them as usual.
        if (!g_overviewMonitorTeardown && g_canvasFullscreen.empty())
            restoreCanvasNativeLayout();
        canvasReleaseX11Windows();
        restoreActiveWorkspaceVisibility();
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
        g_canvasCursorShape.clear();
    } else
        setCanvasCursor(""); // the canvases still open set theirs again as they need it
    if (const auto MONITOR = pMonitor.lock())
        MONITOR->m_blurFBDirty = true;
    SpatialOverview::BarrelShader::disable();
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_, PHLMONITOR monitor_) : startedOn(startedOn_), swipe(swipe_) {
    const auto          PMONITOR = monitor_ ? monitor_ : (startedOn_ && startedOn_->m_monitor ? startedOn_->m_monitor.lock() : Desktop::focusState()->monitor());
    pMonitor                     = PMONITOR;
    if (!g_canvasActiveCameraMonitor) {
        const bool POINTERONTHIS = PMONITOR && g_pInputManager && PMONITOR->logicalBox().containsPoint(g_pInputManager->getMouseCoordsInternal());
        if (POINTERONTHIS || Desktop::focusState()->monitor() == PMONITOR)
            g_canvasActiveCameraMonitor = PMONITOR;
    }
    SpatialOverview::BarrelShader::enable();
    layout                       = ScrollOverview::Config::getLayout();
    canvasNavigationActive      = !isPersistentCanvas();
    sharedStateOwner             = scrollOverviews().empty();
    usesSubmapKeybinds           = hasOverviewSubmap();

    applyWorkspaceAnimationOverrides();
    if (sharedStateOwner)
        forceWorkspaceAlphaVisible();
    applyInputConfigOverrides();
    unconstrainCanvasWindows();
    realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);
    scheduleMinimumPreviewFrame();

    const auto WINDOWSMOVECONFIG = Config::animationTree()->getAnimationPropertyConfig("windowsMove");
    const auto WINDOWSMOVEVALUES = WINDOWSMOVECONFIG && WINDOWSMOVECONFIG->pValues ? WINDOWSMOVECONFIG->pValues.lock() : WINDOWSMOVECONFIG;
    auto       overviewBezier    = ScrollOverview::Config::getAnimationBezier();
    if (!Animation::mgr()->bezierExists(overviewBezier))
        overviewBezier = WINDOWSMOVEVALUES && Animation::mgr()->bezierExists(WINDOWSMOVEVALUES->internalBezier) ? WINDOWSMOVEVALUES->internalBezier : "default";

    overviewAnimationConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    overviewAnimationConfig->overridden      = true;
    overviewAnimationConfig->internalBezier  = overviewBezier;
    overviewAnimationConfig->internalSpeed   = ScrollOverview::Config::getAnimationSpeed();
    overviewAnimationConfig->internalEnabled = ScrollOverview::Config::getAnimationEnabled();
    overviewAnimationConfig->internalStyle   = "";
    overviewAnimationConfig->pValues         = overviewAnimationConfig;

    if (!Animation::mgr()->bezierExists(OVERVIEW_INSERT_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_INSERT_FADE_BEZIER, Vector2D{0.5, 0.0}, Vector2D{0.5, 0.0});
    if (!Animation::mgr()->bezierExists(OVERVIEW_REMOVE_FADE_BEZIER))
        Animation::mgr()->addBezierWithName(OVERVIEW_REMOVE_FADE_BEZIER, Vector2D{0.5, 1.0}, Vector2D{0.5, 1.0});

    workspaceInsertFadeConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    workspaceInsertFadeConfig->overridden      = true;
    workspaceInsertFadeConfig->internalBezier  = OVERVIEW_INSERT_FADE_BEZIER;
    workspaceInsertFadeConfig->internalSpeed   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalSpeed * 1.2F : 12.F;
    workspaceInsertFadeConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    workspaceInsertFadeConfig->internalStyle   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalStyle : "";
    workspaceInsertFadeConfig->pValues         = workspaceInsertFadeConfig;

    workspaceRemoveFadeConfig                  = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
    workspaceRemoveFadeConfig->overridden      = true;
    workspaceRemoveFadeConfig->internalBezier  = OVERVIEW_REMOVE_FADE_BEZIER;
    workspaceRemoveFadeConfig->internalSpeed   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalSpeed : 10.F;
    workspaceRemoveFadeConfig->internalEnabled = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalEnabled : 1;
    workspaceRemoveFadeConfig->internalStyle   = WINDOWSMOVEVALUES ? WINDOWSMOVEVALUES->internalStyle : "";
    workspaceRemoveFadeConfig->pValues         = workspaceRemoveFadeConfig;

    Animation::mgr()->createAnimation(1.F, scale, overviewAnimationConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(0.F, transitionProgress, overviewAnimationConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation({}, viewOffset, overviewAnimationConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, workspaceInsertProgress, overviewAnimationConfig, AVARDAMAGE_NONE);
    Animation::mgr()->createAnimation(1.F, workspaceInsertFadeProgress, workspaceInsertFadeConfig, AVARDAMAGE_NONE);

    if (isCanvasDesktop() && pMonitor && g_canvasCameraBookmarks.contains(pMonitor->m_name))
        viewOffset->setValueAndWarp(g_canvasCameraBookmarks.at(pMonitor->m_name));
    else if (isCanvasDesktop() && pMonitor && ScrollOverview::Config::getCanvasRememberLayout()) {
        if (const auto CAMERA = SpatialOverview::Memory::camera(pMonitor->m_name))
            viewOffset->setValueAndWarp(*CAMERA);
    }

    scale->setUpdateCallback([this](auto) { damage(); });
    transitionProgress->setUpdateCallback([this](auto) { damage(); });
    viewOffset->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertProgress->setUpdateCallback([this](auto) { damage(); });
    workspaceInsertFadeProgress->setUpdateCallback([this](auto) { damage(); });

    if (!swipe) {
        *scale = isCanvasDesktop() ? (canvasNavigationActive ? ScrollOverview::Config::getCanvasInitialZoom() : 1.F) : ScrollOverview::Config::getScale();
        if (isCanvasDesktop())
            *transitionProgress = canvasNavigationActive ? 1.F : 0.F;
    }

    const auto initialFullscreenWindow =
        PMONITOR && PMONITOR->m_activeWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(PMONITOR->m_activeWorkspace)) : PHLWINDOW{};
    emitFullscreenVisibilityState(initialFullscreenWindow ? initialFullscreenWindow : Desktop::focusState()->window(), true);

    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    auto onMouseMove = [this](Vector2D, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        const auto INPUTOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());

        if (closing || (g_pointerGrabOverview && g_pointerGrabOverview != this) || (!g_pointerGrabOverview && INPUTOVERVIEW.get() != this))
            return;

        markCanvasCameraActive(pMonitor.lock());

        const bool     LEFT_HANDED           = ScrollOverview::Config::getLeftHanded();
        const uint32_t MAIN_BUTTON           = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
        const bool     INVERT_DRAG_MODE      = ScrollOverview::Config::getDragMode() == 1;
        const uint32_t SECONDARY_DRAG_BUTTON = BTN_MIDDLE;
        const float    DRAGTHRESHOLD         = ScrollOverview::Config::getDragThreshold() * (pMonitor ? pMonitor->m_scale : 1.F);
        const float    DRAGTHRESHOLDSQ       = std::pow(DRAGTHRESHOLD, 2);

        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
        if (placeDragMotion(lastMousePosLocal)) {
            info.cancelled = true;
            requestInputFrame();
            return;
        }
        if (landingDrag) {
            info.cancelled = true;
            updateExperimentDrag();
            requestInputFrame();
            return;
        }

        const auto beginScrollingPanAtPoint = [&](const Vector2D& point) {
            if (layout == ScrollOverview::Config::ELayout::GRID) {
                beginScrollingPan({});
                scrollingPanLastMouseLocal = point;
                updateScrollingPan();
                return true;
            }

            auto WORKSPACE = workspaceAtOverviewPoint(point);
            if (!WORKSPACE) {
                const auto WINDOW = windowAtOverviewPoint(point);
                if (WINDOW)
                    WORKSPACE = WINDOW->m_workspace;
            }

            if (!isWorkspaceScrolling(WORKSPACE))
                return false;

            beginScrollingPan(WORKSPACE);
            scrollingPanLastMouseLocal = point;
            updateScrollingPan();
            return true;
        };

        if (!dragPendingPrimary && !resizePointerDown && !scrollingPanPointerDown && !dragActiveWindow && !resizeActiveWindow && isPointerOnTopLayer(pMonitor.lock())) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
            return;
        }

        if (canvasArrangeButtonPressed) {
            info.cancelled = true;
            requestInputFrame();
            return;
        }

        info.cancelled = true;
        requestInputFrame();

        if (submapMouseClickPending) {
            const bool DRAGTHRESHOLDREACHED = dragStartMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ;

            if (submapMouseClickButton == MAIN_BUTTON && !dragActiveWindow && !scrollingPanPointerDown && DRAGTHRESHOLDREACHED) {
                submapMouseClickPending = false;
                submapMouseClickButton  = 0;

                if (!INVERT_DRAG_MODE)
                    beginWindowDrag(windowAtOverviewPoint(dragStartMouseLocal));
                else
                    beginScrollingPanAtPoint(dragStartMouseLocal);
            }

            if (submapMouseClickButton == SECONDARY_DRAG_BUTTON && !INVERT_DRAG_MODE && !scrollingPanPointerDown && DRAGTHRESHOLDREACHED) {
                if (beginScrollingPanAtPoint(dragStartMouseLocal)) {
                    submapMouseClickPending = false;
                    submapMouseClickButton  = 0;
                }
            }

            if (submapMouseClickButton == SECONDARY_DRAG_BUTTON && INVERT_DRAG_MODE && !dragActiveWindow && DRAGTHRESHOLDREACHED) {
                submapMouseClickPending = false;
                submapMouseClickButton  = 0;
                beginWindowDrag(windowAtOverviewPoint(dragStartMouseLocal));
            }
        }

        if (dragPendingPrimary) {
            if (!dragActiveWindow && !scrollingPanPointerDown && dragStartMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ) {
                if (isCanvasDesktop() && spacePanHeld)
                    beginScrollingPanAtPoint(dragStartMouseLocal);
                else if (!INVERT_DRAG_MODE) {
                    // In the navigator, empty canvas is something you grab
                    // and throw around, like a map.
                    const auto WINDOW = windowAtOverviewPoint(dragStartMouseLocal);
                    if (!WINDOW && navigatorOwnsPointer())
                        beginScrollingPanAtPoint(dragStartMouseLocal);
                    else
                        beginWindowDrag(WINDOW);
                } else
                    beginScrollingPanAtPoint(dragStartMouseLocal);
            }
        }

        if (dragActiveWindow)
            updateWindowDrag();

        if (resizePointerDown && resizePendingWindow) {
            if (!resizeActiveWindow && resizeStartMouseLocal.distanceSq(lastMousePosLocal) > DRAGTHRESHOLDSQ)
                beginWindowResize();

            if (resizeActiveWindow)
                updateWindowResize();
        }

        if (scrollingPanPointerDown)
            updateScrollingPan();

        if (navigatorOwnsPointer())
            updateNavigatorHover();
        else if (isCanvasDesktop() && !dragPendingPrimary && !resizePointerDown && !scrollingPanPointerDown && !dragActiveWindow && !resizeActiveWindow)
            forwardCanvasPointerMotion();

        //  highlightHoverDebug();
    };

    auto onTouchMove = [this](ITouch::SMotionEvent, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        markCanvasCameraActive(pMonitor.lock());
        info.cancelled    = true;
        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());
        requestInputFrame();
    };

    auto onMouseButton = [this](IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
        if (info.cancelled || sessionLocked())
            return;

        const bool FORWARDEDTOPLAYERRELEASE =
            event.state == WL_POINTER_BUTTON_STATE_RELEASED && g_topLayerPointerButtons.contains(event.button);
        const auto INPUTOVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if (closing || (g_pointerGrabOverview && g_pointerGrabOverview != this) ||
            (!g_pointerGrabOverview && INPUTOVERVIEW.get() != this && !FORWARDEDTOPLAYERRELEASE))
            return;

        markCanvasCameraActive(pMonitor.lock());

        const bool RELEASESPOINTERGRAB = event.state == WL_POINTER_BUTTON_STATE_RELEASED;
        auto       releasePointerGrab  = Hyprutils::Utils::CScopeGuard([this, RELEASESPOINTERGRAB] {
            if (RELEASESPOINTERGRAB && g_pointerGrabOverview == this)
                g_pointerGrabOverview = nullptr;
        });

        const bool POINTERONTOPLAYER =
            !dragPendingPrimary && !resizePointerDown && !scrollingPanPointerDown && !dragActiveWindow && !resizeActiveWindow && isPointerOnTopLayer(pMonitor.lock());

        if (FORWARDEDTOPLAYERRELEASE ||
            (event.state == WL_POINTER_BUTTON_STATE_PRESSED && POINTERONTOPLAYER && usesSubmapKeybinds && isOverviewSubmapActive())) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;

            info.cancelled = true;
            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
                g_topLayerPointerButtons.emplace(event.button);
            else
                g_topLayerPointerButtons.erase(event.button);

            g_pSeatManager->sendPointerButton(event.timeMs, event.button, event.state);
            g_pSeatManager->sendPointerFrame();
            return;
        }

        if (POINTERONTOPLAYER) {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
            return;
        }

        if (isCanvasDesktop()) {
            lastMousePosLocal         = getOverviewMousePosLocal(pMonitor.lock());
            const bool LEFT_HANDED    = ScrollOverview::Config::getLeftHanded();
            const uint32_t MAIN       = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
            const uint32_t SECONDARY  = BTN_MIDDLE;
            const auto MODS           = g_pInputManager->getModsFromAllKBs();
            const bool WINDOWGESTURE  = MODS & (HL_MODIFIER_META | HL_MODIFIER_ALT);

            const auto MONITOR = pMonitor.lock();
            const auto RAWLOCAL = MONITOR ? (g_pInputManager->getMouseCoordsInternal() - MONITOR->m_position) * MONITOR->m_scale : lastMousePosLocal;
            const auto ARRANGEBUTTON = canvasArrangeButtonBox();
            // The minimap chrome is composed after the barrel lens, so its hit
            // target must use raw screen coordinates rather than the inverse-
            // warped point used for application windows beneath it.
            const bool ARRANGEBUTTONHIT = !ARRANGEBUTTON.empty() && ARRANGEBUTTON.containsPoint(RAWLOCAL);
            if (handleExperimentClick(event.button, event.state, RAWLOCAL)) {
                info.cancelled = true;
                requestInputFrame();
                return;
            }

            if ((event.state == WL_POINTER_BUTTON_STATE_PRESSED && event.button == MAIN && ARRANGEBUTTONHIT) ||
                (event.state == WL_POINTER_BUTTON_STATE_RELEASED && event.button == MAIN && canvasArrangeButtonPressed)) {
                info.cancelled = true;
                if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    canvasArrangeButtonPressed = true;
                    g_pointerGrabOverview      = this;
                } else {
                    canvasArrangeButtonPressed = false;
                    if (ARRANGEBUTTONHIT)
                        arrangeCanvasWindows();
                }
                requestInputFrame();
                return;
            }

            if (event.button == MAIN && ScrollOverview::Config::getCanvasSpacePan() && spacePanHeld) {
                info.cancelled = true;
                if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                    g_pointerGrabOverview = this;
                    beginScrollingPan({});
                } else if (scrollingPanPointerDown)
                    endScrollingPan();
                return;
            }

            if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && event.button == MAIN && placeDragRelease()) {
                info.cancelled = true;
                requestInputFrame();
                return;
            }
            if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && event.button == (LEFT_HANDED ? BTN_LEFT : BTN_RIGHT) &&
                placeRightRelease(lastMousePosLocal, !resizeActiveWindow.expired())) {
                info.cancelled           = true;
                resizePointerDown        = false;
                submapMouseClickPending  = false;
                submapMouseClickButton   = 0;
                resizePendingWindow.reset();
                requestInputFrame();
                return;
            }

            if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && navigatorSwallowedButtons.erase(event.button)) {
                info.cancelled = true;
                return;
            }

            if (event.button == MAIN && event.state == WL_POINTER_BUTTON_STATE_PRESSED && showsNavigatorHud()) {
                PHLWINDOW ROWWINDOW;
                const int HIT = SpatialOverview::Hud::paletteHit(RAWLOCAL, &ROWWINDOW);
                if (HIT != SpatialOverview::Hud::PALETTE_MISS) {
                    info.cancelled = true;
                    navigatorSwallowedButtons.emplace(event.button);
                    if (HIT >= 0 && ROWWINDOW)
                        landOnWindow(ROWWINDOW);
                    requestInputFrame();
                    return;
                }
            }

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED && placeMenuOpen() && placeMenuPress(lastMousePosLocal, event.button == MAIN)) {
                info.cancelled = true;
                navigatorSwallowedButtons.emplace(event.button);
                requestInputFrame();
                return;
            }

            if (event.button == MAIN && event.state == WL_POINTER_BUTTON_STATE_PRESSED && (MODS & HL_MODIFIER_META) && !clientGestureButton &&
                canvasSnapDoubleClick(windowAtOverviewCursor(), event.timeMs)) {
                info.cancelled = true;
                navigatorSwallowedButtons.emplace(event.button);
                requestInputFrame();
                return;
            }

            if (event.button == MAIN && event.state == WL_POINTER_BUTTON_STATE_PRESSED && !clientGestureButton) {
                if (const int PLACE = placeCloseAt(lastMousePosLocal); PLACE) {
                    info.cancelled = true;
                    navigatorSwallowedButtons.emplace(event.button);
                    removePlace(PLACE);
                    damage();
                    requestInputFrame();
                    return;
                }
                if (placeDragPress(lastMousePosLocal)) {
                    info.cancelled        = true;
                    g_pointerGrabOverview = this;
                    requestInputFrame();
                    return;
                }
            }

            if (event.button == (LEFT_HANDED ? BTN_LEFT : BTN_RIGHT) && event.state == WL_POINTER_BUTTON_STATE_PRESSED && MONITOR && !WINDOWGESTURE && !clientGestureButton &&
                ScrollOverview::Config::getCanvasPlaces() &&
                !(showsNavigatorHud() && SpatialOverview::Hud::paletteHit(RAWLOCAL) != SpatialOverview::Hud::PALETTE_MISS)) {
                const int  BADGE = placeBadgeAt(lastMousePosLocal);
                const auto WORLD = canvasScreenToWorld(CBox{MONITOR->m_position + lastMousePosLocal / MONITOR->m_scale, {}}, false);
                if (BADGE)
                    openPlaceMenu(BADGE, lastMousePosLocal);
                // On a window, a click (no drag) opens the window's menu on
                // release; a drag still resizes.
                const auto CLICKED  = BADGE ? PHLWINDOW{} : windowAtOverviewCursor();
                const bool ONWINDOW = CLICKED != nullptr;
                if (ONWINDOW && navigatorOwnsPointer())
                    placeRightPress(CLICKED, lastMousePosLocal);
                if (BADGE || (WORLD && !ONWINDOW && placeClick(WORLD->pos(), lastMousePosLocal))) {
                    info.cancelled = true;
                    navigatorSwallowedButtons.emplace(event.button);
                    requestInputFrame();
                    return;
                }
            }

            if (clientGestureButton) {
                // A move/resize the app asked for owns the pointer until its
                // button comes back up.
                info.cancelled = true;
                if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && event.button == clientGestureButton) {
                    clientGestureButton = 0;
                    if (dragActiveWindow)
                        endWindowDrag();
                    if (resizeActiveWindow)
                        endWindowResize();
                    resizePointerDown = false;
                    resizePendingWindow.reset();
                    g_pInputManager->releaseAllMouseButtons();
                }
                requestInputFrame();
                return;
            }

            if (event.button != SECONDARY && !WINDOWGESTURE && !navigatorOwnsPointer() &&
                (event.state == WL_POINTER_BUTTON_STATE_PRESSED || canvasForwardedPointerButtons.contains(event.button))) {
                info.cancelled = forwardCanvasPointerButton(event);
                if (info.cancelled)
                    return;
            }
        }

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
            g_pointerGrabOverview = this;

        info.cancelled = true;
        Config::Actions::state()->m_lastMouseCode = event.button;
        Config::Actions::state()->m_lastCode      = 0;
        Config::Actions::state()->m_timeLastMs    = event.timeMs;
        // Without releasing buttons, mouse-triggered overview consumes release
        // events
        // before they reach Hyprland's input manager, leaving it stuck thinking
        // buttons are still pressed, which locks focus.
        releaseTopLayerPointerButtons(event.timeMs);
        g_pInputManager->releaseAllMouseButtons();

        const bool     LEFT_HANDED        = ScrollOverview::Config::getLeftHanded();
        const uint32_t MAIN_BUTTON        = LEFT_HANDED ? BTN_RIGHT : BTN_LEFT;
        const uint32_t RESIZE_BUTTON      = LEFT_HANDED ? BTN_LEFT : BTN_RIGHT;
        const bool     INVERT_DRAG_MODE   = ScrollOverview::Config::getDragMode() == 1;
        const uint32_t SECONDARY_DRAG_BUTTON = BTN_MIDDLE;
        const auto     clearSubmapMouseClickPending = [&]() {
            submapMouseClickPending = false;
            submapMouseClickButton  = 0;
        };
        const auto     beginSubmapMouseClickPending = [&](uint32_t button) {
            if (!usesSubmapKeybinds || !isOverviewSubmapActive())
                return false;

            submapMouseClickPending = true;
            submapMouseClickButton  = button;
            dragStartMouseLocal     = lastMousePosLocal;
            return true;
        };
        const auto     shouldRunDefaultClickAction = [&](uint32_t button) {
            if (submapMouseClickPending && submapMouseClickButton == button) {
                clearSubmapMouseClickPending();
                dispatchSubmapMouseClick(button);
                return false;
            }

            return !usesSubmapKeybinds || !isOverviewSubmapActive();
        };
        const auto     performClickAction = [&](uint32_t button) {
            if (!shouldRunDefaultClickAction(button))
                return;

            if (isCanvasDesktop()) {
                size_t workspaceIdx = 0;
                const auto WINDOW = windowAtOverviewCursor(&workspaceIdx);
                if (WINDOW && navigatorOwnsPointer() && button == MAIN_BUTTON)
                    landOnWindow(WINDOW);
                else if (WINDOW)
                    selectOverviewWindow(WINDOW, workspaceIdx, false);
                return;
            }

            if (ScrollOverview::Config::getCanvasEnabled() && ScrollOverview::Config::getCanvasViewportEnabled()) {
                updateViewportWorkspaceFromCanvasCenter();
                closeAll();
                return;
            }

            selectHoveredWorkspace();
            selectWindowAtOverviewCursor(true);
            closeAll();
        };
        const auto     finishWindowDragOrClick = [&](uint32_t button, bool allowClick) {
            const float CLICK_MAX_DRAG_DISTANCE = 10.F * (pMonitor ? pMonitor->m_scale : 1.F);

            if (dragActiveWindow) {
                if (dragStartMouseLocal.distanceSq(lastMousePosLocal) < CLICK_MAX_DRAG_DISTANCE * CLICK_MAX_DRAG_DISTANCE) {
                    clearDragPending();

                    if (allowClick)
                        performClickAction(button);
                    else
                        clearSubmapMouseClickPending();

                    return;
                }

                endWindowDrag();
                clearSubmapMouseClickPending();
                return;
            }

            clearDragPending();

            if (allowClick)
                performClickAction(button);
            else
                clearSubmapMouseClickPending();
        };
        const auto     workspaceAtPanPoint = [&](const Vector2D& point) {
            auto WORKSPACE = workspaceAtOverviewPoint(point);
            if (WORKSPACE)
                return WORKSPACE;

            const auto WINDOW = windowAtOverviewPoint(point);
            return WINDOW ? WINDOW->m_workspace : PHLWORKSPACE{};
        };

        if (event.button == MAIN_BUTTON) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (beginSubmapMouseClickPending(event.button))
                    return;

                dragPendingPrimary  = true;
                dragStartMouseLocal = lastMousePosLocal;
                return;
            }

            const bool WASPANNING = scrollingPanPointerDown;
            if (scrollingPanPointerDown)
                endScrollingPan();

            finishWindowDragOrClick(event.button, !WASPANNING);
            return;
        }

        if (event.button == SECONDARY_DRAG_BUTTON && !INVERT_DRAG_MODE) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                if (beginSubmapMouseClickPending(event.button))
                    return;

                const auto WORKSPACE = workspaceAtPanPoint(lastMousePosLocal);

                if (layout == ScrollOverview::Config::ELayout::GRID) {
                    beginScrollingPan({});
                    return;
                }

                if (!isWorkspaceScrolling(WORKSPACE)) {
                    scrollingPanPointerDown = false;
                    return;
                }

                beginScrollingPan(WORKSPACE);
                return;
            }

            if (submapMouseClickPending && submapMouseClickButton == event.button) {
                const bool WASPANNING = scrollingPanPointerDown;
                if (scrollingPanPointerDown)
                    endScrollingPan();

                if (!WASPANNING)
                    shouldRunDefaultClickAction(event.button);
                else
                    clearSubmapMouseClickPending();

                return;
            }

            if (scrollingPanPointerDown)
                endScrollingPan();
            return;
        }

        if (event.button == RESIZE_BUTTON) {
            lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

            if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                beginSubmapMouseClickPending(event.button);

                size_t resizeWorkspace = 0;
                const auto window      = windowAtOverviewCursor(&resizeWorkspace);
                if (!shouldShowOverviewWindow(window) || shouldShowPinnedFloatingOverviewWindow(window)) {
                    resizePointerDown = false;
                    resizePendingWindow.reset();
                    return;
                }

                const auto MONITOR = pMonitor.lock();
                if (!MONITOR)
                    return;

                const auto WORKSPACEOFFSET =
                    workspaceOverviewOffset(resizeWorkspace, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
                const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);

                resizePointerDown    = true;
                resizeStartMouseLocal = lastMousePosLocal;
                resizePendingWindow   = window;
                resizeWorkspaceIdx    = resizeWorkspace;
                resizeCorner          = Layout::cornerFromBox(WINDOWBOX, lastMousePosLocal);
                return;
            }

            if (resizeActiveWindow) {
                endWindowResize();
                clearSubmapMouseClickPending();
                return;
            }

            resizePointerDown = false;
            resizePendingWindow.reset();

            shouldRunDefaultClickAction(event.button);
            return;
        }

        if (event.button != SECONDARY_DRAG_BUTTON || !INVERT_DRAG_MODE)
            return;

        lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

        if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (beginSubmapMouseClickPending(event.button))
                return;

            dragStartMouseLocal = lastMousePosLocal;
            beginWindowDrag(windowAtOverviewCursor());
            return;
        }

        finishWindowDragOrClick(event.button, true);
    };

    auto onCursorSelect = [this](auto, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        if (closing || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        if (isPointerOnTopLayer(pMonitor.lock()))
            return;

        info.cancelled = true;

        selectWindowAtOverviewCursor(true);

        closeAll();
    };

    auto onMouseAxis = [this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (info.cancelled || closing || sessionLocked() || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        markCanvasCameraActive(pMonitor.lock());

        if (isCanvasDesktop()) {
            info.cancelled = true;
            const auto MODS      = g_pInputManager->getModsFromAllKBs();
            const bool NAVIGATOR = navigatorOwnsPointer();
            const bool WHEEL     = e.source == WL_POINTER_AXIS_SOURCE_WHEEL || e.source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
            const bool ZOOM      = (MODS & HL_MODIFIER_CTRL) || (NAVIGATOR && WHEEL);
            if (ZOOM && (!isPersistentCanvas() || canvasNavigationActive) && e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL && e.delta != 0.0) {
                const float DIRECTION = e.delta > 0.0 ? -1.F : 1.F;
                const float FACTOR    = std::exp(DIRECTION * ScrollOverview::Config::getCanvasZoomStep());
                zoomCanvasAt(getOverviewMousePosLocal(pMonitor.lock()), scale->value() * FACTOR);
                updateNavigatorHover();
            } else if (NAVIGATOR) {
                // Two-finger scrolling slides the map, Figma-style.
                const float ZOOMNOW = std::max(scale->value(), 0.01F);
                const float DELTA   = sc<float>(e.delta) * ScrollOverview::Config::getTouchpadScrollFactor() / ZOOMNOW;
                const auto  MOVE    = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? Vector2D{0.F, DELTA} : Vector2D{DELTA, 0.F};
                viewOffset->setValueAndWarp(viewOffset->value() + MOVE);
                markBlurDirty();
                damage();
                updateNavigatorHover();
            } else
                forwardCanvasPointerAxis(e);
            return;
        }

        if (usesSubmapKeybinds && isOverviewSubmapActive() && hasApplicableScrollKeybind(e)) {
            if (scrollKeybindIsThrottled())
                info.cancelled = true;
            return;
        }

        info.cancelled = true;

        const auto ACTION = e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? ScrollOverview::Config::getHorizontalScrollAction(layout) :
                                                                          ScrollOverview::Config::getVerticalScrollAction(layout);

        // mouse wheel: discrete stepping, throttled by scroll_event_delay so one notch is one step
        if (e.source == WL_POINTER_AXIS_SOURCE_WHEEL) {
            if (e.delta == 0.0)
                return;
            trackpadScrollAccum        = 0.0;
            trackpadWorkspaceFollowing = false;
            trackpadTapeFollowing      = false;
            if (!scrollStepAllowed(e.timeMs))
                return;

            if (ACTION == ScrollOverview::Config::EScrollAction::WORKSPACE)
                moveViewportWorkspace(e.delta > 0);
            else
                moveScrollingColumnSelection(e.delta > 0);

            return;
        }

        if (images.empty() || viewportCurrentWorkspace >= images.size())
            return;

        // scroll workspace or layout with 1:1 animation (snaping on release)
        if (ACTION == ScrollOverview::Config::EScrollAction::WORKSPACE)
            trackpadSwipeWorkspace(e.delta);
        else
            trackpadSwipeLayout(images[viewportCurrentWorkspace]->pWorkspace, e.delta);
    };

    auto onWindowOpen = [this](PHLWINDOW window) {
        if (closing)
            return;

        if (isCanvasDesktop() && window && window->m_monitor == pMonitor) {
            notePlaceWindow(window, true);
            manageCanvasWindow(window, true);
            followCanvasWindow(window, false);
            noteCanvasLayoutChanged();
        }

        if (sharedStateOwner && SpatialOverview::Navigator::isOpen())
            SpatialOverview::Navigator::rebuildResults(false);

        rebuildPending = true;
        damage();
    };

    auto onWindowClose = [this](PHLWINDOW window) {
        if (closing)
            return;

        if (window) {
            notePlaceWindow(window, false);
            g_canvasNativeLayout.erase(window->m_stableID);
            g_canvasSpatialLayout.erase(window->m_stableID);
            if (window->layoutTarget()) g_canvasManagedTargets.erase(window->layoutTarget().get());
            if (navigatorHoverWindow.lock() == window)
                navigatorHoverWindow.reset();
        }

        rebuildPending = true;
        damage();
    };

    auto onWindowMove = [this](PHLWINDOW, PHLWORKSPACE) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onWindowActive = [this](PHLWINDOW window, Desktop::eFocusReason) {
        if (closing)
            return;

        unconstrainCanvasWindows();

        const auto overviewWindow = getOverviewWindowToShow(window);
        const auto fullscreenWindow = overviewWindow && overviewWindow->m_workspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(overviewWindow->m_workspace)) : PHLWINDOW{};

        if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == overviewWindow->m_workspace && overviewWindow->m_isFloating)
            emitFullscreenVisibilityState(fullscreenWindow, true);
        else
            emitFullscreenVisibilityState(overviewWindow, true);

        if (shouldShowOverviewWindow(overviewWindow) && overviewWindow->m_monitor == pMonitor) {
            rebuildPending = true;
            closeOnWindow  = overviewWindow;
            rememberSelection(overviewWindow);

            for (size_t i = 0; i < images.size(); ++i) {
                if (images[i]->pWorkspace == overviewWindow->m_workspace) {
                    viewportCurrentWorkspace = i;
                    break;
                }
            }
        }

        // Focus changes initiated by the canvas already move the intended
        // camera explicitly. External activation (dock instance selection,
        // launcher focus, task switchers) follows the selected window with the
        // camera under the top-layer pointer, falling back to the window's
        // owning output when no dock/menu is involved.
        if (isCanvasDesktop() && shouldShowOverviewWindow(overviewWindow) && g_canvasInternalFocusDepth == 0) {
            auto cameraOverview = scrollOverviewForMonitor(overviewWindow->m_monitor.lock());
            if (g_pInputManager) {
                const auto pointerOverview = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
                const auto pointerMonitor  = pointerOverview ? pointerOverview->pMonitor.lock() : PHLMONITOR{};
                if (pointerMonitor && isPointerOnTopLayer(pointerMonitor))
                    cameraOverview = pointerOverview;
            }

            // A window already on screen stays where you see it.
            if (cameraOverview.get() == this && !canvasWindowOnScreen(overviewWindow))
                followCanvasWindow(overviewWindow, false);
        }

        damage();
    };

    auto onWindowFullscreen = [this](PHLWINDOW window) {
        if (closing || emittingFullscreenVisibilityState || g_canvasRestoring)
            return;

        window = getOverviewWindowToShow(window);
        if (!window || window->m_monitor != pMonitor || !Fullscreen::controller()->isFullscreen(window))
            return;

        if (isCanvasDesktop())
            return; // canvasOnWindowFullscreen
        emitFullscreenVisibilityState(window, true);
    };

    auto onWorkspaceLifecycle = [this](auto) {
        if (closing)
            return;

        rebuildPending = true;
        damage();
    };

    auto onKeyboardKey = [this](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        const auto KEYSYM = getOverviewKeysym(event);
        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED &&
            (KEYSYM == XKB_KEY_Super_L || KEYSYM == XKB_KEY_Super_R || KEYSYM == XKB_KEY_Meta_L || KEYSYM == XKB_KEY_Meta_R))
            g_canvasKeyboardNavigationMonitor.reset();

        if (sessionLocked()) {
            SpatialOverview::Navigator::stopRepeat();
            return;
        }

        // Release bookkeeping is global: it must happen even if the press was
        // handled by another output's canvas or a panel has taken focus since.
        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED) {
            SpatialOverview::Navigator::stopRepeat(event.keycode);
            if ((KEYSYM == XKB_KEY_Alt_L || KEYSYM == XKB_KEY_Alt_R || KEYSYM == XKB_KEY_Meta_L || KEYSYM == XKB_KEY_Meta_R) && SpatialOverview::Navigator::isOpen() &&
                SpatialOverview::Navigator::state().switcher) {
                if (auto* canvas = activeCanvasOverview())
                    canvas->finishSwitcher();
            }
            if (SpatialOverview::Navigator::takeConsumed(event.keycode)) {
                info.cancelled = true;
                return;
            }
        } else if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED)
            SpatialOverview::Navigator::takeConsumed(event.keycode); // a fresh press means its old release was lost

        if (!closing && isCanvasDesktop() && placeMenuKey(event, KEYSYM, g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2))) {
            info.cancelled = true;
            return;
        }

        if (closing || activeScrollOverview().get() != this)
            return;

        if (isTopLayerFocused(pMonitor.lock()))
            return;

        // A panel can disappear while the seat still references its now-
        // unmapped layer surface. Reconcile that split before Hyprland forwards
        // this key, so the very first keystroke after returning to a terminal
        // reaches the active application instead of being dropped.
        if (isCanvasDesktop())
            ensureCanvasKeyboardFocus();

        const auto MODS   = g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2);

        if (isCanvasDesktop() && handleNavigatorKey(event, KEYSYM, MODS)) {
            info.cancelled = true;
            return;
        }

        if (isCanvasDesktop() && ScrollOverview::Config::getCanvasSpacePan() && KEYSYM == XKB_KEY_space) {
            spacePanHeld = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
            info.cancelled = true;
            if (!spacePanHeld && scrollingPanPointerDown)
                endScrollingPan();
            return;
        }

        if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        if (isCanvasDesktop()) {
            if (handleExperimentKey(KEYSYM, MODS))
                info.cancelled = true;
            return;
        }

        if ((KEYSYM == XKB_KEY_Return || KEYSYM == XKB_KEY_KP_Enter || KEYSYM == XKB_KEY_Left || KEYSYM == XKB_KEY_KP_Left || KEYSYM == XKB_KEY_Right ||
             KEYSYM == XKB_KEY_KP_Right || KEYSYM == XKB_KEY_Up || KEYSYM == XKB_KEY_KP_Up || KEYSYM == XKB_KEY_Down || KEYSYM == XKB_KEY_KP_Down) &&
            MODS != 0)
            return;

        switch (KEYSYM) {
            case XKB_KEY_Left:
            case XKB_KEY_KP_Left:
                moveSelection("left");
                break;
            case XKB_KEY_Right:
            case XKB_KEY_KP_Right:
                moveSelection("right");
                break;
            case XKB_KEY_Up:
            case XKB_KEY_KP_Up:
                moveSelection("up");
                break;
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down:
                moveSelection("down");
                break;
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter: closeAll(); break;
            default: return;
        }

        info.cancelled = true;
    };

    auto onPinchBegin = [this](IPointer::SPinchBeginEvent, Event::SCallbackInfo& info) {
        if (sessionLocked())
            return;
        if (closing || !isCanvasDesktop() || (isPersistentCanvas() && !canvasNavigationActive) ||
            scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        canvasPinching       = true;
        canvasPinchStartZoom = scale->value();
        info.cancelled       = true;
    };

    auto onPinchUpdate = [this](IPointer::SPinchUpdateEvent event, Event::SCallbackInfo& info) {
        if (closing || !isCanvasDesktop() || !canvasPinching || scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()).get() != this)
            return;

        info.cancelled = true;
        zoomCanvasAt(getOverviewMousePosLocal(pMonitor.lock()), canvasPinchStartZoom * sc<float>(event.scale));
    };

    auto onPinchEnd = [this](IPointer::SPinchEndEvent, Event::SCallbackInfo& info) {
        if (!canvasPinching)
            return;

        canvasPinching = false;
        info.cancelled = isCanvasDesktop();
    };

    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen(onMouseMove);
    touchMoveHook = Event::bus()->m_events.input.touch.motion.listen(onTouchMove);
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    pinchBeginHook = Event::bus()->m_events.gesture.pinch.begin.listen(onPinchBegin);
    pinchUpdateHook = Event::bus()->m_events.gesture.pinch.update.listen(onPinchUpdate);
    pinchEndHook = Event::bus()->m_events.gesture.pinch.end.listen(onPinchEnd);

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    touchDownHook   = Event::bus()->m_events.input.touch.down.listen(onCursorSelect);

    windowOpenHook      = Event::bus()->m_events.window.open.listen(onWindowOpen);
    windowCloseHook     = Event::bus()->m_events.window.close.listen(onWindowClose);
    windowMoveHook      = Event::bus()->m_events.window.moveToWorkspace.listen(onWindowMove);
    windowActiveHook    = Event::bus()->m_events.window.active.listen(onWindowActive);
    windowFullscreenHook = Event::bus()->m_events.window.fullscreen.listen(onWindowFullscreen);
    workspaceCreatedHook = Event::bus()->m_events.workspace.created.listen(onWorkspaceLifecycle);
    workspaceRemovedHook = Event::bus()->m_events.workspace.removed.listen(onWorkspaceLifecycle);
    activateSubmapIfConfigured();
    if (isCanvasDesktop() || !usesSubmapKeybinds)
        keyboardKeyHook = Event::bus()->m_events.input.keyboard.key.listen(onKeyboardKey);

    // The canvas desktop at 100% leaves the cursor to the apps under it.
    if (!isCanvasDesktop() || canvasNavigationActive)
        setCanvasCursor("left_ptr");

    redrawAll();

    if (sharedStateOwner) {
        if (!g_canvasMemoryStarted && isCanvasDesktop()) {
            g_canvasMemoryStarted     = true;
            g_canvasMemoryRestoreUntil = Time::steadyNow() + std::chrono::seconds(45);
        }
        seedCanvasWindows();
        if (SpatialOverview::Experiments::on(ECanvasExperiment::Persist))
            loadSharedCanvasLayout();
    }

    rememberSelection(Desktop::focusState()->window());
    viewportCurrentWorkspace = activeWorkspaceIndex();
    syncSelectionToViewport();
}

static void renderOverviewLayerLevel(PHLMONITOR monitor, uint32_t layer, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F) {
    if (!monitor)
        return;

    bool pushedRenderHints = false;
    const bool MODULATEALPHA = alpha < 0.999F;

    for (auto const& ls : monitor->m_layerSurfaceLayers[layer]) {
        const auto LAYER = ls.lock();
        if (!Desktop::View::validMapped(LAYER))
            continue;

        if (!pushedRenderHints) {
            Render::SRenderModifData modif;
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, renderScale);
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());

            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            pushedRenderHints = true;
        }

		auto& lsAlpha = LAYER->alpha()[Desktop::View::LS_ALPHA_FADE];
        float previousAlpha = 1.F;
        if (MODULATEALPHA && lsAlpha->value()) {
			previousAlpha = lsAlpha->value();
			lsAlpha->setValueAndWarp(previousAlpha * std::clamp(alpha, 0.F, 1.F));
		}

        g_pHyprRenderer->renderLayer(LAYER, monitor, now);

        if (MODULATEALPHA && lsAlpha->value())
			lsAlpha->setValueAndWarp(previousAlpha);
    }

    if (pushedRenderHints)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
}

void CScrollOverview::renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha) {
    if (!monitor)
        return;

    renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, workspaceBox, renderScale, now, alpha);
}

void CScrollOverview::renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor)
        return;

    g_pHyprRenderer->renderBackground(monitor);

    for (auto const& ls : monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
        if (!Desktop::View::validMapped(ls.lock()))
            continue;

        g_pHyprRenderer->renderLayer(ls.lock(), monitor, now);
    }
}

void CScrollOverview::updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now) {
    if (!monitor || wallpaperMode == 1 || !ScrollOverview::Config::getBlur())
        return;

    const float BLURSTRENGTH = ScrollOverview::Config::getBlurStrength();
    if (lastBackdropWallpaperMode != wallpaperMode) {
        backdropBlurDirty         = true;
        lastBackdropWallpaperMode = wallpaperMode;
    }
    if (std::abs(lastBackdropBlurStrength - BLURSTRENGTH) > 0.001F) {
        backdropBlurDirty         = true;
        lastBackdropBlurStrength  = BLURSTRENGTH;
    }

    const auto FBSIZE     = monitor->m_pixelSize;
    const auto RENDERSIZE = monitor->m_transformedSize;
    const auto FBFORMAT   = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
    if (!backdropBlurFB)
        backdropBlurFB = g_pHyprRenderer->createFB("spatialoverview_backdrop_blur");

    if (!backdropBlurFB || !backdropBlurFB->isAllocated() || backdropBlurFB->m_size != FBSIZE || backdropBlurFB->m_drmFormat != FBFORMAT) {
        if (backdropBlurFB)
            backdropBlurFB->release();
        if (!backdropBlurFB || !backdropBlurFB->alloc(sc<int>(FBSIZE.x), sc<int>(FBSIZE.y), FBFORMAT))
            return;
        backdropBlurDirty = true;
    }

    if (!backdropBlurDirty)
        return;

    if (g_pHyprRenderer->m_renderData.currentFB)
        backdropBlurFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());

    const CRegion fullDamage{CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}};

    {
        auto bindBackdrop = g_pHyprRenderer->bindTempFB(backdropBlurFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, fullDamage);
        renderGlobalWallpaper(monitor, now);
        OverviewRender::flushPass(monitor);
    }

    // The blend controls transition timing; repeated cached passes give the
    // upper end of the slider a materially larger blur radius as well. The
    // wallpaper cache is static, so this cost is paid only when it is dirty.
    const int ITERATIONS = 1 + sc<int>(std::round(BLURSTRENGTH * 3.F));
    for (int iteration = 0; iteration < ITERATIONS; ++iteration) {
        auto blurDamage = fullDamage;
        const auto BLURREDTEX = g_pHyprRenderer->blurFramebuffer(backdropBlurFB, 1.F, &blurDamage);
        if (!BLURREDTEX || !BLURREDTEX->m_size.x || !BLURREDTEX->m_size.y)
            return;

        auto bindBackdrop = g_pHyprRenderer->bindTempFB(backdropBlurFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 0.F}}, fullDamage);

        const auto SAVEDTRANSFORM = BLURREDTEX->m_transform;
        BLURREDTEX->m_transform   = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
        auto restoreTransform     = Hyprutils::Utils::CScopeGuard([BLURREDTEX, SAVEDTRANSFORM] { BLURREDTEX->m_transform = SAVEDTRANSFORM; });

        g_pHyprRenderer->pushMonitorTransformEnabled(true);
        auto restoreMonitorTransform = Hyprutils::Utils::CScopeGuard([] { g_pHyprRenderer->popMonitorTransformEnabled(); });

        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex    = BLURREDTEX,
                .box    = CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y},
                .damage = fullDamage,
            },
            fullDamage);
    }

    backdropBlurDirty = false;
}

CScrollOverview::SBackdropTransform CScrollOverview::canvasBackdropTransform(PHLMONITOR monitor, float renderScale) const {
    SBackdropTransform result;
    if (!monitor || !isCanvasDesktop() || !SpatialOverview::Tuning::flag("parallax:enabled"))
        return result;
    // At rest the wallpaper stays exactly where the desktop has it, unless
    // parallax is wanted there too; the zoom-out eases it into its layer.
    const float PROGRESS = SpatialOverview::Tuning::flag("parallax:desktop") ? 1.F : overviewProgress();
    if (PROGRESS <= 0.001F)
        return result;

    const double STRENGTH = SpatialOverview::Tuning::number("parallax:strength");
    const double DEPTH    = SpatialOverview::Tuning::number("parallax:depth");
    const double ZOOM     = std::clamp<double>(renderScale, 0.01, 4.0);
    // The camera is viewOffset from the monitor's home; windows move by that
    // times the zoom, the wallpaper by a fraction of it.
    result.offset = -viewOffset->value() * ZOOM * monitor->m_scale * STRENGTH * PROGRESS;
    result.scale  = sc<float>(std::max(0.3, 1.0 + (std::pow(ZOOM, DEPTH) - 1.0) * PROGRESS));
    result.active = result.offset.size() > 0.25 || std::abs(result.scale - 1.F) > 0.0005F;
    return result;
}

bool CScrollOverview::updateBackdropSharpCache(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor || !g_pHyprRenderer->m_renderData.currentFB)
        return false;

    const auto FBSIZE     = monitor->m_pixelSize;
    const auto RENDERSIZE = monitor->m_transformedSize;
    const auto FBFORMAT   = g_pHyprRenderer->m_renderData.currentFB->m_drmFormat;
    if (!backdropSharpFB)
        backdropSharpFB = g_pHyprRenderer->createFB("spatialoverview_backdrop_sharp");
    if (!backdropSharpFB)
        return false;
    if (!backdropSharpFB->isAllocated() || backdropSharpFB->m_size != FBSIZE || backdropSharpFB->m_drmFormat != FBFORMAT) {
        backdropSharpFB->release();
        if (!backdropSharpFB->alloc(sc<int>(FBSIZE.x), sc<int>(FBSIZE.y), FBFORMAT))
            return false;
        backdropSharpDirty = true;
    }

    if (backdropSharpDirty) {
        backdropSharpFB->setImageDescription(g_pHyprRenderer->m_renderData.currentFB->imageDescription());
        const CRegion fullDamage{CBox{0, 0, RENDERSIZE.x, RENDERSIZE.y}};
        auto          bindBackdrop = g_pHyprRenderer->bindTempFB(backdropSharpFB);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, fullDamage);
        renderGlobalWallpaper(monitor, now);
        OverviewRender::flushPass(monitor);
        backdropSharpDirty = false;
    }
    return backdropSharpFB->getTexture() != nullptr;
}

// The wallpaper, scaled and shifted, repeated with every other copy mirrored
// so the canvas can pan forever without a seam.
void CScrollOverview::renderBackdropTiled(PHLMONITOR monitor, const SP<Render::ITexture>& texture, float alpha, const SBackdropTransform& transform) {
    if (!monitor || !texture || alpha <= 0.001F)
        return;

    const Vector2D FULL   = monitor->m_transformedSize;
    const Vector2D TILE   = FULL * transform.scale;
    const Vector2D ORIGIN = (FULL - TILE) / 2.0 + transform.offset;
    if (TILE.x < 1.0 || TILE.y < 1.0)
        return;
    const int I0 = sc<int>(std::floor(-ORIGIN.x / TILE.x)), I1 = sc<int>(std::ceil((FULL.x - ORIGIN.x) / TILE.x)) - 1;
    const int J0 = sc<int>(std::floor(-ORIGIN.y / TILE.y)), J1 = sc<int>(std::ceil((FULL.y - ORIGIN.y) / TILE.y)) - 1;

    auto&      RENDERDATA = g_pHyprRenderer->m_renderData;
    const auto PREVIOUSTL = RENDERDATA.primarySurfaceUVTopLeft;
    const auto PREVIOUSBR = RENDERDATA.primarySurfaceUVBottomRight;
    auto       restoreUV  = Hyprutils::Utils::CScopeGuard([&RENDERDATA, PREVIOUSTL, PREVIOUSBR] {
        RENDERDATA.primarySurfaceUVTopLeft     = PREVIOUSTL;
        RENDERDATA.primarySurfaceUVBottomRight = PREVIOUSBR;
    });

    const CRegion DAMAGE{CBox{0, 0, FULL.x, FULL.y}};
    for (int j = J0; j <= J1 && j - J0 < 8; ++j) {
        for (int i = I0; i <= I1 && i - I0 < 8; ++i) {
            // Shared rounded edges, so neighbours meet without a hairline.
            const double X0 = std::round(ORIGIN.x + i * TILE.x), X1 = std::round(ORIGIN.x + (i + 1) * TILE.x);
            const double Y0 = std::round(ORIGIN.y + j * TILE.y), Y1 = std::round(ORIGIN.y + (j + 1) * TILE.y);
            const bool   FLIPX = (i % 2 + 2) % 2 == 1, FLIPY = (j % 2 + 2) % 2 == 1;
            RENDERDATA.primarySurfaceUVTopLeft     = Vector2D{FLIPX ? 1.0 : 0.0, FLIPY ? 1.0 : 0.0};
            RENDERDATA.primarySurfaceUVBottomRight = Vector2D{FLIPX ? 0.0 : 1.0, FLIPY ? 0.0 : 1.0};
            g_pHyprRenderer->draw(
                CTexPassElement::SRenderData{
                    .tex           = texture,
                    .box           = CBox{X0, Y0, X1 - X0, Y1 - Y0},
                    .a             = std::clamp(alpha, 0.F, 1.F),
                    .damage        = DAMAGE,
                    .flipEndFrame  = true,
                    .allowCustomUV = true,
                },
                DAMAGE);
        }
    }
}

void CScrollOverview::renderBackdropBlurCache(PHLMONITOR monitor, float alpha, const SBackdropTransform& transform) {
    if (!monitor || alpha <= 0.001F || !backdropBlurFB || !backdropBlurFB->isAllocated() || !backdropBlurFB->getTexture())
        return;

    const auto TEX = backdropBlurFB->getTexture();
    if (transform.active) {
        renderBackdropTiled(monitor, TEX, alpha, transform);
        return;
    }
    const CRegion fullDamage{CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y}};

    g_pHyprRenderer->draw(
        CTexPassElement::SRenderData{
            .tex      = TEX,
            .box      = CBox{0, 0, monitor->m_transformedSize.x, monitor->m_transformedSize.y},
            .a        = std::clamp(alpha, 0.F, 1.F),
            .damage   = fullDamage,
            .flipEndFrame = true,
        },
        fullDamage);
}

static void focusOverviewFullscreenWindowIfActiveWorkspace(const PHLWINDOW& fullscreenWindow_, const PHLWORKSPACE& workspace, PHLMONITOR monitor) {
    const auto FULLSCREENWINDOW = getOverviewWindowToShow(fullscreenWindow_);

    if (!monitor || !workspace || workspace != monitor->m_activeWorkspace || !validMapped(FULLSCREENWINDOW) || FULLSCREENWINDOW->m_workspace != workspace)
        return;

    if (Desktop::focusState()->window() == FULLSCREENWINDOW)
        return;

    Desktop::focusState()->fullWindowFocus(FULLSCREENWINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE, nullptr, true);
}

size_t CScrollOverview::activeWorkspaceIndex() const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn)
            return i;
    }

    return 0;
}

bool CScrollOverview::isSelectedWorkspace(const PHLWORKSPACE& workspace) const {
    return workspace && viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] &&
        images[viewportCurrentWorkspace]->pWorkspace == workspace;
}

float CScrollOverview::workspaceOverviewAxisOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto MONITOR             = pMonitor.lock();
    const auto MONITORSCALE        = MONITOR ? std::max(MONITOR->m_scale, 0.01F) : 1.F;
    const auto MONITORSIZE         = MONITOR ? axisSize(MONITOR->m_size, layout) : 0.F;
    const auto RENDERSCALE         = MONITOR && MONITORSIZE > 0 ?
        std::max(0.01F, (workspacePitch / MONITORSCALE - sc<float>(ScrollOverview::Config::getWorkspaceGap())) / sc<float>(MONITORSIZE)) :
        std::max(scale->value(), 0.01F);
    const auto LOGICALPITCH        = MONITOR ? getWorkspaceLogicalPitch(MONITOR, RENDERSCALE, layout) : workspacePitch / std::max(RENDERSCALE * MONITORSCALE, 0.01F);
    const auto RENDEREDLOGICALUNIT = RENDERSCALE * MONITORSCALE;
    const auto DEFAULTOFFSET       = workspaceOverviewLogicalOffset(workspaceIdx, activeIdx, LOGICALPITCH) * RENDEREDLOGICALUNIT;

    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return DEFAULTOFFSET;

    const auto WORKSPACEID = images[workspaceIdx]->pWorkspace->m_id;
    const auto NEWIT       = workspaceInsertTransition.newRelativeOffsets.find(WORKSPACEID);
    if (NEWIT == workspaceInsertTransition.newRelativeOffsets.end())
        return DEFAULTOFFSET;

    const float T         = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
    const float NEWOFFSET = NEWIT->second * RENDEREDLOGICALUNIT;

    if (const auto OLDIT = workspaceInsertTransition.oldRelativeOffsets.find(WORKSPACEID); OLDIT != workspaceInsertTransition.oldRelativeOffsets.end()) {
        const float OLDOFFSET = OLDIT->second * RENDEREDLOGICALUNIT;
        return OLDOFFSET + (NEWOFFSET - OLDOFFSET) * T;
    }

    return NEWOFFSET;
}

float CScrollOverview::workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    const auto EXTRAINTERVAL = [this](size_t workspaceIdx_) -> float {
        if (workspaceIdx_ + 1 >= images.size() || !images[workspaceIdx_] || !images[workspaceIdx_ + 1])
            return 0.F;

        if (layout == ScrollOverview::Config::ELayout::HORIZONTAL)
            return images[workspaceIdx_]->overflowRight + images[workspaceIdx_ + 1]->overflowLeft;

        return images[workspaceIdx_]->overflowBottom + images[workspaceIdx_ + 1]->overflowTop;
    };

    float offset = 0.F;

    if (workspaceIdx > activeIdx) {
        for (size_t i = activeIdx; i < workspaceIdx; ++i)
            offset += workspacePitch + EXTRAINTERVAL(i);
    } else {
        for (size_t i = workspaceIdx; i < activeIdx; ++i)
            offset -= workspacePitch + EXTRAINTERVAL(i);
    }

    return offset;
}

Vector2D CScrollOverview::workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    if (isCanvasDesktop())
        return {};

    if (layout != ScrollOverview::Config::ELayout::GRID)
        return axisOffsetVector(workspaceOverviewAxisOffset(workspaceIdx, activeIdx, workspacePitch), layout);

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return {};

    const int   COLUMNS = std::max(1, ScrollOverview::Config::getGridColumns());
    const float STAGGER = ScrollOverview::Config::getGridStagger();
    const auto  gridPoint = [this, COLUMNS, STAGGER](size_t index) {
        if (ScrollOverview::Config::getCanvasEnabled()) {
            const auto CELL = canvasCellForWorkspaceIndex(index);
            const int  ROW  = sc<int>(std::round(CELL.y));
            return Vector2D{CELL.x + ((ROW & 1) ? STAGGER : 0.F), CELL.y};
        }

        const int ROW = sc<int>(index / sc<size_t>(COLUMNS));
        const int COL = sc<int>(index % sc<size_t>(COLUMNS));
        return Vector2D{sc<float>(COL) + ((ROW & 1) ? STAGGER : 0.F), sc<float>(ROW)};
    };

    const auto  DELTA        = gridPoint(workspaceIdx) - gridPoint(activeIdx);
    const float RENDERSCALE  = std::max(scale->value(), 0.01F);
    const float MONITORSCALE = std::max(MONITOR->m_scale, 0.01F);
    const float GAP          = sc<float>(ScrollOverview::Config::getWorkspaceGap());
    const Vector2D PITCH{
        MONITOR->m_size.x * RENDERSCALE * MONITORSCALE + GAP * MONITORSCALE,
        MONITOR->m_size.y * RENDERSCALE * MONITORSCALE + GAP * MONITORSCALE,
    };

    return {DELTA.x * PITCH.x, DELTA.y * PITCH.y};
}

Vector2D CScrollOverview::workspaceOverviewLogicalVectorOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const {
    if (isCanvasDesktop())
        return {};

    if (layout != ScrollOverview::Config::ELayout::GRID)
        return axisOffsetVector(workspaceOverviewLogicalOffset(workspaceIdx, activeIdx, workspacePitch), layout);

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return {};

    const int   COLUMNS = std::max(1, ScrollOverview::Config::getGridColumns());
    const float STAGGER = ScrollOverview::Config::getGridStagger();
    const auto  gridPoint = [this, COLUMNS, STAGGER](size_t index) {
        if (ScrollOverview::Config::getCanvasEnabled()) {
            const auto CELL = canvasCellForWorkspaceIndex(index);
            const int  ROW  = sc<int>(std::round(CELL.y));
            return Vector2D{CELL.x + ((ROW & 1) ? STAGGER : 0.F), CELL.y};
        }

        const int ROW = sc<int>(index / sc<size_t>(COLUMNS));
        const int COL = sc<int>(index % sc<size_t>(COLUMNS));
        return Vector2D{sc<float>(COL) + ((ROW & 1) ? STAGGER : 0.F), sc<float>(ROW)};
    };

    const auto  DELTA = gridPoint(workspaceIdx) - gridPoint(activeIdx);
    const float SCALE = std::max(scale->value(), 0.01F);
    const float GAP   = sc<float>(ScrollOverview::Config::getWorkspaceGap()) / SCALE;
    return {DELTA.x * (MONITOR->m_size.x + GAP), DELTA.y * (MONITOR->m_size.y + GAP)};
}

float CScrollOverview::workspaceOverviewAlpha(size_t workspaceIdx) const {
    if (!workspaceInsertTransition.active || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return 1.F;

    if (images[workspaceIdx]->pWorkspace->m_id != workspaceInsertTransition.transitionWorkspaceID)
        return 1.F;

    if (!workspaceInsertTransition.transitionFadeIn)
        return 1.F;

    if (workspaceInsertTransition.oldRelativeOffsets.contains(workspaceInsertTransition.transitionWorkspaceID))
        return 1.F;

    return std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
}

void CScrollOverview::rebuildWorkspaceImages() {
    const auto selectedWorkspace = closeOnWindow ? closeOnWindow->m_workspace : startedOn;
    const auto selectedWindow    = closeOnWindow;
    const auto viewportWorkspace = viewportCurrentWorkspace < images.size() ? images[viewportCurrentWorkspace]->pWorkspace : startedOn;
    const auto REMOVEDWORKSPACE  = pendingRemovedWorkspace.lock();

    images.clear();

    for (const auto& w : State::workspaceState()->workspaces()) {
        const auto WORKSPACE = w.lock();
        if (!valid(WORKSPACE) || (!isCanvasDesktop() && WORKSPACE->m_monitor != pMonitor) || WORKSPACE->m_isSpecialWorkspace)
            continue;

        if (WORKSPACE == REMOVEDWORKSPACE)
            continue;

        images.emplace_back(makeShared<SWorkspaceImage>(WORKSPACE));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    if (ScrollOverview::Config::getCanvasEnabled()) {
        const int COLUMNS = std::max(1, ScrollOverview::Config::getGridColumns());
        size_t    nextSlot = 0;
        for (const auto& image : images) {
            if (!image || !image->pWorkspace || g_canvasWorkspaceCells.contains(image->pWorkspace->m_id))
                continue;

            Vector2D candidate;
            for (;;) {
                candidate = Vector2D{sc<float>(nextSlot % sc<size_t>(COLUMNS)), sc<float>(nextSlot / sc<size_t>(COLUMNS))};
                ++nextSlot;

                const bool OCCUPIED = std::ranges::any_of(images, [&](const auto& existing) {
                    if (!existing || !existing->pWorkspace)
                        return false;
                    const auto IT = g_canvasWorkspaceCells.find(existing->pWorkspace->m_id);
                    return IT != g_canvasWorkspaceCells.end() && sameCanvasCell(IT->second, candidate);
                });
                if (!OCCUPIED)
                    break;
            }

            g_canvasWorkspaceCells[image->pWorkspace->m_id] = candidate;
        }
    }

    if (images.empty()) {
        viewportCurrentWorkspace = 0;
        closeOnWindow.reset();
        return;
    }

    viewportCurrentWorkspace = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == viewportWorkspace) {
            viewportCurrentWorkspace = i;
            break;
        }
    }
    if (images[viewportCurrentWorkspace]->pWorkspace != viewportWorkspace) {
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i]->pWorkspace == selectedWorkspace) {
                viewportCurrentWorkspace = i;
                break;
            }
        }
    }

    closeOnWindow = selectedWindow;
}

void CScrollOverview::seedRememberedSelections() {
    for (const auto& img : images) {
        if (!img->pWorkspace)
            continue;

        const auto WORKSPACEID = img->pWorkspace->m_id;

        if (const auto it = rememberedSelection.find(WORKSPACEID); it != rememberedSelection.end()) {
            const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
            if (rememberedWindow && rememberedWindow->m_workspace == img->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
                continue;
        }

        const auto lastFocusedWindow = getOverviewWindowToShow(img->pWorkspace->getLastFocusedWindow());
        if (!lastFocusedWindow || lastFocusedWindow->m_workspace != img->pWorkspace || !shouldShowOverviewWindow(lastFocusedWindow))
            continue;

        rememberedSelection[WORKSPACEID] = lastFocusedWindow;
    }
}

void CScrollOverview::rememberSelection(PHLWINDOW window) {
    window = getOverviewWindowToShow(window);

    if (!window || !window->m_workspace)
        return;

    rememberedSelection[window->m_workspace->m_id] = window;
}

void CScrollOverview::updateWorkspaceOverflow() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    for (const auto& img : images) {
        if (!img)
            continue;

        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }

    for (const auto& img : images) {
        if (!img || !img->pWorkspace)
            continue;

        for (const auto& windowRef : img->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || (window->m_isFloating && !ScrollOverview::Config::getCanvasAllowWindowOverflow()))
                continue;

            const auto POS  = window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - MONITOR->m_position;
            const auto SIZE = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            // Windows hidden under a fullscreen window can get sentinel geometry like -2x-2.
            if (SIZE.x <= 0 || SIZE.y <= 0)
                continue;

            img->overflowLeft   = std::max(img->overflowLeft, std::max(0.F, sc<float>(-POS.x)));
            img->overflowRight  = std::max(img->overflowRight, std::max(0.F, sc<float>(POS.x + SIZE.x - MONITOR->m_size.x)));
            img->overflowTop    = std::max(img->overflowTop, std::max(0.F, sc<float>(-POS.y)));
            img->overflowBottom = std::max(img->overflowBottom, std::max(0.F, sc<float>(POS.y + SIZE.y - MONITOR->m_size.y)));
        }
    }
}

CBox CScrollOverview::workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const {
    if (workspaceIdx >= images.size() || !images[workspaceIdx] || !monitor)
        return workspaceBox;

    auto box = workspaceBox;
    const auto LEFT   = images[workspaceIdx]->overflowLeft * renderScale * monitor->m_scale;
    const auto RIGHT  = images[workspaceIdx]->overflowRight * renderScale * monitor->m_scale;
    const auto TOP    = images[workspaceIdx]->overflowTop * renderScale * monitor->m_scale;
    const auto BOTTOM = images[workspaceIdx]->overflowBottom * renderScale * monitor->m_scale;

    box.x -= LEFT;
    box.y -= TOP;
    box.width += LEFT + RIGHT;
    box.height += TOP + BOTTOM;

    return box;
}

PHLWINDOW CScrollOverview::windowAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    if (isCanvasDesktop()) {
        const auto WINDOW = canvasDesktopWindowAtPoint(point);
        if (WINDOW && hoveredWorkspaceIdx) {
            *hoveredWorkspaceIdx = 0;
            for (size_t i = 0; i < images.size(); ++i) {
                if (images[i] && images[i]->pWorkspace == WINDOW->m_workspace) {
                    *hoveredWorkspaceIdx = i;
                    break;
                }
            }
        }
        return WINDOW;
    }

    size_t activeIdx = activeWorkspaceIndex();
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);
    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        const auto  offset = workspaceOverviewOffset(workspaceIdx, activeIdx, WORKSPACEPITCH);

        const auto selectWindow = [&](const PHLWINDOW& window) -> PHLWINDOW {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return window;
        };

        const auto fullscreenWindow = wimg->pWorkspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(wimg->pWorkspace)) : PHLWINDOW{};

        if (!isWorkspaceScrolling(wimg->pWorkspace) && shouldShowOverviewWindow(fullscreenWindow)) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(window) || !window->m_isFloating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }

            const auto texbox = getOverviewWindowBox(fullscreenWindow, MONITOR, scale->value(), viewOffset->value(), offset, layout);

            if (texbox.containsPoint(point))
                return selectWindow(fullscreenWindow);

            continue;
        }

        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto window = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(window) || window->m_isFloating != floating)
                    continue;

                const auto texbox = getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), offset, layout);

                if (texbox.containsPoint(point))
                    return selectWindow(window);
            }
        }
    }

    return nullptr;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursor(size_t* hoveredWorkspaceIdx) {
    return windowAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

PHLWINDOW CScrollOverview::windowClosestToWorkspaceCenter(size_t workspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return {};

    const auto& WORKSPACEIMAGE  = images[workspaceIdx];
    const auto  SCALE           = scale->value();
    const auto  WORKSPACEOFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, SCALE, layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(WORKSPACEIMAGE->pWorkspace));
    const bool HASFULLSCREENPATH = !isWorkspaceScrolling(WORKSPACEIMAGE->pWorkspace) && shouldShowOverviewWindow(FULLSCREENWINDOW) &&
        FULLSCREENWINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace;

    PHLWINDOW bestWindow;
    double    bestDistance = std::numeric_limits<double>::max();

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW))
            continue;
        if (HASFULLSCREENPATH && WINDOW != FULLSCREENWINDOW && !WINDOW->m_isFloating)
            continue;

        const auto WINDOWBOX = getOverviewWindowBox(WINDOW, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto DISTANCE  = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (DISTANCE >= bestDistance)
            continue;

        bestWindow   = WINDOW;
        bestDistance = DISTANCE;
    }

    return bestWindow;
}

PHLWINDOW CScrollOverview::windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow, CBox* windowBox) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return nullptr;

    const auto ACTIVEIDX         = activeWorkspaceIndex();
    const auto WORKSPACE_OFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    PHLWINDOW bestWindow;
    CBox      bestBox;
    float     bestDistanceSq = std::numeric_limits<float>::max();

    for (const bool floating : {true, false}) {
        for (auto it = images[workspaceIdx]->windows.rbegin(); it != images[workspaceIdx]->windows.rend(); ++it) {
            const auto WINDOW = getOverviewWindowToShow(it->lock());
            if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating != floating)
                continue;

            const auto box    = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);
            const auto hitbox = expandOverviewWindowHitbox(box, scale->value(), MONITOR->m_scale);
            if (box.containsPoint(lastMousePosLocal)) {
                if (windowBox)
                    *windowBox = box;

                return WINDOW;
            }

            if (!hitbox.containsPoint(lastMousePosLocal))
                continue;

            const auto distanceSq = overviewPointDistanceSqToBox(lastMousePosLocal, box);
            if (distanceSq >= bestDistanceSq)
                continue;

            bestWindow     = WINDOW;
            bestBox        = box;
            bestDistanceSq = distanceSq;
        }

        if (bestWindow)
            break;
    }

    if (bestWindow && windowBox)
        *windowBox = bestBox;

    return bestWindow;
}

CDropIndicator::SDropAnchor CScrollOverview::dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow,
                                                                                   CScrollOverview* dragContext) {
    CDropIndicator::SDropAnchor result;
    const auto                  DRAGCONTEXT = dragContext ? dragContext : this;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx])
        return result;

    const auto& IMAGE     = images[workspaceIdx];
    const auto  WORKSPACE = IMAGE->pWorkspace;
    if (!WORKSPACE)
        return result;

    const auto WORKSPACE_OFFSET =
        workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);

    if (!overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, MONITOR))
        return result;

    if (!DRAGCONTEXT->dragStartedTiled)
        return result;

    const auto boxesForWindow = [&](const PHLWINDOW& window) {
        const auto TARGET = window->layoutTarget();
        return std::pair{
            getOverviewWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false),
            TARGET ? getOverviewGlobalBox(TARGET->position(), MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout, false) : CBox{},
        };
    };
    const auto setAnchor = [](CDropIndicator::SDropAnchor& anchor, const PHLWINDOW& window, const CBox& box, const CBox& logicalBox = {}, const std::string& direction = {}) {
        anchor.window     = window;
        anchor.box        = box;
        anchor.logicalBox = logicalBox;
        anchor.direction  = direction;
    };

    const auto directionForBox = [&](const CBox& box) {
        const auto LOCAL_X = lastMousePosLocal.x - box.x;
        const auto LOCAL_Y = lastMousePosLocal.y - box.y;

        if (LOCAL_X < box.width / 3.F)
            return std::string{"l"};
        if (LOCAL_X > box.width * 2.F / 3.F)
            return std::string{"r"};
        return LOCAL_Y < box.height / 2.F ? std::string{"u"} : std::string{"d"};
    };

    DRAGCONTEXT->refreshDragOriginalOverviewBoxes();

    const auto ORIGINALHITBOX =
        DRAGCONTEXT->dragOriginalOverviewHitbox.empty() ? DRAGCONTEXT->dragOriginalOverviewBox : DRAGCONTEXT->dragOriginalOverviewHitbox;
    if (ignoredWindow && !DRAGCONTEXT->dragOriginalOverviewBox.empty() && ORIGINALHITBOX.containsPoint(lastMousePosLocal) &&
        WORKSPACE == DRAGCONTEXT->dragOriginalWorkspace.lock()) {
        setAnchor(result, ignoredWindow, DRAGCONTEXT->dragOriginalOverviewBox);
        return result;
    }

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    const bool PRIMARYHORIZONTAL = ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const auto pointsToOriginalStackSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->dragOriginalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->dragOriginalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA  = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL   = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || ANCHORCOL != ORIGINALCOL)
            return false;

        const auto ANCHORIDX   = ANCHORCOL->idx(anchor->layoutTarget());
        const auto ORIGINALIDX = ORIGINALCOL->idx(ignoredWindow->layoutTarget());
        if (ANCHORIDX == ORIGINALIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORIDX < ORIGINALIDX;
        if ((ANCHORPREVIOUS && ANCHORIDX + 1 != ORIGINALIDX) || (!ANCHORPREVIOUS && ORIGINALIDX + 1 != ANCHORIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");

        return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");
    };
    const auto pointsToOriginalColumnSlot = [&](const PHLWINDOW& anchor, const std::string& direction) {
        if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller || !ignoredWindow || DRAGCONTEXT->dragOriginalOverviewBox.empty() ||
            WORKSPACE != DRAGCONTEXT->dragOriginalWorkspace.lock() || !anchor || !anchor->layoutTarget() || !ignoredWindow->layoutTarget())
            return false;

        const auto ANCHORDATA   = ALGO->dataFor(anchor->layoutTarget());
        const auto ORIGINALDATA = ALGO->dataFor(ignoredWindow->layoutTarget());
        const auto ANCHORCOL    = ANCHORDATA ? ANCHORDATA->column.lock() : nullptr;
        const auto ORIGINALCOL  = ORIGINALDATA ? ORIGINALDATA->column.lock() : nullptr;
        if (!ANCHORCOL || !ORIGINALCOL || ORIGINALCOL->targetDatas.size() != 1)
            return false;

        const auto ANCHORCOLIDX   = ALGO->m_scrollingData->idx(ANCHORCOL);
        const auto ORIGINALCOLIDX = ALGO->m_scrollingData->idx(ORIGINALCOL);
        if (ANCHORCOLIDX < 0 || ORIGINALCOLIDX < 0 || ANCHORCOLIDX == ORIGINALCOLIDX)
            return false;

        const bool ANCHORPREVIOUS = ANCHORCOLIDX < ORIGINALCOLIDX;
        if ((ANCHORPREVIOUS && ANCHORCOLIDX + 1 != ORIGINALCOLIDX) || (!ANCHORPREVIOUS && ORIGINALCOLIDX + 1 != ANCHORCOLIDX))
            return false;

        if (PRIMARYHORIZONTAL)
            return (ANCHORPREVIOUS && direction == "r") || (!ANCHORPREVIOUS && direction == "l");

        return (ANCHORPREVIOUS && direction == "d") || (!ANCHORPREVIOUS && direction == "u");
    };
    const auto normalizeOriginalSlotAnchor = [&]() {
        if (pointsToOriginalStackSlot(result.window, result.direction) || pointsToOriginalColumnSlot(result.window, result.direction))
            setAnchor(result, ignoredWindow, DRAGCONTEXT->dragOriginalOverviewBox);
    };

    float bestDistanceSq = std::numeric_limits<float>::max();
    for (auto it = IMAGE->windows.rbegin(); it != IMAGE->windows.rend(); ++it) {
        const auto WINDOW = getOverviewWindowToShow(it->lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;

        if (!HITBOX.containsPoint(lastMousePosLocal))
            continue;

        const auto distanceSq = overviewPointDistanceSqToBox(lastMousePosLocal, BOX);
        if (distanceSq >= bestDistanceSq)
            continue;

        setAnchor(result, WINDOW, BOX, LOGICALBOX, directionForBox(BOX));
        bestDistanceSq = distanceSq;
    }

    if (result.window) {
        normalizeOriginalSlotAnchor();
        return result;
    }

    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return result;

    const auto POINTER   = PRIMARYHORIZONTAL ? sc<float>(lastMousePosLocal.x) : sc<float>(lastMousePosLocal.y);
    float      firstEdge = std::numeric_limits<float>::max();
    float      lastEdge  = std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto MIN               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x) : sc<float>(HITBOX.y);
        const auto MAX               = PRIMARYHORIZONTAL ? sc<float>(HITBOX.x + HITBOX.width) : sc<float>(HITBOX.y + HITBOX.height);

        firstEdge = std::min(firstEdge, MIN);
        lastEdge  = std::max(lastEdge, MAX);
    }

    if (POINTER >= firstEdge && POINTER <= lastEdge)
        return result;

    const bool FIND_FIRST = POINTER < firstEdge;
    float      bestEdge   = FIND_FIRST ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();

    for (const auto& windowRef : IMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == ignoredWindow || WINDOW->m_isFloating)
            continue;

        const auto [BOX, LOGICALBOX] = boxesForWindow(WINDOW);
        const auto HITBOX            = LOGICALBOX.empty() ? BOX : LOGICALBOX;
        const auto EDGE              = PRIMARYHORIZONTAL ? sc<float>(FIND_FIRST ? HITBOX.x : HITBOX.x + HITBOX.width) :
                                                           sc<float>(FIND_FIRST ? HITBOX.y : HITBOX.y + HITBOX.height);

        if (FIND_FIRST ? EDGE < bestEdge : EDGE > bestEdge) {
            bestEdge = EDGE;
            setAnchor(result, WINDOW, BOX, LOGICALBOX);
        }
    }

    if (result.window) {
        result.direction = PRIMARYHORIZONTAL ? (FIND_FIRST ? "l" : "r") : (FIND_FIRST ? "u" : "d");
        normalizeOriginalSlotAnchor();
    }

    return result;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout)),
                                                                layout);

        if (WORKSPACEBOX.containsPoint(point)) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewDropPoint(const Vector2D& point, size_t* hoveredWorkspaceIdx, const PHLWINDOW& draggedWindow) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return nullptr;

    const auto ACTIVEIDX       = activeWorkspaceIndex();
    const auto WORKSPACEPITCH = getWorkspaceRenderedPitch(MONITOR, scale->value(), layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH);
        for (const bool floating : {true, false}) {
            for (auto it = wimg->windows.rbegin(); it != wimg->windows.rend(); ++it) {
                const auto WINDOW = getOverviewWindowToShow(it->lock());
                if (!shouldShowOverviewWindow(WINDOW) || WINDOW == draggedWindow || WINDOW->m_isFloating != floating)
                    continue;

                const auto WINDOWBOX = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
                if (!WINDOWBOX.containsPoint(point))
                    continue;

                if (hoveredWorkspaceIdx)
                    *hoveredWorkspaceIdx = workspaceIdx;

                return wimg->pWorkspace;
            }
        }
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& wimg = images[workspaceIdx];
        if (!wimg || !wimg->pWorkspace)
            continue;

        const auto WORKSPACEBOX = getOverviewWorkspaceUsableBox(wimg->pWorkspace, MONITOR, scale->value(), viewOffset->value(),
                                                                workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, WORKSPACEPITCH), layout);
        const bool ONLAYOUTAXIS = layout == ScrollOverview::Config::ELayout::HORIZONTAL ?
            point.x >= WORKSPACEBOX.x && point.x <= WORKSPACEBOX.x + WORKSPACEBOX.width :
            point.y >= WORKSPACEBOX.y && point.y <= WORKSPACEBOX.y + WORKSPACEBOX.height;

        if (WORKSPACEBOX.containsPoint(point) || (ONLAYOUTAXIS && isWorkspaceScrolling(wimg->pWorkspace))) {
            if (hoveredWorkspaceIdx)
                *hoveredWorkspaceIdx = workspaceIdx;

            return wimg->pWorkspace;
        }
    }

    return nullptr;
}

PHLWORKSPACE CScrollOverview::workspaceAtOverviewCursor(size_t* hoveredWorkspaceIdx) const {
    return workspaceAtOverviewPoint(lastMousePosLocal, hoveredWorkspaceIdx);
}

bool CScrollOverview::isCanvasDesktop() const {
    return ScrollOverview::Config::getCanvasEnabled() && ScrollOverview::Config::getCanvasDesktopMode() && layout == ScrollOverview::Config::ELayout::GRID;
}

bool CScrollOverview::isPersistentCanvas() const {
    return isCanvasDesktop() && ScrollOverview::Config::getCanvasPersistent();
}

bool CScrollOverview::isCanvasNavigationActive() const {
    return isCanvasDesktop() && canvasNavigationActive;
}

void CScrollOverview::toggleCanvasNavigation() {
    const auto MONITOR = pMonitor.lock();
    if (!isPersistentCanvas() || !MONITOR || closing)
        return;

    const auto CENTER = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    if (!canvasNavigationActive) {
        navigationReturnOffset = viewOffset->goal();
        hasNavigationReturn = true;
        if (SpatialOverview::Experiments::on(ECanvasExperiment::Landing)) {
            landingDestinationWorld = currentCanvasViewportWorld();
            hasLandingDestination = !landingDestinationWorld.empty();
        }
        canvasNavigationActive = true;
        zoomCanvasAt(CENTER, ScrollOverview::Config::getCanvasInitialZoom(), true);
        *transitionProgress = 1.F;
        beginNavigatorSession();
        if (navigatorOwnsPointer()) {
            // Applications stop receiving the pointer while the navigator
            // owns it; tell the one under it that the pointer left, and that
            // any button it saw go down has come up. The physical release that
            // follows is swallowed so it cannot turn into a click on the map.
            if (!canvasForwardedPointerButtons.empty()) {
                const auto NOW = Time::millis(Time::steadyNow());
                for (const auto button : canvasForwardedPointerButtons) {
                    g_pSeatManager->sendPointerButton(NOW, button, WL_POINTER_BUTTON_STATE_RELEASED);
                    navigatorSwallowedButtons.emplace(button);
                }
                g_pSeatManager->sendPointerFrame();
            }
            canvasForwardedPointerButtons.clear();
            canvasForwardedPointerWindow.reset();
            canvasForwardedPointerSurface.reset();
            g_pSeatManager->setPointerFocus(nullptr, {});
            updateNavigatorHover();
        }
    } else if (SpatialOverview::Experiments::on(ECanvasExperiment::Landing) && hasLandingDestination) {
        commitLanding();
    } else {
        canvasNavigationActive = false;
        canvasPinching = false;
        zoomCanvasAt(CENTER, 1.F, true);
        *transitionProgress = 0.F;
        leaveNavigatorPointer();
        endNavigatorSessionIfIdle();
    }
    if (activeScrollOverview().get() == this)
        ensureCanvasKeyboardFocus();
    noteCanvasLayoutChanged();
    damage();
}

void CScrollOverview::refreshCanvasSettings() {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR || closing)
        return;

    const auto CENTER = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    zoomCanvasAt(CENTER, isPersistentCanvas() && !canvasNavigationActive ? 1.F : ScrollOverview::Config::getCanvasInitialZoom(), true);
    markBlurDirty();
    damage();
}

CBox CScrollOverview::canvasDesktopWindowBox(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (MONITOR && canvasScreenFixedWindow(window))
        return getOverviewGlobalBox(window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT), MONITOR, 1.F, Vector2D{}, Vector2D{}, layout, false);
    if (!MONITOR || !shouldShowOverviewWindow(window))
        return {};

    return getOverviewGlobalBox(window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT), MONITOR, scale->value(), viewOffset->value(), Vector2D{}, layout,
                                false);
}

CBox CScrollOverview::canvasWorldToScreen(const CBox& world) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return world;
    const float    ZOOM = std::max(scale->value(), 0.01F);
    const Vector2D HALF = MONITOR->m_size / 2.0;
    return CBox{MONITOR->m_position + HALF + (world.pos() - MONITOR->m_position - HALF - viewOffset->value()) * ZOOM, world.size() * ZOOM};
}

std::optional<CBox> CScrollOverview::canvasScreenToWorld(const CBox& screenGlobal, bool atGoal) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasDesktop() || closing)
        return std::nullopt;
    // The inverse of getOverviewBox: a monitor-local point p shows the real
    // point  position + half + camera + (p - half) / zoom. atGoal: as it will
    // be once the camera has arrived.
    const float    ZOOM   = std::max(atGoal ? scale->goal() : scale->value(), 0.01F);
    const Vector2D CAMERA = atGoal ? viewOffset->goal() : viewOffset->value();
    const Vector2D HALF   = MONITOR->m_size / 2.0;
    const Vector2D P0     = screenGlobal.pos() - MONITOR->m_position;
    return CBox{MONITOR->m_position + HALF + CAMERA + (P0 - HALF) / ZOOM, screenGlobal.size() / ZOOM};
}

// The canvas that paces a window's frames: the one on the window's own
// monitor if it shows the window, else the first that does. One pacer, so a
// window spanning two monitors isn't asked for frames twice per refresh.
const CScrollOverview* CScrollOverview::canvasFrameOwner(const PHLWINDOW& window) {
    const CScrollOverview* owner = nullptr;
    for (const auto& overview : scrollOverviews()) {
        const auto* CANVAS = canvasOf(overview);
        if (!CANVAS || !CANVAS->canvasDrawsWindow(window))
            continue;
        if (CANVAS->pMonitor.lock() == window->m_monitor.lock())
            return CANVAS;
        if (!owner)
            owner = CANVAS;
    }
    return owner;
}

bool CScrollOverview::canvasDrawsWindow(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasDesktop() || closing)
        return false;
    const auto BOX = canvasDesktopWindowBox(getOverviewWindowToShow(window));
    return !BOX.empty() && overviewBoxIntersectsMonitor(BOX, MONITOR);
}

// ---- X11 windows ------------------------------------------------------------------
// An X11 app knows its window only by the position the X server holds, and
// the X server delivers pointer input only inside its screen. A canvas
// window's real position can be anywhere in the world, so each X11 window
// reports where it is drawn instead: by the canvas the pointer last used it
// on, else the one showing most of it. Clicks then land, and menus the app
// places itself open next to it (see canvasScreenFixedWindow). Positions the
// app asks for are translated back.
struct SX11Sync {
    PHLWINDOWREF  window;
    PHLMONITORREF owner;
    Vector2D      sent;   // X11 position last reported
    Vector2D      offset; // reported minus real, in X11 coordinates
    bool          valid = false;
    PHLMONITORREF covers; // the monitor the app last asked to cover exactly
};
static std::unordered_map<const Desktop::View::CWindow*, SX11Sync> g_x11Sync;

static bool canvasManagesX11(const PHLWINDOW& window) {
    return window && window->m_isX11 && !window->isX11OverrideRedirect() && window->m_xwaylandSurface && shouldShowOverviewWindow(window) && !window->m_pinned;
}

static SX11Sync* x11Sync(const PHLWINDOW& window, bool create) {
    auto it = g_x11Sync.find(window.get());
    if (it != g_x11Sync.end() && it->second.window.lock() != window) {
        g_x11Sync.erase(it); // a new window at a reused address
        it = g_x11Sync.end();
    }
    if (it == g_x11Sync.end()) {
        if (!create)
            return nullptr;
        it = g_x11Sync.emplace(window.get(), SX11Sync{.window = window}).first;
    }
    return &it->second;
}

static std::optional<Vector2D> canvasX11DrawnPosition(const PHLWINDOW& window) {
    const auto*      SYNC   = x11Sync(window, false);
    const auto       OWNER  = SYNC ? SYNC->owner.lock() : nullptr;
    CScrollOverview* chosen = nullptr;
    double           best   = 0.0;
    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (!canvas || !canvas->canvasAtRestZoom())
            continue;
        const auto BOX = canvas->canvasDrawnGlobalBox(window);
        if (BOX.empty())
            continue;
        if (OWNER && canvas->canvasMonitor() == OWNER) {
            chosen = canvas;
            break;
        }
        const auto VISIBLE = BOX.intersection(canvas->canvasMonitor()->logicalBox());
        if (VISIBLE.width * VISIBLE.height > best) {
            best   = VISIBLE.width * VISIBLE.height;
            chosen = canvas;
        }
    }
    if (!chosen)
        return std::nullopt;
    const auto POS = g_pXWaylandManager->waylandToXWaylandCoords(chosen->canvasDrawnGlobalBox(window).pos(), chosen->canvasMonitor());
    return Vector2D{std::round(POS.x), std::round(POS.y)};
}

float CScrollOverview::canvasZoom() const {
    return scale->value();
}

bool CScrollOverview::canvasAtRestZoom() const {
    return std::abs(scale->value() - 1.F) < 0.001F;
}

// Whether (nearly) all of a window is on the screens as the canvases draw it
// now, one screen or across several.
bool CScrollOverview::canvasWindowOnScreen(const PHLWINDOW& window) {
    double visible = 0.0, area = 0.0;
    for (const auto& overview : scrollOverviews()) {
        const auto* CANVAS  = canvasOf(overview);
        const auto  MONITOR = CANVAS ? CANVAS->canvasMonitor() : nullptr;
        if (!MONITOR)
            continue;
        const auto BOX = CANVAS->canvasDrawnGlobalBox(window);
        if (BOX.empty())
            continue;
        area = std::max(area, BOX.width * BOX.height);
        const auto PART = BOX.intersection(MONITOR->logicalBox());
        visible += PART.width * PART.height;
    }
    return area > 0.0 && visible >= area * 0.9;
}

CBox CScrollOverview::canvasDrawnGlobalBox(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasDesktop() || closing)
        return {};
    const auto BOX = canvasDesktopWindowBox(window);
    if (BOX.empty())
        return {};
    const double SCALE = std::max(0.01, sc<double>(MONITOR->m_scale));
    return CBox{MONITOR->m_position + BOX.pos() / SCALE, BOX.size() / SCALE};
}

void CScrollOverview::canvasClaimX11Window(const PHLWINDOW& window) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !canvasManagesX11(window))
        return;
    auto* sync = x11Sync(window, true);
    if (sync->owner.lock() == MONITOR)
        return;
    sync->owner = MONITOR;
    window->sendWindowSize(true);
}

// Hyprland is about to tell an X11 window where it is (hooked).
CBox canvasX11Configure(void* surface, const CBox& box) {
    PHLWINDOW window;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (w && w->m_isX11 && w->m_xwaylandSurface.get() == surface) {
            window = w;
            break;
        }
    }
    if (!canvasManagesX11(window))
        return box;
    const auto DRAWN = canvasX11DrawnPosition(window);
    if (!DRAWN) {
        // Not shown 1:1 anywhere (zoomed out, or off screen): the app keeps
        // the position it last had. Jumping to the real one, which is often
        // on no monitor, or to a zoomed-out spot would have it change
        // monitors, and a game its resolution, as the camera moves.
        auto* sync = x11Sync(window, false);
        if (!sync || !sync->valid)
            return box;
        sync->offset = sync->sent - box.pos();
        return CBox{sync->sent, box.size()};
    }
    auto* sync   = x11Sync(window, true);
    sync->sent   = *DRAWN;
    sync->offset = *DRAWN - box.pos();
    sync->valid  = true;
    return CBox{*DRAWN, box.size()};
}

// An X11 app asks to be put somewhere, in the coordinates it was given (hooked).
CBox canvasX11Request(void* windowPtr, CBox box) {
    const auto* WINDOW = sc<Desktop::View::CWindow*>(windowPtr);
    // Hyprland ignores what a fullscreen window asks for, and a game leaving
    // fullscreen asks for its windowed size before it is out. Keep it for
    // when its screen's canvas is back.
    for (auto& entry : g_canvasFullscreen) {
        if (entry.window.lock().get() == WINDOW) {
            entry.request = box;
            return box;
        }
    }
    const auto  IT     = WINDOW ? g_x11Sync.find(WINDOW) : g_x11Sync.end();
    if (IT == g_x11Sync.end() || !IT->second.valid || IT->second.window.lock().get() != WINDOW || !canvasManagesX11(IT->second.window.lock()))
        return box;
    // A game going borderless fullscreen first asks to cover its monitor
    // exactly: that is the screen it means (see canvasScreenOf).
    const auto SYNCED = IT->second.window.lock();
    const CBox SCREEN{SYNCED->xwaylandPositionToReal(box.pos()), SYNCED->xwaylandSizeToReal(box.size())};
    IT->second.covers.reset();
    for (const auto& monitor : State::monitorState()->monitors()) {
        const auto MONITORBOX = monitor ? monitor->logicalBox() : CBox{};
        if (monitor && std::abs(SCREEN.x - MONITORBOX.x) < 1.5 && std::abs(SCREEN.y - MONITORBOX.y) < 1.5 && std::abs(SCREEN.width - MONITORBOX.width) < 1.5 &&
            std::abs(SCREEN.height - MONITORBOX.height) < 1.5)
            IT->second.covers = monitor;
    }
    box.x -= IT->second.offset.x;
    box.y -= IT->second.offset.y;
    return box;
}

// After the camera moves, X11 windows are told where they now are.
static void canvasResyncX11Windows() {
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!canvasManagesX11(window))
            continue;
        const auto DRAWN = canvasX11DrawnPosition(window);
        if (!DRAWN)
            continue;
        const auto* SYNC = x11Sync(window, false);
        if (SYNC && SYNC->valid && SYNC->sent.distanceSq(*DRAWN) < 0.25)
            continue;
        window->sendWindowSize(true);
    }
}

// With the canvas gone, X11 windows report their real positions again.
void canvasReleaseX11Windows() {
    std::vector<PHLWINDOW> windows;
    for (const auto& [_, sync] : g_x11Sync) {
        if (const auto WINDOW = sync.window.lock(); validMapped(WINDOW))
            windows.push_back(WINDOW);
    }
    g_x11Sync.clear();
    for (const auto& window : windows)
        window->sendWindowSize(true);
}

// ---- Fullscreen on the canvas -----------------------------------------------------
// A window that goes fullscreen (a game, a video, Super+F) fills the screen
// the canvas shows it on. Only that screen's canvas steps aside, nothing is
// re-tiled, and it comes back with its camera when the window leaves
// fullscreen, closes, or its workspace leaves that screen. Other screens keep
// their canvas. Wine games in borderless "windowed fullscreen" ask for
// fullscreen themselves once they cover a monitor.
bool openCanvasOverview(PHLMONITOR monitor); // main.cpp

static std::vector<SCanvasFullscreen> g_canvasFullscreenPending;
static wl_event_source*               g_canvasFullscreenIdle     = nullptr;
static bool                           g_canvasFullscreenApplying = false;

// Every screen leaves the zoomed-out view together. With `leader`, the screen
// acting (landing on a window, going to a place) keeps the camera: the others
// follow it rather than their own way out.
static void leaveNavigationEverywhere(CScrollOverview* leader = nullptr) {
    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (canvas && !canvas->isClosing() && canvas->isCanvasNavigationActive())
            canvas->toggleCanvasNavigation();
    }
    if (!leader)
        return;
    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (canvas && canvas != leader)
            canvas->followLinkedLeader(leader);
    }
}

static PHLMONITOR monitorAt(const Vector2D& point) {
    PHLMONITOR nearest;
    double     best = INFINITY;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || !monitor->m_enabled)
            continue;
        const auto BOX = monitor->logicalBox();
        if (BOX.containsPoint(point))
            return monitor;
        const double D = BOX.closestPoint(point).distanceSq(point);
        if (D < best) {
            best    = D;
            nearest = monitor;
        }
    }
    return nearest;
}

// The screen a window is shown on. An X11 app means the monitor it asked to
// cover, else the one it was told it is on; otherwise the canvas under the pointer if it shows the
// window, else the one showing most of it.
static PHLMONITOR canvasScreenOf(const PHLWINDOW& window) {
    if (window->m_isX11) {
        if (const auto* SYNC = x11Sync(window, false); SYNC && SYNC->valid) {
            if (const auto COVERS = SYNC->covers.lock(); COVERS && canvasOf(scrollOverviewForMonitor(COVERS)))
                return COVERS;
            const auto MONITOR = monitorAt(g_pXWaylandManager->xwaylandToWaylandCoords(SYNC->sent) + window->m_realSize->goal() / 2.0);
            if (MONITOR && canvasOf(scrollOverviewForMonitor(MONITOR)))
                return MONITOR;
        }
    }
    const auto CURSOR = g_pInputManager->getMouseCoordsInternal();
    PHLMONITOR chosen;
    double     best = 0.0;
    for (const auto& overview : scrollOverviews()) {
        const auto* CANVAS  = canvasOf(overview);
        const auto  MONITOR = CANVAS ? CANVAS->canvasMonitor() : nullptr;
        if (!MONITOR)
            continue;
        const auto VISIBLE = CANVAS->canvasDrawnGlobalBox(window).intersection(MONITOR->logicalBox());
        const auto AREA    = VISIBLE.width * VISIBLE.height;
        if (AREA <= 0.0)
            continue;
        if (MONITOR->logicalBox().containsPoint(CURSOR))
            return MONITOR;
        if (AREA > best) {
            best   = AREA;
            chosen = MONITOR;
        }
    }
    if (chosen)
        return chosen;
    const auto OWN = window->m_monitor.lock();
    return OWN && canvasOf(scrollOverviewForMonitor(OWN)) ? OWN : nullptr;
}

// Restart a window's move and resize from `from` (in the coordinates its box
// is in) to where it is going.
static void animateWindowFrom(const PHLWINDOW& window, const CBox& from) {
    const auto POS  = window->m_realPosition->goal();
    const auto SIZE = window->m_realSize->goal();
    window->m_realPosition->setValueAndWarp(from.pos());
    window->m_realSize->setValueAndWarp(from.size());
    *window->m_realPosition = POS;
    *window->m_realSize     = SIZE;
}

static void canvasFullscreenStepAside(const PHLWINDOW& window, const PHLMONITOR& monitor) {
    auto* canvas = canvasOf(scrollOverviewForMonitor(monitor));
    if (!canvas || !canvas->isCanvasDesktop() || canvas->isClosing())
        return;

    g_canvasFullscreenApplying = true;
    // It grows out of where the canvas drew it (zoomed out or not), not out of
    // its place on the canvas, which Hyprland would take for screen coordinates.
    std::optional<CBox> before, from;
    if (const auto IT = g_canvasWindowedBox.find(window.get()); IT != g_canvasWindowedBox.end() && IT->second.window.lock() == window) {
        before = IT->second.box;
        from   = canvas->canvasWorldToScreen(*before);
    }
    // Fullscreen happens on the window's own workspace and monitor; bring it
    // to the screen it was shown on first. (When an X11 app moves itself onto
    // another screen's workspace, Hyprland can leave its monitor behind.)
    if (const auto WORKSPACE = monitor->m_activeWorkspace; WORKSPACE && (window->m_workspace != WORKSPACE || window->m_monitor != monitor)) {
        const auto MODES = Fullscreen::controller()->getFullscreenModes(window);
        Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
        moveCanvasWindowToWorkspace(window, WORKSPACE);
        window->m_monitor = monitor;
        Fullscreen::controller()->setFullscreenMode(window, MODES.internal, MODES.client);
    }
    g_canvasFullscreen.push_back({.window = window, .monitor = monitor, .before = before});
    removeOverview(canvas);
    g_canvasFullscreenApplying = false;
    if (from)
        animateWindowFrom(window, *from);

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    window->sendWindowSize(true);
}

static void canvasPlaceAfterFullscreen(const SCanvasFullscreen& entry, CScrollOverview* canvas, const PHLWINDOW& WINDOW, const PHLMONITOR& MONITOR);


static void canvasFullscreenComeBack(const SCanvasFullscreen& entry) {
    forgetPlaceFullscreen(entry.window.lock());
    const auto WINDOW  = entry.window.lock();
    const auto MONITOR = entry.monitor.lock();
    const bool FOCUSED = WINDOW && Desktop::focusState()->window() == WINDOW;
    if (!MONITOR || scrollOverviewForMonitor(MONITOR) || !openCanvasOverview(MONITOR))
        return;
    auto* canvas = canvasOf(scrollOverviewForMonitor(MONITOR));
    if (!canvas)
        return;
    // Back on the canvas as the other screens are: their camera, and zoomed
    // out if they are.
    canvas->followLinkedCamera();
    if (!canvas->isCanvasNavigationActive() && std::ranges::any_of(scrollOverviews(), [canvas](const auto& overview) {
            const auto* OTHER = canvasOf(overview);
            return OTHER && OTHER != canvas && !OTHER->isClosing() && OTHER->isCanvasNavigationActive();
        }))
        canvas->toggleCanvasNavigation();
    const auto TARGET = validMapped(WINDOW) ? WINDOW->layoutTarget() : nullptr;
    if (!TARGET || WINDOW->m_workspace != MONITOR->m_activeWorkspace || Fullscreen::controller()->isFullscreen(WINDOW))
        return;
    // The keyboard stays with the window that left fullscreen, not with
    // whatever the reopened canvas had selected.
    auto keepFocus = Hyprutils::Utils::CScopeGuard([canvas, WINDOW, FOCUSED] {
        if (!FOCUSED)
            return;
        if (Desktop::focusState()->window() != WINDOW)
            Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        canvas->canvasAdoptFocus(WINDOW);
    });
    const auto FULLSCREEN = canvas->canvasScreenToWorld(MONITOR->logicalBox());
    canvasPlaceAfterFullscreen(entry, canvas, WINDOW, MONITOR);
    // It shrinks back from the full screen into its place.
    if (FULLSCREEN)
        animateWindowFrom(WINDOW, *FULLSCREEN);
}

// Where a window that left fullscreen goes on its screen's canvas.
static void canvasPlaceAfterFullscreen(const SCanvasFullscreen& entry, CScrollOverview* canvas, const PHLWINDOW& WINDOW, const PHLMONITOR& MONITOR) {
    const auto TARGET = WINDOW->layoutTarget();
    if (!TARGET)
        return;
    // Back from fullscreen: an X11 app gets what it asked for meanwhile, on
    // the screen as the canvas shows it.
    if (entry.request) {
        const CBox SCREEN{WINDOW->xwaylandPositionToReal(entry.request->pos()), WINDOW->xwaylandSizeToReal(entry.request->size())};
        if (const auto WORLD = canvas->canvasScreenToWorld(SCREEN)) {
            TARGET->rememberFloatingSize(SCREEN.size());
            TARGET->setPositionGlobal(CBox{WORLD->pos(), SCREEN.size()});
            WINDOW->sendWindowSize(true);
        }
        return;
    }
    // Everything else goes back where it was.
    if (entry.before) {
        TARGET->rememberFloatingSize(entry.before->size());
        TARGET->setPositionGlobal(*entry.before);
        WINDOW->sendWindowSize(true);
        return;
    }
    if (canvas->canvasDrawsWindow(WINDOW))
        return;
    // Never drawn before: if it put itself at a screen position meanwhile,
    // while nothing translated it, show it there.
    const auto SCREEN = MONITOR->logicalBox();
    auto       box    = WINDOW->getWindowMainSurfaceBox();
    box.x             = std::clamp(box.x, SCREEN.x, std::max(SCREEN.x, SCREEN.x + SCREEN.width - box.width));
    box.y             = std::clamp(box.y, SCREEN.y, std::max(SCREEN.y, SCREEN.y + SCREEN.height - box.height));
    if (const auto WORLD = canvas->canvasScreenToWorld(box)) {
        TARGET->setPositionGlobal(CBox{WORLD->pos(), box.size()});
        TARGET->warpPositionSize();
    }
}

static void canvasFullscreenCheck(void*) {
    g_canvasFullscreenIdle = nullptr;
    if (g_canvasRestoring)
        return;

    auto pending = std::move(g_canvasFullscreenPending);
    g_canvasFullscreenPending.clear();
    // Going fullscreen from the zoomed-out view: every screen returns to 100%
    // (the window grows out of where it was drawn), so no screen is left
    // zoomed out beside it.
    if (!pending.empty())
        leaveNavigationEverywhere();
    for (const auto& request : pending) {
        const auto WINDOW  = request.window.lock();
        const auto MONITOR = request.monitor.lock();
        if (validMapped(WINDOW) && MONITOR && Fullscreen::controller()->isFullscreen(WINDOW) && !canvasFullscreenWindow(WINDOW))
            canvasFullscreenStepAside(WINDOW, MONITOR);
    }

    std::vector<SCanvasFullscreen> done;
    std::erase_if(g_canvasFullscreen, [&done](const auto& entry) {
        const auto WINDOW  = entry.window.lock();
        const auto MONITOR = entry.monitor.lock();
        const bool OVER    = !validMapped(WINDOW) || !MONITOR || !Fullscreen::controller()->isFullscreen(WINDOW) || WINDOW->m_workspace != MONITOR->m_activeWorkspace;
        if (OVER)
            done.push_back(entry);
        return OVER;
    });
    for (const auto& entry : done)
        canvasFullscreenComeBack(entry);
}

// Hyprland keeps a pointer confinement in the window's real coordinates, which
// on the canvas are not where it is drawn: the pointer would be trapped
// somewhere invisible. Release those; a window the canvas does not draw (a
// fullscreen game on a screen of its own) keeps its own, so its mouse-look
// stays on it.
void unconstrainCanvasWindows() {
    const auto CONSTRAINTS = g_pInputManager->m_constraints; // deactivating a one-shot one removes it
    for (const auto& ref : CONSTRAINTS) {
        const auto CONSTRAINT = ref.lock();
        if (!CONSTRAINT || !CONSTRAINT->isActive())
            continue;
        const auto OWNER  = CONSTRAINT->owner();
        const auto WINDOW = OWNER && OWNER->view() ? Desktop::View::CWindow::fromView(OWNER->view()) : nullptr;
        if (WINDOW && !shouldShowOverviewWindow(WINDOW))
            continue;
        CONSTRAINT->deactivate();
    }
}

bool canvasCursorHidden(); // Cursor.cpp

// A fullscreen game with its cursor hidden (mouse-look) keeps the pointer on
// its screen: a pointer you cannot see must not wander onto the next screen
// and give focus to a window there. Games often do not confine it. Wine hides
// the cursor with an empty 1x1 image rather than none. From main.cpp's
// mouse-move listener; true: the move is undone.
bool canvasKeepPointerOnFullscreen();

// The canvas turns Hyprland's focus-follows-mouse off (it focuses on its own
// terms). A screen whose canvas stepped aside for a fullscreen window gets it
// back, as your follow_mouse setting says: the pointer moving onto it focuses
// that window.
static void canvasFollowMouseOntoFullscreen() {
    if (g_canvasFullscreen.empty() || g_userFollowMouse != 1 || scrollOverviews().empty())
        return;
    const auto POS = g_pInputManager->getMouseCoordsInternal();
    for (const auto& entry : g_canvasFullscreen) {
        const auto WINDOW  = entry.window.lock();
        const auto MONITOR = entry.monitor.lock();
        if (validMapped(WINDOW) && MONITOR && MONITOR->logicalBox().containsPoint(POS) && Desktop::focusState()->window() != WINDOW) {
            Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_FFM);
            return;
        }
    }
}

// Pointer moves, from main.cpp's listener; true: the move is undone.
bool canvasPointerMoved() {
    if (canvasKeepPointerOnFullscreen())
        return true;
    canvasFollowMouseOntoFullscreen();
    return false;
}

bool canvasKeepPointerOnFullscreen() {
    const auto WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    const auto ENTRY  = std::ranges::find_if(g_canvasFullscreen, [&WINDOW](const auto& entry) { return WINDOW && entry.window.lock() == WINDOW; });
    const auto MONITOR = ENTRY != g_canvasFullscreen.end() ? ENTRY->monitor.lock() : nullptr;
    if (!MONITOR)
        return false;
    if (!canvasCursorHidden())
        return false;
    const auto BOX = MONITOR->logicalBox();
    const auto POS = g_pInputManager->getMouseCoordsInternal();
    if (BOX.containsPoint(POS))
        return false;
    Pointer::pointerController()->warpTo(Vector2D{std::clamp(POS.x, BOX.x, BOX.x + BOX.width - 1.0), std::clamp(POS.y, BOX.y, BOX.y + BOX.height - 1.0)}, true);
    return true;
}

// Opening the canvas on a screen a fullscreen window has to itself (Super+
// Ctrl+G) takes the screen back: the window leaves fullscreen and is on the
// canvas where it was, where it can be seen and picked. Going back to it
// right away (it is focused when the navigator closes) makes it fullscreen
// again. Called by main.cpp once that screen's canvas is open.
struct SCanvasFullscreenReturn {
    PHLWINDOWREF                window;
    Fullscreen::SFullscreenMode modes;
};
static std::optional<SCanvasFullscreenReturn> g_canvasFullscreenReturn;
static wl_event_source*                       g_canvasFullscreenReturnIdle = nullptr;

void canvasReclaimScreen(const PHLMONITOR& monitor) {
    const auto IT = std::ranges::find_if(g_canvasFullscreen, [&monitor](const auto& entry) { return entry.monitor.lock() == monitor; });
    if (IT == g_canvasFullscreen.end())
        return;
    const auto ENTRY  = *IT;
    const auto WINDOW = ENTRY.window.lock();
    g_canvasFullscreen.erase(IT);
    auto* canvas = canvasOf(scrollOverviewForMonitor(monitor));
    if (!canvas || !validMapped(WINDOW) || !Fullscreen::controller()->isFullscreen(WINDOW))
        return;
    g_canvasFullscreenReturn   = SCanvasFullscreenReturn{.window = WINDOW, .modes = Fullscreen::controller()->getFullscreenModes(WINDOW)};
    g_canvasFullscreenApplying = true;
    Fullscreen::controller()->setFullscreenMode(WINDOW, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);
    g_canvasFullscreenApplying = false;
    canvasPlaceAfterFullscreen(ENTRY, canvas, WINDOW, monitor);
    notePlaceFullscreen(WINDOW, g_canvasFullscreenReturn->modes);
}

static void canvasFullscreenReturnCheck() {
    if (!g_canvasFullscreenReturn || g_canvasFullscreenReturnIdle || !g_pCompositor)
        return;
    const auto RETURN = *g_canvasFullscreenReturn;
    const auto WINDOW = RETURN.window.lock();
    if (!validMapped(WINDOW) || getOverviewWindowToShow(Desktop::focusState()->window()) != WINDOW || Fullscreen::controller()->isFullscreen(WINDOW)) {
        g_canvasFullscreenReturn.reset();
        return;
    }
    // Not from inside the canvas's own callbacks.
    g_canvasFullscreenReturnIdle = wl_event_loop_add_idle(
        g_pCompositor->m_wlEventLoop,
        [](void*) {
            g_canvasFullscreenReturnIdle = nullptr;
            const auto RETURN            = std::exchange(g_canvasFullscreenReturn, std::nullopt);
            const auto WINDOW            = RETURN ? RETURN->window.lock() : nullptr;
            if (validMapped(WINDOW) && !Fullscreen::controller()->isFullscreen(WINDOW))
                Fullscreen::controller()->setFullscreenMode(WINDOW, RETURN->modes.internal, RETURN->modes.client);
        },
        nullptr);
}

// While the focused screen's canvas has stepped aside, no canvas is the
// active one: focus and keys there are the fullscreen window's.
bool canvasSteppedAside(const PHLMONITOR& monitor) {
    return monitor && std::ranges::any_of(g_canvasFullscreen, [&monitor](const auto& entry) { return entry.monitor.lock() == monitor; });
}

// Window and workspace events, from main.cpp.
void canvasFullscreenEvent(PHLWINDOW window) {
    if (g_canvasFullscreenApplying || g_canvasRestoring)
        return;
    window = getOverviewWindowToShow(window);
    if (window && Fullscreen::controller()->isFullscreen(window) && shouldShowOverviewWindow(window)) {
        if (const auto MONITOR = canvasScreenOf(window))
            g_canvasFullscreenPending.push_back({window, MONITOR});
    }
    if (g_canvasFullscreenPending.empty() && g_canvasFullscreen.empty())
        return;
    if (!g_canvasFullscreenIdle && g_pCompositor)
        g_canvasFullscreenIdle = wl_event_loop_add_idle(g_pCompositor->m_wlEventLoop, canvasFullscreenCheck, nullptr);
}

void canvasFullscreenReset() {
    if (g_canvasFullscreenIdle)
        wl_event_source_remove(g_canvasFullscreenIdle);
    g_canvasFullscreenIdle = nullptr;
    if (g_canvasFullscreenReturnIdle)
        wl_event_source_remove(g_canvasFullscreenReturnIdle);
    g_canvasFullscreenReturnIdle = nullptr;
    g_canvasFullscreenReturn.reset();
    g_canvasFullscreenPending.clear();
    g_canvasFullscreen.clear();
    g_canvasWindowedBox.clear();
}

// ---- State for scripts and tests (hyprctl spatialoverview) --------------------------
static std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (sc<unsigned char>(c) < 0x20)
            out += ' ';
        else
            out += c;
    }
    return out + "\"";
}

static std::string canvasFillJson(); // below

std::string canvasStateJson() {
    std::string screens, fullscreen;
    for (const auto& overview : scrollOverviews()) {
        const auto* CANVAS  = canvasOf(overview);
        const auto  MONITOR = CANVAS ? CANVAS->canvasMonitor() : nullptr;
        if (!MONITOR)
            continue;
        const auto CAMERA = CANVAS->canvasScreenToWorld(MONITOR->logicalBox());
        screens += std::format("{}{{\"monitor\": {}, \"zoom\": {:.3f}, \"navigating\": {}, \"closing\": {}, \"view\": [{:.0f}, {:.0f}, {:.0f}, {:.0f}]}}", screens.empty() ? "" : ", ",
                               jsonString(MONITOR->m_name), CANVAS->canvasZoom(), CANVAS->isCanvasNavigationActive() ? "true" : "false", CANVAS->isClosing() ? "true" : "false",
                               CAMERA ? CAMERA->x : 0.0, CAMERA ? CAMERA->y : 0.0, CAMERA ? CAMERA->width : 0.0, CAMERA ? CAMERA->height : 0.0);
    }
    for (const auto& entry : g_canvasFullscreen) {
        const auto WINDOW  = entry.window.lock();
        const auto MONITOR = entry.monitor.lock();
        fullscreen += std::format("{}{{\"window\": {}, \"monitor\": {}}}", fullscreen.empty() ? "" : ", ", jsonString(WINDOW ? WINDOW->m_title : ""), jsonString(MONITOR ? MONITOR->m_name : ""));
    }
    const auto* LEADER   = g_linkedLeader;
    const auto  SELECTED = SpatialOverview::Navigator::isOpen() ? SpatialOverview::Navigator::selectedWindow() : PHLWINDOW{};
    return std::format("{{\"linked\": {}, \"leader\": {}, \"screens\": [{}], \"fullscreen\": [{}], \"filled\": [{}], \"selected\": {}}}\n",
                       ScrollOverview::Config::getCanvasLinkedScreens() ? "true" : "false", jsonString(LEADER && LEADER->canvasMonitor() ? LEADER->canvasMonitor()->m_name : ""),
                       screens, fullscreen, canvasFillJson(), jsonString(SELECTED ? SELECTED->m_title : ""));
}

// ---- Fill the screen (Super+T on the canvas) ---------------------------------------
// Everything floats on the canvas, so the float/tile toggle has nothing to
// do there. Instead the window fills the screen it is shown on, 1:1 and
// minus bars; again puts it back where and how big it was.
struct SCanvasFill {
    PHLWINDOWREF window;
    CBox         before;
    CBox         filled;
};
static std::unordered_map<const Desktop::View::CWindow*, SCanvasFill> g_canvasFill;

// False only when no canvas desktop is running (the key then does what it
// does without the canvas); a window it can't fill is left as it is.
static std::string canvasFillJson() {
    std::string out;
    for (const auto& [_, entry] : g_canvasFill) {
        if (const auto WINDOW = entry.window.lock())
            out += (out.empty() ? "" : ", ") + jsonString(WINDOW->m_title);
    }
    return out;
}

// SUPER + O on the canvas: the window stays where it is on the screen while
// the canvas moves under it (pinned); again puts it back on the canvas right
// where it then is. From the zoomed-out view it lands at 100% first.
bool canvasTogglePin(PHLWINDOW window) {
    if (std::ranges::none_of(scrollOverviews(), [](const auto& overview) {
            const auto* CANVAS = canvasOf(overview);
            return CANVAS && CANVAS->isCanvasDesktop() && !CANVAS->isClosing();
        }))
        return false;

    window            = getOverviewWindowToShow(window);
    const auto TARGET = validMapped(window) ? window->layoutTarget() : nullptr;
    if (!TARGET || !window->m_isFloating || Fullscreen::controller()->isFullscreen(window) || (window->m_workspace && window->m_workspace->m_isSpecialWorkspace))
        return true;

    if (window->m_pinned) {
        const auto BOX     = CBox{window->m_realPosition->goal(), window->m_realSize->goal()};
        auto*      canvas  = canvasOf(scrollOverviewForMonitor(monitorAt(BOX.middle())));
        (void)Config::Actions::pinWindow(Config::Actions::TOGGLE_ACTION_DISABLE, window);
        if (const auto WORLD = canvas ? canvas->canvasScreenToWorld(CBox{BOX.pos(), BOX.size() * canvas->canvasZoom()}) : std::nullopt) {
            TARGET->setPositionGlobal(CBox{WORLD->pos(), BOX.size()});
            TARGET->warpPositionSize();
            window->sendWindowSize(true);
        }
        return true;
    }

    const auto MONITOR = canvasScreenOf(window);
    auto*      canvas  = MONITOR ? canvasOf(scrollOverviewForMonitor(MONITOR)) : nullptr;
    if (!canvas || !shouldShowOverviewWindow(window))
        return true;
    if (canvas->isCanvasNavigationActive()) {
        canvas->canvasAdoptFocus(window);
        canvas->flightDeckAction("land");
        leaveNavigationEverywhere(canvas);
    }
    // Where it is drawn once the camera has arrived, at its own size.
    const auto REAL   = CBox{window->m_realPosition->goal(), window->m_realSize->goal()};
    const auto ORIGIN = canvas->canvasScreenToWorld(MONITOR->logicalBox(), true);
    if (!ORIGIN)
        return true;
    const Vector2D SCREENPOS = MONITOR->m_position + (REAL.pos() - ORIGIN->pos()) * (MONITOR->m_size.x / std::max(1.0, ORIGIN->width));
    TARGET->setPositionGlobal(CBox{SCREENPOS, REAL.size()});
    TARGET->warpPositionSize();
    (void)Config::Actions::pinWindow(Config::Actions::TOGGLE_ACTION_ENABLE, window);
    window->sendWindowSize(true);
    return true;
}

// The area Super+T fills: the screen minus what bars and docks reserve, with
// the gap tiled windows keep (general:gaps_out) all around it, and the
// window's border inside that gap, as for a tiled window.
static CBox canvasFillArea(const PHLMONITOR& monitor, const PHLWINDOW& window) {
    auto box = monitor->logicalBoxMinusReserved();
    if (const auto* GAPS = sc<Config::CCssGapData*>(ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("general:gaps_out").ptr())) {
        box.x += GAPS->m_left;
        box.y += GAPS->m_top;
        box.width -= GAPS->m_left + GAPS->m_right;
        box.height -= GAPS->m_top + GAPS->m_bottom;
    }
    const double BORDER = window ? window->getRealBorderSize() : 0;
    return CBox{box.x + BORDER, box.y + BORDER, box.width - 2 * BORDER, box.height - 2 * BORDER};
}

bool canvasToggleFill(PHLWINDOW window) {
    if (std::ranges::none_of(scrollOverviews(), [](const auto& overview) {
            const auto* CANVAS = canvasOf(overview);
            return CANVAS && CANVAS->isCanvasDesktop() && !CANVAS->isClosing();
        }))
        return false;

    window            = getOverviewWindowToShow(window);
    const auto TARGET = window ? window->layoutTarget() : nullptr;
    if (!shouldShowOverviewWindow(window) || !TARGET || !TARGET->floating() || window->m_pinned || Fullscreen::controller()->isFullscreen(window))
        return true;

    const auto MONITOR = canvasScreenOf(window);
    auto*      canvas  = MONITOR ? canvasOf(scrollOverviewForMonitor(MONITOR)) : nullptr;
    if (!canvas)
        return true;

    // Super+T always ends at 100%, where the result is what you see: from the
    // zoomed-out view it lands on the window first.
    if (canvas->isCanvasNavigationActive()) {
        canvas->canvasAdoptFocus(window);
        canvas->flightDeckAction("land");
        leaveNavigationEverywhere(canvas);
    }
    // A filled window is the one you are looking at: on top of the others.
    (void)Config::Actions::alterZOrder("top", window);

    const auto apply = [&](const CBox& box) {
        TARGET->rememberFloatingSize(box.size());
        TARGET->setPositionGlobal(box);
        window->sendWindowSize(true);
    };

    const auto NOW = CBox{window->m_realPosition->goal(), window->m_realSize->goal()};
    if (const auto IT = g_canvasFill.find(window.get()); IT != g_canvasFill.end()) {
        const auto ENTRY = IT->second;
        g_canvasFill.erase(IT);
        if (ENTRY.window.lock() == window) {
            // Untouched since: back exactly where and how big it was.
            if (NOW.pos().distanceSq(ENTRY.filled.pos()) < 1.0 && NOW.size().distanceSq(ENTRY.filled.size()) < 1.0) {
                apply(ENTRY.before);
                return true;
            }
            // Moved since: its old size, around where it is now.
            if (NOW.size().distanceSq(ENTRY.filled.size()) < 1.0) {
                apply(CBox{NOW.middle() - ENTRY.before.size() / 2.0, ENTRY.before.size()});
                return true;
            }
            // Resized since: it is not filling anything any more; fill again.
        }
    }

    const auto AREA  = canvasFillArea(MONITOR, window);
    const auto WORLD = canvas->canvasScreenToWorld(AREA, true);
    if (!WORLD)
        return true;
    const CBox FILLED{WORLD->pos(), AREA.size()};
    g_canvasFill[window.get()] = {.window = window, .before = NOW, .filled = FILLED};
    apply(FILLED);
    return true;
}

// Where a canvas window's popups must stay: the screen as the canvas that
// shows the window draws it (minus bars), in the real coordinates the popup
// positioner works in. That canvas is the one under the pointer when it draws
// the window (a right click happens there), else any that draws it.
std::optional<CBox> canvasPopupConstraint(const PHLWINDOW& window) {
    const auto       CURSOR = g_pInputManager->getMouseCoordsInternal();
    CScrollOverview* chosen = nullptr;
    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (!canvas || !canvas->canvasDrawsWindow(window))
            continue;
        if (const auto MONITOR = canvas->canvasMonitor(); MONITOR && MONITOR->logicalBox().containsPoint(CURSOR)) {
            chosen = canvas;
            break;
        }
        if (!chosen)
            chosen = canvas;
    }
    const auto MONITOR = chosen ? chosen->canvasMonitor() : nullptr;
    return MONITOR ? chosen->canvasScreenToWorld(MONITOR->logicalBoxMinusReserved()) : std::nullopt;
}

// Popups fade in and out on Hyprland's animation timer, which asks for a
// redraw of the popup's real box; on the canvas that box may be on no monitor
// at all, and the fade would freeze part-way. While a popup this canvas draws
// is fading, keep asking for frames. (A timer rather than a callback on the
// popup, so nothing of the plugin is left inside Hyprland's objects.)
static wl_event_source* g_popupFadeTimer = nullptr;
bool                    popupTreeFading(const PHLWINDOW& window); // Popups.cpp

static int popupFadeTick(void*) {
    for (const auto& overview : scrollOverviews()) {
        if (auto* canvas = canvasOf(overview))
            canvas->damage();
    }
    return 0;
}

bool CScrollOverview::canvasPopupFading() const {
    for (const auto& window : Desktop::windowState()->windows()) {
        if (window && canvasDrawsWindow(window) && popupTreeFading(window))
            return true;
    }
    return false;
}

static void schedulePopupFadeFrame() {
    if (!g_popupFadeTimer && g_pCompositor)
        g_popupFadeTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, popupFadeTick, nullptr);
    if (g_popupFadeTimer)
        wl_event_source_timer_update(g_popupFadeTimer, 4);
}

PHLWINDOW CScrollOverview::canvasDesktopWindowAtPoint(const Vector2D& point, CBox* renderedBox, Vector2D* surfaceLocal) const {
    if (!isCanvasDesktop())
        return {};

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return {};

    const float FACTOR  = std::max(0.01F, scale->value() * MONITOR->m_scale);
    const auto& WINDOWS = Desktop::windowState()->windows();

    // X11 menus and tooltips first: they are on top, at their screen spot.
    for (auto it = WINDOWS.rbegin(); it != WINDOWS.rend(); ++it) {
        const auto& WINDOW = *it;
        if (!canvasScreenFixedWindow(WINDOW))
            continue;
        const auto BOX = canvasDesktopWindowBox(WINDOW);
        if (!BOX.containsPoint(point))
            continue;
        if (renderedBox)
            *renderedBox = BOX;
        if (surfaceLocal)
            *surfaceLocal = (point - BOX.pos()) * (1.F / std::max(0.01F, sc<float>(MONITOR->m_scale)));
        return WINDOW;
    }

    // Popups first: menus hang outside their window, drawn above everything,
    // and must get the pointer there too. Hyprland's hit tester then picks
    // the popup surface from the window-relative point.
    for (auto it = WINDOWS.rbegin(); it != WINDOWS.rend(); ++it) {
        const auto WINDOW = getOverviewWindowToShow(*it);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || WINDOW->m_isX11 || !WINDOW->m_popupHead)
            continue;
        const auto BOX = canvasDesktopWindowBox(WINDOW);
        if (BOX.empty())
            continue;
        const Vector2D LOCAL = (point - BOX.pos()) * (1.F / FACTOR);
        if (!WINDOW->m_popupHead->at(WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT).pos() + LOCAL))
            continue;
        if (renderedBox)
            *renderedBox = BOX;
        if (surfaceLocal)
            *surfaceLocal = LOCAL;
        return WINDOW;
    }

    std::unordered_set<const void*> visited;
    for (auto it = WINDOWS.rbegin(); it != WINDOWS.rend(); ++it) {
        const auto WINDOW = getOverviewWindowToShow(*it);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;

        const auto BOX = canvasDesktopWindowBox(WINDOW);
        if (!BOX.containsPoint(point))
            continue;

        if (renderedBox)
            *renderedBox = BOX;
        if (surfaceLocal)
            *surfaceLocal = (point - BOX.pos()) * (1.F / FACTOR);
        return WINDOW;
    }

    return {};
}

void CScrollOverview::zoomCanvasAt(const Vector2D& point, float requestedZoom, bool animate) {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR)
        return;

    const float OLDZOOM     = std::max(scale->value(), 0.01F);
    const float NEWZOOM     = std::clamp(requestedZoom, ScrollOverview::Config::getCanvasMinZoom(), ScrollOverview::Config::getCanvasMaxZoom());
    const float MONITORSCALE = std::max(MONITOR->m_scale, 0.01F);
    const auto  CENTERPX    = CBox{{}, MONITOR->m_size * MONITORSCALE}.middle();
    const auto  WORLDLOCAL  = ((point + viewOffset->value() * OLDZOOM * MONITORSCALE - CENTERPX) * (1.F / (OLDZOOM * MONITORSCALE))) +
        CBox{{}, MONITOR->m_size}.middle();
    const auto NEWOFFSET = WORLDLOCAL - CBox{{}, MONITOR->m_size}.middle() - (point - CENTERPX) * (1.F / (NEWZOOM * MONITORSCALE));

    if (animate) {
        *viewOffset = NEWOFFSET;
        *scale      = NEWZOOM;
    } else {
        viewOffset->setValueAndWarp(NEWOFFSET);
        *viewOffset = NEWOFFSET;
        scale->setValueAndWarp(NEWZOOM);
        *scale = NEWZOOM;
    }

    markBlurDirty();
    damage();
}

void CScrollOverview::ensureCanvasKeyboardFocus(PHLWINDOW window) {
    if (!isCanvasDesktop() || closing || activeScrollOverview().get() != this || !g_pSeatManager)
        return;

    window = getOverviewWindowToShow(window ? window : Desktop::focusState()->window());
    if (!shouldShowOverviewWindow(window))
        window = getOverviewWindowToShow(closeOnWindow.lock());
    if (!shouldShowOverviewWindow(window) || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    const auto SURFACE = window->wlSurface()->resource();
    if (g_pSeatManager->m_state.keyboardFocus.lock() != SURFACE)
        g_pSeatManager->setKeyboardFocus(SURFACE);
}

bool CScrollOverview::followCanvasWindow(PHLWINDOW window, bool syncFocus, bool animate) {
    window = getOverviewWindowToShow(window);
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR || !shouldShowOverviewWindow(window) || closing)
        return false;

    markCanvasCameraActive(MONITOR);
    // The window you go to is the one you see: in front of the others.
    if (syncFocus)
        (void)Config::Actions::alterZOrder("top", window);

    size_t workspaceIdx = viewportCurrentWorkspace;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace) {
            workspaceIdx = i;
            break;
        }
    }

    selectOverviewWindow(window, workspaceIdx, syncFocus);
    if (syncFocus && Desktop::focusState()->monitor() != MONITOR)
        Desktop::focusState()->rawMonitorFocus(MONITOR);

    const auto CAMERAOFFSET = canvasCameraOffsetFor(window, scale->goal(), canvasNavigationActive);
    if (animate)
        *viewOffset = CAMERAOFFSET;
    else {
        viewOffset->setValueAndWarp(CAMERAOFFSET);
        *viewOffset = CAMERAOFFSET;
    }

    if (Desktop::focusState()->window() == window)
        ensureCanvasKeyboardFocus(window);
    markBlurDirty();
    damage();
    return true;
}

bool CScrollOverview::manageCanvasWindow(PHLWINDOW window, bool placeNew) {
    if (!isCanvasDesktop())
        return false;

    window = getOverviewWindowToShow(window);
    auto TARGET = window ? window->layoutTarget() : nullptr;
    if (!shouldShowOverviewWindow(window) || !TARGET || window->m_pinned)
        return false;

    if (window->m_workspace && window->m_workspace->m_isSpecialWorkspace) return false;
    if (!g_canvasNativeLayout.contains(window->m_stableID))
        g_canvasNativeLayout.emplace(window->m_stableID, saveCanvasWindow(window));

    if (ScrollOverview::Config::getCanvasAutoFloat() && !SpatialOverview::Experiments::on(ECanvasExperiment::Areas) && !TARGET->floating()) {
        const auto RESULT = setCanvasFloating(window, true);
        if (!RESULT)
            return false;
        TARGET = window->layoutTarget();
        if (!TARGET)
            return false;
    }

    if (!g_canvasManagedTargets.emplace(TARGET.get()).second)
        return true;

    if (const auto saved = g_canvasSpatialLayout.find(window->m_stableID); saved != g_canvasSpatialLayout.end()) {
        TARGET->rememberFloatingSize(saved->second.size());
        TARGET->setPositionGlobal(saved->second);
        TARGET->warpPositionSize();
        return true;
    }
    if (placeNew && ScrollOverview::Config::getCanvasRememberLayout()) {
        if (const auto HOME = SpatialOverview::Memory::claim(window, Time::steadyNow() < g_canvasMemoryRestoreUntil)) {
            TARGET->rememberFloatingSize(HOME->size());
            TARGET->setPositionGlobal(*HOME);
            TARGET->warpPositionSize();
            TARGET->damageEntire();
            return true;
        }
    }
    if (!placeNew || !ScrollOverview::Config::getCanvasAutoPlace()) {
        TARGET->warpPositionSize();
        return true;
    }

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;

    auto SIZE = TARGET->lastFloatingSize();
    if (SIZE.x <= 1.F || SIZE.y <= 1.F || SIZE.x > MONITOR->m_size.x * 0.9F || SIZE.y > MONITOR->m_size.y * 0.9F)
        SIZE = MONITOR->m_size * 0.42F;

    const auto CENTERPX    = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    const auto WORLDCENTER = overviewPointToGlobal(0, CENTERPX);
    const float GAP        = sc<float>(ScrollOverview::Config::getCanvasPlacementGap());
    const float STEPX      = SIZE.x + GAP;
    const float STEPY      = SIZE.y + GAP;
    CBox       PLACEMENT{WORLDCENTER - SIZE / 2.F, SIZE};

    // In a tiled place the tiling finds it a spot.
    if (const int PLACE = placeAt(MONITOR, WORLDCENTER); PLACE && placeTiling(PLACE)) {
        TARGET->rememberFloatingSize(SIZE);
        TARGET->setPositionGlobal(PLACEMENT);
        TARGET->warpPositionSize();
        return true;
    }

    const auto occupied = [&](const CBox& candidate) {
        for (const auto& existingRef : Desktop::windowState()->windows()) {
            const auto EXISTING = getOverviewWindowToShow(existingRef);
            if (!shouldShowOverviewWindow(EXISTING) || EXISTING == window || !EXISTING->layoutTarget())
                continue;
            auto BOX = EXISTING->layoutTarget()->position();
            BOX.expand(GAP * 0.5F);
            if (BOX.overlaps(candidate))
                return true;
        }
        return false;
    };

    bool found = !occupied(PLACEMENT);
    for (int ring = 1; !found && ring <= 16; ++ring) {
        std::vector<Vector2D> OFFSETS{
            {sc<float>(ring), 0.F}, {-sc<float>(ring), 0.F}, {0.F, sc<float>(ring)}, {0.F, -sc<float>(ring)},
            {sc<float>(ring), sc<float>(ring)}, {-sc<float>(ring), sc<float>(ring)}, {sc<float>(ring), -sc<float>(ring)}, {-sc<float>(ring), -sc<float>(ring)},
        };
        for (const auto& offset : OFFSETS) {
            PLACEMENT = CBox{WORLDCENTER - SIZE / 2.F + Vector2D{offset.x * STEPX, offset.y * STEPY}, SIZE};
            if (!occupied(PLACEMENT)) {
                found = true;
                break;
            }
        }
    }

    PLACEMENT = snapCanvasWindowBox(PLACEMENT, MONITOR);
    TARGET->rememberFloatingSize(PLACEMENT.size());
    TARGET->setPositionGlobal(PLACEMENT);
    TARGET->warpPositionSize();
    TARGET->damageEntire();
    return true;
}

bool CScrollOverview::arrangeCanvasWindows() {
    checkpointCanvas();
    canvasPlacementFailure.clear();
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR || closing) {
        canvasPlacementFailure = "continuous canvas mode is not active";
        return false;
    }

    struct SArrangeItem {
        PHLWINDOW                 window;
        SP<Layout::ITarget>       target;
        CBox                      originalBox;
        Vector2D                  arrangedSize;
        ECanvasWindowCategory     category = ECanvasWindowCategory::OTHER;
        std::set<std::string>     tokens;
        std::string               groupKey;
    };

    std::vector<SArrangeItem> items;
    std::unordered_set<const void*> visited;
    const std::vector<PHLWINDOW> windows{Desktop::windowState()->windows().begin(), Desktop::windowState()->windows().end()};
    items.reserve(windows.size());

    for (const auto& windowRef : windows) {
        auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;

        manageCanvasWindow(WINDOW, false);
        const auto TARGET = WINDOW->layoutTarget();
        if (!TARGET)
            continue;

        auto BOX = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (BOX.width <= 1.F || BOX.height <= 1.F)
            BOX = CBox{BOX.pos(), TARGET->lastFloatingSize()};
        if (BOX.width <= 1.F || BOX.height <= 1.F)
            continue;

        items.emplace_back(SArrangeItem{
            .window       = WINDOW,
            .target       = TARGET,
            .originalBox  = BOX,
            .arrangedSize = BOX.size(),
            .category     = canvasWindowCategory(WINDOW),
            .tokens       = canvasContextTokens(WINDOW),
        });
    }

    if (items.empty()) {
        canvasPlacementFailure = "there are no canvas windows to arrange";
        return false;
    }

    if (ScrollOverview::Config::getCanvasArrangeContextGrouping()) {
        std::map<std::string, std::vector<size_t>> tokenWindows;
        for (size_t i = 0; i < items.size(); ++i) {
            for (const auto& token : items[i].tokens)
                tokenWindows[token].emplace_back(i);
        }

        for (size_t i = 0; i < items.size(); ++i) {
            std::string bestToken;
            int         bestScore = -1;
            for (const auto& token : items[i].tokens) {
                const auto IT = tokenWindows.find(token);
                if (IT == tokenWindows.end() || IT->second.size() < 2)
                    continue;

                std::set<ECanvasWindowCategory> categories;
                for (const auto index : IT->second)
                    categories.emplace(items[index].category);
                if (categories.size() < 2)
                    continue;

                // Longer shared words tend to be project/product names; a
                // small rarity bonus prevents generic title fragments from
                // swallowing a more precise local context.
                const int SCORE = sc<int>(token.size() * 100) - sc<int>(IT->second.size());
                if (SCORE > bestScore) {
                    bestScore = SCORE;
                    bestToken = token;
                }
            }

            if (!bestToken.empty())
                items[i].groupKey = "00-context-" + bestToken;
        }
    }

    for (auto& item : items) {
        if (item.groupKey.empty())
            item.groupKey = canvasCategoryKey(item.category);
    }

    const float GRID            = sc<float>(ScrollOverview::Config::getCanvasGridSize());
    const float SIMILARITY      = ScrollOverview::Config::getCanvasArrangeSizeSimilarity();
    const float RESIZELIMIT     = ScrollOverview::Config::getCanvasArrangeResizeLimit();
    const auto relativeChange = [](const Vector2D& from, const Vector2D& to) {
        return std::max(std::abs(to.x - from.x) / std::max(1.0, from.x), std::abs(to.y - from.y) / std::max(1.0, from.y));
    };
    const auto snappedSize = [&](const Vector2D& size) {
        return Vector2D{
            std::max(sc<double>(GRID * 2.F), std::round(size.x / GRID) * GRID),
            std::max(sc<double>(GRID * 2.F), std::round(size.y / GRID) * GRID),
        };
    };

    // Every window first receives the nearest grid-sized geometry when that is
    // within the resize safety limit. Similar windows in the same semantic
    // group are then normalized to one shared size only when every member can
    // reach it without exceeding that same limit.
    for (auto& item : items) {
        const auto CANDIDATE = snappedSize(item.originalBox.size());
        if (relativeChange(item.originalBox.size(), CANDIDATE) <= RESIZELIMIT)
            item.arrangedSize = CANDIDATE;
    }

    std::map<std::string, std::vector<size_t>> groupedIndices;
    for (size_t i = 0; i < items.size(); ++i)
        groupedIndices[items[i].groupKey].emplace_back(i);

    for (const auto& [key, indices] : groupedIndices) {
        std::unordered_set<size_t> matched;
        for (const auto seed : indices) {
            if (matched.contains(seed))
                continue;

            std::vector<size_t> cluster{seed};
            matched.emplace(seed);
            const auto SEEDSIZE = items[seed].originalBox.size();
            for (const auto candidate : indices) {
                if (matched.contains(candidate))
                    continue;
                const auto SIZE = items[candidate].originalBox.size();
                const float DW  = std::abs(SIZE.x - SEEDSIZE.x) / std::max({1.0, SIZE.x, SEEDSIZE.x});
                const float DH  = std::abs(SIZE.y - SEEDSIZE.y) / std::max({1.0, SIZE.y, SEEDSIZE.y});
                if (DW <= SIMILARITY && DH <= SIMILARITY) {
                    cluster.emplace_back(candidate);
                    matched.emplace(candidate);
                }
            }

            if (cluster.size() < 2)
                continue;

            Vector2D average;
            for (const auto index : cluster)
                average += items[index].originalBox.size();
            average /= sc<float>(cluster.size());
            const auto COMMONSIZE = snappedSize(average);
            if (!std::all_of(cluster.begin(), cluster.end(), [&](size_t index) {
                    return relativeChange(items[index].originalBox.size(), COMMONSIZE) <= RESIZELIMIT;
                }))
                continue;

            for (const auto index : cluster)
                items[index].arrangedSize = COMMONSIZE;
        }
    }

    struct SGroupLayout {
        std::string           key;
        std::vector<size_t>   items;
        std::vector<Vector2D> localPositions;
        Vector2D              size;
        Vector2D              packedPosition;
    };

    std::vector<SGroupLayout> groups;
    groups.reserve(groupedIndices.size());
    const float WINDOWGAP = GRID;
    for (const auto& [key, indices] : groupedIndices) {
        SGroupLayout group{.key = key, .items = indices};
        const size_t COUNT = indices.size();
        double averageAspect = 0.0;
        for (const auto index : indices)
            averageAspect += items[index].arrangedSize.x / std::max(1.0, items[index].arrangedSize.y);
        averageAspect /= std::max<size_t>(1, COUNT);

        size_t columns = COUNT <= 1 ? 1 : sc<size_t>(std::ceil(std::sqrt(sc<double>(COUNT))));
        if (COUNT <= 3 && averageAspect >= 1.35)
            columns = 1; // compact landscape sets read better as a column
        const size_t rows = (COUNT + columns - 1) / columns;

        std::vector<float> columnWidths(columns, 0.F);
        std::vector<float> rowHeights(rows, 0.F);
        for (size_t i = 0; i < COUNT; ++i) {
            const auto SIZE = items[indices[i]].arrangedSize;
            columnWidths[i % columns] = std::max(columnWidths[i % columns], sc<float>(SIZE.x));
            rowHeights[i / columns]   = std::max(rowHeights[i / columns], sc<float>(SIZE.y));
        }

        std::vector<float> columnX(columns, 0.F);
        std::vector<float> rowY(rows, 0.F);
        for (size_t i = 1; i < columns; ++i)
            columnX[i] = columnX[i - 1] + columnWidths[i - 1] + WINDOWGAP;
        for (size_t i = 1; i < rows; ++i)
            rowY[i] = rowY[i - 1] + rowHeights[i - 1] + WINDOWGAP;

        group.localPositions.reserve(COUNT);
        for (size_t i = 0; i < COUNT; ++i)
            group.localPositions.emplace_back(columnX[i % columns], rowY[i / columns]);
        group.size = Vector2D{
            columnX.back() + columnWidths.back(),
            rowY.back() + rowHeights.back(),
        };
        groups.emplace_back(std::move(group));
    }

    const float GROUPGAP    = GRID * 2.F;
    float       maxRowWidth = MONITOR->m_size.x * 1.9F;
    for (const auto& group : groups)
        maxRowWidth = std::max(maxRowWidth, sc<float>(group.size.x));

    float cursorX = 0.F;
    float cursorY = 0.F;
    float rowHeight = 0.F;
    float packedWidth = 0.F;
    for (auto& group : groups) {
        if (cursorX > 0.F && cursorX + group.size.x > maxRowWidth) {
            cursorX = 0.F;
            cursorY += rowHeight + GROUPGAP;
            rowHeight = 0.F;
        }
        group.packedPosition = Vector2D{cursorX, cursorY};
        cursorX += group.size.x + GROUPGAP;
        rowHeight = std::max(rowHeight, sc<float>(group.size.y));
        packedWidth = std::max(packedWidth, cursorX - GROUPGAP);
    }
    const float packedHeight = cursorY + rowHeight;

    const auto CAMERA_CENTER = MONITOR->m_position + viewOffset->value() + MONITOR->m_size * 0.5F;
    Vector2D   worldOrigin   = CAMERA_CENTER - Vector2D{packedWidth, packedHeight} * 0.5F;
    worldOrigin.x            = std::round(worldOrigin.x / GRID) * GRID;
    worldOrigin.y            = std::round(worldOrigin.y / GRID) * GRID;

    for (const auto& group : groups) {
        for (size_t i = 0; i < group.items.size(); ++i) {
            auto& item = items[group.items[i]];
            const CBox BOX{worldOrigin + group.packedPosition + group.localPositions[i], item.arrangedSize};
            item.target->rememberFloatingSize(BOX.size());
            item.target->setPositionGlobal(BOX);
            item.target->warpPositionSize();
            item.target->damageEntire();
        }
    }

    redrawAll();
    markBlurDirty();
    for (const auto& overview : scrollOverviews()) {
        if (overview)
            overview->damage();
    }
    ensureCanvasKeyboardFocus();
    noteCanvasLayoutChanged();
    return true;
}

void CScrollOverview::seedCanvasWindows() {
    if (!isCanvasDesktop())
        return;

    const std::vector<PHLWINDOW> WINDOWS{Desktop::windowState()->windows().begin(), Desktop::windowState()->windows().end()};
    for (const auto& window : WINDOWS)
        manageCanvasWindow(window, true);
}

void CScrollOverview::forwardCanvasPointerMotion(uint32_t timeMs) {
    if (!isCanvasDesktop() || !ScrollOverview::Config::getCanvasDirectInput() || spacePanHeld || scrollingPanPointerDown || dragActiveWindow || resizeActiveWindow)
        return;

    CBox       BOX;
    Vector2D   WINDOWLOCAL;
    Vector2D   SURFACELOCAL;
    const auto PREVIOUSWINDOW  = canvasForwardedPointerWindow.lock();
    auto       WINDOW          = PREVIOUSWINDOW;
    auto       SURFACE         = canvasForwardedPointerSurface.lock();
    if (canvasForwardedPointerButtons.empty())
        WINDOW = canvasDesktopWindowAtPoint(lastMousePosLocal, &BOX, &WINDOWLOCAL);
    else if (WINDOW) {
        BOX = canvasDesktopWindowBox(WINDOW);
        const auto MONITOR = pMonitor.lock();
        const float FACTOR = MONITOR ? std::max(0.01F, (canvasScreenFixedWindow(WINDOW) ? 1.F : scale->value()) * MONITOR->m_scale) : 1.F;
        WINDOWLOCAL        = (lastMousePosLocal - BOX.pos()) * (1.F / FACTOR);
    }

    // The X server has to agree about where this window is on the monitor
    // the pointer is using it on.
    if (WINDOW && WINDOW->m_isX11)
        canvasClaimX11Window(WINDOW);

    if (WINDOW && WINDOW->wlSurface() && WINDOW->wlSurface()->resource()) {
        const auto WORLDPOINT = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT).pos() + WINDOWLOCAL;
        // windowSurfaceAt is Wayland-only and aborts for XWayland windows.
        // X11 clients expose one surface; convert the canvas-local position to
        // its native coordinates, including Hyprland's force-zero scaling.
        if (WINDOW->m_isX11) {
            SURFACE      = WINDOW->wlSurface()->resource();
            SURFACELOCAL = WINDOWLOCAL * WINDOW->m_X11SurfaceScaledBy;
        } else if (canvasForwardedPointerButtons.empty())
            SURFACE = Desktop::viewState()->hitTest().windowSurfaceAt(WORLDPOINT, WINDOW, SURFACELOCAL);
        else if (SURFACE)
            SURFACELOCAL = Desktop::viewState()->hitTest().surfaceLocalAt(WORLDPOINT, WINDOW, SURFACE);

        // Keep a top-level fallback for clients with an unusual or temporarily
        // empty input region, but prefer the exact popup/subsurface returned by
        // Hyprland's normal window hit tester. Chromium's web content commonly
        // lives on one of those child surfaces rather than on the xdg_toplevel.
        if (!SURFACE) {
            SURFACE      = WINDOW->wlSurface()->resource();
            SURFACELOCAL = Desktop::viewState()->hitTest().surfaceLocalAt(WORLDPOINT, WINDOW, SURFACE);
        }
    }

    if (hoverFocusSettling) {
        if (!hoverFocusSettleSeen) {
            hoverFocusSettleSeen   = true;
            hoverFocusSettleWindow = WINDOW;
        } else if (WINDOW != hoverFocusSettleWindow.lock())
            hoverFocusSettling = false;
    }

    if (!WINDOW || !SURFACE) {
        if (canvasForwardedPointerButtons.empty()) {
            canvasForwardedPointerWindow.reset();
            canvasForwardedPointerSurface.reset();
            g_pSeatManager->setPointerFocus(nullptr, {});
        }
        return;
    }

    canvasForwardedPointerWindow  = WINDOW;
    canvasForwardedPointerSurface = SURFACE;
    g_pSeatManager->setPointerFocus(SURFACE, SURFACELOCAL);
    g_pSeatManager->sendPointerMotion(timeMs ? timeMs : Time::millis(Time::steadyNow()), SURFACELOCAL);
    g_pSeatManager->sendPointerFrame();

    if (ScrollOverview::Config::getCanvasHoverFocus() && !(canvasNavigationActive && SpatialOverview::Navigator::isOpen()) && !hoverFocusSettling &&
        canvasForwardedPointerButtons.empty() && WINDOW != PREVIOUSWINDOW) {
        size_t workspaceIdx = viewportCurrentWorkspace;
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == WINDOW->m_workspace) {
                workspaceIdx = i;
                break;
            }
        }
        selectOverviewWindow(WINDOW, workspaceIdx, true);
    }
}

bool CScrollOverview::forwardCanvasPointerButton(const IPointer::SButtonEvent& event) {
    if (!isCanvasDesktop() || !ScrollOverview::Config::getCanvasDirectInput())
        return false;

    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED)
        forwardCanvasPointerMotion(event.timeMs);

    const auto WINDOW = canvasForwardedPointerWindow.lock();
    if (!WINDOW || !WINDOW->wlSurface() || !WINDOW->wlSurface()->resource())
        return false;

    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        canvasForwardedPointerButtons.emplace(event.button);
        size_t workspaceIdx = viewportCurrentWorkspace;
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == WINDOW->m_workspace) {
                workspaceIdx = i;
                break;
            }
        }
        selectOverviewWindow(WINDOW, workspaceIdx, true);
        ensureCanvasKeyboardFocus(WINDOW);
    } else
        canvasForwardedPointerButtons.erase(event.button);

    g_pSeatManager->sendPointerButton(event.timeMs, event.button, event.state);
    g_pSeatManager->sendPointerFrame();
    return true;
}

bool CScrollOverview::forwardCanvasPointerAxis(const IPointer::SAxisEvent& event) {
    if (!isCanvasDesktop() || !ScrollOverview::Config::getCanvasDirectInput())
        return false;

    forwardCanvasPointerMotion(event.timeMs);
    if (!canvasForwardedPointerWindow || !canvasForwardedPointerSurface)
        return false;

    // Libinput already reports the wheel in v120 units (120 = one notch).
    // Passing that number through as a discrete step, then multiplying it by
    // 120 again for the v120 axis, makes every notch scroll about 120 lines.
    const bool wheel = event.source == WL_POINTER_AXIS_SOURCE_WHEEL || event.source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
    if (!wheel) {
        g_pSeatManager->sendPointerAxis(event.timeMs, event.axis, event.delta, event.deltaDiscrete, 0, event.source, event.relativeDirection);
        g_pSeatManager->sendPointerFrame();
        return true;
    }

    const double factor   = std::max(0.0, sc<double>(ScrollOverview::Config::getMouseScrollFactor()));
    const double scaledV120 = sc<double>(event.deltaDiscrete) * factor;
    const int32_t value120  = sc<int32_t>(scaledV120 >= 0.0 ? scaledV120 + 0.5 : scaledV120 - 0.5);
    const bool    directionChanged =
        value120 != 0 && canvasWheelValue120Accum != 0 && ((value120 > 0) != (canvasWheelValue120Accum > 0));
    if (event.axis != canvasWheelAccumAxis || directionChanged || event.timeMs - canvasWheelAccumTimeMs > 500)
        canvasWheelValue120Accum = 0;

    canvasWheelAccumAxis   = event.axis;
    canvasWheelAccumTimeMs = event.timeMs;
    canvasWheelValue120Accum += value120;

    const int32_t discreteSteps = canvasWheelValue120Accum / 120;
    canvasWheelValue120Accum -= discreteSteps * 120;

    g_pSeatManager->sendPointerAxis(event.timeMs, event.axis, event.delta * factor, discreteSteps, value120, event.source, event.relativeDirection);
    g_pSeatManager->sendPointerFrame();
    return true;
}

Vector2D CScrollOverview::canvasCellForWorkspaceIndex(size_t workspaceIdx) const {
    if (workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return {};

    if (const auto IT = g_canvasWorkspaceCells.find(images[workspaceIdx]->pWorkspace->m_id); IT != g_canvasWorkspaceCells.end())
        return IT->second;

    const int COLUMNS = std::max(1, ScrollOverview::Config::getGridColumns());
    return Vector2D{sc<float>(workspaceIdx % sc<size_t>(COLUMNS)), sc<float>(workspaceIdx / sc<size_t>(COLUMNS))};
}

Vector2D CScrollOverview::canvasCellAtOverviewPoint(const Vector2D& point) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || images.empty())
        return {};

    const float SCALE        = std::max(scale->value(), 0.01F);
    const float MONITORSCALE = std::max(MONITOR->m_scale, 0.01F);
    const auto  CENTER       = CBox{{}, MONITOR->m_size * MONITORSCALE}.middle();
    const auto  WORLDPOINT   = ((point + viewOffset->value() * SCALE * MONITORSCALE - CENTER) * (1.F / SCALE) + CENTER) * (1.F / MONITORSCALE);
    const auto  ACTIVECELL   = canvasCellForWorkspaceIndex(activeWorkspaceIndex());

    return Vector2D{
        ACTIVECELL.x + std::floor(WORLDPOINT.x / std::max(MONITOR->m_size.x, 1.0)),
        ACTIVECELL.y + std::floor(WORLDPOINT.y / std::max(MONITOR->m_size.y, 1.0)),
    };
}

CBox CScrollOverview::snapCanvasWindowBox(const CBox& box, PHLMONITOR monitor) const {
    if (!ScrollOverview::Config::getCanvasEnabled() || !ScrollOverview::Config::getCanvasSnapEnabled() || !monitor)
        return box;

    const float SNAP = sc<float>(ScrollOverview::Config::getCanvasSnapSize());
    auto        result = box;
    // The infinite canvas has one world lattice. Anchoring at an output's
    // origin would shift the snap points whenever monitor positions are not an
    // exact multiple of the grid interval.
    result.x = std::round(result.x / SNAP) * SNAP;
    result.y = std::round(result.y / SNAP) * SNAP;
    return result;
}

bool CScrollOverview::placeWindowOnCanvasCell(int x, int y) {
    checkpointCanvas();
    canvasPlacementFailure.clear();
    auto WINDOW = getOverviewWindowToShow(closeOnWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW))
        WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    if (!shouldShowOverviewWindow(WINDOW)) {
        for (const auto& candidate : Desktop::windowState()->windows()) {
            WINDOW = getOverviewWindowToShow(candidate);
            if (WINDOW && WINDOW->layoutTarget())
                break;
        }
    }

    const auto MONITOR = pMonitor.lock();
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    if (!WINDOW || !TARGET || !MONITOR || !valid(WINDOW->m_workspace)) {
        canvasPlacementFailure = "selected window is no longer attached to a live workspace";
        return false;
    }

    const bool WASTILED = !TARGET->floating();
    if (WASTILED) {
        const auto FLOAT = setCanvasFloating(WINDOW, true);
        if (!FLOAT || !valid(WINDOW->m_workspace)) {
            canvasPlacementFailure = !FLOAT ? FLOAT.error().message : "window lost its workspace while becoming floating";
            return false;
        }
    }

    if (!valid(WINDOW->m_workspace)) {
        canvasPlacementFailure = "source workspace expired before the canvas placement";
        return false;
    }

    auto SIZE = TARGET->lastFloatingSize();
    if (WASTILED || SIZE.x <= 1.F || SIZE.y <= 1.F || SIZE.x > MONITOR->m_size.x * 0.9F || SIZE.y > MONITOR->m_size.y * 0.9F)
        SIZE = MONITOR->m_size * 0.55F;

    const float GRID = sc<float>(ScrollOverview::Config::getCanvasGridSize());
    auto BOX = snapCanvasWindowBox(CBox{MONITOR->m_position + Vector2D{sc<float>(x) * GRID, sc<float>(y) * GRID}, SIZE}, MONITOR);
    TARGET->rememberFloatingSize(BOX.size());
    TARGET->setPositionGlobal(BOX);
    TARGET->warpPositionSize();

    redrawAll();
    closeOnWindow = WINDOW;
    damage();
    return true;
}

bool CScrollOverview::setCanvasViewport(int x, int y) {
    canvasPlacementFailure.clear();
    if (!ScrollOverview::Config::getCanvasEnabled() || layout != ScrollOverview::Config::ELayout::GRID || !pMonitor) {
        canvasPlacementFailure = "continuous grid canvas is not active";
        return false;
    }

    viewOffset->setValueAndWarp(Vector2D{sc<float>(x), sc<float>(y)});
    *viewOffset = Vector2D{sc<float>(x), sc<float>(y)};
    updateViewportWorkspaceFromCanvasCenter();
    markBlurDirty();
    damage();
    return true;
}

const std::string& CScrollOverview::canvasPlacementError() const {
    return canvasPlacementFailure;
}

static void syncWorkspaceGeometry(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return;

    for (const auto& targetRef : workspace->m_space->targets()) {
        const auto TARGET = targetRef.lock();
        if (TARGET)
            TARGET->warpPositionSize();
    }
}

bool CScrollOverview::selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus) {
    if (!window)
        return false;

    closeOnWindow            = window;
    viewportCurrentWorkspace = workspaceIdx;
    rememberSelection(window);
    if (syncFocus) {
        if (const auto MONITOR = pMonitor.lock(); MONITOR && Desktop::focusState()->monitor() != MONITOR)
            Desktop::focusState()->rawMonitorFocus(MONITOR);
        syncFocusedSelection();
    }
    damage();
    return true;
}

bool CScrollOverview::selectWindowAtOverviewCursor(bool syncFocus) {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t    workspaceIdx = viewportCurrentWorkspace;
    PHLWINDOW window       = windowAtOverviewCursor(&workspaceIdx);

    return selectOverviewWindow(window, workspaceIdx, syncFocus);
}

void CScrollOverview::selectHoveredWorkspace() {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t     workspaceIdx = viewportCurrentWorkspace;
    const auto WORKSPACE    = workspaceAtOverviewCursor(&workspaceIdx);
    if (!WORKSPACE)
        return;

    closeOnWindow.reset();
    viewportCurrentWorkspace = workspaceIdx;

    if (pMonitor && pMonitor->m_activeWorkspace != WORKSPACE) {
        if (focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
            focusSyncedFromWorkspaceID = pMonitor->m_activeWorkspace ? pMonitor->m_activeWorkspace->m_id : WORKSPACE_INVALID;
        pMonitor->changeWorkspace(WORKSPACE, false, true, true);
    }

    damage();
}

bool CScrollOverview::windowDispatcherAction(const std::string& action) {
    lastMousePosLocal = getOverviewMousePosLocal(pMonitor.lock());

    size_t    workspaceIdx = viewportCurrentWorkspace;
    PHLWINDOW WINDOW       = windowAtOverviewCursor(&workspaceIdx);

    if (!WINDOW)
        return false;

    if (action == "select")
        return selectOverviewWindow(WINDOW, workspaceIdx, true);

    if (action == "close") {
        WINDOW->sendClose();
        damage();
        return true;
    }

    return false;
}

Vector2D CScrollOverview::overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return pointLocal;

    const auto  SAFE_SCALE       = std::max(scale->value(), 0.01F);
    const auto  SAFE_MON_SCALE   = std::max(MONITOR->m_scale, 0.01F);
    const auto  VIEWPORT_CENTER  = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();
    const auto  VIEWPORT_CENTER_LOGICAL = CBox{{}, MONITOR->m_size}.middle();
    const auto  WORKSPACE_OFFSET   = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));

    return ((pointLocal - WORKSPACE_OFFSET + viewOffset->value() * scale->value() * SAFE_MON_SCALE - VIEWPORT_CENTER) * (1.F / (SAFE_SCALE * SAFE_MON_SCALE))) +
        VIEWPORT_CENTER_LOGICAL + MONITOR->m_position;
}

CBox CScrollOverview::draggedWindowBox(size_t workspaceIdx) const {
    const auto WINDOW  = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACE_OFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box               = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACE_OFFSET, layout);
    box.x = lastMousePosLocal.x - dragGrabOffsetLocal.x;
    box.y = lastMousePosLocal.y - dragGrabOffsetLocal.y;

    return box;
}

CBox CScrollOverview::draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const {
    window             = getOverviewWindowToShow(window);
    const auto MONITOR = pMonitor.lock();
    if (!window || !MONITOR || workspaceIdx >= images.size())
        return {};

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    auto       box             = getOverviewDragWindowBox(window, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    box.x = pointLocal.x - box.width * std::clamp(grabRatio.x, 0.0, 1.0);
    box.y = pointLocal.y - box.height * std::clamp(grabRatio.y, 0.0, 1.0);
    return box;
}

CBox CScrollOverview::draggedWindowGlobalBox() const {
    const auto WINDOW  = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR)
        return {};

    const auto WORKSPACEIDX = dragWorkspaceIndex(WINDOW);
    if (WORKSPACEIDX >= images.size())
        return {};

    const auto WORKSPACEOFFSET =
        workspaceOverviewOffset(WORKSPACEIDX, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto SOURCEBOX =
        getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false);
    const auto GLOBALSIZE = SOURCEBOX.size() * (1.F / std::max(MONITOR->m_scale, 0.01F));
    const auto CURSOR     = g_pInputManager->getMouseCoordsInternal();

    return {
        CURSOR.x - GLOBALSIZE.x * std::clamp(dragGrabRatio.x, 0.0, 1.0),
        CURSOR.y - GLOBALSIZE.y * std::clamp(dragGrabRatio.y, 0.0, 1.0),
        GLOBALSIZE.x,
        GLOBALSIZE.y,
    };
}

void CScrollOverview::refreshDragOriginalOverviewBoxes() {
    const auto WINDOW    = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto WORKSPACE = dragOriginalWorkspace.lock();
    const auto MONITOR   = pMonitor.lock();
    if (!WINDOW || !WORKSPACE || !MONITOR || dragOriginalVisualBox.empty() || dragOriginalBox.empty())
        return;

    size_t workspaceIdx = images.size();
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == WORKSPACE) {
            workspaceIdx = i;
            break;
        }
    }

    if (workspaceIdx >= images.size())
        return;

    Vector2D tapeDelta;
    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACE); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
        tapeDelta = overviewScrollingCameraTranslation(ALGO) - dragOriginalTapeTranslation;

    auto visualBox = dragOriginalVisualBox;
    auto hitbox    = dragOriginalBox;
    visualBox.translate(tapeDelta);
    hitbox.translate(tapeDelta);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    dragOriginalOverviewBox    = getOverviewGlobalBox(visualBox, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    dragOriginalOverviewHitbox = getOverviewGlobalBox(hitbox, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout, false);
}

CBox CScrollOverview::resizedWindowBox() const {
    const auto WINDOW  = getOverviewWindowToShow(resizeActiveWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!WINDOW || !MONITOR || resizeWorkspaceIdx >= images.size() || !WINDOW->m_isFloating)
        return {};

    const float SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto  TARGET      = WINDOW->layoutTarget();
    const auto  DELTA       = lastMousePosLocal - resizeStartMouseLocal;

    Vector2D    minSizePx   = Vector2D{1.F, 1.F};
    if (TARGET) {
        if (const auto MINSIZE = TARGET->minSize(); MINSIZE.has_value())
            minSizePx = Vector2D{std::max(1.F, sc<float>(MINSIZE->x * SCALEFACTOR)), std::max(1.F, sc<float>(MINSIZE->y * SCALEFACTOR))};
    }

    std::optional<Vector2D> maxSizePx;
    if (TARGET) {
        if (const auto MAXSIZE = TARGET->maxSize(); MAXSIZE.has_value() && MAXSIZE->x > 0.F && MAXSIZE->y > 0.F)
            maxSizePx = Vector2D{std::max(minSizePx.x, sc<double>(MAXSIZE->x * SCALEFACTOR)), std::max(minSizePx.y, sc<double>(MAXSIZE->y * SCALEFACTOR))};
    }

    auto box = resizedOverviewBoxFromCorner(resizeOriginalBox, DELTA, resizeCorner, minSizePx, maxSizePx);

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resizeWorkspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    const auto WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto BORDERMARGIN = WINDOW->getRealBorderSize() * MONITOR->m_scale * scale->value();

    if (ScrollOverview::Config::getCanvasEnabled() && ScrollOverview::Config::getCanvasAllowWindowOverflow())
        return box;

    return clampResizedOverviewBoxToWorkspace(box, WORKSPACEBOX, resizeCorner, BORDERMARGIN);
}

// Apps that draw their own title bar (Chromium, GTK header bars) ask the
// compositor to move or resize them when dragged there. Hyprland's drag works
// in real coordinates and ends on a button release it never sees here (the
// canvas forwards the pointer itself), which left a stuck grab. The canvas's
// own drag takes over instead, with the button the app was pressed with.
bool CScrollOverview::beginClientWindowGesture(PHLWINDOW window, std::optional<Layout::eRectCorner> resizeEdge) {
    window = getOverviewWindowToShow(window);
    if (!isCanvasDesktop() || closing || !shouldShowOverviewWindow(window) || shouldShowPinnedFloatingOverviewWindow(window) || dragActiveWindow || resizeActiveWindow ||
        canvasForwardedPointerButtons.empty() || getOverviewWindowToShow(canvasForwardedPointerWindow.lock()) != window)
        return false;

    const uint32_t BUTTON = *canvasForwardedPointerButtons.begin();
    canvasForwardedPointerButtons.clear();
    canvasForwardedPointerWindow.reset();
    canvasForwardedPointerSurface.reset();
    // The app sees the pointer leave, as it would under a compositor grab.
    g_pSeatManager->setPointerFocus(nullptr, {});
    g_pointerGrabOverview = this;
    lastMousePosLocal     = getOverviewMousePosLocal(pMonitor.lock());

    if (resizeEdge && *resizeEdge != Layout::CORNER_NONE) {
        resizePendingWindow   = window;
        resizePointerDown     = true;
        resizeStartMouseLocal = lastMousePosLocal;
        resizeCorner          = *resizeEdge;
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == window->m_workspace)
                resizeWorkspaceIdx = i;
        }
        beginWindowResize();
        resizeCorner = *resizeEdge; // beginWindowResize must not re-derive it
        if (!resizeActiveWindow) {
            resizePointerDown = false;
            resizePendingWindow.reset();
            return false;
        }
    } else {
        dragStartMouseLocal = lastMousePosLocal;
        beginWindowDrag(window);
        if (!dragActiveWindow)
            return false;
    }
    clientGestureButton = BUTTON;
    requestInputFrame();
    return true;
}

bool canvasTakeClientWindowGesture(const PHLWINDOW& window, std::optional<Layout::eRectCorner> resizeEdge) {
    for (const auto& overview : scrollOverviews()) {
        if (auto* canvas = canvasOf(overview); canvas && canvas->beginClientWindowGesture(window, resizeEdge))
            return true;
    }
    return false;
}

void CScrollOverview::beginWindowDrag(PHLWINDOW window) {
    checkpointCanvas();
    const auto WINDOW = getOverviewWindowToShow(window);
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget())
        return;

    g_pseudoFocusedWindow.reset();
    g_pseudoFocusUntil = {};

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    size_t workspaceIdx = viewportCurrentWorkspace;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            workspaceIdx              = i;
            break;
        }
    }

    const auto TARGET             = WINDOW->layoutTarget();
    dragStartedTiled              = !TARGET->floating();
    if (ScrollOverview::Config::getCanvasEnabled() && ScrollOverview::Config::getCanvasFloatOnDrag() && dragStartedTiled) {
        const auto SPACE = TARGET->space();
        const auto ALGO  = SPACE ? SPACE->algorithm() : nullptr;
        if (ALGO) {
            ALGO->setFloating(TARGET, true, true);
            dragStartedTiled = false;
        }
    }
    dragOriginalFloatSize         = TARGET->lastFloatingSize();
    dragOriginalTapeTranslation  = Vector2D{};
    dragOriginalWorkspace         = WINDOW->m_workspace;
    dragOriginalBox               = TARGET->position();
    dragOriginalVisualBox         = WINDOW->m_group ? TARGET->position() : WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    dragOriginalOverviewBox       = CBox{};
    dragOriginalOverviewHitbox    = CBox{};
    dragActiveWindow              = WINDOW;

    const auto MONITOR = pMonitor.lock();
    if (MONITOR && workspaceIdx < images.size()) {
        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
        const auto WINDOWBOX       = getOverviewDragWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
        dragGrabOffsetLocal        = dragStartMouseLocal - WINDOWBOX.pos();
        dragGrabRatio              = Vector2D{
            WINDOWBOX.width > 0.0 ? dragGrabOffsetLocal.x / WINDOWBOX.width : 0.5,
            WINDOWBOX.height > 0.0 ? dragGrabOffsetLocal.y / WINDOWBOX.height : 0.5,
        };
        if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(WINDOW->m_workspace); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller)
            dragOriginalTapeTranslation = overviewScrollingCameraTranslation(ALGO);
        refreshDragOriginalOverviewBoxes();
    } else {
        dragGrabOffsetLocal = Vector2D{};
        dragGrabRatio       = Vector2D{0.5, 0.5};
    }

    updateWindowDrag();
}

void CScrollOverview::beginWindowResize() {
    checkpointCanvas();
    const auto WINDOW  = getOverviewWindowToShow(resizePendingWindow.lock());
    const auto MONITOR = pMonitor.lock();
    if (!shouldShowOverviewWindow(WINDOW) || shouldShowPinnedFloatingOverviewWindow(WINDOW) || !WINDOW->layoutTarget() || !MONITOR || resizeWorkspaceIdx >= images.size())
        return;

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace == WINDOW->m_workspace) {
            viewportCurrentWorkspace = i;
            resizeWorkspaceIdx       = i;
            break;
        }
    }

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(resizeWorkspaceIdx, activeWorkspaceIndex(), getWorkspaceRenderedPitch(MONITOR, scale->value(), layout));
    resizeOriginalBox   = getOverviewWindowBox(WINDOW, MONITOR, scale->value(), viewOffset->value(), WORKSPACEOFFSET, layout);
    resizeActiveWindow  = WINDOW;
    resizeLastMouseLocal = lastMousePosLocal;

    updateWindowResize();
}

void CScrollOverview::updateWindowDrag() {
    if (!dragActiveWindow)
        return;

    for (const auto& overview : scrollOverviews()) {
        if (!overview)
            continue;

        overview->requestInputFrame();
        overview->damage();
    }
}

void CScrollOverview::updateWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resizeActiveWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (!WINDOW || !TARGET || !MONITOR || resizeWorkspaceIdx >= images.size())
        return;

    TARGET->damageEntire();

    if (WINDOW->m_isFloating) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto GLOBALBOX   = CBox{overviewPointToGlobal(resizeWorkspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
    } else {
        const auto DELTALOCAL = lastMousePosLocal - resizeLastMouseLocal;
        const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);

        if (std::abs(DELTAGLOBAL.x) > 0.01F || std::abs(DELTAGLOBAL.y) > 0.01F) {
            g_layoutManager->resizeTarget(DELTAGLOBAL, TARGET, resizeCorner);
            syncWorkspaceGeometry(TARGET->workspace());
            resizeLastMouseLocal = lastMousePosLocal;
        }
    }

    TARGET->damageEntire();
    damage();
}

void CScrollOverview::clearDragPending() {
    dragPendingPrimary          = false;
    dragActiveWindow.reset();
    dragOriginalWorkspace.reset();
    dragStartedTiled            = false;
    dragOriginalFloatSize       = Vector2D{};
    dragOriginalTapeTranslation = Vector2D{};
    dragGrabOffsetLocal         = Vector2D{};
    dragGrabRatio               = Vector2D{0.5, 0.5};
    dragOriginalBox             = CBox{};
    dragOriginalVisualBox       = CBox{};
    dragOriginalOverviewBox     = CBox{};
    dragOriginalOverviewHitbox  = CBox{};
}

void CScrollOverview::updateScrollingPan() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto WORKSPACE = scrollingPanWorkspace.lock();
    if (layout == ScrollOverview::Config::ELayout::GRID && !WORKSPACE) {
        const auto DELTALOCAL = lastMousePosLocal - scrollingPanLastMouseLocal;
        const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
        const auto DELTAGLOBAL = DELTALOCAL * (ScrollOverview::Config::getPanSensitivity() / SAFEFACTOR);
        viewOffset->setValueAndWarp(viewOffset->value() - DELTAGLOBAL);
        scrollingPanLastMouseLocal = lastMousePosLocal;
        markBlurDirty();
        damage();
        return;
    }

    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller) {
        scrollingPanLastMouseLocal = lastMousePosLocal;
        return;
    }

    const auto DELTALOCAL = lastMousePosLocal - scrollingPanLastMouseLocal;
    const auto SAFEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR->m_scale, 0.01F);
    const auto DELTAGLOBAL = DELTALOCAL * (1.F / SAFEFACTOR);
    const auto DELTA       = ALGO->m_scrollingData->controller->isPrimaryHorizontal() ? DELTAGLOBAL.x : DELTAGLOBAL.y;

    if (std::abs(DELTA) > 0.01F) {
        const double OFFSET = ALGO->m_scrollingData->controller->getOffset() - DELTA;
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset = OFFSET;
        ALGO->m_scrollingData->recalculate(true);
        scrollingPanLastMouseLocal = lastMousePosLocal;
        markBlurDirty();
        damage();
    }
}

void CScrollOverview::beginScrollingPan(PHLWORKSPACE workspace) {
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO && layout != ScrollOverview::Config::ELayout::GRID)
        return;

    scrollingPanPointerDown    = true;
    scrollingPanWorkspace      = workspace;
    scrollingPanLastMouseLocal = lastMousePosLocal;
    scrollingPanInitialWindow  = getOverviewWindowToShow(closeOnWindow.lock());
    if (!scrollingPanInitialWindow || scrollingPanInitialWindow->m_workspace != workspace)
        scrollingPanInitialWindow = getOverviewWindowToShow(Desktop::focusState()->window());
    if (scrollingPanInitialWindow && scrollingPanInitialWindow->m_workspace != workspace)
        scrollingPanInitialWindow.reset();

    if (ALGO)
        ALGO->m_scrollingData->lockedCameraOffset = ALGO->m_scrollingData->controller->getOffset();

    if (Desktop::focusState()->window() && Desktop::focusState()->window()->m_workspace == workspace)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void CScrollOverview::endScrollingPan() {
    const auto WORKSPACE = scrollingPanWorkspace.lock();
    const auto ALGO      = overviewScrollingAlgorithmForWorkspace(WORKSPACE);
    if (ALGO) {
        const double OFFSET = clampOverviewScrollingOffset(ALGO, ALGO->m_scrollingData->controller->getOffset());
        ALGO->m_scrollingData->controller->setOffset(OFFSET);
        ALGO->m_scrollingData->lockedCameraOffset.reset();
        ALGO->m_scrollingData->recalculate();
    }

    if (!WORKSPACE)
        updateViewportWorkspaceFromCanvasCenter();

    scrollingPanPointerDown    = false;
    scrollingPanLastMouseLocal = Vector2D{};
    scrollingPanWorkspace.reset();

    if (WORKSPACE)
        focusMostVisibleScrollingWindow(WORKSPACE);
    scrollingPanInitialWindow.reset();
}

void CScrollOverview::updateViewportWorkspaceFromCanvasCenter() {
    if (!ScrollOverview::Config::getCanvasEnabled() || !ScrollOverview::Config::getCanvasSnapViewportOnPan() || images.empty())
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto SCALE          = scale->value();
    const auto ACTIVEIDX      = activeWorkspaceIndex();
    const auto PITCH          = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto VIEWPORTCENTER = CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle();

    size_t targetIdx    = viewportCurrentWorkspace < images.size() ? viewportCurrentWorkspace : 0;
    double bestDistance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const auto BOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), workspaceOverviewOffset(i, ACTIVEIDX, PITCH), layout);
        const auto DELTA = BOX.middle() - VIEWPORTCENTER;
        const double DISTANCE = DELTA.x * DELTA.x + DELTA.y * DELTA.y;
        if (DISTANCE >= bestDistance)
            continue;

        bestDistance = DISTANCE;
        targetIdx    = i;
    }

    if (targetIdx >= images.size())
        return;

    viewportCurrentWorkspace = targetIdx;
    closeOnWindow.reset();

    const auto WORKSPACE = images[targetIdx]->pWorkspace;
    if (WORKSPACE) {
        if (const auto IT = rememberedSelection.find(WORKSPACE->m_id); IT != rememberedSelection.end()) {
            const auto WINDOW = getOverviewWindowToShow(IT->second.lock());
            if (shouldShowOverviewWindow(WINDOW) && WINDOW->m_workspace == WORKSPACE)
                closeOnWindow = WINDOW;
        }
    }

    if (!closeOnWindow)
        closeOnWindow = windowClosestToWorkspaceCenter(targetIdx);

    damage();
}

bool CScrollOverview::commitCanvasViewport(size_t workspaceIdx) {
    const auto MONITOR = pMonitor.lock();
    if (!ScrollOverview::Config::getCanvasEnabled() || isCanvasDesktop() || !ScrollOverview::Config::getCanvasCommitViewportOnClose() ||
        layout != ScrollOverview::Config::ELayout::GRID ||
        !MONITOR || workspaceIdx >= images.size() || !images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
        return false;

    const auto WORKSPACE       = images[workspaceIdx]->pWorkspace;
    const auto ACTIVEIDX       = activeWorkspaceIndex();
    const auto WORKSPACEORIGIN = workspaceOverviewLogicalVectorOffset(workspaceIdx, ACTIVEIDX, getWorkspaceLogicalPitch(MONITOR, scale->value(), layout));
    const auto REBASE          = viewOffset->value() - WORKSPACEORIGIN;

    std::unordered_set<const void*> movedTargets;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = windowRef;
        if (!WINDOW || WINDOW->m_workspace != WORKSPACE || !WINDOW->m_isFloating || WINDOW->m_pinned)
            continue;

        const auto TARGET = WINDOW->layoutTarget();
        if (!TARGET || !TARGET->floating() || !movedTargets.emplace(TARGET.get()).second)
            continue;

        auto BOX = TARGET->position();
        BOX.translate(-REBASE);
        TARGET->setPositionGlobal(BOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    // Before the active workspace changes, keep its canvas origin in the camera.
    // Once it is active, both offsets become zero with the same pixels on screen.
    viewOffset->setValueAndWarp(WORKSPACEORIGIN);
    *viewOffset = WORKSPACEORIGIN;
    markBlurDirty();
    damage();
    return true;
}

void CScrollOverview::focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : pMonitor.lock();
    if (!workspace || !MONITOR)
        return;

    PHLWINDOW  bestFullWindow;
    PHLWINDOW  bestPartialWindow;
    double     bestFullDistance     = std::numeric_limits<double>::max();
    double     bestPartialVisibleArea = 0.0;
    double     bestPartialDistance  = std::numeric_limits<double>::max();
    const auto MONITORBOX           = MONITOR->logicalBox();
    const auto ALGO                 = overviewScrollingAlgorithmForWorkspace(workspace);
    auto       WORKSPACEBOX         = ALGO ? ALGO->usableArea() : MONITORBOX;
    if (ALGO)
        WORKSPACEBOX.translate(MONITOR->m_position);

    const auto preferredWindow = getOverviewWindowToShow(scrollingPanInitialWindow.lock());
    if (shouldShowOverviewWindow(preferredWindow) && preferredWindow->m_workspace == workspace && !preferredWindow->m_isFloating && preferredWindow->layoutTarget()) {
        const auto WINDOWBOX   = preferredWindow->layoutTarget()->position();
        const auto AREA        = overviewBoxArea(WINDOWBOX);
        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (AREA > 0.0 && std::abs(VISIBLEAREA - AREA) < 0.5) {
            closeOnWindow = preferredWindow;
            rememberSelection(preferredWindow);
            Desktop::focusState()->fullWindowFocus(preferredWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            return;
        }
    }

    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_workspace != workspace || WINDOW->m_isFloating || !WINDOW->layoutTarget())
            continue;

        const auto WINDOWBOX = WINDOW->layoutTarget()->position();
        const auto AREA      = overviewBoxArea(WINDOWBOX);
        if (AREA <= 0.0)
            continue;

        const auto VISIBLEAREA = overviewBoxIntersectionArea(WINDOWBOX, WORKSPACEBOX);
        if (VISIBLEAREA <= 0.0)
            continue;

        const auto DISTANCE = overviewBoxCenterDistanceSquared(WINDOWBOX, WORKSPACEBOX);
        if (std::abs(VISIBLEAREA - AREA) < 0.5) {
            if (DISTANCE < bestFullDistance) {
                bestFullWindow   = WINDOW;
                bestFullDistance = DISTANCE;
            }
            continue;
        }

        if (VISIBLEAREA > bestPartialVisibleArea || (std::abs(VISIBLEAREA - bestPartialVisibleArea) < 0.5 && DISTANCE < bestPartialDistance)) {
            bestPartialWindow     = WINDOW;
            bestPartialVisibleArea = VISIBLEAREA;
            bestPartialDistance   = DISTANCE;
        }
    }

    const auto bestWindow = bestFullWindow ? bestFullWindow : bestPartialWindow;
    if (!bestWindow)
        return;

    closeOnWindow = bestWindow;
    rememberSelection(bestWindow);
    Desktop::focusState()->fullWindowFocus(bestWindow, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

bool CScrollOverview::moveScrollingColumnSelection(bool next) {
    if (images.empty() || viewportCurrentWorkspace >= images.size())
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
    if (!ALGO || !ALGO->m_scrollingData || ALGO->m_scrollingData->columns.empty())
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
        syncSelectionToViewport();

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    const auto CURRENTDATA = CURRENT && CURRENT->layoutTarget() ? ALGO->dataFor(CURRENT->layoutTarget()) : nullptr;
    const auto CURRENTCOL  = CURRENTDATA ? CURRENTDATA->column.lock() : ALGO->currentColumn();
    if (!CURRENTCOL)
        return false;

    const auto firstWindowInColumn = [&](const SP<Layout::Tiled::SColumnData>& column) -> PHLWINDOW {
        if (!column)
            return {};

        for (const auto& targetData : column->targetDatas) {
            if (!targetData || !targetData->target)
                continue;

            for (const auto& windowRef : WORKSPACEIMAGE->windows) {
                const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
                if (shouldShowOverviewWindow(WINDOW) && !WINDOW->m_isFloating && WINDOW->layoutTarget() == targetData->target && WINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace)
                    return WINDOW;
            }
        }

        return {};
    };

    std::vector<SP<Layout::Tiled::SColumnData>> columns;
    for (const auto& column : ALGO->m_scrollingData->columns) {
        if (firstWindowInColumn(column))
            columns.emplace_back(column);
    }

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    std::ranges::sort(columns, [PRIMARYHORIZONTAL](const auto& a, const auto& b) {
        const auto getColumnPosition = [PRIMARYHORIZONTAL](const auto& column) {
            if (!column || column->targetDatas.empty() || !column->targetDatas[0])
                return std::numeric_limits<double>::max();

            return PRIMARYHORIZONTAL ? column->targetDatas[0]->layoutBox.x : column->targetDatas[0]->layoutBox.y;
        };

        return getColumnPosition(a) < getColumnPosition(b);
    });

    const auto CURRENTIT = std::ranges::find(columns, CURRENTCOL);
    if (CURRENTIT == columns.end())
        return false;

    const auto CURRENTIDX = sc<int64_t>(std::distance(columns.begin(), CURRENTIT));
    const auto TARGETIDX  = CURRENTIDX + (next ? 1 : -1);
    if (TARGETIDX < 0 || TARGETIDX >= sc<int64_t>(columns.size()))
        return false;

    const auto TARGETCOL = columns[TARGETIDX];
    const auto WINDOW    = firstWindowInColumn(TARGETCOL);
    if (!WINDOW)
        return false;

    ALGO->m_scrollingData->centerOrFitCol(TARGETCOL);
    ALGO->m_scrollingData->recalculate();

    closeOnWindow = WINDOW;
    rememberSelection(WINDOW);
    syncFocusedSelection();
    damage();

    return true;
}

bool CScrollOverview::moveScrollingStackSelection(bool next) {
    if (images.empty() || viewportCurrentWorkspace >= images.size())
        return false;

    const auto& WORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
    if (!ALGO || !ALGO->m_scrollingData || ALGO->m_scrollingData->columns.empty())
        return false;

    if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
        syncSelectionToViewport();

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    if (!CURRENT || !CURRENT->layoutTarget())
        return false;

    const auto CURRENTDATA = ALGO->dataFor(CURRENT->layoutTarget());
    const auto CURRENTCOL  = CURRENTDATA ? CURRENTDATA->column.lock() : nullptr;
    if (!CURRENTCOL)
        return false;

    const auto CURRENTIDX = CURRENTCOL->idx(CURRENT->layoutTarget());
    const auto TARGETIDX  = CURRENTIDX + (next ? 1 : -1);
    if (TARGETIDX < 0 || TARGETIDX >= sc<int>(CURRENTCOL->targetDatas.size()))
        return false;

    const auto TARGETDATA = CURRENTCOL->targetDatas[TARGETIDX];
    if (!TARGETDATA || !TARGETDATA->target)
        return false;

    for (const auto& windowRef : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (shouldShowOverviewWindow(WINDOW) && !WINDOW->m_isFloating && WINDOW->layoutTarget() == TARGETDATA->target && WINDOW->m_workspace == WORKSPACEIMAGE->pWorkspace) {
            closeOnWindow = WINDOW;
            rememberSelection(WINDOW);
            syncFocusedSelection();
            damage();
            return true;
        }
    }

    return false;
}

void CScrollOverview::endWindowDrag() {
    const auto WINDOW = getOverviewWindowToShow(dragActiveWindow.lock());
    const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto SPACE  = TARGET ? TARGET->space() : nullptr;
    const auto ALGO   = SPACE ? SPACE->algorithm() : nullptr;

    const auto          targetOverview = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    auto*               dropOverview   = targetOverview ? dynamic_cast<CScrollOverview*>(targetOverview.get()) : nullptr;

    const auto          MONITOR           = pMonitor.lock();
    const auto          DROPMONITOR       = dropOverview ? dropOverview->pMonitor.lock() : PHLMONITOR{};
    const auto          ACTIVEWORKSPACEBEFOREDROP     = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    const auto          DROPACTIVEWORKSPACEBEFOREDROP = DROPMONITOR ? DROPMONITOR->m_activeWorkspace : PHLWORKSPACE{};
    const auto          DROPPOINTLOCAL    = getOverviewMousePosLocal(DROPMONITOR);
    size_t              dropWorkspaceIdx  = 0;
    auto                DROPWORKSPACE     = dropOverview ? dropOverview->workspaceAtOverviewDropPoint(DROPPOINTLOCAL, &dropWorkspaceIdx, WINDOW) : PHLWORKSPACE{};
    const bool          CANVASDESKTOPDROP = dropOverview && dropOverview->isCanvasDesktop() && valid(dragOriginalWorkspace.lock());
    const bool          EMPTYCANVASDROP   = CANVASDESKTOPDROP || (!DROPWORKSPACE && dropOverview && ScrollOverview::Config::getCanvasEnabled() && valid(dragOriginalWorkspace.lock()));
    // Empty canvas territory is a world coordinate, not an implicit workspace.
    // Keep the window in its source workspace and let the free-floating branch
    // below commit its snapped global geometry at the pointer position.
    if (EMPTYCANVASDROP) {
        DROPWORKSPACE    = dragOriginalWorkspace.lock();
        dropWorkspaceIdx = dropOverview->dragWorkspaceIndex(WINDOW);
    }
    const bool          RETILEONEND       = dragStartedTiled && TARGET && SPACE && ALGO && !EMPTYCANVASDROP;
    const auto          DROPSCROLLINGALGO  = overviewScrollingAlgorithmForWorkspace(DROPWORKSPACE);
    const bool          DROPSCROLLINGLAYOUT = DROPSCROLLINGALGO != nullptr;
    const bool          DROPSCROLLINGPRIMARYHORIZONTAL =
        DROPSCROLLINGALGO && DROPSCROLLINGALGO->m_scrollingData && DROPSCROLLINGALGO->m_scrollingData->controller &&
        DROPSCROLLINGALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const auto          ORIGINALWORKSPACE = dragOriginalWorkspace.lock();
    const bool          MOVEWORKSPACE    = DROPWORKSPACE && DROPWORKSPACE != ORIGINALWORKSPACE;
    const auto          DRAGBOX          = DROPWORKSPACE ? dropOverview->draggedWindowBoxFor(WINDOW, dropWorkspaceIdx, DROPPOINTLOCAL, dragGrabRatio) : CBox{};
    int                 dropSide         = 0;
    CDropIndicator::SDropAnchor dropAnchor;
    std::string         dropDirection;

    if (!DROPWORKSPACE) {
        clearDragPending();
        damage();
        if (dropOverview && dropOverview != this)
            dropOverview->damage();
        return;
    }

    const auto SOURCEFULLSCREENWINDOW = ORIGINALWORKSPACE ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(ORIGINALWORKSPACE)) : PHLWINDOW{};
    const bool RESTORESOURCEFULLSCREENFOCUS = WINDOW && WINDOW->m_isFloating && MOVEWORKSPACE && shouldShowOverviewWindow(SOURCEFULLSCREENWINDOW) &&
        SOURCEFULLSCREENWINDOW != WINDOW && SOURCEFULLSCREENWINDOW->m_workspace == ORIGINALWORKSPACE && Fullscreen::controller()->isFullscreen(SOURCEFULLSCREENWINDOW);

    const bool DROPSIDEHORIZONTAL = dropOverview->layout != ScrollOverview::Config::ELayout::HORIZONTAL || DROPSCROLLINGPRIMARYHORIZONTAL;

    const auto RESTOREACTIVEWORKSPACE = [&]() {
        if (MONITOR && ACTIVEWORKSPACEBEFOREDROP && MONITOR->m_activeWorkspace != ACTIVEWORKSPACEBEFOREDROP)
            MONITOR->changeWorkspace(ACTIVEWORKSPACEBEFOREDROP, false, true, true);
        if (DROPMONITOR && DROPMONITOR != MONITOR && DROPACTIVEWORKSPACEBEFOREDROP && DROPMONITOR->m_activeWorkspace != DROPACTIVEWORKSPACEBEFOREDROP)
            DROPMONITOR->changeWorkspace(DROPACTIVEWORKSPACEBEFOREDROP, false, true, true);
    };
    const auto RESTOREVIEWPORTWORKSPACE = [&]() {
        if (!ACTIVEWORKSPACEBEFOREDROP)
            return;

        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == ACTIVEWORKSPACEBEFOREDROP) {
                viewportCurrentWorkspace = i;
                return;
            }
        }
    };

    bool       dropWorkspaceFullyVisible = false;
    if (DROPMONITOR) {
        const auto WORKSPACEBOX =
            getOverviewWorkspaceBox(DROPMONITOR, dropOverview->scale->value(), dropOverview->viewOffset->value(),
                                    dropOverview->workspaceOverviewOffset(dropWorkspaceIdx, dropOverview->activeWorkspaceIndex(),
                                                                         getWorkspaceRenderedPitch(DROPMONITOR, dropOverview->scale->value(), dropOverview->layout)),
                                    dropOverview->layout);
        dropWorkspaceFullyVisible = overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, DROPMONITOR);

        if (DROPSIDEHORIZONTAL) {
            if (DROPPOINTLOCAL.x < WORKSPACEBOX.x)
                dropSide = -1;
            else if (DROPPOINTLOCAL.x > WORKSPACEBOX.x + WORKSPACEBOX.width)
                dropSide = 1;
        } else {
            if (DROPPOINTLOCAL.y < WORKSPACEBOX.y)
                dropSide = -1;
            else if (DROPPOINTLOCAL.y > WORKSPACEBOX.y + WORKSPACEBOX.height)
                dropSide = 1;
        }
    }

    if (DROPWORKSPACE) {
        dropOverview->lastMousePosLocal = DROPPOINTLOCAL;
        dropAnchor                      = dropOverview->dropAnchorAtOverviewCursorOnWorkspace(dropWorkspaceIdx, WINDOW, this);
    }

    const auto DROPANCHOR = dropAnchor.window;
    dropDirection         = dropAnchor.direction;
    const bool DROPPINGONSCROLLINGCROSSAXIS =
        DROPSCROLLINGPRIMARYHORIZONTAL ? dropDirection == "u" || dropDirection == "d" : dropDirection == "l" || dropDirection == "r";

    if (DROPSCROLLINGLAYOUT && DROPANCHOR && DROPPINGONSCROLLINGCROSSAXIS && Fullscreen::controller()->isFullscreen(DROPANCHOR)) {
        Fullscreen::controller()->setFullscreenMode(DROPANCHOR, Fullscreen::FSMODE_NONE);
        if (const auto ANCHORDATA = DROPSCROLLINGALGO->dataFor(DROPANCHOR->layoutTarget()); ANCHORDATA) {
            if (const auto ANCHORCOL = ANCHORDATA->column.lock())
                ANCHORCOL->setColumnWidth(1.F);
        }
    }

    if (RETILEONEND && MOVEWORKSPACE) {
        Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
        RESTOREACTIVEWORKSPACE();

        if (DROPSCROLLINGLAYOUT) {
            if (!dropWorkspaceFullyVisible)
                moveOverviewScrollingTargetToWorkspaceEdge(TARGET, 1);
            else if (DROPANCHOR && !dropDirection.empty())
                moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, dropDirection);
            else if (!DROPANCHOR)
                moveOverviewScrollingTargetToWorkspaceEdge(TARGET, dropSide);
        }

        TARGET->rememberFloatingSize(dragOriginalFloatSize);
        RESTOREACTIVEWORKSPACE();
        RESTOREVIEWPORTWORKSPACE();
    } else if (RETILEONEND) {
        TARGET->damageEntire();

        if (DROPANCHOR && !dropDirection.empty() && !DROPSCROLLINGLAYOUT && DROPANCHOR->layoutTarget()) {
            DROPANCHOR->layoutTarget()->damageEntire();
            g_layoutManager->switchTargets(TARGET, DROPANCHOR->layoutTarget(), true);
            DROPANCHOR->layoutTarget()->damageEntire();
        } else if (DROPANCHOR && !dropDirection.empty())
            moveOverviewTargetNextToWindow(TARGET, DROPANCHOR, dropDirection);
        else if (DROPSCROLLINGLAYOUT && !DROPANCHOR)
            moveOverviewScrollingTargetToWorkspaceEdge(TARGET, dropSide);

        TARGET->rememberFloatingSize(dragOriginalFloatSize);
        TARGET->warpPositionSize();
        TARGET->damageEntire();

        Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        if (const auto WORKSPACE = SPACE->workspace())
        WORKSPACE->updateWindows();
    } else if (WINDOW && MOVEWORKSPACE) {
        Desktop::globalWindowController()->moveWindowToWorkspace(WINDOW, DROPWORKSPACE);
        RESTOREACTIVEWORKSPACE();
        if (TARGET) {
            const auto GLOBALSIZE = DRAGBOX.size() * (1.F / (std::max(dropOverview->scale->value(), 0.01F) * std::max(DROPMONITOR ? DROPMONITOR->m_scale : 1.F, 0.01F)));
            const bool FREECANVAS = ScrollOverview::Config::getCanvasEnabled() && ScrollOverview::Config::getCanvasAllowWindowOverflow();
            auto       GLOBALBOX  = dropWorkspaceFullyVisible || FREECANVAS ? CBox{dropOverview->overviewPointToGlobal(dropWorkspaceIdx, DRAGBOX.pos()), GLOBALSIZE} :
                                                                              centerBoxInWorkspace(CBox{Vector2D{}, GLOBALSIZE}, DROPWORKSPACE, DROPMONITOR);
            if (FREECANVAS)
                GLOBALBOX = dropOverview->snapCanvasWindowBox(GLOBALBOX, DROPMONITOR);
            if (dropWorkspaceFullyVisible && !FREECANVAS)
                GLOBALBOX = clampBoxToWorkspace(GLOBALBOX, DROPWORKSPACE, DROPMONITOR, WINDOW->getRealBorderSize());

            TARGET->setPositionGlobal(GLOBALBOX);
            TARGET->warpPositionSize();
        }

        RESTOREACTIVEWORKSPACE();
        RESTOREVIEWPORTWORKSPACE();
    } else if (TARGET && (!dragStartedTiled || EMPTYCANVASDROP)) {
        const auto workspaceIdx = dragWorkspaceIndex(WINDOW);

        const auto FLOATBOX   = draggedWindowBox(workspaceIdx);
        const auto GLOBALPOS  = overviewPointToGlobal(workspaceIdx, FLOATBOX.pos());
        const auto GLOBALSIZE = FLOATBOX.size() * (1.F / (std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F)));
        auto       GLOBALBOX  = CBox{GLOBALPOS, GLOBALSIZE};
        const auto WORKSPACE  = workspaceIdx < images.size() && images[workspaceIdx] ? images[workspaceIdx]->pWorkspace : WINDOW->m_workspace;

        if (!ScrollOverview::Config::getCanvasEnabled() || !ScrollOverview::Config::getCanvasAllowWindowOverflow())
            GLOBALBOX = clampBoxToWorkspace(GLOBALBOX, WORKSPACE, MONITOR, WINDOW->getRealBorderSize());
        else
            GLOBALBOX = snapCanvasWindowBox(GLOBALBOX, MONITOR);

        TARGET->damageEntire();
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    if (RESTORESOURCEFULLSCREENFOCUS) {
        const bool POSTHIDDENEVENT = MONITOR && ORIGINALWORKSPACE == MONITOR->m_activeWorkspace;

        if (POSTHIDDENEVENT) {
            closeOnWindow = SOURCEFULLSCREENWINDOW;
            rememberSelection(SOURCEFULLSCREENWINDOW);
            focusOverviewFullscreenWindowIfActiveWorkspace(SOURCEFULLSCREENWINDOW, ORIGINALWORKSPACE, MONITOR);
            emitFullscreenVisibilityState(SOURCEFULLSCREENWINDOW, true);
        }
    }

    if (DROPWORKSPACE && DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
        const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(DROPWORKSPACE));
        if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == DROPWORKSPACE)
            dropOverview->emitFullscreenVisibilityState(FULLSCREENWINDOW, true);
    }

    if (WINDOW && DROPMONITOR && DROPWORKSPACE == DROPMONITOR->m_activeWorkspace) {
        dropOverview->selectOverviewWindow(WINDOW, dropWorkspaceIdx, true);
        g_pseudoFocusedWindow = WINDOW;
        g_pseudoFocusUntil    = Time::steadyNow() + POST_DROP_PSEUDO_FOCUS_DURATION;
    }

    clearDragPending();
    rebuildPending = true;
    noteCanvasLayoutChanged();
    damage();
    if (dropOverview != this) {
        dropOverview->rebuildPending = true;
        dropOverview->damage();
    }
}

void CScrollOverview::endWindowResize() {
    const auto WINDOW  = getOverviewWindowToShow(resizeActiveWindow.lock());
    const auto TARGET  = WINDOW ? WINDOW->layoutTarget() : nullptr;
    const auto MONITOR = pMonitor.lock();

    if (WINDOW && TARGET && WINDOW->m_isFloating && resizeWorkspaceIdx < images.size()) {
        const auto RESIZEDBOX  = resizedWindowBox();
        const auto SCALEFACTOR = std::max(scale->value(), 0.01F) * std::max(MONITOR ? MONITOR->m_scale : 1.F, 0.01F);
        auto       GLOBALBOX   = CBox{overviewPointToGlobal(resizeWorkspaceIdx, RESIZEDBOX.pos()), RESIZEDBOX.size() * (1.F / SCALEFACTOR)};

        TARGET->damageEntire();
        TARGET->rememberFloatingSize(GLOBALBOX.size());
        TARGET->setPositionGlobal(GLOBALBOX);
        TARGET->warpPositionSize();
        TARGET->damageEntire();
    }

    resizePointerDown = false;
    resizePendingWindow.reset();
    resizeActiveWindow.reset();
    resizeOriginalBox  = CBox{};
    resizeLastMouseLocal = Vector2D{};
    resizeCorner       = Layout::CORNER_NONE;
    rebuildPending     = true;
    noteCanvasLayoutChanged();
    damage();
}

void CScrollOverview::moveViewportWorkspace(bool up) {
    if (images.empty())
        return;

    if (viewportCurrentWorkspace == 0 && !up)
        return;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return;

    moveViewportWorkspaceTo(up ? viewportCurrentWorkspace + 1 : viewportCurrentWorkspace - 1);
}

void CScrollOverview::moveViewportWorkspaceTo(size_t targetIndex) {
    if (targetIndex >= images.size() || targetIndex == viewportCurrentWorkspace)
        return;

    viewportCurrentWorkspace = targetIndex;

    const auto& TARGETWORKSPACEIMAGE = images[viewportCurrentWorkspace];
    if (!TARGETWORKSPACEIMAGE || !TARGETWORKSPACEIMAGE->pWorkspace)
        return;

    closeOnWindow.reset();

    if (const auto it = rememberedSelection.find(TARGETWORKSPACEIMAGE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
        if (rememberedWindow && rememberedWindow->m_workspace == TARGETWORKSPACEIMAGE->pWorkspace && shouldShowOverviewWindow(rememberedWindow))
            closeOnWindow = rememberedWindow;
    }

    if (!closeOnWindow)
        closeOnWindow = windowClosestToWorkspaceCenter(viewportCurrentWorkspace);

    if (pMonitor && pMonitor->m_activeWorkspace != TARGETWORKSPACEIMAGE->pWorkspace)
        pMonitor->changeWorkspace(TARGETWORKSPACEIMAGE->pWorkspace, false, true, true);

    damage();
}

bool CScrollOverview::scrollStepAllowed(uint32_t timeMs) {
    const uint32_t DELAY = sc<uint32_t>(ScrollOverview::Config::getScrollEventDelay());

    // throttle discrete scroll steps so a single notch / a burst of high-res events only steps once
    if (lastScrollStepTimeMs != 0 && timeMs >= lastScrollStepTimeMs && timeMs - lastScrollStepTimeMs < DELAY)
        return false;

    lastScrollStepTimeMs = timeMs;
    return true;
}

void CScrollOverview::trackpadSwipeLayout(const PHLWORKSPACE target, const double delta) {
    const float SCALE = std::max<float>(scale->value(), 0.01F);
    const auto ALGO = overviewScrollingAlgorithmForWorkspace(target);

    if (!ALGO) {
        return;
    }

    // fingers lifted — snap to the nearest column
    if (delta == 0.0) {
        if (trackpadTapeFollowing) {
            trackpadTapeFollowing = false;
            ALGO->snapToGrid();
            focusMostVisibleScrollingWindow(target);
            damage();
        }
        return;
    }

    trackpadTapeFollowing = true;
    ALGO->moveTape(sc<float>(-1 * delta * ScrollOverview::Config::getTouchpadScrollFactor() / SCALE));
    damage();
}

void CScrollOverview::trackpadSwipeWorkspace(const double delta) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    // fingers lifted — snap to the nearest workspace
    if (delta == 0.0) {
        finishWorkspaceScrollFollow();
        return;
    }

    const float SCALE = std::max<float>(scale->value(), 0.01F);

    trackpadWorkspaceFollowing  = true;
    trackpadScrollAccum         += delta * ScrollOverview::Config::getTouchpadScrollFactor();

    viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(trackpadWorkspaceScrollOffset(MONITOR, SCALE)), layout));
    damage();
}

double CScrollOverview::trackpadWorkspaceScrollOffset(PHLMONITOR monitor, float renderScale) {
    const float RENDEREDLOGICALUNIT = renderScale * std::max<float>(monitor->m_scale, 0.01F);
    const float LOGICALPITCH        = getWorkspaceLogicalPitch(monitor, renderScale, layout);

    double offset    = trackpadScrollAccum / RENDEREDLOGICALUNIT;
    double minOffset = 0.0;
    double maxOffset = 0.0;

    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const double WORKSPACEOFFSET = workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, LOGICALPITCH);
        minOffset                    = std::min(minOffset, WORKSPACEOFFSET);
        maxOffset                    = std::max(maxOffset, WORKSPACEOFFSET);
    }

    offset              = std::clamp(offset, minOffset, maxOffset);
    trackpadScrollAccum = offset * RENDEREDLOGICALUNIT;

    return offset;
}

void CScrollOverview::finishWorkspaceScrollFollow() {
    if (!trackpadWorkspaceFollowing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR) {
        trackpadWorkspaceFollowing = false;
        trackpadScrollAccum        = 0.0;
        return;
    }

    trackpadWorkspaceFollowing  = false;
    trackpadScrollAccum         = 0.0;

    const float  SCALE          = std::max<float>(scale->value(), 0.01F);
    const float  LOGICALPITCH   = getWorkspaceLogicalPitch(MONITOR, SCALE, layout);
    const float  RENDEREDPITCH  = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const size_t ACTIVEIDX      = activeWorkspaceIndex();
    const double VIEWPORTCENTER = axisSize(MONITOR->m_size * MONITOR->m_scale, layout) / 2.0;

    size_t targetIdx    = viewportCurrentWorkspace;
    double bestDistance = std::numeric_limits<double>::max();

    for (size_t i = 0; i < images.size(); ++i) {
        if (!images[i] || !images[i]->pWorkspace)
            continue;

        const auto   WORKSPACEBOX = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), workspaceOverviewOffset(i, ACTIVEIDX, RENDEREDPITCH), layout);
        const double DISTANCE     = std::abs(axisValue(WORKSPACEBOX.middle(), layout) - VIEWPORTCENTER);
        if (DISTANCE < bestDistance) {
            bestDistance = DISTANCE;
            targetIdx    = i;
        }
    }

    if (targetIdx == viewportCurrentWorkspace) {
        *viewOffset = Vector2D{};
        return;
    }

    const double OFFSET       = axisValue(viewOffset->value(), layout);
    const double TARGETOFFSET = workspaceOverviewLogicalOffset(targetIdx, viewportCurrentWorkspace, LOGICALPITCH);

    trackpadGestureSettleOffset  = OFFSET - TARGETOFFSET;
    trackpadGestureSettlePending = true;

    const size_t BEFORE = viewportCurrentWorkspace;
    const bool   NEXT   = targetIdx > viewportCurrentWorkspace;
    const size_t STEPS  = sc<size_t>(std::abs(sc<long>(targetIdx) - sc<long>(viewportCurrentWorkspace)));
    for (size_t i = 0; i < STEPS; ++i)
        moveViewportWorkspace(NEXT);

    if (viewportCurrentWorkspace == BEFORE) {
        trackpadGestureSettlePending = false;
        *viewOffset                  = Vector2D{};
    }
}

void CScrollOverview::syncSelectionToViewport() {
    // On the canvas desktop at 100% the selection follows focus; rebuilding
    // (a window changed workspace, a monitor changed) never moves focus.
    if (isCanvasDesktop() && !canvasNavigationActive) {
        const auto FOCUSED = getOverviewWindowToShow(Desktop::focusState()->window());
        if (shouldShowOverviewWindow(FOCUSED)) {
            closeOnWindow = FOCUSED;
            rememberSelection(FOCUSED);
        }
        return;
    }

    if (images.empty() || viewportCurrentWorkspace >= images.size()) {
        closeOnWindow.reset();
        return;
    }

    const auto& WSPACE = images[viewportCurrentWorkspace];

    if (closeOnWindow && closeOnWindow->m_workspace == WSPACE->pWorkspace) {
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (const auto& windowRef : WSPACE->windows) {
            if (getOverviewWindowToShow(windowRef.lock()) == selectedWindow) {
                closeOnWindow = selectedWindow;
                rememberSelection(selectedWindow);
                syncFocusedSelection();
                return;
            }
        }
    }

    if (const auto it = rememberedSelection.find(WSPACE->pWorkspace->m_id); it != rememberedSelection.end()) {
        const auto rememberedWindow = getOverviewWindowToShow(it->second.lock());
        if (rememberedWindow && rememberedWindow->m_workspace == WSPACE->pWorkspace && shouldShowOverviewWindow(rememberedWindow)) {
            for (const auto& windowRef : WSPACE->windows) {
                if (getOverviewWindowToShow(windowRef.lock()) == rememberedWindow) {
                    closeOnWindow = rememberedWindow;
                    syncFocusedSelection();
                    return;
                }
            }
        }
    }

    const auto focusedWindow = Desktop::focusState()->window();
    if (shouldShowOverviewWindow(focusedWindow) && focusedWindow->m_workspace == WSPACE->pWorkspace) {
        closeOnWindow = focusedWindow;
        rememberSelection(focusedWindow);
        syncFocusedSelection();
        return;
    }

    if (const auto window = windowClosestToWorkspaceCenter(viewportCurrentWorkspace)) {
        closeOnWindow = window;
        rememberSelection(window);
        syncFocusedSelection();
        return;
    }

    closeOnWindow.reset();
    if (activeScrollOverview().get() == this)
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
}

void CScrollOverview::canvasAdoptFocus(const PHLWINDOW& window) {
    if (!shouldShowOverviewWindow(window))
        return;
    closeOnWindow = window;
    rememberSelection(window);
    syncFocusedSelection();
}

void CScrollOverview::syncFocusedSelection() {
    const auto window = getOverviewWindowToShow(closeOnWindow.lock());
    if (!shouldShowOverviewWindow(window))
        return;

    closeOnWindow = window;

    if (activeScrollOverview().get() != this)
        return;

    if (Desktop::focusState()->window() == window && window->m_workspace == pMonitor->m_activeWorkspace) {
        ensureCanvasKeyboardFocus(window);
        return;
    }

    const auto PREVIOUSWORKSPACE = pMonitor ? pMonitor->m_activeWorkspace : PHLWORKSPACE{};

    ++g_canvasInternalFocusDepth;
    auto restoreInternalFocusDepth = Hyprutils::Utils::CScopeGuard([] { --g_canvasInternalFocusDepth; });
    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
    ensureCanvasKeyboardFocus(window);

    if (window->m_workspace != PREVIOUSWORKSPACE && focusSyncedFromWorkspaceID == WORKSPACE_INVALID)
        focusSyncedFromWorkspaceID = PREVIOUSWORKSPACE ? PREVIOUSWORKSPACE->m_id : WORKSPACE_INVALID;
}

size_t CScrollOverview::dragWorkspaceIndex(PHLWINDOW window) const {
    if (viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] && images[viewportCurrentWorkspace]->pWorkspace)
        return viewportCurrentWorkspace;

    if (!window)
        return images.size();

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace)
            return i;
    }

    return images.size();
}

bool CScrollOverview::moveSelection(const std::string& direction) {
    const bool MOVINGLEFT  = direction == "left";
    const bool MOVINGRIGHT = direction == "right";
    const bool MOVINGUP    = direction == "up";
    const bool MOVINGDOWN  = direction == "down";

    if (!MOVINGLEFT && !MOVINGRIGHT && !MOVINGUP && !MOVINGDOWN)
        return false;

    if (isCanvasDesktop()) {
        markCanvasCameraActive(pMonitor.lock());
        const bool METAHELD = g_pInputManager && (g_pInputManager->getModsFromAllKBs() & HL_MODIFIER_META);
        if (!METAHELD)
            g_canvasKeyboardNavigationMonitor.reset();
        else if (const auto LOCKEDMONITOR = g_canvasKeyboardNavigationMonitor.lock()) {
            if (LOCKEDMONITOR != pMonitor) {
                const auto LOCKEDOVERVIEW = scrollOverviewForMonitor(LOCKEDMONITOR);
                auto*      LOCKEDCANVAS   = LOCKEDOVERVIEW ? dynamic_cast<CScrollOverview*>(LOCKEDOVERVIEW.get()) : nullptr;
                return LOCKEDCANVAS && LOCKEDCANVAS != this ? LOCKEDCANVAS->moveSelection(direction) : false;
            }
        } else
            g_canvasKeyboardNavigationMonitor = pMonitor;

        const auto MONITOR = pMonitor.lock();
        if (!MONITOR)
            return false;

        auto CURRENT = getOverviewWindowToShow(Desktop::focusState()->window());
        if (!shouldShowOverviewWindow(CURRENT))
            CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
        if (!shouldShowOverviewWindow(CURRENT))
            CURRENT.reset();

        // Where a window is, seen along the arrow: its near and far edge
        // along it (flipped so the arrow points to larger values) and its span
        // across it. Always from where the windows are now.
        struct SAlong {
            double nearEdge = 0, farEdge = 0, acrossMin = 0, acrossMax = 0;
        };
        const bool HORIZONTAL = MOVINGLEFT || MOVINGRIGHT;
        const bool FORWARD    = MOVINGRIGHT || MOVINGDOWN;
        const auto along      = [&](const CBox& box) {
            const double MIN = HORIZONTAL ? box.x : box.y, MAX = MIN + (HORIZONTAL ? box.width : box.height);
            const double ACROSS = HORIZONTAL ? box.y : box.x, ACROSSMAX = ACROSS + (HORIZONTAL ? box.height : box.width);
            return FORWARD ? SAlong{MIN, MAX, ACROSS, ACROSSMAX} : SAlong{-MAX, -MIN, ACROSS, ACROSSMAX};
        };

        struct SCandidate {
            PHLWINDOW window;
            double    gap = 0, reach = 0, weighted = 0;
            bool      inLine = false, beyond = false;
        };
        std::unordered_set<const void*> visited;
        std::optional<SCandidate>       best;
        PHLWINDOW                       bestCandidate;
        const auto CURRENTBOX = CURRENT ? CURRENT->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT) : CBox{};
        const auto FROM       = along(CURRENTBOX);

        // Which of two windows the arrow means (the rules Android uses for
        // arrow keys): one in line with the current window (their spans
        // across the arrow overlap) wins over one that is not, unless, going
        // up or down, that one is wholly nearer; otherwise the nearer, with
        // an offset sideways counting less than distance along the arrow.
        const auto better = [&](const SCandidate& a, const SCandidate& b) {
            if (a.inLine != b.inLine) {
                const auto& IN = a.inLine ? a : b;
                const auto& OUT = a.inLine ? b : a;
                const bool  INWINS = !OUT.beyond || HORIZONTAL || IN.gap < OUT.reach;
                return a.inLine ? INWINS : !INWINS;
            }
            return a.weighted < b.weighted;
        };

        for (const auto& windowRef : Desktop::windowState()->windows()) {
            const auto WINDOW = getOverviewWindowToShow(windowRef);
            // A window's Hyprland monitor is only its surface owner in desktop
            // canvas mode. It must not partition the shared spatial world or
            // an adjacent window can become unreachable from this camera.
            if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || WINDOW == CURRENT || !visited.emplace(WINDOW.get()).second)
                continue;
            // While searching, arrows move between the windows that match.
            if (canvasNavigationActive && !SpatialOverview::Navigator::isMatch(WINDOW))
                continue;

            if (!CURRENT) {
                bestCandidate = WINDOW;
                break;
            }

            // It has to reach further that way than the current window, with
            // both edges: a wide window's neighbour below is not to its left
            // just because its middle is. And some of it has to be within 45°
            // of that side, or it is beside the window, not beyond it.
            const auto TO = along(WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
            if (TO.nearEdge <= FROM.nearEdge || TO.farEdge <= FROM.farEdge)
                continue;
            const double REACH  = TO.farEdge - FROM.farEdge;
            const double ASIDE  = std::max({0.0, TO.acrossMin - FROM.acrossMax, FROM.acrossMin - TO.acrossMax});
            if (ASIDE >= REACH)
                continue;

            const double GAP    = std::max(0.0, TO.nearEdge - FROM.farEdge);
            const double OFFSET = (TO.acrossMin + TO.acrossMax - FROM.acrossMin - FROM.acrossMax) / 2.0;
            SCandidate   candidate{
                  .window   = WINDOW,
                  .gap      = GAP,
                  .reach    = REACH,
                  .weighted = 13.0 * GAP * GAP + OFFSET * OFFSET,
                  .inLine   = TO.acrossMax > FROM.acrossMin && TO.acrossMin < FROM.acrossMax,
                  .beyond   = TO.nearEdge >= FROM.farEdge,
            };
            if (!best || better(candidate, *best))
                best = candidate;
        }

        if (best)
            bestCandidate = best->window;
        if (!bestCandidate)
            return false;

        size_t workspaceIdx = viewportCurrentWorkspace;
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i] && images[i]->pWorkspace == bestCandidate->m_workspace) {
                workspaceIdx = i;
                break;
            }
        }

        selectOverviewWindow(bestCandidate, workspaceIdx, true);

        // Focusing a surface may temporarily switch Hyprland to the monitor
        // that owns it. Keep the original output as the keyboard camera even
        // though the selected surface can live anywhere in the shared world.
        if (Desktop::focusState()->monitor() != MONITOR)
            Desktop::focusState()->rawMonitorFocus(MONITOR);

        *viewOffset = canvasCameraOffsetFor(bestCandidate, scale->goal(), canvasNavigationActive);
        if (canvasNavigationActive)
            SpatialOverview::Navigator::selectWindow(bestCandidate);
        markBlurDirty();

        damage();
        return true;
    }

    bool shouldMoveWorkspace = images.empty() || viewportCurrentWorkspace >= images.size();

    const auto WORKSPACEIMAGE = shouldMoveWorkspace ? SP<SWorkspaceImage>{} : images[viewportCurrentWorkspace];
    if (!WORKSPACEIMAGE || !WORKSPACEIMAGE->pWorkspace)
        shouldMoveWorkspace = true;

    if (!shouldMoveWorkspace && (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)) {
        syncSelectionToViewport();
        if (!closeOnWindow || closeOnWindow->m_workspace != WORKSPACEIMAGE->pWorkspace || !shouldShowOverviewWindow(closeOnWindow.lock()) || closeOnWindow->m_isFloating)
            shouldMoveWorkspace = true;
    }

    const auto CURRENT = getOverviewWindowToShow(closeOnWindow.lock());
    if (!CURRENT)
        shouldMoveWorkspace = true;

    if (!shouldMoveWorkspace)
        closeOnWindow = CURRENT;

    if (!shouldMoveWorkspace) {
        const auto ALGO = overviewScrollingAlgorithmForWorkspace(WORKSPACEIMAGE->pWorkspace);
        if (ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller) {
            const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller->isPrimaryHorizontal();
            const bool MOVINGPRIMARY     = PRIMARYHORIZONTAL ? MOVINGLEFT || MOVINGRIGHT : MOVINGUP || MOVINGDOWN;
            const bool MOVINGSTACK       = PRIMARYHORIZONTAL ? MOVINGUP || MOVINGDOWN : MOVINGLEFT || MOVINGRIGHT;
            const bool NEXT              = MOVINGRIGHT || MOVINGDOWN;

            if (MOVINGPRIMARY || MOVINGSTACK) {
                if (MOVINGPRIMARY ? moveScrollingColumnSelection(NEXT) : moveScrollingStackSelection(NEXT))
                    return true;

                shouldMoveWorkspace = true;
            }
        }
    }

    const auto CURRENTCENTER = shouldMoveWorkspace ? Vector2D{} : CURRENT->middle();

    PHLWINDOW bestCandidate;
    float     bestPrimaryDistance   = std::numeric_limits<float>::max();
    float     bestSecondaryDistance = std::numeric_limits<float>::max();
    float     bestOverlap           = -1.F;
    bool      bestHasOverlap         = false;

    for (const auto& windowRef : shouldMoveWorkspace ? std::vector<PHLWINDOWREF>{} : WORKSPACEIMAGE->windows) {
        const auto WINDOW = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW == CURRENT || WINDOW->m_isFloating)
            continue;

        if (WINDOW->m_workspace != WORKSPACEIMAGE->pWorkspace || WINDOW->m_monitor != pMonitor)
            continue;

        const auto WINDOWCENTER = WINDOW->middle();

        const float PRIMARYDISTANCE =
            MOVINGRIGHT ? WINDOWCENTER.x - CURRENTCENTER.x : MOVINGLEFT ? CURRENTCENTER.x - WINDOWCENTER.x : MOVINGDOWN ? WINDOWCENTER.y - CURRENTCENTER.y : CURRENTCENTER.y - WINDOWCENTER.y;

        if (PRIMARYDISTANCE <= 0.F)
            continue;

        const float OVERLAP = MOVINGLEFT || MOVINGRIGHT ? getWindowVerticalOverlap(CURRENT, WINDOW) : getWindowHorizontalOverlap(CURRENT, WINDOW);
        const bool  HASOVERLAP       = OVERLAP > 0.F;
        const float SECONDARYDISTANCE =
            MOVINGLEFT || MOVINGRIGHT ? std::abs(WINDOWCENTER.y - CURRENTCENTER.y) : std::abs(WINDOWCENTER.x - CURRENTCENTER.x);

        if ((MOVINGUP || MOVINGDOWN) && !HASOVERLAP)
            continue;

        if (!bestCandidate) {
            bestCandidate         = WINDOW;
            bestPrimaryDistance   = PRIMARYDISTANCE;
            bestSecondaryDistance = SECONDARYDISTANCE;
            bestOverlap           = OVERLAP;
            bestHasOverlap        = HASOVERLAP;
            continue;
        }

        if (HASOVERLAP != bestHasOverlap) {
            if (HASOVERLAP) {
                bestCandidate         = WINDOW;
                bestPrimaryDistance   = PRIMARYDISTANCE;
                bestSecondaryDistance = SECONDARYDISTANCE;
                bestOverlap           = OVERLAP;
                bestHasOverlap        = true;
            }

            continue;
        }

        if (PRIMARYDISTANCE < bestPrimaryDistance - 0.5F) {
            bestCandidate         = WINDOW;
            bestPrimaryDistance   = PRIMARYDISTANCE;
            bestSecondaryDistance = SECONDARYDISTANCE;
            bestOverlap           = OVERLAP;
            continue;
        }

        if (std::abs(PRIMARYDISTANCE - bestPrimaryDistance) <= 0.5F) {
            if ((HASOVERLAP && OVERLAP > bestOverlap + 0.5F) || (!HASOVERLAP && SECONDARYDISTANCE < bestSecondaryDistance - 0.5F)) {
                bestCandidate         = WINDOW;
                bestPrimaryDistance   = PRIMARYDISTANCE;
                bestSecondaryDistance = SECONDARYDISTANCE;
                bestOverlap           = OVERLAP;
            }
        }
    }

    const auto WINDOWSELECTIONMOVED = bestCandidate;
    if (!WINDOWSELECTIONMOVED)
        shouldMoveWorkspace = true;

    if (shouldMoveWorkspace) {
        if (layout == ScrollOverview::Config::ELayout::GRID) {
            if (images.empty() || viewportCurrentWorkspace >= images.size())
                return false;

            const size_t COLUMNS = sc<size_t>(std::max(1, ScrollOverview::Config::getGridColumns()));
            const size_t CURRENT = viewportCurrentWorkspace;
            const size_t COLUMN  = CURRENT % COLUMNS;
            std::optional<size_t> TARGET;

            if (MOVINGLEFT && COLUMN > 0)
                TARGET = CURRENT - 1;
            else if (MOVINGRIGHT && COLUMN + 1 < COLUMNS && CURRENT + 1 < images.size())
                TARGET = CURRENT + 1;
            else if (MOVINGUP && CURRENT >= COLUMNS)
                TARGET = CURRENT - COLUMNS;
            else if (MOVINGDOWN && CURRENT + COLUMNS < images.size())
                TARGET = CURRENT + COLUMNS;

            if (!TARGET || !images[*TARGET] || !images[*TARGET]->pWorkspace)
                return false;

            moveViewportWorkspaceTo(*TARGET);
            return true;
        }

        if (((MOVINGLEFT || MOVINGRIGHT) && layout != ScrollOverview::Config::ELayout::HORIZONTAL) || ((MOVINGUP || MOVINGDOWN) && layout == ScrollOverview::Config::ELayout::HORIZONTAL))
            return false;

        moveViewportWorkspace(MOVINGRIGHT || MOVINGDOWN);
        return true;
    }

    closeOnWindow = bestCandidate;
    rememberSelection(bestCandidate);
    syncFocusedSelection();
    damage();

    return true;
}

void CScrollOverview::forceSurfaceVisibility(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return;

    for (auto& entry : forcedSurfaceVisibility) {
        if (entry.surface.lock() == surface) {
            HLSURFACE->m_visibleRegion = {};
            return;
        }
    }

    forcedSurfaceVisibility.push_back({surface, HLSURFACE->m_visibleRegion});
    HLSURFACE->m_visibleRegion = {};
}

void CScrollOverview::forceWindowSurfaceVisibility(PHLWINDOW window) {
    if (!window || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    window->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);

    if (window->m_isX11 || !window->m_popupHead)
        return;

    window->m_popupHead->breadthfirst([this](WP<Desktop::View::CPopup> popup, void*) {
        if (!popup || !popup->aliveAndVisible() || !popup->wlSurface() || !popup->wlSurface()->resource())
            return;

        popup->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);
    }, nullptr);
}

void CScrollOverview::forceWindowVisible(PHLWINDOW window) {
    if (!window)
        return;

    constexpr auto FULLSCREENALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;

    for (auto& entry : forcedWindowVisibility) {
        if (entry.window == window) {
            window->m_hidden = false;
            window->alpha(FULLSCREENALPHA)->setValueAndWarp(1.F);
            return;
        }
    }

    auto& entry                  = forcedWindowVisibility.emplace_back();
    entry.window                 = window;
    entry.hidden                 = window->m_hidden;

    window->m_hidden = false;
    window->alpha(FULLSCREENALPHA)->setValueAndWarp(1.F);
}

void CScrollOverview::forceLayersAboveFullscreen() {
    if (!pMonitor)
        return;

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& ls : pMonitor->m_layerSurfaceLayers[LAYER]) {
            if (!ls)
                continue;

            bool known = false;
            for (auto& entry : forcedLayerVisibility) {
                if (entry.layer == ls) {
                    known = true;
                    break;
                }
            }

			auto& lsAlpha = ls->alpha()[Desktop::View::LS_ALPHA_FADE];
            if (!known)
                forcedLayerVisibility.push_back({ls, ls->m_aboveFullscreen, lsAlpha->value()});

            if (!ls->m_aboveFullscreen)
                ls->m_aboveFullscreen = true;

            if (lsAlpha->value() != 1.F || lsAlpha->goal() != 1.F || lsAlpha->isBeingAnimated())
                lsAlpha->setValueAndWarp(1.F);
        }
    }
}

void CScrollOverview::restoreForcedSurfaceVisibility() {
    for (auto& entry : forcedSurfaceVisibility) {
        const auto SURFACE = entry.surface.lock();
        if (!SURFACE)
            continue;

        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        if (!HLSURFACE)
            continue;

        HLSURFACE->m_visibleRegion = entry.visibleRegion;
    }

    forcedSurfaceVisibility.clear();
}

void CScrollOverview::restoreForcedWindowVisibility() {
    std::vector<SP<Desktop::View::CGroup>> groupsToRefresh;

    for (auto& entry : forcedWindowVisibility) {
        const auto WINDOW = entry.window.lock();
        if (!WINDOW)
            continue;

        constexpr auto FULLSCREENALPHA = Desktop::View::WINDOW_ALPHA_FULLSCREEN;
        WINDOW->updateFullscreenInputState();
        *WINDOW->alpha(FULLSCREENALPHA) = WINDOW->isBlockedByFullscreen() ? 0.F : 1.F;

        if (WINDOW->m_group) {
            if (std::ranges::find(groupsToRefresh, WINDOW->m_group) == groupsToRefresh.end())
                groupsToRefresh.emplace_back(WINDOW->m_group);
            continue;
        }

        WINDOW->m_hidden = entry.hidden;
    }

    for (const auto& group : groupsToRefresh) {
        if (group)
            group->updateWindowVisibility();
    }

    forcedWindowVisibility.clear();
}

void CScrollOverview::restoreForcedLayerVisibility() {
    for (auto& entry : forcedLayerVisibility) {
        if (!entry.layer)
            continue;

		auto& entryLsAlpha = entry.layer->alpha()[Desktop::View::LS_ALPHA_FADE];
        const auto MONITOR = entry.layer->m_monitor.lock();
        if (!MONITOR) {
            entry.layer->m_aboveFullscreen = entry.aboveFullscreen;
            entryLsAlpha->setValueAndWarp(entry.alpha);
            continue;
        }

        // What Hyprland sets when a window goes fullscreen (the canvas held
        // it off while it drew): layers stay above windows only while the
        // screen has no fullscreen window. The value saved when the canvas
        // opened may predate a fullscreen window, and would keep the bar on
        // top of it.
        entry.layer->m_aboveFullscreen = !Fullscreen::controller()->hasFullscreen(MONITOR->m_activeWorkspace);

        const bool fullscreen = Fullscreen::controller()->hasFullscreen(MONITOR);
        const bool visible    = !fullscreen || entry.layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || entry.layer->m_aboveFullscreen;
        entryLsAlpha->setValueAndWarp(visible ? 1.F : 0.F);
    }

    forcedLayerVisibility.clear();
}

void CScrollOverview::applyWorkspaceAnimationOverrides() {
    if (!sharedStateOwner || workspaceAnimationsOverridden)
        return;

    savedWorkspaceAnimationConfigs.clear();

    for (const std::string name : {"workspaces", "workspacesIn", "workspacesOut"}) {
        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig(name);
        if (!CONFIG)
            continue;

        auto& saved   = savedWorkspaceAnimationConfigs.emplace_back();
        saved.name    = name;
        saved.config  = *CONFIG;
    }

    for (const auto& saved : savedWorkspaceAnimationConfigs)
        Config::animationTree()->setConfigForNode(saved.name, false, 1.F, "default", "");

    workspaceAnimationsOverridden = true;
}

void CScrollOverview::restoreWorkspaceAnimationOverrides() {
    if (!workspaceAnimationsOverridden)
        return;

    const auto propagateAnimationValues = [](const SP<Hyprutils::Animation::SAnimationPropertyConfig>& parent, auto&& self) -> void {
        if (!parent)
            return;

        for (const auto& [name, animation] : Config::animationTree()->getAnimationConfig()) {
            if (!animation || animation->overridden || animation->pParentAnimation != parent)
                continue;

            animation->pValues = parent->pValues;
            self(animation, self);
        }
    };

    for (const auto& saved : savedWorkspaceAnimationConfigs) {
        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig(saved.name);
        if (!CONFIG)
            continue;

        *CONFIG = saved.config;
        propagateAnimationValues(CONFIG, propagateAnimationValues);
    }

    savedWorkspaceAnimationConfigs.clear();
    workspaceAnimationsOverridden = false;
}

void CScrollOverview::forceWorkspaceAlphaVisible() {
    for (const auto& workspace : State::workspaceState()->workspaces()) {
        if (!workspace || !workspace->m_alpha)
            continue;

        workspace->m_alpha->setValueAndWarp(1.F);
        *workspace->m_alpha = 1.F;
    }
}

void CScrollOverview::forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return;

    const auto workspaceImage = std::ranges::find_if(images, [&workspace](const auto& image) { return image && image->pWorkspace == workspace; });
    if (workspaceImage == images.end())
        return;

    for (const auto& windowRef : (*workspaceImage)->windows)
        OverviewWindow::forceDecoRecalc(windowRef.lock());
}

void CScrollOverview::applyInputConfigOverrides() {
    if (!sharedStateOwner || inputConfigOverridden)
        return;

    previousNoWarps                    = ScrollOverview::Config::getValue<int>("cursor:no_warps");
    previousWarpOnChangeWorkspace      = ScrollOverview::Config::getValue<int>("cursor:warp_on_change_workspace");
    previousWarpOnToggleSpecial        = ScrollOverview::Config::getValue<int>("cursor:warp_on_toggle_special");
    previousWarpBackAfterNonMouseInput = ScrollOverview::Config::getValue<int>("cursor:warp_back_after_non_mouse_input");
    previousFollowMouse                = ScrollOverview::Config::getValue<int>("input:follow_mouse");
    g_userFollowMouse                  = previousFollowMouse;
    inputConfigOverridden = true;

    ScrollOverview::Config::setValue("cursor:no_warps", 1);
    ScrollOverview::Config::setValue("cursor:warp_on_change_workspace", 0);
    ScrollOverview::Config::setValue("cursor:warp_on_toggle_special", 0);
    ScrollOverview::Config::setValue("cursor:warp_back_after_non_mouse_input", 0);
    ScrollOverview::Config::setValue("input:follow_mouse", 0);
}

void CScrollOverview::restoreInputConfigOverrides() {
    if (!inputConfigOverridden)
        return;

    ScrollOverview::Config::setValue("cursor:no_warps", previousNoWarps);
    ScrollOverview::Config::setValue("cursor:warp_on_change_workspace", previousWarpOnChangeWorkspace);
    ScrollOverview::Config::setValue("cursor:warp_on_toggle_special", previousWarpOnToggleSpecial);
    ScrollOverview::Config::setValue("cursor:warp_back_after_non_mouse_input", previousWarpBackAfterNonMouseInput);
    ScrollOverview::Config::setValue("input:follow_mouse", previousFollowMouse);

    inputConfigOverridden = false;
}

void CScrollOverview::transferSharedStateOwnership() {
    if (!sharedStateOwner)
        return;

    CScrollOverview* successor = nullptr;
    for (const auto& overview : scrollOverviews()) {
        auto* candidate = overview ? dynamic_cast<CScrollOverview*>(overview.get()) : nullptr;
        if (candidate && candidate != this && !candidate->closing) {
            successor = candidate;
            break;
        }
    }

    if (!successor)
        return;

    successor->sharedStateOwner = true;

    successor->savedWorkspaceAnimationConfigs = std::move(savedWorkspaceAnimationConfigs);
    successor->workspaceAnimationsOverridden  = workspaceAnimationsOverridden;
    savedWorkspaceAnimationConfigs.clear();
    workspaceAnimationsOverridden = false;

    successor->previousNoWarps                    = previousNoWarps;
    successor->previousWarpOnChangeWorkspace      = previousWarpOnChangeWorkspace;
    successor->previousWarpOnToggleSpecial        = previousWarpOnToggleSpecial;
    successor->previousWarpBackAfterNonMouseInput = previousWarpBackAfterNonMouseInput;
    successor->previousFollowMouse                = previousFollowMouse;
    successor->inputConfigOverridden              = inputConfigOverridden;
    inputConfigOverridden = false;

    successor->usesSubmapKeybinds   = usesSubmapKeybinds;
    successor->submapActive         = submapActive;
    successor->previousSubmapName   = std::move(previousSubmapName);
    if (successor->usesSubmapKeybinds)
        successor->keyboardKeyHook.reset();
    usesSubmapKeybinds = false;
    submapActive       = false;
    sharedStateOwner   = false;
}

void CScrollOverview::emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen) {
    if (emittingFullscreenVisibilityState)
        return;

    emittingFullscreenVisibilityState = true;
    auto resetEmittingFullscreenVisibilityState = Hyprutils::Utils::CScopeGuard([this] { emittingFullscreenVisibilityState = false; });

    window = getOverviewWindowToShow(window);

    if (!validMapped(window) || !window->m_workspace || window->m_monitor != pMonitor) {
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});
        return;
    }

    if (!hideFullscreen || !Fullscreen::controller()->isFullscreen(window)) {
        Event::bus()->m_events.window.fullscreen.emit(window);

        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = Fullscreen::controller()->isFullscreen(window) ? "1" : "0"});

        return;
    }

    const auto SAVEDMODES = Fullscreen::controller()->getFullscreenModes(window);

    Fullscreen::controller()->setFullscreenMode(window, Fullscreen::FSMODE_NONE, Fullscreen::FSMODE_NONE);

    Event::bus()->m_events.window.fullscreen.emit(window);

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{.event = "fullscreen", .data = "0"});

    Fullscreen::controller()->setFullscreenMode(window, SAVEDMODES.internal, SAVEDMODES.client);
}

static PHLWINDOW getOverviewFullscreenVisibilityWindow(const PHLWORKSPACE& workspace, const PHLWINDOW& fallback) {
    const auto FULLSCREENWINDOW = workspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace)) : PHLWINDOW{};

    if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == workspace)
        return FULLSCREENWINDOW;

    return getOverviewWindowToShow(fallback);
}

void CScrollOverview::renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode,
                                                const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto WORKSPACEALPHA   = workspaceOverviewAlpha(workspaceIdx);

    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    renderOverviewWorkspaceShadow(monitor, WORKSPACEBOX, renderScale, wallpaperMode == 0, WORKSPACEALPHA);

    if (ScrollOverview::Config::getBlur() && wallpaperMode != 1 && WORKSPACEALPHA > 0.001F)
        OverviewRender::queueBlur(WORKSPACEBOX, 0, 2.F, WORKSPACEALPHA, false);

    if (wallpaperMode != 0 && WORKSPACEALPHA > 0.001F)
        renderWallpaperLayers(monitor, WORKSPACEBOX, renderScale, now, WORKSPACEALPHA);

    if (WORKSPACEALPHA >= 0.999F)
        renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, WORKSPACEBOX, renderScale, now);
}

void CScrollOverview::renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);

    if (!overviewBoxIntersectsMonitor(VISIBLEBOX, monitor))
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    const auto renderOverviewWindow = [&](const PHLWINDOW& window) {
        if (!shouldShowOverviewWindow(window))
            return;
        if (dragActiveWindow && window == getOverviewWindowToShow(dragActiveWindow.lock()))
            return;

        const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            return;

        renderWindowLive(monitor, window, windowBox, renderScale, now, &WORKSPACEBOX);
    };

    const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
    const bool scrollingLayout   = isWorkspaceScrolling(workspace);
    const bool hasFullscreenPath = shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace;
    const auto renderDropIndicator = [&] {
        if (hasRunningWorkspaceAnimation())
            return;

        auto*      dragContext = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
        const auto DRAGGED     = getOverviewWindowToShow(dragContext->dragActiveWindow.lock());
        if (!DRAGGED)
            return;

        if (!isOverviewPointerOnMonitor(monitor))
            return;

        size_t     dropWorkspaceIdx = 0;
        const auto DROPWORKSPACE    = workspaceAtOverviewDropPoint(lastMousePosLocal, &dropWorkspaceIdx, DRAGGED);
        if (DROPWORKSPACE != workspace || dropWorkspaceIdx != workspaceIdx)
            return;

        const auto ANCHOR      = dropAnchorAtOverviewCursorOnWorkspace(workspaceIdx, DRAGGED, dragContext);
        const auto WORKSPACEBOX = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);

        CDropIndicator::renderDropIndicator({
            .monitor               = monitor,
            .workspace             = workspace,
            .workspaceUsableBox    = getOverviewWorkspaceUsableBox(workspace, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout),
            .anchor                = ANCHOR,
            .renderScale           = renderScale,
            .workspaceFullyVisible = overviewBoxFullyVisibleOnMonitor(WORKSPACEBOX, monitor),
            .floating              = DRAGGED->m_isFloating,
            .layout                = layout,
        });
    };

    if (!scrollingLayout && hasFullscreenPath) {
        renderOverviewWindow(fullscreenWindow);
        OverviewRender::flushPass(monitor);
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || !window->m_isFloating || window == fullscreenWindow)
                continue;

            renderOverviewWindow(window);
        }
        renderDropIndicator();
        return;
    }

    const auto renderWindowsByState = [&](bool floating) {
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!window || window->m_isFloating != floating)
                continue;

            renderOverviewWindow(window);
        }
    };

    renderWindowsByState(false);
    renderWindowsByState(true);
    renderDropIndicator();
}

void CScrollOverview::renderWorkspaceOutline(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale) {
    if (!ScrollOverview::Config::getWorkspaceOutlineEnabled() || !monitor || workspaceIdx >= images.size() || !images[workspaceIdx] ||
        !images[workspaceIdx]->pWorkspace)
        return;

    const auto& WORKSPACEIMAGE  = images[workspaceIdx];
    const auto  WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto  WORKSPACEBOX    = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout).round();
    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
        return;

    auto*      dragOwner = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
    const auto DRAGGED   = getOverviewWindowToShow(dragOwner->dragActiveWindow.lock());
    bool       dropTarget = false;
    if (DRAGGED && isOverviewPointerOnMonitor(monitor)) {
        size_t     dropWorkspaceIdx = 0;
        const auto DROPWORKSPACE    = workspaceAtOverviewDropPoint(lastMousePosLocal, &dropWorkspaceIdx, DRAGGED);
        dropTarget = DROPWORKSPACE == WORKSPACEIMAGE->pWorkspace && dropWorkspaceIdx == workspaceIdx;
    }

    const bool ACTIVE = viewportCurrentWorkspace == workspaceIdx;
    const auto COLORNAME = ACTIVE || dropTarget ? "general:col.active_border" : "general:col.inactive_border";
    auto&      COLORREF  = ScrollOverview::Config::valueRef<Config::IComplexConfigValue>(COLORNAME);

    Config::CGradientValueData GRADIENT{Colors::WHITE};
    if (COLORREF.good()) {
        if (const auto COLOR = dc<Config::CGradientValueData*>(COLORREF.ptr()); COLOR && !COLOR->m_colors.empty())
            GRADIENT = *COLOR;
    }

    const float TRANSITIONALPHA = overviewProgress() * workspaceOverviewAlpha(workspaceIdx);
    const float OPACITY = dropTarget ? ScrollOverview::Config::getWorkspaceOutlineDropOpacity() :
        ACTIVE ? ScrollOverview::Config::getWorkspaceOutlineActiveOpacity() : ScrollOverview::Config::getWorkspaceOutlineOpacity();
    const int BORDERWIDTH = sc<int>(std::round((dropTarget ? ScrollOverview::Config::getWorkspaceOutlineDropWidth() :
                                               ScrollOverview::Config::getWorkspaceOutlineWidth()) * monitor->m_scale));
    const int ROUNDING = sc<int>(std::round(ScrollOverview::Config::getWorkspaceOutlineRounding() * monitor->m_scale));

    if (dropTarget && !GRADIENT.m_colors.empty()) {
        auto fillColor = GRADIENT.m_colors.front();
        fillColor.a *= ScrollOverview::Config::getWorkspaceOutlineDropFillOpacity() * TRANSITIONALPHA;

        CRectPassElement::SRectData fill;
        fill.box           = WORKSPACEBOX;
        fill.color         = fillColor;
        fill.round         = ROUNDING;
        fill.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(fill));
    }

    if (BORDERWIDTH <= 0 || OPACITY <= 0.F || TRANSITIONALPHA <= 0.F)
        return;

    CBorderPassElement::SBorderData border;
    border.box           = WORKSPACEBOX;
    border.grad1         = GRADIENT;
    border.a             = OPACITY * TRANSITIONALPHA;
    border.borderSize    = BORDERWIDTH;
    border.round         = ROUNDING;
    border.outerRound    = ROUNDING;
    border.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
}

void CScrollOverview::renderCanvasBackgroundDim(PHLMONITOR monitor) {
    if (!isCanvasDesktop() || !monitor)
        return;

    float ALPHA = ScrollOverview::Config::getCanvasBackgroundDim() * overviewProgress();
    if (SpatialOverview::Experiments::on(ECanvasExperiment::Quiet))
        ALPHA *= 0.35F;
    if (ALPHA <= 0.001F)
        return;

    CRectPassElement::SRectData overlay;
    overlay.box   = CBox{{}, monitor->m_size * monitor->m_scale}.round();
    overlay.color = CHyprColor{0.F, 0.F, 0.F, ALPHA};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(overlay));
}

void CScrollOverview::renderCanvasGrid(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) {
    if (!ScrollOverview::Config::getCanvasEnabled() || !ScrollOverview::Config::getCanvasGridEnabled() || !monitor)
        return;

    const float TRANSITION = overviewProgress();
    float OPACITY = ScrollOverview::Config::getCanvasGridOpacity() * TRANSITION;
    if (SpatialOverview::Experiments::on(ECanvasExperiment::Quiet))
        OPACITY *= 0.4F;
    const float SPACING    = ScrollOverview::Config::getCanvasGridSize() * std::max(renderScale, 0.01F) * std::max(monitor->m_scale, 0.01F);
    const float LINEWIDTH  = std::max(1.F, sc<float>(ScrollOverview::Config::getCanvasGridWidth()) * monitor->m_scale);
    if (OPACITY <= 0.001F || SPACING <= 0.01F)
        return;

    const auto FULLSIZE = monitor->m_size * monitor->m_scale;
    const auto ORIGIN   = isCanvasDesktop() ?
        getOverviewGlobalBox(CBox{{}, {1.F, 1.F}}, monitor, renderScale, viewOffset->value(), Vector2D{}, layout, false).pos() :
        getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), workspaceOverviewOffset(activeIdx, activeIdx, workspacePitch), layout).pos();

    // Level of detail: once marks would crowd closer than this, show every
    // other one and cross-fade the in-between level, as map apps do. Every
    // level stays anchored to the world origin, so marks never jump.
    const bool  DOTS    = ScrollOverview::Config::getCanvasGridStyle() == 1;
    const float MINCELL = (DOTS ? 14.F : 12.F) * std::max(monitor->m_scale, 1.F);
    float       cell    = SPACING;
    while (cell < MINCELL)
        cell *= 2.F;
    const float T    = std::clamp((cell - MINCELL) / (MINCELL * 0.5F), 0.F, 1.F);
    const float FINE = T * T * (3.F - 2.F * T);

    // Sub-two-logical-pixel dots are unstable when the camera and barrel
    // shader both resample them; keep a 3-device-pixel floor.
    const float MARK = DOTS ? std::max(3.F, std::round(std::max(0.01F, ScrollOverview::Config::getCanvasGridDotSize() * monitor->m_scale))) : LINEWIDTH;
    const auto  TILE = SpatialOverview::Hud::gridTile(cell, MARK, FINE, DOTS ? 1 : 0);
    if (!TILE)
        return;

    // The whole grid is one quad sampling a repeating 2x2-cell tile, instead
    // of one draw per mark (tens of thousands on a zoomed-out 4K output).
    // Whatever was queued before it (the dim) has to land first.
    OverviewRender::flushPass(monitor);

    const double SPAN = cell * 2.0;
    auto&        RENDERDATA = g_pHyprRenderer->m_renderData;
    const auto   PREVIOUSTL = RENDERDATA.primarySurfaceUVTopLeft;
    const auto   PREVIOUSBR = RENDERDATA.primarySurfaceUVBottomRight;
    auto         restoreUV  = Hyprutils::Utils::CScopeGuard([&RENDERDATA, PREVIOUSTL, PREVIOUSBR] {
        RENDERDATA.primarySurfaceUVTopLeft     = PREVIOUSTL;
        RENDERDATA.primarySurfaceUVBottomRight = PREVIOUSBR;
    });
    // The tile's first mark sits at half a cell; map it onto the world origin.
    RENDERDATA.primarySurfaceUVTopLeft     = Vector2D{(cell * 0.5 - ORIGIN.x) / SPAN, (cell * 0.5 - ORIGIN.y) / SPAN};
    RENDERDATA.primarySurfaceUVBottomRight = Vector2D{(FULLSIZE.x + cell * 0.5 - ORIGIN.x) / SPAN, (FULLSIZE.y + cell * 0.5 - ORIGIN.y) / SPAN};

    const CRegion DAMAGE{CBox{{}, monitor->m_transformedSize}};
    g_pHyprRenderer->draw(
        CTexPassElement::SRenderData{
            .tex           = TILE,
            .box           = CBox{{}, FULLSIZE},
            .a             = OPACITY,
            .damage        = DAMAGE,
            .allowCustomUV = true,
            .wrapX         = WRAP_REPEAT,
            .wrapY         = WRAP_REPEAT,
        },
        DAMAGE);
}

CBox CScrollOverview::canvasArrangeButtonBox() const {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !ScrollOverview::Config::getCanvasMinimapEnabled() || !MONITOR || overviewProgress() <= 0.001F)
        return {};

    const float SCALE  = std::max(MONITOR->m_scale, 0.01F);
    const float WIDTH  = ScrollOverview::Config::getCanvasMinimapWidth() * SCALE;
    const float HEIGHT = ScrollOverview::Config::getCanvasMinimapHeight() * SCALE;
    const float MARGIN = ScrollOverview::Config::getCanvasMinimapMargin() * SCALE;
    const float SIZE   = 46.F * SCALE;
    const float GAP    = 10.F * SCALE;
    const auto  FULL   = MONITOR->m_size * SCALE;
    const CBox  PANEL{FULL.x - MARGIN - WIDTH, FULL.y - MARGIN - HEIGHT, WIDTH, HEIGHT};
    return CBox{PANEL.x - GAP - SIZE, PANEL.y + PANEL.height - SIZE, SIZE, SIZE}.round();
}

void CScrollOverview::renderCanvasMinimap(PHLMONITOR monitor) {
    const float PROGRESS   = overviewProgress();
    const float TRANSITION = std::max(PROGRESS, minimapFlashAlpha(Time::steadyNow()));
    if (!isCanvasDesktop() || !ScrollOverview::Config::getCanvasMinimapEnabled() || !monitor || TRANSITION <= 0.001F) {
        // The lens shader keeps the last minimap it was given: hide it once.
        if (minimapShown) {
            minimapShown = false;
            SpatialOverview::BarrelShader::updateMinimap(SpatialOverview::BarrelShader::SMinimapData{});
        }
        return;
    }
    minimapShown = true;

    const float ZOOM = std::max(scale->value(), 0.01F);
    const auto  CENTERLOGICAL = monitor->m_size / 2.F;
    const CBox  VIEWPORTWORLD{
        monitor->m_position + viewOffset->value() + CENTERLOGICAL - CENTERLOGICAL * (1.F / ZOOM),
        monitor->m_size * (1.F / ZOOM),
    };

    std::optional<CBox> worldBounds;
    const auto includeWorldBox = [&worldBounds](const CBox& box) {
        if (box.empty())
            return;
        if (!worldBounds) {
            worldBounds = box;
            return;
        }

        const float LEFT   = std::min(sc<float>(worldBounds->x), sc<float>(box.x));
        const float TOP    = std::min(sc<float>(worldBounds->y), sc<float>(box.y));
        const float RIGHT  = std::max(sc<float>(worldBounds->x + worldBounds->width), sc<float>(box.x + box.width));
        const float BOTTOM = std::max(sc<float>(worldBounds->y + worldBounds->height), sc<float>(box.y + box.height));
        *worldBounds       = CBox{LEFT, TOP, RIGHT - LEFT, BOTTOM - TOP};
    };

    includeWorldBox(VIEWPORTWORLD);
    std::unordered_set<const void*> visited;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;
        includeWorldBox(WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
    }

    if (!worldBounds || worldBounds->empty())
        return;

    const float WORLDPADDING = std::max(80.F, sc<float>(std::max(worldBounds->width, worldBounds->height)) * 0.06F);
    worldBounds->expand(WORLDPADDING);

    const float MONITORSCALE = std::max(monitor->m_scale, 0.01F);
    const float WIDTH        = ScrollOverview::Config::getCanvasMinimapWidth() * MONITORSCALE;
    const float HEIGHT       = ScrollOverview::Config::getCanvasMinimapHeight() * MONITORSCALE;
    const float MARGIN       = ScrollOverview::Config::getCanvasMinimapMargin() * MONITORSCALE;
    const float INSET        = 12.F * MONITORSCALE;
    const auto  FULLSIZE     = monitor->m_size * MONITORSCALE;
    const CBox  PANEL{FULLSIZE.x - MARGIN - WIDTH, FULLSIZE.y - MARGIN - HEIGHT, WIDTH, HEIGHT};
    const CBox  CONTENT{PANEL.x + INSET, PANEL.y + INSET, std::max(1.F, sc<float>(PANEL.width - INSET * 2.F)),
                       std::max(1.F, sc<float>(PANEL.height - INSET * 2.F))};

    const float FIT = std::min(sc<float>(CONTENT.width / std::max(worldBounds->width, 1.0)), sc<float>(CONTENT.height / std::max(worldBounds->height, 1.0)));
    const Vector2D DRAWORIGIN{
        CONTENT.x + (CONTENT.width - worldBounds->width * FIT) / 2.F,
        CONTENT.y + (CONTENT.height - worldBounds->height * FIT) / 2.F,
    };
    const auto mapWorldBox = [&](const CBox& box) {
        auto mapped = CBox{DRAWORIGIN + (box.pos() - worldBounds->pos()) * FIT, box.size() * FIT};
        mapped.width  = std::max(2.F * MONITORSCALE, sc<float>(mapped.width));
        mapped.height = std::max(2.F * MONITORSCALE, sc<float>(mapped.height));
        return mapped.round();
    };

    auto gradientFor = [](const std::string& name, const CHyprColor& fallback) {
        auto& REF = ScrollOverview::Config::valueRef<Config::IComplexConfigValue>(name);
        if (REF.good()) {
            if (const auto VALUE = dc<Config::CGradientValueData*>(REF.ptr()); VALUE && !VALUE->m_colors.empty())
                return *VALUE;
        }
        return Config::CGradientValueData{fallback};
    };

    // With the navigator the minimap wears the HUD's colors; otherwise the
    // theme's borders.
    const bool NAVIGATOR        = ScrollOverview::Config::getNavigatorEnabled();
    const auto ACTIVEGRADIENT   = NAVIGATOR ? Config::CGradientValueData{SpatialOverview::Hud::theme().accent} : gradientFor("general:col.active_border", Colors::WHITE);
    const auto INACTIVEGRADIENT = NAVIGATOR ? Config::CGradientValueData{CHyprColor{0.5F, 0.5F, 0.51F, 1.F}} :
                                              gradientFor("general:col.inactive_border", CHyprColor{0.45F, 0.48F, 0.55F, 1.F});
    CHyprColor activeColor      = ACTIVEGRADIENT.m_colors.empty() ? Colors::WHITE : ACTIVEGRADIENT.m_colors.front();
    CHyprColor inactiveColor    = INACTIVEGRADIENT.m_colors.empty() ? CHyprColor{0.45F, 0.48F, 0.55F, 1.F} : INACTIVEGRADIENT.m_colors.front();

    const auto FOCUSED = getOverviewWindowToShow(Desktop::focusState()->window());
    SpatialOverview::BarrelShader::SMinimapData shaderMinimap;
    const auto toArray = [](const CBox& box) {
        return std::array<float, 4>{sc<float>(box.x), sc<float>(box.y), sc<float>(box.width), sc<float>(box.height)};
    };
    shaderMinimap.panel         = toArray(PANEL.copy().round());
    // The tidy button belongs to the zoomed-out view, not to a passing glance.
    shaderMinimap.arrangeButton = PROGRESS > 0.001F ? toArray(canvasArrangeButtonBox()) : std::array<float, 4>{0.F, 0.F, 0.F, 0.F};
    shaderMinimap.viewport      = toArray(mapWorldBox(VIEWPORTWORLD));
    shaderMinimap.activeColor   = {activeColor.r, activeColor.g, activeColor.b};
    shaderMinimap.inactiveColor = {inactiveColor.r, inactiveColor.g, inactiveColor.b};
    shaderMinimap.opacity       = ScrollOverview::Config::getCanvasMinimapOpacity();
    shaderMinimap.transition    = TRANSITION;

    visited.clear();
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;

        if (WINDOW == FOCUSED)
            shaderMinimap.focusedIndex = sc<int>(shaderMinimap.windows.size());
        shaderMinimap.windows.emplace_back(toArray(mapWorldBox(WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT))));
        shaderMinimap.windowAlpha.emplace_back(SpatialOverview::Navigator::isMatch(WINDOW) ? 1.F : 0.22F);
    }

    // With the barrel shader installed, compose the minimap after the world
    // lens inside the final pass. The render-pass version remains as a flat
    // fallback when distortion is disabled or no screen shader is available.
    if (SpatialOverview::BarrelShader::updateMinimap(shaderMinimap))
        return;

    CRectPassElement::SRectData background;
    background.box           = PANEL.copy().round();
    background.color         = CHyprColor{0.025F, 0.03F, 0.045F, ScrollOverview::Config::getCanvasMinimapOpacity() * TRANSITION};
    background.round         = sc<int>(std::round(14.F * MONITORSCALE));
    background.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(background));

    visited.clear();
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;

        auto color = WINDOW == FOCUSED ? activeColor : inactiveColor;
        color.a    = (WINDOW == FOCUSED ? 0.92F : 0.58F) * TRANSITION;
        CRectPassElement::SRectData marker;
        marker.box           = mapWorldBox(WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
        marker.color         = color;
        marker.round         = sc<int>(std::round(3.F * MONITORSCALE));
        marker.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(marker));
    }

    auto viewportColor = activeColor;
    viewportColor.a    = 0.12F * TRANSITION;
    CRectPassElement::SRectData viewportFill;
    viewportFill.box           = mapWorldBox(VIEWPORTWORLD);
    viewportFill.color         = viewportColor;
    viewportFill.round         = sc<int>(std::round(5.F * MONITORSCALE));
    viewportFill.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(viewportFill));

    CBorderPassElement::SBorderData viewportBorder;
    viewportBorder.box           = viewportFill.box;
    viewportBorder.grad1         = ACTIVEGRADIENT;
    viewportBorder.a             = 0.95F * TRANSITION;
    viewportBorder.borderSize    = sc<int>(std::round(2.F * MONITORSCALE));
    viewportBorder.round         = viewportFill.round;
    viewportBorder.outerRound    = viewportFill.round;
    viewportBorder.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(viewportBorder));

    CBorderPassElement::SBorderData panelBorder;
    panelBorder.box           = PANEL.copy().round();
    panelBorder.grad1         = INACTIVEGRADIENT;
    panelBorder.a             = 0.5F * TRANSITION;
    panelBorder.borderSize    = std::max(1, sc<int>(std::round(MONITORSCALE)));
    panelBorder.round         = background.round;
    panelBorder.outerRound    = background.round;
    panelBorder.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(panelBorder));

    const auto ARRANGEBUTTON = canvasArrangeButtonBox();
    if (!ARRANGEBUTTON.empty()) {
        CRectPassElement::SRectData buttonBackground;
        buttonBackground.box           = ARRANGEBUTTON;
        buttonBackground.color         = CHyprColor{0.025F, 0.03F, 0.045F, ScrollOverview::Config::getCanvasMinimapOpacity() * TRANSITION};
        buttonBackground.round         = sc<int>(std::round(10.F * MONITORSCALE));
        buttonBackground.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(buttonBackground));

        CBorderPassElement::SBorderData buttonBorder;
        buttonBorder.box           = ARRANGEBUTTON;
        buttonBorder.grad1         = INACTIVEGRADIENT;
        buttonBorder.a             = 0.65F * TRANSITION;
        buttonBorder.borderSize    = std::max(1, sc<int>(std::round(MONITORSCALE)));
        buttonBorder.round         = buttonBackground.round;
        buttonBorder.outerRound    = buttonBackground.round;
        buttonBorder.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(buttonBorder));

        const float INSET = ARRANGEBUTTON.width * 0.23F;
        const float CELLW = (ARRANGEBUTTON.width - INSET * 2.F) * 0.42F;
        const float CELLH = (ARRANGEBUTTON.height - INSET * 2.F) * 0.25F;
        const float GAPX  = ARRANGEBUTTON.width - INSET * 2.F - CELLW * 2.F;
        const float GAPY  = (ARRANGEBUTTON.height - INSET * 2.F - CELLH * 3.F) * 0.5F;
        auto iconColor    = activeColor;
        iconColor.a       = 0.88F * TRANSITION;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 2; ++column) {
                CRectPassElement::SRectData cell;
                cell.box = CBox{
                    ARRANGEBUTTON.x + INSET + column * (CELLW + GAPX),
                    ARRANGEBUTTON.y + INSET + row * (CELLH + GAPY),
                    CELLW,
                    CELLH,
                }.round();
                cell.color         = iconColor;
                cell.round         = std::max(1, sc<int>(std::round(1.5F * MONITORSCALE)));
                cell.roundingPower = 2.F;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(cell));
            }
        }
    }
}

void CScrollOverview::renderCanvasViewport(PHLMONITOR monitor, float renderScale) {
    if (!ScrollOverview::Config::getCanvasEnabled() || !ScrollOverview::Config::getCanvasViewportEnabled() || !monitor)
        return;

    const float TRANSITION = overviewProgress();
    if (TRANSITION <= 0.001F)
        return;

    const auto FULLSIZE = monitor->m_size * monitor->m_scale;
    const CBox FULLBOX  = CBox{{}, FULLSIZE};
    const auto FRAMESIZE = FULLSIZE * std::clamp(renderScale, 0.01F, 1.F);
    const CBox FRAMEBOX = CBox{FULLBOX.middle() - FRAMESIZE / 2.F, FRAMESIZE}.round();

    auto scrimColor = CHyprColor{0.F, 0.F, 0.F, ScrollOverview::Config::getCanvasViewportOutsideOpacity() * TRANSITION};
    const auto drawScrim = [&](const CBox& box) {
        if (box.width <= 0.F || box.height <= 0.F || scrimColor.a <= 0.001F)
            return;
        auto roundedBox = box;
        roundedBox.round();
        CRectPassElement::SRectData scrim;
        scrim.box   = roundedBox;
        scrim.color = scrimColor;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(scrim));
    };

    drawScrim(CBox{0.F, 0.F, FULLSIZE.x, FRAMEBOX.y});
    drawScrim(CBox{0.F, FRAMEBOX.y + FRAMEBOX.height, FULLSIZE.x, FULLSIZE.y - (FRAMEBOX.y + FRAMEBOX.height)});
    drawScrim(CBox{0.F, FRAMEBOX.y, FRAMEBOX.x, FRAMEBOX.height});
    drawScrim(CBox{FRAMEBOX.x + FRAMEBOX.width, FRAMEBOX.y, FULLSIZE.x - (FRAMEBOX.x + FRAMEBOX.width), FRAMEBOX.height});

    auto& COLORREF = ScrollOverview::Config::valueRef<Config::IComplexConfigValue>("general:col.active_border");
    Config::CGradientValueData GRADIENT{Colors::WHITE};
    if (COLORREF.good()) {
        if (const auto COLOR = dc<Config::CGradientValueData*>(COLORREF.ptr()); COLOR && !COLOR->m_colors.empty())
            GRADIENT = *COLOR;
    }

    CBorderPassElement::SBorderData border;
    border.box           = FRAMEBOX;
    border.grad1         = GRADIENT;
    border.a             = ScrollOverview::Config::getCanvasViewportBorderOpacity() * TRANSITION;
    border.borderSize    = sc<int>(std::round(ScrollOverview::Config::getCanvasViewportWidth() * monitor->m_scale));
    border.round         = sc<int>(std::round(ScrollOverview::Config::getCanvasViewportRounding() * monitor->m_scale));
    border.outerRound    = border.round;
    border.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
}

void CScrollOverview::renderCanvasDesktopScene(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now) {
    if (!isCanvasDesktop() || !monitor)
        return;

    std::unordered_set<const void*> rendered;
    const auto renderPass = [&](bool floating) {
        for (const auto& windowRef : Desktop::windowState()->windows()) {
            const auto WINDOW = getOverviewWindowToShow(windowRef);
            if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || WINDOW->m_isFloating != floating || !rendered.emplace(WINDOW.get()).second)
                continue;
            if (dragActiveWindow && WINDOW == getOverviewWindowToShow(dragActiveWindow.lock()))
                continue;

            const auto WINDOWBOX = canvasDesktopWindowBox(WINDOW);
            if (!overviewBoxIntersectsMonitor(WINDOWBOX, monitor))
                continue;

            noteCanvasWindowedBox(WINDOW);
            renderWindowLive(monitor, WINDOW, WINDOWBOX, renderScale, now);
            renderNavigatorWindowOverlay(monitor, WINDOW, WINDOWBOX);
        }
    };

    renderPass(false);
    renderPass(true);

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!canvasScreenFixedWindow(window))
            continue;
        const auto BOX = canvasDesktopWindowBox(window);
        if (overviewBoxIntersectsMonitor(BOX, monitor))
            renderWindowLive(monitor, window, BOX, 1.F, now);
    }
}

void CScrollOverview::renderChromeLayers(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor)
        return;

    const bool  ANIMATE  = ScrollOverview::Config::getChromeAnimationEnabled();
    const float PROGRESS = ANIMATE ? overviewProgress() : 0.F;
    const auto  TOPNS    = ScrollOverview::Config::getChromeTopNamespace();
    const auto  BOTTOMNS = ScrollOverview::Config::getChromeBottomNamespace();
    const float FULLHEIGHT = monitor->m_size.y * monitor->m_scale;

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (auto const& ls : monitor->m_layerSurfaceLayers[LAYER]) {
            const auto SURFACE = ls.lock();
            if (!Desktop::View::validMapped(SURFACE))
                continue;

            const bool ISTOP    = !TOPNS.empty() && SURFACE->m_namespace == TOPNS;
            const bool ISBOTTOM = !BOTTOMNS.empty() && SURFACE->m_namespace == BOTTOMNS;
            if (!ANIMATE || PROGRESS <= 0.001F || (!ISTOP && !ISBOTTOM)) {
                g_pHyprRenderer->renderLayer(SURFACE, monitor, now);
                continue;
            }

            const float TARGETSCALE = ISTOP ? ScrollOverview::Config::getChromeTopScale() : ScrollOverview::Config::getChromeBottomScale();
            const float LAYERSCALE  = 1.F + (TARGETSCALE - 1.F) * PROGRESS;
            const float TRAVEL      = ISTOP ? ScrollOverview::Config::getChromeTopTravel() : ScrollOverview::Config::getChromeBottomTravel();
            const float EDGEHEIGHT  = std::max(1.F, sc<float>(SURFACE->m_geometry.height) * monitor->m_scale);
            const float CENTERCOMPENSATION = FULLHEIGHT * 0.5F * (1.F - LAYERSCALE);
            const float DISTANCE = EDGEHEIGHT * TRAVEL * PROGRESS + CENTERCOMPENSATION;

            Render::SRenderModifData modif;
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALECENTER, LAYERSCALE);
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, Vector2D{0.F, ISTOP ? -DISTANCE : DISTANCE});
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));

            auto& ALPHA = SURFACE->alpha()[Desktop::View::LS_ALPHA_FADE];
            const float PREVIOUSALPHA = ALPHA->value();
            const float TARGETALPHA   = ScrollOverview::Config::getChromeOpacity();
            ALPHA->setValueAndWarp(PREVIOUSALPHA * (1.F + (TARGETALPHA - 1.F) * PROGRESS));
            g_pHyprRenderer->renderLayer(SURFACE, monitor, now);
            ALPHA->setValueAndWarp(PREVIOUSALPHA);

            g_pHyprRenderer->m_renderPass.add(
                makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
        }
    }
}

void CScrollOverview::renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now) {
    auto*      dragOwner = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
    const auto WINDOW    = getOverviewWindowToShow(dragOwner->dragActiveWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
        return;

    const auto GLOBALBOX  = dragOwner->draggedWindowGlobalBox();
    const auto MONITORBOX = monitor ? monitor->logicalBox() : CBox{};
    if (GLOBALBOX.empty() || MONITORBOX.empty() || GLOBALBOX.intersection(MONITORBOX).empty())
        return;

    const auto windowBox = CBox{
        (GLOBALBOX.pos() - monitor->m_position) * monitor->m_scale,
        GLOBALBOX.size() * monitor->m_scale,
    };

    renderWindowLive(monitor, WINDOW, windowBox, dragOwner->scale->value(), now, nullptr, true);
}

bool CScrollOverview::hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const {
    if (!monitor)
        return false;

    const auto DRAGGEDWINDOW = getOverviewWindowToShow(dragActiveWindow.lock());

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, monitor))
            continue;

        const auto workspace = workspaceImage->pWorkspace;

        const auto isVisiblePrecomputedBlurWindow = [&](const PHLWINDOW& window) {
            if (window == DRAGGEDWINDOW || !OverviewWindow::shouldUseBlurFramebuffer(window))
                return false;

            const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
            return overviewBoxIntersectsMonitor(windowBox, monitor);
        };

        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                if (isVisiblePrecomputedBlurWindow(fullscreenWindow))
                    return true;

                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            if (isVisiblePrecomputedBlurWindow(getOverviewWindowToShow(windowRef.lock())))
                return true;
        }
    }

    return false;
}

void CScrollOverview::renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now) {
    if (!monitor)
        return;

    const auto TARGETOVERVIEWSCALE = ScrollOverview::Config::getScale();
    const auto ANIMATIONPROGRESS   = (1.F - TARGETOVERVIEWSCALE) > 0.001F ? (1.F - overviewScale) / (1.F - TARGETOVERVIEWSCALE) : 1.F;

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window))
            continue;
        if (dragActiveWindow && window == getOverviewWindowToShow(dragActiveWindow.lock()))
            continue;

        if (window->m_monitor != monitor)
            continue;

        float renderScale = 1.F;
        CBox  windowBox   = getPinnedFloatingOverviewWindowBox(monitor, window, TARGETOVERVIEWSCALE, ANIMATIONPROGRESS, &renderScale);

        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            continue;

        renderWindowLive(monitor, window, windowBox, renderScale, now);
    }
}

void CScrollOverview::renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox,
                                       bool dragged) {
    if (!window)
        return;

    auto* const DRAGOWNER = g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow ? g_pointerGrabOverview : this;
    const auto  DRAGGED   = getOverviewWindowToShow(DRAGOWNER->dragActiveWindow.lock());
    auto        PSEUDOFOCUSED = PHLWINDOW{};
    if (now < g_pseudoFocusUntil)
        PSEUDOFOCUSED = getOverviewWindowToShow(g_pseudoFocusedWindow.lock());
    else {
        g_pseudoFocusedWindow.reset();
        g_pseudoFocusUntil = {};
    }

    if (!shouldShowOverviewWindow(PSEUDOFOCUSED))
        PSEUDOFOCUSED.reset();

    const auto PSEUDOFOCUSWINDOW = DRAGGED ? DRAGGED : PSEUDOFOCUSED;

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    if (SpatialOverview::Experiments::on(ECanvasExperiment::Depth) && isCanvasNavigationActive() && !windowBox.empty()) {
        const float SHIFT = 14.F * std::max(monitor->m_scale, 0.01F) * overviewProgress();
        CRectPassElement::SRectData shadow;
        shadow.box = CBox{windowBox.x + SHIFT, windowBox.y + SHIFT * 0.65F, windowBox.width, windowBox.height}.round();
        shadow.color = CHyprColor{0.F, 0.F, 0.F, 0.38F * overviewProgress()};
        shadow.round = std::max(1, sc<int>(std::round(10.F * monitor->m_scale)));
        shadow.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(shadow));

        CRectPassElement::SRectData edge;
        edge.box = CBox{windowBox.x + windowBox.width, windowBox.y + SHIFT * 0.35F, std::max(2.F, 8.F * monitor->m_scale), windowBox.height}.round();
        edge.color = CHyprColor{0.15F, 0.2F, 0.28F, 0.55F * overviewProgress()};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(edge));
    }

    OverviewWindow::renderOverviewWindow({
        .monitor              = monitor,
        .window               = window,
        .windowBox            = windowBox,
        .renderScale          = renderScale,
        .now                  = now,
        .workspaceBox         = workspaceBox,
        .selected             = closeOnWindow == window,
        .dragged              = dragged,
        .cameraTransform      = isCanvasDesktop() && !dragged,
        .pseudoFocusWindow    = PSEUDOFOCUSWINDOW,
    });
}

void CScrollOverview::redrawAll(bool forcelowres) {
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windows.clear();
        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }
    pinnedFloatingWindows.clear();

    std::unordered_map<WORKSPACEID, SP<SWorkspaceImage>> imagesByWorkspace;
    imagesByWorkspace.reserve(images.size());

    for (const auto& img : images) {
        if (img && img->pWorkspace)
            imagesByWorkspace.emplace(img->pWorkspace->m_id, img);
    }

    std::vector<PHLWINDOW> addedWindows;
    addedWindows.reserve(Desktop::windowState()->windows().size());

    std::vector<PHLWINDOW> addedPinnedFloatingWindows;
    addedPinnedFloatingWindows.reserve(Desktop::windowState()->windows().size());

    const auto addOverviewWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowOverviewWindow(overviewWindow) || !overviewWindow->m_workspace)
            return;

        if (std::ranges::find(addedWindows, overviewWindow) != addedWindows.end())
            return;

        const auto imageIt = imagesByWorkspace.find(overviewWindow->m_workspace->m_id);
        if (imageIt == imagesByWorkspace.end())
            return;

        addedWindows.emplace_back(overviewWindow);
        imageIt->second->windows.emplace_back(overviewWindow);
    };

    const auto addPinnedFloatingWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowPinnedFloatingOverviewWindow(overviewWindow))
            return;

        if (std::ranges::find(addedPinnedFloatingWindows, overviewWindow) != addedPinnedFloatingWindows.end())
            return;

        addedPinnedFloatingWindows.emplace_back(overviewWindow);
        pinnedFloatingWindows.emplace_back(overviewWindow);
    };

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) != window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) == window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    updateWorkspaceOverflow();
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::requestInputFrame() {
    if (closing)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    inputFramePending = true;
    MONITOR->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_CURSOR_MOVE);
}

void CScrollOverview::markBlurDirty() {
    overviewBlurDirty = true;
}

void CScrollOverview::markBackdropBlurDirty() {
    backdropBlurDirty  = true;
    backdropSharpDirty = true;
}

void CScrollOverview::onDamageReported() {
    return;
}

bool CScrollOverview::isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !window || !Fullscreen::controller()->isFullscreen(window) || window->m_monitor != MONITOR)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
        if (fullscreenWindow != window)
            return false;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        return overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR);
    }

    return false;
}

bool CScrollOverview::shouldAllowRealtimePreviewFrame() const {
    if (lastRealtimePreviewFrame.time_since_epoch().count() == 0)
        return true;

    return Time::steadyNow() - lastRealtimePreviewFrame >= OVERVIEW_WINDOW_FRAME_INTERVAL;
}

bool CScrollOverview::shouldAllowRealtimePreviewSchedule() {
    if (inputFramePending) {
        inputFramePending = false;
        return true;
    }

    if (selectedWorkspaceFramePending) {
        selectedWorkspaceFramePending = false;
        return true;
    }

    if (closing)
        return true;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return true;

    if (realtimePreviewFrameQueued) {
        scheduleRealtimePreviewFrame();
        return false;
    }

    if (shouldAllowRealtimePreviewFrame()) {
        realtimePreviewFrameQueued = true;
        return true;
    }

    scheduleRealtimePreviewFrame();
    return false;
}

void CScrollOverview::schedulePreviewFrameAfter(std::chrono::milliseconds delay) {
    if (!realtimePreviewTimer)
        return;

    const auto DELAY = std::max<int>(1, sc<int>(delay.count()));
    const auto DUE   = Time::steadyNow() + std::chrono::milliseconds(DELAY);

    if (realtimePreviewTimerArmed && realtimePreviewTimerDue <= DUE)
        return;

    realtimePreviewTimerArmed = true;
    realtimePreviewTimerDue   = DUE;
    wl_event_source_timer_update(realtimePreviewTimer, DELAY);
}

void CScrollOverview::scheduleMinimumPreviewFrame() {
    schedulePreviewFrameAfter(getOverviewIdleFrameInterval());
}

void CScrollOverview::scheduleRealtimePreviewFrame() {
    const auto NOW     = Time::steadyNow();
    const auto ELAPSED = lastRealtimePreviewFrame.time_since_epoch().count() == 0 ? OVERVIEW_WINDOW_FRAME_INTERVAL :
                                                                                   std::chrono::duration_cast<std::chrono::milliseconds>(NOW - lastRealtimePreviewFrame);
    const auto DELAY   = OVERVIEW_WINDOW_FRAME_INTERVAL - std::min(ELAPSED, OVERVIEW_WINDOW_FRAME_INTERVAL);
    schedulePreviewFrameAfter(DELAY);
}

int CScrollOverview::realtimePreviewTimerCallback(void* data) {
    const auto OVERVIEW = sc<CScrollOverview*>(data);
    if (!OVERVIEW)
        return 0;

    OVERVIEW->realtimePreviewTimerArmed  = false;
    OVERVIEW->realtimePreviewTimerDue    = {};
    OVERVIEW->realtimePreviewFrameQueued = false;
    OVERVIEW->damage();
    OVERVIEW->scheduleMinimumPreviewFrame();
    return 0;
}

bool CScrollOverview::hasRunningWorkspaceAnimation() const {
    return viewOffset->isBeingAnimated() || workspaceInsertProgress->isBeingAnimated() || workspaceInsertFadeProgress->isBeingAnimated();
}

bool CScrollOverview::shouldSuppressRenderDamage() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return false;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto DRAGGED   = getOverviewWindowToShow(dragActiveWindow.lock());

    const auto isVisibleAnimatedWindow = [&](const PHLWINDOW& window, const Vector2D& workspaceOffset) {
        if (!shouldShowOverviewWindow(window) || window == DRAGGED)
            return false;

        const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceOffset, layout);
        return overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR) && windowHasOverviewAnimation(window);
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (windowHasOverviewAnimation(window))
            return false;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, SCALE, MONITOR);
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, MONITOR))
            continue;

        const auto workspace = workspaceImage->pWorkspace;
        if (!isWorkspaceScrolling(workspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                if (isVisibleAnimatedWindow(fullscreenWindow, WORKSPACEOFFSET))
                    return false;

                for (const auto& windowRef : workspaceImage->windows) {
                    const auto window = getOverviewWindowToShow(windowRef.lock());
                    if (window && window->m_isFloating && isVisibleAnimatedWindow(window, WORKSPACEOFFSET))
                        return false;
                }

                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (isVisibleAnimatedWindow(window, WORKSPACEOFFSET))
                return false;
        }
    }

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (layerHasOverviewAnimation(layerRef.lock()))
                return false;
        }
    }

    return true;
}

void CScrollOverview::sendOverviewFrameCallbacks(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);
    const auto DRAGGED   = getOverviewWindowToShow(dragActiveWindow.lock());
    const bool CANFRAMETHROTTLEDWINDOWS = closing || shouldAllowRealtimePreviewFrame();
    bool       sentThrottledWindowFrame = false;

    const bool PREVSENDINGFRAMECALLBACKS = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks        = CANFRAMETHROTTLEDWINDOWS;
    auto resetSendingFrameCallbacks      = Hyprutils::Utils::CScopeGuard([this, PREVSENDINGFRAMECALLBACKS] { sendingOverviewFrameCallbacks = PREVSENDINGFRAMECALLBACKS; });

    const auto frameWindow = [&](const PHLWINDOW& window, const Vector2D& workspaceOffset, bool realtime) {
        if (!shouldShowOverviewWindow(window))
            return;

        const bool ISDRAGGED = window == DRAGGED;
        if (!ISDRAGGED) {
            const auto WINDOWBOX = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), workspaceOffset, layout);
            if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
                return;
        }

        if (!realtime && !ISDRAGGED && !CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            return;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        popupTreePresent(window, MONITOR, now);
        if (!realtime && !ISDRAGGED)
            sentThrottledWindowFrame = true;
    };

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window) || window->m_monitor != MONITOR)
            continue;

        if (!CANFRAMETHROTTLEDWINDOWS) {
            scheduleRealtimePreviewFrame();
            continue;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        popupTreePresent(window, MONITOR, now);
        sentThrottledWindowFrame = true;
    }

    // The canvas desktop: every window this canvas draws gets a frame on
    // every refresh, as on a plain desktop (video pacing and audio/video sync
    // depend on it). Windows no canvas shows are throttled, by the canvas of
    // the monitor they belong to.
    if (isCanvasDesktop()) {
        for (const auto& windowRef : Desktop::windowState()->windows()) {
            const auto window = getOverviewWindowToShow(windowRef);
            if (!shouldShowOverviewWindow(window) || window->m_pinned)
                continue;
            const auto* OWNER = canvasFrameOwner(window);
            if (OWNER != this && window != DRAGGED) {
                if (OWNER || window->m_monitor != MONITOR)
                    continue;
                if (!CANFRAMETHROTTLEDWINDOWS) {
                    scheduleRealtimePreviewFrame();
                    continue;
                }
                sentThrottledWindowFrame = true;
            }
            surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
            popupTreePresent(window, MONITOR, now);
        }
    } else {
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            const auto& workspaceImage = images[workspaceIdx];
            if (!workspaceImage || !workspaceImage->pWorkspace)
                continue;

            const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
            const auto WORKSPACEBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
            const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, SCALE, MONITOR);
            if (!overviewBoxIntersectsMonitor(VISIBLEBOX, MONITOR))
                continue;

            const auto workspace = workspaceImage->pWorkspace;
            const bool REALTIME  = isSelectedWorkspace(workspace);
            if (!isWorkspaceScrolling(workspace)) {
                const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
                if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspace) {
                    frameWindow(fullscreenWindow, WORKSPACEOFFSET, REALTIME);
                    for (const auto& windowRef : workspaceImage->windows) {
                        const auto window = getOverviewWindowToShow(windowRef.lock());
                        if (window && window->m_isFloating)
                            frameWindow(window, WORKSPACEOFFSET, REALTIME);
                    }
                    continue;
                }
            }

            for (const auto& windowRef : workspaceImage->windows) {
                frameWindow(getOverviewWindowToShow(windowRef.lock()), WORKSPACEOFFSET, REALTIME);
            }
        }
    }

    if (sentThrottledWindowFrame)
        lastRealtimePreviewFrame = now;

    for (const auto& window : Desktop::windowState()->windows()) {
        if (canvasScreenFixedWindow(window) && overviewBoxIntersectsMonitor(canvasDesktopWindowBox(window), MONITOR))
            surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
    }

    realtimePreviewFrameQueued = false;

    for (const auto LAYER :
         {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            const auto layer = layerRef.lock();
            if (Desktop::View::validMapped(layer) && surfaceTreeHasFrameCallbacks(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr))
                surfaceTreePresent(layer->wlSurface() ? layer->wlSurface()->resource() : nullptr, MONITOR, now);
        }
    }
}

bool CScrollOverview::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return true;

    auto view = HLSURFACE->view();
    if (!view)
        return true;

    auto layerOwner  = Desktop::View::CLayerSurface::fromView(view);
    auto windowOwner = Desktop::View::CWindow::fromView(view);

    if (!layerOwner && !windowOwner) {
        if (const auto POPUP = Desktop::View::CPopup::fromView(view)) {
            if (const auto T1OWNER = POPUP->getT1Owner(); T1OWNER && T1OWNER->view()) {
                layerOwner  = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
                windowOwner = Desktop::View::CWindow::fromView(T1OWNER->view());
            }
        }
    }

    if (layerOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);
    if (g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow &&
        window == getOverviewWindowToShow(g_pointerGrabOverview->dragActiveWindow.lock()))
        return true;

    // Fixed to the screen (pinned, X11 menus): drawn every frame where they are.
    if (isCanvasDesktop() && canvasScreenFixedWindow(window))
        return true;

    // The canvas desktop: whatever a canvas draws gets its frames at that
    // monitor's refresh (see sendOverviewFrameCallbacks). Windows no canvas
    // shows only get the throttled ones.
    if (isCanvasDesktop() && shouldShowOverviewWindow(window) && !window->m_pinned) {
        if (canvasFrameOwner(window))
            return true;
        return std::ranges::any_of(scrollOverviews(), [](const auto& overview) {
            const auto* CANVAS = canvasOf(overview);
            return CANVAS && CANVAS->sendingOverviewFrameCallbacks;
        });
    }

    if (!window || window->m_monitor != MONITOR)
        return true;

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (!shouldShowOverviewWindow(window) || !window->m_workspace)
        return true;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        if (!isWorkspaceScrolling(workspaceImage->pWorkspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
                return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (isSelectedWorkspace(workspaceImage->pWorkspace))
            return true;

        if (sendingOverviewFrameCallbacks)
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    return false;
}

bool CScrollOverview::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return true;

    auto view = HLSURFACE->view();
    if (!view)
        return true;

    auto layerOwner = Desktop::View::CLayerSurface::fromView(view);
    auto windowOwner = Desktop::View::CWindow::fromView(view);

    if (!layerOwner && !windowOwner) {
        if (const auto POPUP = Desktop::View::CPopup::fromView(view)) {
            if (const auto T1OWNER = POPUP->getT1Owner(); T1OWNER && T1OWNER->view()) {
                layerOwner  = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
                windowOwner = Desktop::View::CWindow::fromView(T1OWNER->view());
            }
        }
    }

    if (layerOwner) {
        if (layerOwner->m_monitor != MONITOR)
            return true;

        if (layerOwner->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)
            return false;

        markBlurDirty();
        if (layerOwner->m_layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
            markBackdropBlurDirty();
        return true;
    }

    if (!windowOwner)
        return true;

    auto window = getOverviewWindowToShow(windowOwner);
    if (g_pointerGrabOverview && g_pointerGrabOverview->dragActiveWindow &&
        window == getOverviewWindowToShow(g_pointerGrabOverview->dragActiveWindow.lock())) {
        const auto DRAGBOX = g_pointerGrabOverview->draggedWindowGlobalBox();
        if (!DRAGBOX.empty() && !DRAGBOX.intersection(MONITOR->logicalBox()).empty())
            damage();
        return true;
    }

    // Fixed to the screen: damage where it is, as Hyprland reports it.
    if (isCanvasDesktop() && canvasScreenFixedWindow(window))
        return true;

    // The canvas desktop draws windows away from their real position, so the
    // damage Hyprland would report lands where nothing shows the window.
    // Instead every canvas showing it repaints, right away: a new frame from
    // a video must reach the screen on the next refresh, also when it comes
    // in while the previous one is still being shown (the redraw is then
    // asked for again at the flip, which the frame pending flag lets
    // through). This runs for the first canvas asked (the caller stops at the
    // first false), so it does the work for all of them.
    if (isCanvasDesktop() && shouldShowOverviewWindow(window) && !window->m_pinned && canvasFrameOwner(window)) {
        for (const auto& overview : scrollOverviews()) {
            if (auto* canvas = canvasOf(overview); canvas && canvas->canvasDrawsWindow(window)) {
                canvas->selectedWorkspaceFramePending = true;
                canvas->damage();
            }
        }
        return false;
    }

    if (shouldShowPinnedFloatingOverviewWindow(window)) {
        if (window->m_monitor != MONITOR)
            return true;

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;
    }

    if (window && window->m_monitor != MONITOR)
        return true;

    if (!shouldShowOverviewWindow(window) || !window->m_workspace)
        return false;

    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || workspaceImage->pWorkspace != window->m_workspace)
            continue;

        if (!isWorkspaceScrolling(workspaceImage->pWorkspace)) {
            const auto fullscreenWindow = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspaceImage->pWorkspace));
            if (shouldShowOverviewWindow(fullscreenWindow) && fullscreenWindow->m_workspace == workspaceImage->pWorkspace && fullscreenWindow != window && !window->m_isFloating)
                return false;
        }

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, ACTIVEIDX, PITCH);
        const auto WINDOWBOX        = getOverviewWindowBox(window, MONITOR, SCALE, viewOffset->value(), WORKSPACEOFFSET, layout);
        if (!overviewBoxIntersectsMonitor(WINDOWBOX, MONITOR))
            return false;

        if (isSelectedWorkspace(workspaceImage->pWorkspace)) {
            selectedWorkspaceFramePending = true;
            return true;
        }

        if (!realtimePreviewFrameQueued && shouldAllowRealtimePreviewFrame())
            return true;

        scheduleRealtimePreviewFrame();
        return false;

    }

    return false;
}

void CScrollOverview::close() {
    if (closeApplied)
        return;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR) {
        removeOverview(this);
        return;
    }

    closeApplied = true;

    const bool ACTIVATESELECTION = activeScrollOverview().get() == this;
    setClosing(true);
    endNavigatorSessionIfIdle();

    const auto SELECTEDWORKSPACE =
        viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};

    const auto finishClose = [&](const PHLWORKSPACE& finalWorkspace, const PHLWINDOW& finalWindow) {
        emitFullscreenVisibilityState(getOverviewFullscreenVisibilityWindow(finalWorkspace, finalWindow), false);

        if (!ScrollOverview::Config::getValue<int>("animations:enabled")) {
            forceWorkspaceWindowsDecoRecalc(finalWorkspace ? finalWorkspace : MONITOR->m_activeWorkspace);
            damage();
        }

        if (isCanvasDesktop()) {
            if (transitionProgress->value() <= 0.001F && !transitionProgress->isBeingAnimated()) {
                *scale = 1.F;
                removeOverview(this);
                return;
            }

            transitionProgress->setCallbackOnEnd([this](auto) { removeOverview(this); });
            *scale = 1.F;
            *transitionProgress = 0.F;
        } else {
            scale->setCallbackOnEnd([this](auto) { removeOverview(this); });
            *scale = 1.F;
        }
    };

    if (!closeOnWindow && (!SELECTEDWORKSPACE || SELECTEDWORKSPACE == MONITOR->m_activeWorkspace)) {
        const auto FOCUSEDWINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
        if (!SELECTEDWORKSPACE || (FOCUSEDWINDOW && FOCUSEDWINDOW->m_workspace == SELECTEDWORKSPACE))
            closeOnWindow = FOCUSEDWINDOW;
    }

    closeOnWindow = getOverviewWindowToShow(closeOnWindow.lock());

    if (SELECTEDWORKSPACE && commitCanvasViewport(viewportCurrentWorkspace)) {
        const auto FINALWINDOW = closeOnWindow && closeOnWindow->m_workspace == SELECTEDWORKSPACE ? getOverviewWindowToShow(closeOnWindow.lock()) : PHLWINDOW{};

        if (SELECTEDWORKSPACE != MONITOR->m_activeWorkspace)
            MONITOR->changeWorkspace(SELECTEDWORKSPACE, false, true, true);

        viewOffset->setValueAndWarp(Vector2D{});
        *viewOffset                = Vector2D{};
        startedOn                  = SELECTEDWORKSPACE;
        focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

        if (ACTIVATESELECTION && FINALWINDOW)
            Desktop::focusState()->fullWindowFocus(FINALWINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        finishClose(SELECTEDWORKSPACE, FINALWINDOW);
        return;
    }

    if (closeOnWindow && focusSyncedFromWorkspaceID != WORKSPACE_INVALID) {
        const auto FINALWORKSPACE = closeOnWindow->m_workspace;
        size_t     sourceIdx      = images.size();
        size_t     targetIdx      = images.size();

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || !images[workspaceIdx]->pWorkspace)
                continue;

            if (images[workspaceIdx]->pWorkspace->m_id == focusSyncedFromWorkspaceID)
                sourceIdx = workspaceIdx;
            if (images[workspaceIdx]->pWorkspace == FINALWORKSPACE)
                targetIdx = workspaceIdx;
        }

        if (sourceIdx < images.size() && targetIdx < images.size()) {
            if (FINALWORKSPACE != MONITOR->m_activeWorkspace)
                MONITOR->changeWorkspace(FINALWORKSPACE, false, true, true);

            if (ACTIVATESELECTION)
                Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

            startedOn                = FINALWORKSPACE;
            viewportCurrentWorkspace = targetIdx;

            const auto FINALPITCH = getWorkspaceLogicalPitch(MONITOR, 1.F, layout);
            viewOffset->setValueAndWarp(workspaceOverviewLogicalVectorOffset(sourceIdx, targetIdx, FINALPITCH));
            *viewOffset = Vector2D{};

            focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

            const auto FINALWINDOW = getOverviewWindowToShow(closeOnWindow.lock());
            finishClose(FINALWORKSPACE, FINALWINDOW);
            return;
        }
    }

    if (!closeOnWindow) {
        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceLogicalPitch(MONITOR, 1.F, layout);
        *viewOffset = Vector2D{};

        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            if (!images[workspaceIdx] || images[workspaceIdx]->pWorkspace != SELECTEDWORKSPACE)
                continue;

            *viewOffset = workspaceOverviewLogicalVectorOffset(workspaceIdx, ACTIVEIDX, FINALPITCH);
            break;
        }

        if (SELECTEDWORKSPACE && SELECTEDWORKSPACE != MONITOR->m_activeWorkspace)
            MONITOR->changeWorkspace(SELECTEDWORKSPACE, false, true, true);
    } else if (closeOnWindow == Desktop::focusState()->window() && closeOnWindow->m_workspace == MONITOR->m_activeWorkspace) {
        if (focusSyncedFromWorkspaceID != WORKSPACE_INVALID) {
            const auto ACTIVEIDX   = activeWorkspaceIndex();
            const auto FINALPITCH = getWorkspaceLogicalPitch(MONITOR, 1.F, layout);

            for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
                if (!images[workspaceIdx] || !images[workspaceIdx]->pWorkspace || images[workspaceIdx]->pWorkspace->m_id != focusSyncedFromWorkspaceID)
                    continue;

                viewOffset->setValueAndWarp(workspaceOverviewLogicalVectorOffset(workspaceIdx, ACTIVEIDX, FINALPITCH));
                break;
            }
        }

        *viewOffset = Vector2D{};
    } else {

        if (closeOnWindow->m_workspace != MONITOR->m_activeWorkspace)
            MONITOR->changeWorkspace(closeOnWindow->m_workspace, false, true, true);

        if (ACTIVATESELECTION)
            Desktop::focusState()->fullWindowFocus(closeOnWindow.lock(), Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

        const auto ACTIVEIDX = activeWorkspaceIndex();
        const auto FINALPITCH = getWorkspaceLogicalPitch(MONITOR, 1.F, layout);
        bool       found      = false;
        const auto selectedWindow = getOverviewWindowToShow(closeOnWindow.lock());
        for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
            const auto& wimg = images[workspaceIdx];
            for (const auto& windowRef : wimg->windows) {
                const auto window = getOverviewWindowToShow(windowRef.lock());
                if (window == selectedWindow && window) {
                    *viewOffset = workspaceOverviewLogicalVectorOffset(workspaceIdx, ACTIVEIDX, FINALPITCH);
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }

    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    const auto FINALWINDOW    = getOverviewWindowToShow(closeOnWindow.lock());
    const auto FINALWORKSPACE = FINALWINDOW ? FINALWINDOW->m_workspace : SELECTEDWORKSPACE;
    finishClose(FINALWORKSPACE, FINALWINDOW);
}

bool CScrollOverview::isClosing() const {
    return closing;
}

float CScrollOverview::overviewProgress() const {
    if (isCanvasDesktop() && transitionProgress)
        return std::clamp(transitionProgress->value(), 0.F, 1.F);

    if (!scale)
        return 0.F;

    const float TARGET = ScrollOverview::Config::getScale();
    const float RANGE  = 1.F - TARGET;
    if (RANGE <= 0.0001F)
        return 1.F;

    return std::clamp((1.F - scale->value()) / RANGE, 0.F, 1.F);
}

float CScrollOverview::distortionProgress() const {
    using SpatialOverview::Experiments::current;
    using SpatialOverview::Experiments::on;
    if (current() == ECanvasExperiment::Lens)
        return std::clamp(0.55F + 0.45F * overviewProgress(), 0.F, 1.F);
    if (on(ECanvasExperiment::Landing) || on(ECanvasExperiment::Labels) || on(ECanvasExperiment::Quiet))
        return 0.F;
    return std::pow(overviewProgress(), ScrollOverview::Config::getBarrelTransitionPower());
}

void CScrollOverview::reopen() {
    if (!closing)
        return;

    scale->setCallbackOnEnd({});
    transitionProgress->setCallbackOnEnd({});
    closeApplied = false;
    setClosing(false);
    if (isPersistentCanvas())
        canvasNavigationActive = true;
    activateSubmapIfConfigured();
    emitFullscreenVisibilityState(Desktop::focusState()->window(), true);
    *scale = isCanvasDesktop() ? ScrollOverview::Config::getCanvasInitialZoom() : ScrollOverview::Config::getScale();
    if (isCanvasDesktop())
        *transitionProgress = 1.F;
    damage();
}

// ---- Linked screens --------------------------------------------------------------
// The screens are one wide desk on the canvas: each shows the part next to
// the other's, as the monitors are arranged, and they move together. A window
// is never shown twice, and one dragged across the seam lands where it was
// dropped. Whichever canvas's own code moved its camera last leads; the others
// take the leader's camera every frame (so every camera feature works on any
// screen without knowing about the others). A point g (global logical) shows
// the canvas point  center + camera + (g - center) / zoom  on every screen,
// so another screen's camera is the leader's plus the distance between their
// centers times (1/zoom - 1): at 100% they are the same.
void CScrollOverview::inheritLinkedCamera(const CScrollOverview* from) {
    const auto MONITOR = pMonitor.lock();
    const auto FROM    = from ? from->canvasMonitor() : nullptr;
    if (!MONITOR || !FROM || !isCanvasDesktop() || !ScrollOverview::Config::getCanvasLinkedScreens())
        return;
    const float ZOOM     = std::max(from->scale->goal(), 0.01F);
    const auto  DISTANCE = (MONITOR->m_position + MONITOR->m_size / 2.0) - (FROM->m_position + FROM->m_size / 2.0);
    *scale               = ZOOM;
    *viewOffset          = from->viewOffset->goal() + DISTANCE * (1.0 / ZOOM - 1.0);
}

void CScrollOverview::followLinkedLeader(const CScrollOverview* leader) {
    g_linkedLeader      = leader;
    linkedAppliedOffset = viewOffset->goal();
    linkedAppliedScale  = scale->goal();
}

void CScrollOverview::followLinkedCamera() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasDesktop() || !ScrollOverview::Config::getCanvasLinkedScreens())
        return;

    const bool NEW         = linkedAppliedScale < 0.F;
    const bool MOVEDITSELF = !NEW && (scale->goal() != linkedAppliedScale || viewOffset->goal() != linkedAppliedOffset);
    const CScrollOverview* leader = nullptr;
    for (const auto& overview : scrollOverviews()) {
        const auto* CANVAS = canvasOf(overview);
        if (CANVAS && CANVAS == g_linkedLeader && !CANVAS->isClosing() && CANVAS->isCanvasDesktop())
            leader = CANVAS;
    }
    // One that moved its own camera leads; a new one takes the leader's. When
    // both screens move at once (leaving the zoomed-out view, each its own
    // way), the one you are working on keeps the lead.
    const auto NOW = Time::steadyNow();
    if (MOVEDITSELF) {
        const auto* ACTIVE     = canvasOf(activeScrollOverview());
        const bool  LEADERBUSY = leader && leader != this && leader == ACTIVE && NOW - g_linkedLeaderMovedAt < std::chrono::milliseconds(200);
        if (!LEADERBUSY) {
            leader                = this;
            g_linkedLeaderMovedAt = NOW;
        }
    } else if (!leader)
        leader = this;
    g_linkedLeader = leader;

    if (leader == this) {
        linkedAppliedOffset = viewOffset->goal();
        linkedAppliedScale  = scale->goal();
        return;
    }

    const auto  LEADERMONITOR = leader->canvasMonitor();
    const float ZOOM          = std::max(leader->scale->value(), 0.01F);
    const auto  DISTANCE      = (MONITOR->m_position + MONITOR->m_size / 2.0) - (LEADERMONITOR->m_position + LEADERMONITOR->m_size / 2.0);
    const auto  OFFSET        = leader->viewOffset->value() + DISTANCE * (1.0 / ZOOM - 1.0);
    if (scale->value() != ZOOM || scale->goal() != ZOOM)
        scale->setValueAndWarp(ZOOM);
    if (viewOffset->value() != OFFSET || viewOffset->goal() != OFFSET)
        viewOffset->setValueAndWarp(OFFSET);
    linkedAppliedOffset = OFFSET;
    linkedAppliedScale  = ZOOM;
}

// A canvas window belongs to the workspace of the screen that shows most of
// it. Hyprland keys much to that: one fullscreen window per workspace (a
// window on the other screen's workspace would throw it out of fullscreen),
// which monitor focus is on, where an X11 app is. Checked while the camera is
// at rest at 100%; moving a window to a workspace must not move it on the
// canvas, so it goes back exactly where it was.
void CScrollOverview::syncCanvasWindowScreens() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasDesktop() || closing || canvasNavigationActive || !canvasAtRestZoom() || viewOffset->isBeingAnimated() || scale->isBeingAnimated() ||
        dragActiveWindow || g_pointerGrabOverview || !MONITOR->m_activeWorkspace || MONITOR->m_activeWorkspace->m_isSpecialWorkspace)
        return;
    const auto SCREEN = MONITOR->logicalBox();
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || WINDOW->m_workspace == MONITOR->m_activeWorkspace || Fullscreen::controller()->isFullscreen(WINDOW) ||
            !WINDOW->layoutTarget() || WINDOW->m_realPosition->isBeingAnimated())
            continue;
        const auto BOX  = canvasDrawnGlobalBox(WINDOW);
        const auto MINE = BOX.intersection(SCREEN);
        if (MINE.width * MINE.height < BOX.width * BOX.height * 0.5) // mostly here
            continue;
        const auto KEEP = CBox{WINDOW->m_realPosition->goal(), WINDOW->m_realSize->goal()};
        moveCanvasWindowToWorkspace(WINDOW, MONITOR->m_activeWorkspace);
        WINDOW->m_monitor = MONITOR;
        WINDOW->layoutTarget()->setPositionGlobal(KEEP);
        WINDOW->layoutTarget()->warpPositionSize();
    }
}

void CScrollOverview::onPreRender() {
    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    followLinkedCamera();
    syncCanvasWindowScreens();
    reconcilePlaces();

    forceLayersAboveFullscreen();
    updateWorkspaceOverflow();

    if (closing)
        return;

    if (isCanvasDesktop() && pMonitor && pMonitor->m_activeWorkspace && pMonitor->m_activeWorkspace != startedOn) {
        startedOn      = pMonitor->m_activeWorkspace;
        rebuildPending = true;
    } else if (pMonitor && pMonitor->m_activeWorkspace && pMonitor->m_activeWorkspace != startedOn) {
        rebuildPending = false;
        markBlurDirty();
        onWorkspaceChange();
        focusSyncedFromWorkspaceID = WORKSPACE_INVALID;
        emitFullscreenVisibilityState(Desktop::focusState()->window(), true);
        return;
    }

    focusSyncedFromWorkspaceID = WORKSPACE_INVALID;

    if (rebuildPending) {
        rebuildPending = false;
        markBlurDirty();
        redrawAll();
        syncSelectionToViewport();
        damage();
        return;
    }
}

void CScrollOverview::onWorkspaceChange() {
    if (!pMonitor || !pMonitor->m_activeWorkspace)
        return;

    const auto previousActiveIdx = activeWorkspaceIndex();
    const auto previousStartedOn = startedOn;

    // consume any pending gesture-driven settle (set by finishWorkspaceScrollFollow)
    const bool   GESTURESETTLE       = trackpadGestureSettlePending;
    const double GESTURESETTLEOFFSET = trackpadGestureSettleOffset;
    trackpadGestureSettlePending     = false;

    std::vector<WORKSPACEID> previousWorkspaceIDs;
    previousWorkspaceIDs.reserve(images.size());
    std::unordered_map<WORKSPACEID, float> previousWorkspaceOffsets;
    const auto PREVIOUSLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (!image || !image->pWorkspace)
            continue;

        previousWorkspaceIDs.push_back(image->pWorkspace->m_id);
        previousWorkspaceOffsets.emplace(image->pWorkspace->m_id, workspaceOverviewLogicalOffset(i, previousActiveIdx, PREVIOUSLOGICALPITCH));
    }

    const auto NEWWORKSPACE      = pMonitor->m_activeWorkspace;
    const bool INSERTEDWORKSPACE = std::find(previousWorkspaceIDs.begin(), previousWorkspaceIDs.end(), NEWWORKSPACE->m_id) == previousWorkspaceIDs.end();
    const auto REQUESTEDREMOVEDWORKSPACE = pendingRemovedWorkspace.lock();
    const bool SHOULDREMOVEPREVIOUSWORKSPACE =
        previousStartedOn && previousStartedOn != NEWWORKSPACE && !previousStartedOn->m_isSpecialWorkspace && !previousStartedOn->isPersistent() && previousStartedOn->getWindowCount() == 0;
    const auto REMOVEDWORKSPACE = REQUESTEDREMOVEDWORKSPACE ? REQUESTEDREMOVEDWORKSPACE : SHOULDREMOVEPREVIOUSWORKSPACE ? previousStartedOn : PHLWORKSPACE{};

    pendingRemovedWorkspace = REMOVEDWORKSPACE;

    startedOn = NEWWORKSPACE;
    redrawAll();
    viewportCurrentWorkspace = activeWorkspaceIndex();

    const bool REMOVEDPREVIOUSWORKSPACE =
        REMOVEDWORKSPACE &&
        std::find_if(images.begin(), images.end(), [REMOVEDWORKSPACE](const auto& image) { return image && image->pWorkspace == REMOVEDWORKSPACE; }) == images.end();

    if (INSERTEDWORKSPACE || REMOVEDPREVIOUSWORKSPACE) {
        workspaceInsertTransition.active                 = true;
        workspaceInsertTransition.transitionWorkspaceID  = INSERTEDWORKSPACE ? NEWWORKSPACE->m_id : REMOVEDWORKSPACE->m_id;
        workspaceInsertTransition.transitionFadeIn       = INSERTEDWORKSPACE;
        workspaceInsertFadeProgress->setConfig(INSERTEDWORKSPACE ? workspaceInsertFadeConfig : workspaceRemoveFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;

        for (size_t i = 0; i < previousWorkspaceIDs.size(); ++i) {
            const auto OFFSET = previousWorkspaceOffsets.contains(previousWorkspaceIDs[i]) ? previousWorkspaceOffsets.at(previousWorkspaceIDs[i]) : 0.F;
            workspaceInsertTransition.oldRelativeOffsets.emplace(previousWorkspaceIDs[i], OFFSET);
            if (REMOVEDPREVIOUSWORKSPACE && previousWorkspaceIDs[i] == REMOVEDWORKSPACE->m_id)
                workspaceInsertTransition.transitionOldRelativeOffset = OFFSET;
        }

        const auto NEWLOGICALPITCH = getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout);
        for (size_t i = 0; i < images.size(); ++i) {
            if (!images[i] || !images[i]->pWorkspace)
                continue;

            workspaceInsertTransition.newRelativeOffsets.emplace(images[i]->pWorkspace->m_id, workspaceOverviewLogicalOffset(i, viewportCurrentWorkspace, NEWLOGICALPITCH));
        }

        workspaceInsertProgress->setValueAndWarp(0.F);
        workspaceInsertFadeProgress->setValueAndWarp(0.F);
        *workspaceInsertProgress = 1.F;
        *workspaceInsertFadeProgress = 1.F;
        viewOffset->setValueAndWarp(Vector2D{});
        *viewOffset = Vector2D{};
    } else {
        workspaceInsertTransition.active                 = false;
        workspaceInsertTransition.transitionWorkspaceID  = WORKSPACE_INVALID;
        workspaceInsertTransition.transitionFadeIn       = true;
        workspaceInsertFadeProgress->setConfig(workspaceInsertFadeConfig);
        workspaceInsertTransition.oldRelativeOffsets.clear();
        workspaceInsertTransition.newRelativeOffsets.clear();
        workspaceInsertTransition.transitionOldRelativeOffset = 0.F;
        workspaceInsertProgress->setValueAndWarp(1.F);
        workspaceInsertFadeProgress->setValueAndWarp(1.F);
        if (GESTURESETTLE) // a trackpad follow committed this change: settle from the drag position, not a full pitch
            viewOffset->setValueAndWarp(axisOffsetVector(sc<float>(GESTURESETTLEOFFSET), layout));
        else
            viewOffset->setValueAndWarp(
                workspaceOverviewLogicalVectorOffset(previousActiveIdx, viewportCurrentWorkspace, getWorkspaceLogicalPitch(pMonitor.lock(), scale->value(), layout)));
        *viewOffset = Vector2D{};
    }

    syncSelectionToViewport();
    markBlurDirty();
    damage();
}

void CScrollOverview::render() {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;


    SpatialOverview::BarrelShader::updateUniforms(distortionProgress());

    if (g_pointerGrabOverview && g_pointerGrabOverview != this && g_pointerGrabOverview->dragActiveWindow && isOverviewPointerOnMonitor(MONITOR))
        lastMousePosLocal = getOverviewMousePosLocal(MONITOR);

    const bool PREVBLOCKSURFACEFEEDBACK       = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    g_pHyprRenderer->m_bBlockSurfaceFeedback  = true;
    auto restoreSurfaceFeedback               = Hyprutils::Utils::CScopeGuard([PREVBLOCKSURFACEFEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVBLOCKSURFACEFEEDBACK; });

    const auto NOW       = Time::steadyNow();
    const auto ACTIVEIDX = activeWorkspaceIndex();
    const auto SCALE     = scale->value();
    const auto PITCH     = getWorkspaceRenderedPitch(MONITOR, SCALE, layout);

    const auto VIEWOFFSET = viewOffset->value();
    if (!overviewBlurStateValid || std::abs(lastOverviewBlurScale - SCALE) > 0.001F || lastOverviewBlurViewOffset.distanceSq(VIEWOFFSET) > 0.001F) {
        markBlurDirty();
        overviewBlurStateValid     = true;
        lastOverviewBlurScale      = SCALE;
        lastOverviewBlurViewOffset = VIEWOFFSET;
    }

    const auto WALLPAPERMODE = ScrollOverview::Config::getWallpaperMode();

    const float BLURALPHA = ScrollOverview::Config::getBlur() ? ScrollOverview::Config::getBlurStrength() * overviewProgress() : 0.F;
    if (BLURALPHA > 0.001F && WALLPAPERMODE != 1)
        updateBackdropBlurCache(MONITOR, WALLPAPERMODE, NOW);

    // Persistent canvas mode is sharp at rest. During the navigation
    // transition, blend the cached blurred wallpaper over that sharp source
    // using the same progress that drives zoom, grid, and distortion.
    const auto BACKDROP = WALLPAPERMODE == 1 ? SBackdropTransform{} : canvasBackdropTransform(MONITOR, SCALE);
    if (BACKDROP.active && updateBackdropSharpCache(MONITOR, NOW)) {
        OverviewRender::flushPass(MONITOR);
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, {});
        renderBackdropTiled(MONITOR, backdropSharpFB->getTexture(), 1.F, BACKDROP);
    } else if (WALLPAPERMODE == 0 || WALLPAPERMODE == 2)
        renderGlobalWallpaper(MONITOR, NOW);
    else
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}}, {});

    if (BLURALPHA > 0.001F && backdropBlurFB && backdropBlurFB->isAllocated() && backdropBlurFB->getTexture())
        renderBackdropBlurCache(MONITOR, BLURALPHA, BACKDROP);

    Event::bus()->m_events.render.stage.emit(RENDER_POST_WALLPAPER);

    if (isCanvasDesktop()) {
        renderCanvasBackgroundDim(MONITOR);
        renderCanvasGrid(MONITOR, ACTIVEIDX, PITCH, SCALE);
        OverviewRender::flushPass(MONITOR);
        renderCanvasDesktopScene(MONITOR, SCALE, NOW);
        renderPlaceOutlines(MONITOR);
        renderPlaceMenu(MONITOR);
        renderNavigatorReticle(MONITOR, NOW);
        renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);
        renderExperimentChrome(MONITOR);
        renderCanvasViewport(MONITOR, SCALE);
        renderCanvasMinimap(MONITOR);
        renderChromeLayers(MONITOR, NOW);
        renderNavigatorHud(MONITOR);
        SpatialOverview::Hud::endFrame();
        sendOverviewFrameCallbacks(NOW);
        if (canvasPopupFading())
            schedulePopupFadeFrame();
        if (minimapFlashAlpha(Time::steadyNow()) > 0.F || minimapShown) {
            if (viewOffset->isBeingAnimated())
                g_minimapFlashUntil = std::max(g_minimapFlashUntil, Time::steadyNow() + std::chrono::milliseconds(1000));
            damage();
        }
        canvasResyncX11Windows();
        return;
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceBackground(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    if (workspaceInsertTransition.active && !workspaceInsertTransition.transitionFadeIn) {
        const auto GHOSTALPHA   = 1.F - std::clamp(workspaceInsertFadeProgress->value(), 0.F, 1.F);
        const auto GHOSTOFFSET = workspaceInsertTransition.transitionOldRelativeOffset * SCALE * MONITOR->m_scale;
        const auto GHOSTBOX     = getOverviewWorkspaceBox(MONITOR, SCALE, viewOffset->value(), GHOSTOFFSET, layout);

        if (GHOSTALPHA > 0.001F && overviewBoxIntersectsMonitor(GHOSTBOX, MONITOR)) {
            renderOverviewWorkspaceShadow(MONITOR, GHOSTBOX, SCALE, WALLPAPERMODE == 0, GHOSTALPHA);

            if (WALLPAPERMODE != 0)
                renderWallpaperLayers(MONITOR, GHOSTBOX, SCALE, NOW, GHOSTALPHA);

            renderOverviewLayerLevel(MONITOR, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, GHOSTBOX, SCALE, NOW);
        }
    }

    renderCanvasGrid(MONITOR, ACTIVEIDX, PITCH, SCALE);

    const bool NEEDS_PRECOMPUTED_BLUR = hasVisiblePrecomputedBlurWindow(MONITOR, ACTIVEIDX, PITCH, SCALE);
    if (NEEDS_PRECOMPUTED_BLUR && overviewBlurDirty)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CPreBlurElement>());

    OverviewRender::flushPass(MONITOR);

    if (NEEDS_PRECOMPUTED_BLUR)
        overviewBlurDirty = false;

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceLive(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE, WALLPAPERMODE, NOW);
    }

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        renderWorkspaceOutline(MONITOR, workspaceIdx, ACTIVEIDX, PITCH, SCALE);
    }

    renderDraggedWindow(MONITOR, ACTIVEIDX, PITCH, SCALE, NOW);
    renderPinnedFloatingWindows(MONITOR, SCALE, NOW);
    renderCanvasViewport(MONITOR, SCALE);
    renderChromeLayers(MONITOR, NOW);

    sendOverviewFrameCallbacks(NOW);
}

void CScrollOverview::fullRender() {
    return;
}

static float hyprlerp(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{hyprlerp(from.x, to.x, perc), hyprlerp(from.y, to.y, perc)};
}

void CScrollOverview::setClosing(bool closing_) {
    closing = closing_;
    if (closing) {
        transferSharedStateOwnership();
        inputFramePending = false;
        if (scrollingPanPointerDown)
            endScrollingPan();
        releaseTopLayerPointerButtons(Time::millis(Time::steadyNow()));
        clearDragPending();
        canvasArrangeButtonPressed = false;
        restoreSubmapIfActive();
    } else
        applyWorkspaceAnimationOverrides();
}

void CScrollOverview::releaseInputListeners() {
    if (scrollingPanPointerDown)
        endScrollingPan();
    releaseTopLayerPointerButtons(Time::millis(Time::steadyNow()));
    clearDragPending();
    submapMouseClickPending = false;
    submapMouseClickButton  = 0;
    canvasArrangeButtonPressed = false;

    mouseMoveHook.reset();
    touchMoveHook.reset();
    mouseAxisHook.reset();
    pinchBeginHook.reset();
    pinchUpdateHook.reset();
    pinchEndHook.reset();
    mouseButtonHook.reset();
    touchDownHook.reset();
    keyboardKeyHook.reset();
}

void CScrollOverview::activateSubmapIfConfigured() {
    if (!sharedStateOwner || !usesSubmapKeybinds || !g_pKeybindManager)
        return;

    previousSubmapName = g_pKeybindManager->getCurrentSubmap().name;

    const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find("submap");
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end()) {
        usesSubmapKeybinds = false;
        return;
    }

    const auto RESULT = DISPATCHER->second(OVERVIEW_SUBMAP);
    if (!RESULT.success) {
        usesSubmapKeybinds = false;
        return;
    }

    submapActive = true;
}

void CScrollOverview::restoreSubmapIfActive() {
    if (!submapActive || !g_pKeybindManager)
        return;

    const auto CURRENT = g_pKeybindManager->getCurrentSubmap().name;
    if (CURRENT == OVERVIEW_SUBMAP) {
        const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find("submap");
        if (DISPATCHER != g_pKeybindManager->m_dispatchers.end())
            DISPATCHER->second(previousSubmapName.empty() ? "reset" : previousSubmapName);
    }

    submapActive = false;
}

bool CScrollOverview::dispatchSubmapMouseClick(uint32_t button) {
    if (!usesSubmapKeybinds || !isOverviewSubmapActive() || !g_pKeybindManager || !g_pInputManager)
        return false;

    const auto KEYNAME = "mouse:" + std::to_string(button);
    const auto MODS    = g_pInputManager->getModsFromAllKBs();

    const auto KEYBIND = std::ranges::find_if(g_pKeybindManager->m_keybinds, [&](const auto& keybind) {
        return keybind && keybind->enabled && !keybind->shadowed && keybind->key == KEYNAME && keybind->submap.name == OVERVIEW_SUBMAP &&
            (keybind->modmask == MODS || keybind->ignoreMods);
    });

    if (KEYBIND == g_pKeybindManager->m_keybinds.end())
        return false;

    const auto DISPATCHERNAME = (*KEYBIND)->mouse ? "mouse" : (*KEYBIND)->handler;
    const auto DISPATCHER     = g_pKeybindManager->m_dispatchers.find(DISPATCHERNAME);
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end())
        return false;

    const auto PREVIOUSKEYBIND = g_pKeybindManager->m_currentKeybind;
    g_pKeybindManager->m_currentKeybind = *KEYBIND;
    auto restoreKeybind = Hyprutils::Utils::CScopeGuard([PREVIOUSKEYBIND] { g_pKeybindManager->m_currentKeybind = PREVIOUSKEYBIND; });

    const int PREVIOUSPASSPRESSED = Config::Actions::state()->m_passPressed;
    Config::Actions::state()->m_passPressed = 0;
    auto restorePassPressed = Hyprutils::Utils::CScopeGuard([PREVIOUSPASSPRESSED] { Config::Actions::state()->m_passPressed = PREVIOUSPASSPRESSED; });

    DISPATCHER->second((*KEYBIND)->mouse ? "0" + (*KEYBIND)->arg : (*KEYBIND)->arg);
    return true;
}

void CScrollOverview::resetSwipe() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = isCanvasDesktop() ? ScrollOverview::Config::getCanvasInitialZoom() : ScrollOverview::Config::getScale();
    if (isCanvasDesktop()) {
        canvasNavigationActive = true;
        *transitionProgress = 1.F;
    }
    m_isSwiping = false;
}

void CScrollOverview::onSwipeUpdate(double delta) {
    const int DISTANCE = ScrollOverview::Config::getGestureDistance();

    m_isSwiping = true;

    const float PERC = closing ? 1.0 - std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0) : std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0);

    const float TARGET = isCanvasDesktop() ? ScrollOverview::Config::getCanvasInitialZoom() : ScrollOverview::Config::getScale();
    scale->setValueAndWarp(hyprlerp(1.F, TARGET, PERC));
    if (isCanvasDesktop()) {
        canvasNavigationActive = true;
        transitionProgress->setValueAndWarp(PERC);
    }
}

void CScrollOverview::onSwipeEnd() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = isCanvasDesktop() ? ScrollOverview::Config::getCanvasInitialZoom() : ScrollOverview::Config::getScale();
    if (isCanvasDesktop()) {
        canvasNavigationActive = true;
        *transitionProgress = 1.F;
    }
    m_isSwiping = false;
}


static std::filesystem::path canvasLayoutPath() {
    const auto STATE = std::filesystem::path{SpatialOverview::Experiments::statePath()};
    return STATE.parent_path() / "layout.tsv";
}

void CScrollOverview::saveSharedCanvasLayout() {
    std::error_code error;
    std::filesystem::create_directories(canvasLayoutPath().parent_path(), error);
    std::ofstream out{canvasLayoutPath(), std::ios::trunc};
    if (!out)
        return;

    for (const auto& overview : scrollOverviews()) {
        auto* canvas = dynamic_cast<CScrollOverview*>(overview.get());
        const auto MONITOR = canvas ? canvas->pMonitor.lock() : PHLMONITOR{};
        if (!canvas || !MONITOR || !canvas->viewOffset)
            continue;
        const auto OFFSET = canvas->viewOffset->value();
        out << "camera\t" << MONITOR->m_name << '\t' << OFFSET.x << '\t' << OFFSET.y << '\n';
    }

    std::unordered_set<const void*> visited;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !WINDOW->layoutTarget() || !visited.emplace(WINDOW.get()).second)
            continue;
        const auto BOX = WINDOW->layoutTarget()->position();
        std::string klass = WINDOW->m_class;
        std::string title = WINDOW->m_title;
        std::ranges::replace(klass, '\t', ' ');
        std::ranges::replace(title, '\t', ' ');
        std::ranges::replace(title, '\n', ' ');
        out << "window\t" << klass << '\t' << title << '\t' << BOX.x << '\t' << BOX.y << '\t' << BOX.width << '\t' << BOX.height << '\n';
    }
}

void CScrollOverview::loadSharedCanvasLayout() {
    std::ifstream in{canvasLayoutPath()};
    if (!in)
        return;

    struct SSavedWindow {
        std::string klass;
        std::string title;
        CBox        box;
    };
    std::vector<SSavedWindow> savedWindows;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        std::string field;
        std::istringstream stream{line};
        while (std::getline(stream, field, '\t'))
            fields.push_back(field);
        if (fields.size() == 4 && fields[0] == "camera") {
            try {
                const Vector2D OFFSET{std::stof(fields[2]), std::stof(fields[3])};
                if (!std::isfinite(OFFSET.x) || !std::isfinite(OFFSET.y))
                    continue;
                for (const auto& overview : scrollOverviews()) {
                    auto* canvas = dynamic_cast<CScrollOverview*>(overview.get());
                    const auto MONITOR = canvas ? canvas->pMonitor.lock() : PHLMONITOR{};
                    if (!canvas || !MONITOR || MONITOR->m_name != fields[1] || !canvas->viewOffset)
                        continue;
                    canvas->viewOffset->setValueAndWarp(OFFSET);
                    *canvas->viewOffset = OFFSET;
                }
            } catch (...) {
            }
        } else if (fields.size() == 7 && fields[0] == "window") {
            try {
                const CBox BOX{std::stof(fields[3]), std::stof(fields[4]), std::stof(fields[5]), std::stof(fields[6])};
                if (std::isfinite(BOX.x) && std::isfinite(BOX.y) && std::isfinite(BOX.width) && std::isfinite(BOX.height) && BOX.width >= 1 && BOX.height >= 1 &&
                    BOX.width <= 16384 && BOX.height <= 16384)
                    savedWindows.push_back({fields[1], fields[2], BOX});
            } catch (...) {
            }
        }
    }

    std::unordered_set<const void*> used;
    for (const auto& saved : savedWindows) {
        PHLWINDOW exact;
        PHLWINDOW byClass;
        int classCount = 0;
        for (const auto& windowRef : Desktop::windowState()->windows()) {
            const auto WINDOW = getOverviewWindowToShow(windowRef);
            if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->layoutTarget() || used.contains(WINDOW.get()))
                continue;
            if (WINDOW->m_class != saved.klass)
                continue;
            ++classCount;
            byClass = WINDOW;
            if (WINDOW->m_title == saved.title)
                exact = WINDOW;
        }
        const auto MATCH = exact ? exact : (classCount == 1 ? byClass : PHLWINDOW{});
        if (!MATCH)
            continue;
        used.emplace(MATCH.get());
        if (!MATCH->layoutTarget()->floating()) {
            if (!setCanvasFloating(MATCH, true))
                continue;
        }
        if (!MATCH->layoutTarget())
            continue;
        MATCH->layoutTarget()->rememberFloatingSize(saved.box.size());
        MATCH->layoutTarget()->setPositionGlobal(saved.box);
        MATCH->layoutTarget()->warpPositionSize();
    }
}

void onCanvasExperimentChanged(std::string_view previous) {
    CScrollOverview* canvas = nullptr;
    for (const auto& overview : scrollOverviews()) {
        if (auto* candidate = dynamic_cast<CScrollOverview*>(overview.get())) {
            canvas = candidate;
            break;
        }
    }
    if (canvas && (previous == "persist" || previous == "all") && !SpatialOverview::Experiments::on(ECanvasExperiment::Persist))
        canvas->saveSharedCanvasLayout();
    if (canvas && SpatialOverview::Experiments::on(ECanvasExperiment::Persist) && previous != SpatialOverview::Experiments::id())
        canvas->loadSharedCanvasLayout();
    for (const auto& overview : scrollOverviews()) {
        auto* candidate = dynamic_cast<CScrollOverview*>(overview.get());
        if (!candidate)
            continue;
        candidate->markBlurDirty();
        candidate->damage();
    }
}

static int saveCanvasMemory(void*) {
    if (scrollOverviews().empty() || !ScrollOverview::Config::getCanvasRememberLayout())
        return 0;

    std::vector<SpatialOverview::Memory::SWindowPlacement> placements;
    std::unordered_set<const void*>                        visited;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !WINDOW->layoutTarget() || !WINDOW->layoutTarget()->floating() ||
            !visited.emplace(WINDOW.get()).second)
            continue;
        placements.push_back({.window = WINDOW, .box = WINDOW->layoutTarget()->position()});
    }

    // Cameras are remembered where they rest at 100%, not mid-flight or zoomed out.
    std::unordered_map<std::string, Vector2D> cameras;
    for (const auto& overview : scrollOverviews()) {
        const auto* canvas  = canvasOf(overview);
        const auto  MONITOR = overview ? overview->pMonitor.lock() : PHLMONITOR{};
        if (!canvas || !MONITOR || !canvas->isCanvasDesktop() || canvas->isClosing())
            continue;
        cameras[MONITOR->m_name] = canvas->isCanvasNavigationActive() && canvas->hasNavigationReturn ? canvas->navigationReturnOffset : canvas->restingCameraOffset();
    }
    SpatialOverview::Memory::save(placements, cameras);
    return 0;
}

void disarmCanvasTimers() {
    disarmCanvasSwitcher();
    if (g_popupFadeTimer)
        wl_event_source_remove(g_popupFadeTimer);
    g_popupFadeTimer = nullptr;
    if (g_canvasMemoryTimer)
        wl_event_source_remove(g_canvasMemoryTimer);
    g_canvasMemoryTimer = nullptr;
}

void CScrollOverview::noteCanvasLayoutChanged() {
    if (SpatialOverview::Experiments::on(ECanvasExperiment::Persist))
        saveSharedCanvasLayout();
    if (!ScrollOverview::Config::getCanvasRememberLayout() || !g_pCompositor)
        return;
    if (!g_canvasMemoryTimer)
        g_canvasMemoryTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, saveCanvasMemory, nullptr);
    if (g_canvasMemoryTimer)
        wl_event_source_timer_update(g_canvasMemoryTimer, 600);
}

Vector2D CScrollOverview::restingCameraOffset() const {
    return viewOffset ? viewOffset->goal() : Vector2D{};
}

CBox CScrollOverview::currentCanvasViewportWorld() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return {};

    const float ZOOM = std::max(scale->value(), 0.01F);
    const auto  CENTER = MONITOR->m_size / 2.F;
    return CBox{MONITOR->m_position + viewOffset->value() + CENTER - CENTER * (1.F / ZOOM), MONITOR->m_size * (1.F / ZOOM)};
}

void CScrollOverview::commitLanding() {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR)
        return;

    if (!hasLandingDestination) {
        landingDestinationWorld = currentCanvasViewportWorld();
        hasLandingDestination = !landingDestinationWorld.empty();
    }

    canvasNavigationActive = false;
    canvasPinching         = false;
    landingDrag            = false;
    const Vector2D OFFSET  = landingDestinationWorld.pos() - MONITOR->m_position;
    *viewOffset            = OFFSET;
    *scale                 = 1.F;
    *transitionProgress    = 0.F;
    leaveNavigatorPointer();
    endNavigatorSessionIfIdle();
    if (activeScrollOverview().get() == this)
        ensureCanvasKeyboardFocus();
    noteCanvasLayoutChanged();
    damage();
}

void CScrollOverview::revertNavigation() {
    if (!isCanvasDesktop())
        return;

    canvasNavigationActive = false;
    canvasPinching         = false;
    landingDrag            = false;
    if (hasNavigationReturn) {
        *viewOffset = navigationReturnOffset;
    }
    *scale              = 1.F;
    *transitionProgress = 0.F;
    leaveNavigatorPointer();
    endNavigatorSessionIfIdle();
    if (activeScrollOverview().get() == this)
        ensureCanvasKeyboardFocus();
    damage();
}

bool CScrollOverview::showsNavigatorHud() const {
    return isCanvasNavigationActive() && !closing && SpatialOverview::Navigator::isOpen() && ScrollOverview::Config::getNavigatorEnabled() &&
        activeScrollOverview().get() == static_cast<const IOverview*>(this);
}

bool CScrollOverview::navigatorOwnsPointer() const {
    return isCanvasNavigationActive() && ScrollOverview::Config::getNavigatorPointer();
}

Vector2D CScrollOverview::canvasCameraOffsetFor(const PHLWINDOW& window, float zoom, bool navigating) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !window)
        return viewOffset->goal();

    const auto BOX    = window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    auto       target = BOX.middle();
    if (navigating) {
        // Keep the selection clear of the search palette above it.
        if (showsNavigatorHud())
            target.y -= (SpatialOverview::Hud::selectionFocusY(MONITOR->m_size) - MONITOR->m_size.y * 0.5) / std::max(zoom, 0.01F);
    } else {
        // At 100% an oversized window keeps its top-left corner on screen,
        // where title bars, tabs and menus live.
        if (BOX.width > MONITOR->m_size.x)
            target.x = BOX.x + MONITOR->m_size.x * 0.5;
        if (BOX.height > MONITOR->m_size.y)
            target.y = BOX.y + MONITOR->m_size.y * 0.5;
    }
    return target - MONITOR->m_position - MONITOR->m_size * 0.5F;
}

void CScrollOverview::followNavigatorSelection() {
    if (const auto WINDOW = SpatialOverview::Navigator::selectedWindow())
        followCanvasWindow(WINDOW, true, true);
    damage();
}

bool CScrollOverview::openNavigator(const std::string& query) {
    if (!isPersistentCanvas() || closing)
        return false;

    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (canvas && !canvas->isClosing() && canvas->isPersistentCanvas() && !canvas->isCanvasNavigationActive())
            canvas->toggleCanvasNavigation();
    }
    if (!SpatialOverview::Navigator::isOpen())
        return false;

    if (!query.empty()) {
        SpatialOverview::Navigator::clearQuery();
        SpatialOverview::Navigator::appendText(query);
        SpatialOverview::Navigator::rebuildResults(true);
        followNavigatorSelection();
    }
    damage();
    return true;
}

bool CScrollOverview::switchWindow(int direction) {
    namespace Navigator = SpatialOverview::Navigator;
    if (!isPersistentCanvas() || closing || !ScrollOverview::Config::getNavigatorEnabled())
        return false;

    if (!Navigator::isOpen()) {
        if (!altHeld()) {
            // Invoked from a script or a non-Alt binding: nothing will ever
            // be released, so show the recent list and let the user choose.
            if (!openNavigator())
                return false;
            Navigator::state().listRevealed = true;
            Navigator::moveSelection(direction);
            followNavigatorSelection();
            return true;
        }

        Navigator::begin(getOverviewWindowToShow(Desktop::focusState()->window()));
        Navigator::state().switcher     = true;
        Navigator::state().listRevealed = true;
        if (!g_switcherTimer && g_pCompositor)
            g_switcherTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, switcherTimerCallback, nullptr);
        if (g_switcherTimer)
            wl_event_source_timer_update(g_switcherTimer, SWITCHER_REVEAL_MS);
    }

    Navigator::moveSelection(direction);
    if (isCanvasNavigationActive())
        followNavigatorSelection();
    damage();
    return true;
}

void CScrollOverview::revealSwitcher() {
    namespace Navigator = SpatialOverview::Navigator;
    if (!Navigator::isOpen() || !Navigator::state().switcher || isCanvasNavigationActive())
        return;
    openNavigator();
    Navigator::state().listRevealed = true;
    Navigator::touch();
    followNavigatorSelection();
}

void CScrollOverview::finishSwitcher(bool cancel) {
    namespace Navigator = SpatialOverview::Navigator;
    if (g_switcherTimer)
        wl_event_source_timer_update(g_switcherTimer, 0);
    if (!Navigator::isOpen() || !Navigator::state().switcher)
        return;

    Navigator::state().switcher = false;
    const bool VISIBLE = std::ranges::any_of(scrollOverviews(), [](const auto& overview) {
        const auto* canvas = canvasOf(overview);
        return canvas && canvas->isCanvasNavigationActive();
    });

    if (cancel) {
        if (VISIBLE)
            revertAllNavigation();
        else
            Navigator::end();
        return;
    }

    const auto WINDOW = Navigator::selectedWindow();
    if (VISIBLE) {
        landOnWindow(WINDOW ? WINDOW : getOverviewWindowToShow(Desktop::focusState()->window()));
        return;
    }

    Navigator::end();
    if (!shouldShowOverviewWindow(WINDOW))
        return;
    Desktop::windowState()->raise(WINDOW);
    followCanvasWindow(WINDOW, true, true);
    leaveNavigatorPointer();
    Navigator::noteFocus(WINDOW, true);
}

void CScrollOverview::landOnWindow(PHLWINDOW window) {
    const auto MONITOR = pMonitor.lock();
    window             = getOverviewWindowToShow(window);
    if (!isCanvasDesktop() || !MONITOR || closing)
        return;

    if (!shouldShowOverviewWindow(window) || window->m_pinned) {
        if (canvasNavigationActive)
            toggleCanvasNavigation();
        return;
    }

    Desktop::windowState()->raise(window);
    followCanvasWindow(window, true, true);

    canvasNavigationActive = false;
    canvasPinching         = false;
    landingDrag            = false;
    *viewOffset            = canvasCameraOffsetFor(window, 1.F, false);
    *scale                 = 1.F;
    *transitionProgress    = 0.F;
    leaveNavigatorPointer();
    SpatialOverview::Navigator::noteFocus(window, true);

    // Other monitors come back to 100% wherever their cameras are.
    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (canvas && canvas != this && canvas->isCanvasNavigationActive())
            canvas->toggleCanvasNavigation();
    }
    endNavigatorSessionIfIdle();
    ensureCanvasKeyboardFocus(window);
    markBlurDirty();
    noteCanvasLayoutChanged();
    damage();
}

void CScrollOverview::summonWindow(PHLWINDOW window) {
    const auto MONITOR = pMonitor.lock();
    window             = getOverviewWindowToShow(window);
    if (!isCanvasDesktop() || !MONITOR || closing || !shouldShowOverviewWindow(window) || window->m_pinned || !window->layoutTarget())
        return;

    checkpointCanvas();
    if (!window->layoutTarget()->floating() && !setCanvasFloating(window, true))
        return;
    const auto TARGET = window->layoutTarget();
    if (!TARGET)
        return;

    // The window travels to the middle of the view the session started
    // from, and the camera returns there to meet it.
    const Vector2D ORIGIN = hasNavigationReturn ? navigationReturnOffset : viewOffset->goal();
    const auto     CENTER = MONITOR->m_position + ORIGIN + MONITOR->m_size * 0.5F;
    auto           BOX    = TARGET->position();
    BOX.x                 = CENTER.x - BOX.width * 0.5;
    BOX.y                 = CENTER.y - BOX.height * 0.5;
    BOX                   = snapCanvasWindowBox(BOX, MONITOR);
    TARGET->rememberFloatingSize(BOX.size());
    TARGET->setPositionGlobal(BOX);
    TARGET->damageEntire();
    Desktop::windowState()->raise(window);

    size_t workspaceIdx = viewportCurrentWorkspace;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] && images[i]->pWorkspace == window->m_workspace) {
            workspaceIdx = i;
            break;
        }
    }
    selectOverviewWindow(window, workspaceIdx, true);

    canvasNavigationActive = false;
    canvasPinching         = false;
    landingDrag            = false;
    *viewOffset            = ORIGIN;
    *scale                 = 1.F;
    *transitionProgress    = 0.F;
    leaveNavigatorPointer();
    SpatialOverview::Navigator::noteFocus(window, true);
    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (canvas && canvas != this && canvas->isCanvasNavigationActive())
            canvas->toggleCanvasNavigation();
    }
    endNavigatorSessionIfIdle();
    ensureCanvasKeyboardFocus(window);
    redrawAll();
    markBlurDirty();
    noteCanvasLayoutChanged();
    damage();
}

void CScrollOverview::revertAllNavigation() {
    const auto MONITOR = pMonitor.lock();
    const auto RETURN  = getOverviewWindowToShow(SpatialOverview::Navigator::state().returnWindow.lock());

    if (shouldShowOverviewWindow(RETURN) && Desktop::focusState()->window() != RETURN) {
        ++g_canvasInternalFocusDepth;
        auto restoreInternalFocusDepth = Hyprutils::Utils::CScopeGuard([] { --g_canvasInternalFocusDepth; });
        Desktop::focusState()->fullWindowFocus(RETURN, Desktop::FOCUS_REASON_KEYBIND);
        closeOnWindow = RETURN;
        if (MONITOR && Desktop::focusState()->monitor() != MONITOR)
            Desktop::focusState()->rawMonitorFocus(MONITOR);
    }

    for (const auto& overview : scrollOverviews()) {
        auto* canvas = canvasOf(overview);
        if (canvas && canvas->isCanvasNavigationActive())
            canvas->revertNavigation();
    }
    endNavigatorSessionIfIdle();
}

void CScrollOverview::fitAllWindows() {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR || closing)
        return;

    // With a search running, "everything" means everything that matches.
    std::optional<CBox>             bounds;
    std::unordered_set<const void*> visited;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second || !SpatialOverview::Navigator::isMatch(WINDOW))
            continue;
        const auto BOX = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (!bounds)
            bounds = BOX;
        else {
            const double LEFT   = std::min(bounds->x, BOX.x);
            const double TOP    = std::min(bounds->y, BOX.y);
            const double RIGHT  = std::max(bounds->x + bounds->width, BOX.x + BOX.width);
            const double BOTTOM = std::max(bounds->y + bounds->height, BOX.y + BOX.height);
            bounds              = CBox{LEFT, TOP, RIGHT - LEFT, BOTTOM - TOP};
        }
    }
    if (!bounds || bounds->empty())
        return;

    if (!canvasNavigationActive)
        toggleCanvasNavigation();

    const bool  HUD       = showsNavigatorHud();
    const float USABLEH   = MONITOR->m_size.y * (HUD ? 0.74F : 0.9F);
    const float ZOOM      = std::clamp(std::min(sc<float>(MONITOR->m_size.x * 0.9F / bounds->width), sc<float>(USABLEH / bounds->height)),
                                       ScrollOverview::Config::getCanvasMinZoom(), std::min(0.9F, ScrollOverview::Config::getCanvasMaxZoom()));
    auto        target    = bounds->middle();
    if (HUD)
        target.y -= (SpatialOverview::Hud::selectionFocusY(MONITOR->m_size) - MONITOR->m_size.y * 0.5) / ZOOM;
    *viewOffset = target - MONITOR->m_position - MONITOR->m_size * 0.5F;
    *scale      = ZOOM;
    markBlurDirty();
    damage();
}

void CScrollOverview::panCamera(const Vector2D& direction) {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR)
        return;
    const float ZOOM = std::max(scale->goal(), 0.01F);
    *viewOffset      = viewOffset->goal() + Vector2D{direction.x * MONITOR->m_size.x, direction.y * MONITOR->m_size.y} * (0.22F / ZOOM);
    markCanvasCameraActive(MONITOR);
    markBlurDirty();
    damage();
}

void CScrollOverview::zoomCameraBy(float factor) {
    const auto MONITOR = pMonitor.lock();
    if (!isCanvasDesktop() || !MONITOR)
        return;
    const float ZOOM = std::clamp(scale->goal() * factor, ScrollOverview::Config::getCanvasMinZoom(), ScrollOverview::Config::getCanvasMaxZoom());
    *scale           = ZOOM;
    markBlurDirty();
    damage();
}

bool CScrollOverview::nudgeWindow(PHLWINDOW window, const Vector2D& direction, bool checkpoint) {
    const auto MONITOR = pMonitor.lock();
    window             = getOverviewWindowToShow(window);
    const auto TARGET  = window ? window->layoutTarget() : nullptr;
    if (!isCanvasDesktop() || !MONITOR || !shouldShowOverviewWindow(window) || !TARGET || !TARGET->floating())
        return false;
    if (nudgeInPlace(window, direction))
        return true;

    if (checkpoint)
        checkpointCanvas();
    const float STEP = std::max(8, ScrollOverview::Config::getCanvasGridSize());
    auto        BOX  = TARGET->position();
    BOX.translate(direction * STEP);
    TARGET->setPositionGlobal(BOX);
    TARGET->warpPositionSize();
    TARGET->damageEntire();

    // Only chase the window once it starts to leave the view; otherwise the
    // world would appear to slide under a window that stands still.
    auto VIEW = currentCanvasViewportWorld();
    VIEW.expand(-STEP);
    if (!VIEW.containsPoint(BOX.middle()))
        *viewOffset = viewOffset->goal() + direction * STEP;
    markBlurDirty();
    noteCanvasLayoutChanged();
    damage();
    return true;
}

bool CScrollOverview::handleNavigatorKey(const IKeyboard::SKeyEvent& event, uint32_t keysym, uint32_t mods) {
    namespace Navigator = SpatialOverview::Navigator;

    if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return false;
    if (Navigator::isOpen() && Navigator::state().switcher && !isCanvasNavigationActive()) {
        // The switcher has not surfaced yet; only Escape means anything.
        if (keysym != XKB_KEY_Escape)
            return false;
        finishSwitcher(true);
        Navigator::markConsumed(event.keycode);
        return true;
    }
    if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED || !isCanvasNavigationActive() || !Navigator::isOpen() || !ScrollOverview::Config::getNavigatorEnabled())
        return false;
    if (isModifierKeysym(keysym))
        return false;

    Navigator::stopRepeat();
    // Super chords stay with Hyprland, so the canvas toggle, window
    // management and launcher bindings keep working while searching.
    if (mods & HL_MODIFIER_META)
        return false;

    const bool TEXTINPUT = !(mods & (HL_MODIFIER_CTRL | HL_MODIFIER_ALT));
    const auto TEXT      = TEXTINPUT ? navigatorKeyText(event) : std::string{};
    if (!navigatorKeyAction(keysym, mods, TEXT, false)) {
        if (navigatorPassesThrough(keysym, mods) || !isCanvasNavigationActive() || !Navigator::isOpen())
            return false;
        Navigator::markConsumed(event.keycode);
        return true;
    }

    Navigator::markConsumed(event.keycode);

    const bool REPEATS = keysym != XKB_KEY_Escape && keysym != XKB_KEY_Return && keysym != XKB_KEY_KP_Enter && keysym != XKB_KEY_F1 &&
        !((mods & HL_MODIFIER_CTRL) && (keysym == XKB_KEY_a || keysym == XKB_KEY_A || keysym == XKB_KEY_0 || keysym == XKB_KEY_f || keysym == XKB_KEY_F ||
                                         keysym == XKB_KEY_z || keysym == XKB_KEY_Z || keysym == XKB_KEY_y || keysym == XKB_KEY_Y || keysym == XKB_KEY_slash));
    if (REPEATS) {
        const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();
        Navigator::startRepeat({.keycode = event.keycode, .keysym = keysym, .mods = mods, .text = TEXT}, KEYBOARD ? KEYBOARD->m_repeatDelay : 600,
                               KEYBOARD ? KEYBOARD->m_repeatRate : 25, [](const Navigator::SRepeatKey& key) {
                                   // A release can be lost to a panel grabbing focus; never
                                   // keep repeating a key nobody is holding.
                                   auto* canvas = activeCanvasOverview();
                                   if (!keyStillHeld(key.keycode) || sessionLocked() || !canvas ||
                                       !canvas->navigatorKeyAction(key.keysym, key.mods, key.text, true))
                                       Navigator::stopRepeat();
                               });
    }
    return true;
}

bool CScrollOverview::tunerKeyAction(uint32_t keysym, uint32_t mods, const std::string& text) {
    namespace Navigator = SpatialOverview::Navigator;
    auto&      STATE = Navigator::state();
    const bool CTRL  = mods & HL_MODIFIER_CTRL;
    const bool SHIFT = mods & HL_MODIFIER_SHIFT;

    const auto selected = [&]() -> const SpatialOverview::Tuning::SParam* {
        if (STATE.tunerResults.empty())
            return nullptr;
        return &SpatialOverview::Tuning::params()[STATE.tunerResults[std::clamp(STATE.tunerSelected, 0, sc<int>(STATE.tunerResults.size()) - 1)]];
    };
    // Settings reach every output (the lens, the grid, the wallpaper), so
    // every canvas redraws.
    const auto redraw = [&](bool changed) {
        if (changed) {
            for (const auto& overview : scrollOverviews())
                overview->damage();
            // The zoomed-out level is seen best by going there.
            if (const auto* PARAM = selected(); PARAM && std::string_view{PARAM->key} == "canvas:initial_zoom") {
                *scale = sc<float>(SpatialOverview::Tuning::number("canvas:initial_zoom"));
                markBlurDirty();
            }
        }
        damage();
        return true;
    };
    const double STEPS = CTRL ? 10.0 : SHIFT ? 0.1 : 1.0;

    switch (keysym) {
        case XKB_KEY_Up:
        case XKB_KEY_KP_Up:
        case XKB_KEY_ISO_Left_Tab: return redraw(Navigator::tunerMove(-1)) || true;
        case XKB_KEY_Down:
        case XKB_KEY_KP_Down: return redraw(Navigator::tunerMove(1)) || true;
        case XKB_KEY_Tab: return redraw(Navigator::tunerMove(SHIFT ? -1 : 1)) || true;
        case XKB_KEY_Page_Up: return redraw(Navigator::tunerMove(-Navigator::TUNER_ROWS)) || true;
        case XKB_KEY_Page_Down: return redraw(Navigator::tunerMove(Navigator::TUNER_ROWS)) || true;
        case XKB_KEY_Home: return redraw(Navigator::tunerMove(-STATE.tunerSelected)) || true;
        case XKB_KEY_End: return redraw(Navigator::tunerMove(sc<int>(STATE.tunerResults.size()) - 1 - STATE.tunerSelected)) || true;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left: return redraw(Navigator::tunerAdjust(-STEPS));
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right: return redraw(Navigator::tunerAdjust(STEPS));
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete: return redraw(Navigator::tunerRevert());
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter: {
            // Switches flip and choices cycle; on anything else Enter is done.
            const auto* PARAM = selected();
            const bool  BOOL  = PARAM && PARAM->kind == SpatialOverview::Tuning::EKind::BOOL;
            if (PARAM && (BOOL || !PARAM->choices.empty()))
                return redraw(Navigator::tunerAdjust(BOOL && SpatialOverview::Tuning::get(*PARAM) >= 0.5 ? -1.0 : 1.0));
            Navigator::closeTuner();
            return redraw(false);
        }
        case XKB_KEY_Escape:
            if (Navigator::tunerClear())
                return redraw(false);
            Navigator::closeTuner();
            return redraw(false);
        case XKB_KEY_BackSpace:
            if (CTRL)
                return redraw(Navigator::tunerClear()) || true;
            return redraw(Navigator::tunerPop()) || true;
        default: break;
    }
    if (CTRL && (keysym == XKB_KEY_u || keysym == XKB_KEY_U || keysym == XKB_KEY_w || keysym == XKB_KEY_W))
        return redraw(Navigator::tunerClear()) || true;
    if (!CTRL && !text.empty() && std::ranges::none_of(text, [](unsigned char c) { return c < 0x20 || c == 0x7F; }))
        return redraw(Navigator::tunerAppend(text)) || true;
    return false;
}

bool CScrollOverview::navigatorKeyAction(uint32_t keysym, uint32_t mods, const std::string& text, bool repeat) {
    namespace Navigator = SpatialOverview::Navigator;
    if (!isCanvasNavigationActive() || !Navigator::isOpen() || closing)
        return false;

    auto&      STATE = Navigator::state();
    const bool CTRL  = mods & HL_MODIFIER_CTRL;
    const bool SHIFT = mods & HL_MODIFIER_SHIFT;
    const bool ALT   = mods & HL_MODIFIER_ALT;

    const auto queryEdited = [&](bool changed) {
        if (!changed)
            return;
        // Clearing the query keeps whatever is selected; a real query
        // always jumps to its best hit.
        if (Navigator::queryActive()) {
            Navigator::rebuildResults(true);
            followNavigatorSelection();
        } else
            Navigator::rebuildResults(false);
        damage();
    };
    const auto selectionMoved = [&](int delta) {
        if (Navigator::moveSelection(delta))
            followNavigatorSelection();
        damage();
        return true;
    };
    const auto direction = [](uint32_t key) -> std::optional<std::pair<std::string, Vector2D>> {
        switch (key) {
            case XKB_KEY_Left:
            case XKB_KEY_KP_Left: return std::pair<std::string, Vector2D>{"left", {-1, 0}};
            case XKB_KEY_Right:
            case XKB_KEY_KP_Right: return std::pair<std::string, Vector2D>{"right", {1, 0}};
            case XKB_KEY_Up:
            case XKB_KEY_KP_Up: return std::pair<std::string, Vector2D>{"up", {0, -1}};
            case XKB_KEY_Down:
            case XKB_KEY_KP_Down: return std::pair<std::string, Vector2D>{"down", {0, 1}};
            default: return std::nullopt;
        }
    };

    if (keysym == XKB_KEY_Escape && STATE.switcher && !CTRL) {
        finishSwitcher(true);
        return true;
    }

    // Ctrl+, opens the live tuner (the usual settings chord); in it the keys
    // pick and adjust settings instead of windows.
    if (CTRL && !ALT && !STATE.switcher && (keysym == XKB_KEY_comma || keysym == XKB_KEY_less)) {
        if (repeat)
            return true;
        if (STATE.tuner)
            Navigator::closeTuner();
        else
            Navigator::openTuner();
        damage();
        return true;
    }
    if (STATE.tuner)
        return tunerKeyAction(keysym, mods, text);

    // Alt reaches the experiment lab's single-key actions, which plain keys
    // can no longer carry while every letter types into the search.
    if (ALT && !CTRL)
        return handleExperimentKey(keysym, mods & ~HL_MODIFIER_ALT);

    if (const auto DIRECTION = direction(keysym)) {
        const auto FOCUSED = getOverviewWindowToShow(Desktop::focusState()->window());
        if (SHIFT && !CTRL) {
            nudgeWindow(FOCUSED, DIRECTION->second, !repeat);
            return true;
        }
        if (CTRL && !SHIFT) {
            panCamera(DIRECTION->second);
            return true;
        }
        if (CTRL || SHIFT)
            return false;
        if (Navigator::listVisible() && (keysym == XKB_KEY_Up || keysym == XKB_KEY_KP_Up || keysym == XKB_KEY_Down || keysym == XKB_KEY_KP_Down))
            return selectionMoved(DIRECTION->second.y < 0 ? -1 : 1);
        if (moveSelection(DIRECTION->first))
            Navigator::selectWindow(Desktop::focusState()->window());
        damage();
        return true;
    }

    switch (keysym) {
        case XKB_KEY_Escape:
            if (CTRL)
                return false;
            if (STATE.helpOpen) {
                STATE.helpOpen = false;
                Navigator::touch();
                damage();
                return true;
            }
            if (Navigator::clearQuery()) {
                queryEdited(true);
                return true;
            }
            if (STATE.listRevealed) {
                STATE.listRevealed = false;
                Navigator::touch();
                damage();
                return true;
            }
            revertAllNavigation();
            return true;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter: {
            if (CTRL)
                return false;
            if (SpatialOverview::Experiments::on(ECanvasExperiment::Landing) && hasLandingDestination && !Navigator::queryActive() && !STATE.listRevealed) {
                commitLanding();
                return true;
            }
            if (Navigator::queryActive() && STATE.results.empty())
                return true;
            auto WINDOW = Navigator::selectedWindow();
            if (!WINDOW || (!Navigator::queryActive() && !STATE.listRevealed))
                WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
            if (SHIFT)
                summonWindow(WINDOW);
            else
                landOnWindow(WINDOW);
            return true;
        }
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            if (CTRL)
                return false;
            return selectionMoved(SHIFT || keysym == XKB_KEY_ISO_Left_Tab ? -1 : 1);
        case XKB_KEY_F1:
            STATE.helpOpen = !STATE.helpOpen;
            Navigator::touch();
            damage();
            return true;
        case XKB_KEY_BackSpace:
            queryEdited(CTRL ? Navigator::popWord() : Navigator::popChar());
            return true;
        default: break;
    }

    if (CTRL) {
        switch (keysym) {
            case XKB_KEY_u:
            case XKB_KEY_U: queryEdited(Navigator::clearQuery()); return true;
            case XKB_KEY_w:
            case XKB_KEY_W: queryEdited(Navigator::popWord()); return true;
            case XKB_KEY_n:
            case XKB_KEY_N:
            case XKB_KEY_j:
            case XKB_KEY_J: return selectionMoved(1);
            case XKB_KEY_p:
            case XKB_KEY_P:
            case XKB_KEY_k:
            case XKB_KEY_K: return selectionMoved(-1);
            case XKB_KEY_plus:
            case XKB_KEY_equal:
            case XKB_KEY_KP_Add: zoomCameraBy(1.25F); return true;
            case XKB_KEY_minus:
            case XKB_KEY_underscore:
            case XKB_KEY_KP_Subtract: zoomCameraBy(0.8F); return true;
            case XKB_KEY_0:
            case XKB_KEY_KP_0:
            case XKB_KEY_KP_Insert: fitAllWindows(); return true;
            case XKB_KEY_f:
            case XKB_KEY_F: flightDeckAction("frame"); return true;
            case XKB_KEY_a:
            case XKB_KEY_A:
                if (arrangeCanvasWindows())
                    Navigator::showNotice(std::format("Tidied {} windows  ·  ⌃Z undoes", Navigator::candidateCount()));
                damage();
                return true;
            case XKB_KEY_z:
            case XKB_KEY_Z:
                if (SHIFT ? canvasRedo() : flightDeckAction("undo"))
                    Navigator::showNotice(SHIFT ? "Redone" : "Undone  ·  ⌃⇧Z redoes");
                else
                    Navigator::showNotice(SHIFT ? "Nothing to redo" : "Nothing to undo");
                damage();
                return true;
            case XKB_KEY_y:
            case XKB_KEY_Y:
                Navigator::showNotice(canvasRedo() ? "Redone" : "Nothing to redo");
                damage();
                return true;
            case XKB_KEY_slash:
            case XKB_KEY_question:
                STATE.helpOpen = !STATE.helpOpen;
                Navigator::touch();
                damage();
                return true;
            default: break;
        }
        if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
            const size_t INDEX = sc<size_t>(STATE.listOffset) + (keysym - XKB_KEY_1);
            if (INDEX < STATE.results.size())
                landOnWindow(STATE.results[INDEX].window.lock());
            return true;
        }
        return false;
    }

    if (!text.empty()) {
        queryEdited(Navigator::appendText(text));
        return true;
    }
    return false;
}

bool CScrollOverview::cycleAltTab(int direction) {
    if (!isCanvasDesktop() || direction == 0)
        return false;

    if (!canvasNavigationActive)
        toggleCanvasNavigation();

    std::vector<PHLWINDOW> windows;
    std::unordered_set<const void*> visited;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;
        windows.push_back(WINDOW);
    }
    if (windows.empty())
        return false;

    std::ranges::sort(windows, [](const PHLWINDOW& a, const PHLWINDOW& b) {
        const auto A = a->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto B = b->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (std::abs(A.x - B.x) > 8.F)
            return A.x < B.x;
        return A.y < B.y;
    });

    const auto CURRENT = getOverviewWindowToShow(Desktop::focusState()->window());
    const auto FOUND = std::ranges::find(windows, CURRENT);
    const int INDEX = FOUND == windows.end() ? 0 : sc<int>(FOUND - windows.begin());
    const int COUNT = sc<int>(windows.size());
    const int NEXT = (INDEX + (direction > 0 ? 1 : -1) + COUNT) % COUNT;
    return followCanvasWindow(windows[NEXT], true, true);
}

bool CScrollOverview::jumpCanvasMinimap(const Vector2D& rawLocal) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasNavigationActive())
        return false;

    const float ZOOM = std::max(scale->value(), 0.01F);
    const auto CENTERLOGICAL = MONITOR->m_size / 2.F;
    const CBox VIEWPORTWORLD{MONITOR->m_position + viewOffset->value() + CENTERLOGICAL - CENTERLOGICAL * (1.F / ZOOM), MONITOR->m_size * (1.F / ZOOM)};
    std::optional<CBox> worldBounds;
    const auto includeWorldBox = [&worldBounds](const CBox& box) {
        if (box.empty())
            return;
        if (!worldBounds) {
            worldBounds = box;
            return;
        }
        const float LEFT = std::min(sc<float>(worldBounds->x), sc<float>(box.x));
        const float TOP = std::min(sc<float>(worldBounds->y), sc<float>(box.y));
        const float RIGHT = std::max(sc<float>(worldBounds->x + worldBounds->width), sc<float>(box.x + box.width));
        const float BOTTOM = std::max(sc<float>(worldBounds->y + worldBounds->height), sc<float>(box.y + box.height));
        *worldBounds = CBox{LEFT, TOP, RIGHT - LEFT, BOTTOM - TOP};
    };
    includeWorldBox(VIEWPORTWORLD);
    std::unordered_set<const void*> visited;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !visited.emplace(WINDOW.get()).second)
            continue;
        includeWorldBox(WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT));
    }
    if (!worldBounds)
        return false;

    worldBounds->expand(std::max(80.F, sc<float>(std::max(worldBounds->width, worldBounds->height)) * 0.06F));
    const float MONITORSCALE = std::max(MONITOR->m_scale, 0.01F);
    const float WIDTH = ScrollOverview::Config::getCanvasMinimapWidth() * MONITORSCALE;
    const float HEIGHT = ScrollOverview::Config::getCanvasMinimapHeight() * MONITORSCALE;
    const float MARGIN = ScrollOverview::Config::getCanvasMinimapMargin() * MONITORSCALE;
    const float INSET = 12.F * MONITORSCALE;
    const auto FULLSIZE = MONITOR->m_size * MONITORSCALE;
    const CBox PANEL{FULLSIZE.x - MARGIN - WIDTH, FULLSIZE.y - MARGIN - HEIGHT, WIDTH, HEIGHT};
    if (!PANEL.containsPoint(rawLocal) || canvasArrangeButtonBox().containsPoint(rawLocal))
        return false;

    const CBox CONTENT{PANEL.x + INSET, PANEL.y + INSET, std::max(1.F, sc<float>(PANEL.width - INSET * 2.F)), std::max(1.F, sc<float>(PANEL.height - INSET * 2.F))};
    const float FIT = std::min(sc<float>(CONTENT.width / std::max(worldBounds->width, 1.0)), sc<float>(CONTENT.height / std::max(worldBounds->height, 1.0)));
    const Vector2D DRAWORIGIN{CONTENT.x + (CONTENT.width - worldBounds->width * FIT) / 2.F, CONTENT.y + (CONTENT.height - worldBounds->height * FIT) / 2.F};
    const Vector2D WORLD = worldBounds->pos() + (rawLocal - DRAWORIGIN) * (1.F / std::max(FIT, 0.0001F));
    *viewOffset = WORLD - MONITOR->m_position - MONITOR->m_size / 2.F;
    markBlurDirty();
    damage();
    return true;
}

bool CScrollOverview::handleExperimentClick(uint32_t button, uint32_t state, const Vector2D& rawLocal) {
    const bool LEFT = button == (ScrollOverview::Config::getLeftHanded() ? BTN_RIGHT : BTN_LEFT);
    if (!LEFT || !isCanvasDesktop())
        return false;

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        const bool WAS = landingDrag;
        landingDrag = false;
        return WAS;
    }
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || !isCanvasNavigationActive())
        return false;

    if (jumpCanvasMinimap(rawLocal)) {
        g_pointerGrabOverview = this;
        return true;
    }

    if (!SpatialOverview::Experiments::on(ECanvasExperiment::Landing) || !hasLandingDestination)
        return false;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;
    const auto FRAME = getOverviewGlobalBox(landingDestinationWorld, MONITOR, scale->value(), viewOffset->value(), Vector2D{}, layout, false);
    if (!FRAME.containsPoint(lastMousePosLocal))
        return false;

    const float EDGE = 18.F * MONITOR->m_scale;
    const bool NEAR_EDGE = lastMousePosLocal.x < FRAME.x + EDGE || lastMousePosLocal.y < FRAME.y + EDGE ||
        lastMousePosLocal.x > FRAME.x + FRAME.width - EDGE || lastMousePosLocal.y > FRAME.y + FRAME.height - EDGE;
    if (!NEAR_EDGE && canvasDesktopWindowAtPoint(lastMousePosLocal))
        return false;

    landingDrag = true;
    landingDragLast = lastMousePosLocal;
    g_pointerGrabOverview = this;
    return true;
}

void CScrollOverview::updateExperimentDrag() {
    if (!landingDrag)
        return;
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;
    const float FACTOR = std::max(0.01F, scale->value() * MONITOR->m_scale);
    const auto DELTA = (lastMousePosLocal - landingDragLast) * (1.F / FACTOR);
    landingDragLast = lastMousePosLocal;
    landingDestinationWorld.translate(DELTA);
    damage();
}

static void drawCanvasText(const std::string& text, const CBox& box, const CHyprColor& color, int points) {
    if (text.empty() || box.width < 4.F || box.height < 4.F || color.a <= 0.01F)
        return;
    auto texture = g_pHyprRenderer->renderText(text, color, std::max(points, 1), false, "", std::max(1, sc<int>(std::round(box.width))));
    if (!texture || !texture->ok())
        return;
    CBox textBox{box.pos(), texture->m_size};
    if (textBox.width > box.width)
        textBox.width = box.width;
    textBox.round();
    CTexPassElement::SRenderData data;
    data.tex = texture;
    data.box = textBox;
    data.a = 1.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
}

void CScrollOverview::renderExperimentChrome(PHLMONITOR monitor) {
    if (!monitor || !isCanvasDesktop())
        return;

    const float SCALE = std::max(monitor->m_scale, 0.01F);

    if (SpatialOverview::Experiments::on(ECanvasExperiment::Landing) && isCanvasNavigationActive() && hasLandingDestination) {
        const auto FRAME = getOverviewGlobalBox(landingDestinationWorld, monitor, scale->value(), viewOffset->value(), Vector2D{}, layout, false).round();
        if (!FRAME.empty()) {
            const float ARM = 22.F * SCALE;
            const float THICK = std::max(2.F, 2.F * SCALE);
            CHyprColor color{0.55F, 0.93F, 1.F, 0.95F * overviewProgress()};
            const auto arm = [&](CBox box) {
                CRectPassElement::SRectData mark;
                mark.box = box.round();
                mark.color = color;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(mark));
            };
            arm(CBox{FRAME.x, FRAME.y, ARM, THICK});
            arm(CBox{FRAME.x, FRAME.y, THICK, ARM});
            arm(CBox{FRAME.x + FRAME.width - ARM, FRAME.y, ARM, THICK});
            arm(CBox{FRAME.x + FRAME.width - THICK, FRAME.y, THICK, ARM});
            arm(CBox{FRAME.x, FRAME.y + FRAME.height - THICK, ARM, THICK});
            arm(CBox{FRAME.x, FRAME.y + FRAME.height - ARM, THICK, ARM});
            arm(CBox{FRAME.x + FRAME.width - ARM, FRAME.y + FRAME.height - THICK, ARM, THICK});
            arm(CBox{FRAME.x + FRAME.width - THICK, FRAME.y + FRAME.height - ARM, THICK, ARM});
            drawCanvasText("100%", CBox{FRAME.x + 8.F * SCALE, FRAME.y + 6.F * SCALE, 80.F * SCALE, 16.F * SCALE}, color, sc<int>(std::round(12.F * SCALE)));
        }
    }

    if (!SpatialOverview::Experiments::on(ECanvasExperiment::Areas) || !isCanvasNavigationActive())
        return;

    std::unordered_map<WORKSPACEID, CBox> areas;
    std::unordered_map<WORKSPACEID, std::string> names;
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
            continue;
        const auto BOX = canvasDesktopWindowBox(WINDOW);
        if (BOX.empty())
            continue;
        const auto ID = WINDOW->m_workspace->m_id;
        names[ID] = WINDOW->m_workspace->m_name.empty() ? std::to_string(ID) : WINDOW->m_workspace->m_name;
        if (!areas.contains(ID))
            areas[ID] = BOX;
        else {
            const float LEFT = std::min(sc<float>(areas[ID].x), sc<float>(BOX.x));
            const float TOP = std::min(sc<float>(areas[ID].y), sc<float>(BOX.y));
            const float RIGHT = std::max(sc<float>(areas[ID].x + areas[ID].width), sc<float>(BOX.x + BOX.width));
            const float BOTTOM = std::max(sc<float>(areas[ID].y + areas[ID].height), sc<float>(BOX.y + BOX.height));
            areas[ID] = CBox{LEFT, TOP, RIGHT - LEFT, BOTTOM - TOP};
        }
    }
    for (const auto& [id, box] : areas) {
        CBorderPassElement::SBorderData border;
        border.box = box;
        border.grad1 = Config::CGradientValueData{CHyprColor{0.64F, 0.84F, 1.F, 1.F}};
        border.a = 0.45F * overviewProgress();
        border.borderSize = std::max(1, sc<int>(std::round(SCALE)));
        border.round = sc<int>(std::round(10.F * SCALE));
        border.outerRound = border.round;
        border.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
        drawCanvasText(names[id], CBox{box.x, std::max(0.F, sc<float>(box.y - 18.F * SCALE)), 160.F * SCALE, 16.F * SCALE},
                       CHyprColor{0.7F, 0.88F, 1.F, 0.9F * overviewProgress()}, sc<int>(std::round(12.F * SCALE)));
    }
}

bool CScrollOverview::handleExperimentKey(uint32_t keysym, uint32_t mods) {
    using SpatialOverview::Experiments::on;
    const bool NAV = isCanvasNavigationActive();
    const bool PLAIN = (mods & ~(HL_MODIFIER_SHIFT)) == 0;

    if (!NAV)
        return false;

    if (on(ECanvasExperiment::Landing) && PLAIN && (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter)) {
        commitLanding();
        return true;
    }
    if ((on(ECanvasExperiment::Landing) || on(ECanvasExperiment::AltTab)) && PLAIN && keysym == XKB_KEY_Escape) {
        revertNavigation();
        return true;
    }
    if (on(ECanvasExperiment::Landing) && PLAIN && (keysym == XKB_KEY_z || keysym == XKB_KEY_Z))
        return flightDeckAction("undo");
    if (on(ECanvasExperiment::Landing) && PLAIN && (keysym == XKB_KEY_m || keysym == XKB_KEY_M)) {
        const auto WINDOW = Desktop::focusState()->window();
        if (!shouldShowOverviewWindow(WINDOW))
            return false;
        requestFlightDeckNative(WINDOW, Fullscreen::FSMODE_MAXIMIZED);
        return true;
    }
    if (on(ECanvasExperiment::AltTab) && PLAIN && (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter)) {
        if (on(ECanvasExperiment::Landing))
            commitLanding();
        else {
            canvasNavigationActive = false;
            canvasPinching = false;
            const auto MONITOR = pMonitor.lock();
            if (MONITOR)
                zoomCanvasAt(CBox{{}, MONITOR->m_size * MONITOR->m_scale}.middle(), 1.F, true);
            *transitionProgress = 0.F;
            damage();
        }
        return true;
    }

    if (!on(ECanvasExperiment::Areas) || !PLAIN)
        return false;

    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9)
        return flightDeckAction("area " + std::to_string(keysym - XKB_KEY_1 + 1));
    if (keysym != XKB_KEY_w && keysym != XKB_KEY_W)
        return false;

    const auto WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
        return false;
    const auto WORKSPACE = WINDOW->m_workspace;
    checkpointCanvas();
    for (const auto& candidateRef : Desktop::windowState()->windows()) {
        const auto CANDIDATE = getOverviewWindowToShow(candidateRef);
        if (!shouldShowOverviewWindow(CANDIDATE) || CANDIDATE->m_workspace != WORKSPACE || !CANDIDATE->layoutTarget() || !CANDIDATE->layoutTarget()->floating())
            continue;
        const auto UNTILED = setCanvasFloating(CANDIDATE, false);
        if (!UNTILED)
            continue;
    }
    followCanvasWindow(WINDOW, true, false);
    canvasNavigationActive = false;
    *scale = 1.F;
    *transitionProgress = 0.F;
    noteCanvasLayoutChanged();
    damage();
    return true;
}

void CScrollOverview::updateNavigatorHover() {
    if (!navigatorOwnsPointer()) {
        if (navigatorHoverWindow) {
            navigatorHoverWindow.reset();
            damage();
        }
        setCanvasCursor(isCanvasDesktop() ? "" : "left_ptr");
        return;
    }

    const auto MONITOR = pMonitor.lock();
    const auto RAW     = MONITOR ? (g_pInputManager->getMouseCoordsInternal() - MONITOR->m_position) * MONITOR->m_scale : Vector2D{};
    const bool OVERHUD = showsNavigatorHud() && SpatialOverview::Hud::paletteHit(RAW) != SpatialOverview::Hud::PALETTE_MISS;
    const auto WINDOW  = OVERHUD ? PHLWINDOW{} : canvasDesktopWindowAtPoint(lastMousePosLocal);
    if (WINDOW != navigatorHoverWindow.lock()) {
        navigatorHoverWindow = WINDOW;
        damage();
    }

    if (dragActiveWindow || scrollingPanPointerDown)
        setCanvasCursor("grabbing");
    else if (OVERHUD && SpatialOverview::Hud::paletteHit(RAW) >= 0)
        setCanvasCursor("pointer");
    else if (WINDOW)
        setCanvasCursor("pointer");
    else
        setCanvasCursor("left_ptr");
}

void CScrollOverview::leaveNavigatorPointer() {
    hoverFocusSettling   = true;
    hoverFocusSettleSeen = false;
    hoverFocusSettleWindow.reset();
    navigatorHoverWindow.reset();
    setCanvasCursor(isCanvasDesktop() ? "" : "left_ptr");
}

// Empty: no override, the app under the pointer picks the cursor (a game's
// own, a text beam). The override is global, as is what the canvases last
// set (g_canvasCursorShape), so the canvas only sets one while it is using
// the pointer itself.
void CScrollOverview::setCanvasCursor(const std::string& shape) {
    if (shape == g_canvasCursorShape)
        return;
    g_canvasCursorShape = shape;
    if (shape.empty())
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
    else
        Pointer::Cursor::overrideController->setOverride(shape, Pointer::Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
}

void CScrollOverview::renderNavigatorWindowOverlay(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox) {
    namespace Navigator = SpatialOverview::Navigator;
    if (!monitor || !window || windowBox.empty() || !isCanvasNavigationActive() || !Navigator::isOpen())
        return;

    const float FADE = overviewProgress();
    if (FADE <= 0.01F)
        return;

    const float SCALE    = std::max(monitor->m_scale, 0.01F);
    const int   ROUNDING = std::max(0, sc<int>(std::round(ScrollOverview::Config::getValue<int>("decoration:rounding") * SCALE * scale->value())));
    const auto& THEME    = SpatialOverview::Hud::theme();
    const bool  MATCH    = Navigator::isMatch(window);
    const bool  FOCUSED  = getOverviewWindowToShow(Desktop::focusState()->window()) == window;
    const bool  HOVERED  = navigatorHoverWindow.lock() == window;

    if (!MATCH) {
        CRectPassElement::SRectData dim;
        dim.box           = windowBox.copy().round();
        dim.color         = CHyprColor{0.01F, 0.015F, 0.02F, ScrollOverview::Config::getNavigatorDimUnmatched() * FADE};
        dim.round         = ROUNDING;
        dim.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(dim));
    } else if (Navigator::queryActive() && !FOCUSED) {
        CBorderPassElement::SBorderData border;
        border.box           = windowBox.copy().expand(1.F * SCALE).round();
        border.grad1         = Config::CGradientValueData{THEME.text};
        border.a             = 0.38F * FADE;
        border.borderSize    = std::max(1, sc<int>(std::round(1.25F * SCALE)));
        border.round         = ROUNDING + border.borderSize;
        border.outerRound    = border.round;
        border.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }

    if (HOVERED && !FOCUSED) {
        CBorderPassElement::SBorderData border;
        border.box           = windowBox.copy().expand(2.F * SCALE).round();
        border.grad1         = Config::CGradientValueData{CHyprColor{1.F, 1.F, 1.F, 1.F}};
        border.a             = 0.42F * FADE;
        border.borderSize    = std::max(1, sc<int>(std::round(1.5F * SCALE)));
        border.round         = ROUNDING + border.borderSize * 2;
        border.outerRound    = border.round;
        border.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }

    if (!ScrollOverview::Config::getNavigatorLabels())
        return;

    const auto LABEL = SpatialOverview::Hud::label(window, FOCUSED && MATCH, SCALE, sc<float>(windowBox.width) - 16.F * SCALE);
    if (!LABEL.texture || windowBox.height < LABEL.size.y + 16.F * SCALE)
        return;

    CTexPassElement::SRenderData data;
    data.tex = LABEL.texture;
    data.box = CBox{windowBox.x + 8.F * SCALE, windowBox.y + 8.F * SCALE, LABEL.size.x, LABEL.size.y}.round();
    data.a   = FADE * (MATCH ? 1.F : 0.4F);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
}

void CScrollOverview::renderNavigatorReticle(PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!monitor || !isCanvasNavigationActive() || !SpatialOverview::Navigator::isOpen())
        return;

    const float FADE   = overviewProgress();
    const auto  WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    if (FADE <= 0.01F || !shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || !SpatialOverview::Navigator::isMatch(WINDOW))
        return;

    const auto BOX = canvasDesktopWindowBox(WINDOW);
    if (BOX.empty() || !overviewBoxIntersectsMonitor(BOX.copy().expand(16.F), monitor))
        return;

    // A crisp accent outline standing a few pixels off the window, like the
    // selected card in the palette.
    const float SCALE    = std::max(monitor->m_scale, 0.01F);
    const int   ROUNDING = std::max(0, sc<int>(std::round(ScrollOverview::Config::getValue<int>("decoration:rounding") * SCALE * scale->value())));
    const float GAP      = std::round(4.F * SCALE);

    CBorderPassElement::SBorderData border;
    border.box           = BOX.copy().expand(GAP).round();
    border.grad1         = Config::CGradientValueData{SpatialOverview::Hud::theme().accent};
    border.a             = FADE;
    border.borderSize    = std::max(2, sc<int>(std::round(2.F * SCALE)));
    border.round         = ROUNDING + sc<int>(GAP);
    border.outerRound    = border.round + border.borderSize;
    border.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
}

void CScrollOverview::renderNavigatorHud(PHLMONITOR monitor) {
    if (!monitor || !showsNavigatorHud())
        return;

    const float PROGRESS = overviewProgress();
    if (PROGRESS <= 0.01F)
        return;

    const Vector2D ORIGINOFFSET = hasNavigationReturn ? navigationReturnOffset : viewOffset->goal();
    const SpatialOverview::Hud::SPaletteView VIEW{
        .monitorSize = monitor->m_size,
        .scale       = monitor->m_scale,
        .zoom        = scale->goal(),
        .origin      = monitor->m_position + ORIGINOFFSET + monitor->m_size * 0.5F,
        .monitorName = monitor->m_name,
        .experiment  = SpatialOverview::Experiments::current() == ECanvasExperiment::Baseline ? std::string{} : SpatialOverview::Experiments::title(),
    };

    SP<Render::ITexture> texture;
    CBox                 box;
    if (!SpatialOverview::Hud::palette(VIEW, texture, box))
        return;
    SpatialOverview::Hud::SChrome chrome;
    SpatialOverview::Hud::chrome(VIEW, chrome);

    // Ease in with the zoom and settle downward into place.
    const float EASE = PROGRESS * PROGRESS * (3.F - 2.F * PROGRESS);
    box.y -= (1.F - EASE) * 18.F * monitor->m_scale;

    std::vector<SpatialOverview::BarrelShader::SHudRegion> regions;
    for (const auto& region : chrome.regions)
        regions.push_back({.screen = {sc<float>(region.screen.x), sc<float>(region.screen.y), sc<float>(region.screen.width), sc<float>(region.screen.height)},
                           .uv     = region.uv});
    if (SpatialOverview::BarrelShader::updateHud(texture, {sc<float>(box.x), sc<float>(box.y), sc<float>(box.width), sc<float>(box.height)}, EASE, chrome.texture,
                                                 regions))
        return;

    // Without the lens shader, draw the same layers into the frame.
    CTexPassElement::SRenderData data;
    data.tex = texture;
    data.box = box.round();
    data.a   = EASE;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    if (!chrome.texture)
        return;

    OverviewRender::flushPass(monitor);
    auto&      RENDERDATA = g_pHyprRenderer->m_renderData;
    const auto PREVIOUSTL = RENDERDATA.primarySurfaceUVTopLeft;
    const auto PREVIOUSBR = RENDERDATA.primarySurfaceUVBottomRight;
    const CRegion DAMAGE{CBox{{}, monitor->m_transformedSize}};
    for (const auto& region : chrome.regions) {
        RENDERDATA.primarySurfaceUVTopLeft     = Vector2D{region.uv[0], region.uv[1]};
        RENDERDATA.primarySurfaceUVBottomRight = Vector2D{region.uv[0] + region.uv[2], region.uv[1] + region.uv[3]};
        g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = chrome.texture, .box = region.screen, .a = EASE, .damage = DAMAGE, .allowCustomUV = true}, DAMAGE);
    }
    RENDERDATA.primarySurfaceUVTopLeft     = PREVIOUSTL;
    RENDERDATA.primarySurfaceUVBottomRight = PREVIOUSBR;
}

static std::vector<SCanvasSavedWindow> captureCanvasSnapshot() {
    std::vector<SCanvasSavedWindow> snapshot;
    for (const auto& w : Desktop::windowState()->windows())
        if (shouldShowOverviewWindow(w) && w->layoutTarget()) snapshot.push_back(saveCanvasWindow(w));
    return snapshot;
}

static void applyCanvasSnapshot(const std::vector<SCanvasSavedWindow>& snapshot) {
    g_canvasRestoring = true;
    for (const auto& saved : snapshot) {
        const auto w = saved.window.lock();
        if (!validMapped(w) || !w->layoutTarget()) continue;
        if (const auto ws = saved.workspace.lock(); ws) {
            moveCanvasWindowToWorkspace(w, ws);
            if (g_canvasNativeLayout.contains(w->m_stableID)) g_canvasNativeLayout.at(w->m_stableID).workspace = ws;
        }
        const auto TARGET = w->layoutTarget(); // moving workspaces can replace it
        if (!TARGET) continue;
        TARGET->rememberFloatingSize(saved.box.size());
        TARGET->setPositionGlobal(saved.box); TARGET->warpPositionSize();
    }
    g_canvasRestoring = false;
}

void CScrollOverview::checkpointCanvas() {
    if (!isCanvasDesktop() || g_canvasRestoring) return;
    if (g_canvasUndo.size() >= 30) g_canvasUndo.erase(g_canvasUndo.begin());
    g_canvasUndo.push_back(captureCanvasSnapshot());
    g_canvasRedo.clear();
}

bool CScrollOverview::canvasRedo() {
    if (!isCanvasDesktop() || closing || g_canvasRedo.empty()) return false;
    auto snapshot = std::move(g_canvasRedo.back()); g_canvasRedo.pop_back();
    g_canvasUndo.push_back(captureCanvasSnapshot());
    applyCanvasSnapshot(snapshot);
    redrawAll(); noteCanvasLayoutChanged(); damage(); return true;
}

bool CScrollOverview::flightDeckAction(const std::string& action) {
    if (!isCanvasDesktop() || closing || !pMonitor) return false;
    const auto m = pMonitor.lock();
    if (action == "back") {
        if (SpatialOverview::Experiments::on(ECanvasExperiment::Landing) || SpatialOverview::Experiments::on(ECanvasExperiment::AltTab)) {
            revertNavigation();
            return true;
        }
        if (hasNavigationReturn) viewOffset->setValueAndWarp(navigationReturnOffset);
        if (canvasNavigationActive) toggleCanvasNavigation();
        damage(); return true;
    }
    if (action == "land") {
        if (SpatialOverview::Experiments::on(ECanvasExperiment::Landing)) {
            commitLanding();
            return true;
        }
        if (canvasNavigationActive) toggleCanvasNavigation();
        damage(); return true;
    }
    if (action == "frame") {
        const auto w = getOverviewWindowToShow(Desktop::focusState()->window());
        if (!shouldShowOverviewWindow(w)) return false;
        if (!canvasNavigationActive) toggleCanvasNavigation();
        const auto size = w->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT).size();
        const float usable = showsNavigatorHud() ? 0.72F : 0.86F;
        const float zoom = std::min(m->m_size.x * 0.86 / std::max(1.0, size.x), m->m_size.y * usable / std::max(1.0, size.y));
        *scale = std::clamp(zoom, ScrollOverview::Config::getCanvasMinZoom(), ScrollOverview::Config::getCanvasMaxZoom());
        followCanvasWindow(w, true, true);
        return true;
    }
    if (action == "undo") {
        if (g_canvasUndo.empty()) return false;
        auto snapshot = std::move(g_canvasUndo.back()); g_canvasUndo.pop_back();
        g_canvasRedo.push_back(captureCanvasSnapshot());
        applyCanvasSnapshot(snapshot);
        redrawAll(); noteCanvasLayoutChanged(); damage(); return true;
    }
    if (action == "redo")
        return canvasRedo();
    if (action == "fit") {
        fitAllWindows();
        return true;
    }
    if (action == "summon")
        return canvasNavigationActive ? (summonWindow(Desktop::focusState()->window()), true) : false;
    if (action == "search" || action.starts_with("search "))
        return openNavigator(action.size() > 7 ? action.substr(7) : std::string{});
    if (action == "tune") {
        if (!SpatialOverview::Navigator::isOpen() && !openNavigator(std::string{}))
            return false;
        if (!SpatialOverview::Navigator::state().tuner)
            SpatialOverview::Navigator::openTuner();
        damage();
        return true;
    }
    if (action == "zoom in" || action == "zoom out") {
        if (!canvasNavigationActive) toggleCanvasNavigation();
        zoomCameraBy(action == "zoom in" ? 1.25F : 0.8F);
        return true;
    }
    if (action.starts_with("pan ") || action.starts_with("nudge ")) {
        const auto DIRECTION = action.substr(action.find(' ') + 1);
        const Vector2D VECTOR = DIRECTION == "left" ? Vector2D{-1, 0} : DIRECTION == "right" ? Vector2D{1, 0} : DIRECTION == "up" ? Vector2D{0, -1} : Vector2D{0, 1};
        if (action.starts_with("pan ")) {
            panCamera(VECTOR);
            return true;
        }
        return nudgeWindow(Desktop::focusState()->window(), VECTOR, true);
    }
    if (action.starts_with("area ")) {
        int id=0; try { id=std::stoi(action.substr(5)); } catch (...) { return false; }
        const auto ws = State::workspaceState()->query().id(id).run();
        if (!ws || ws->m_isSpecialWorkspace) return false;
        for (const auto& w : Desktop::windowState()->windows())
            if (shouldShowOverviewWindow(w) && w->m_workspace==ws) return followCanvasWindow(w,true,true);
        return false;
    }
    if (action.starts_with("go ") || action.starts_with("send ") || action.starts_with("assign ") || action == "menu" || action.starts_with("menu "))
        return canvasPlaceAction(action);
    return false;
}

// ---- Places: the workspace keys on the canvas ---------------------------------------
// SUPER + 1…0 go to a place on the canvas, SHIFT + SUPER + N sends the focused
// window there and follows it (SHIFT + ALT + SUPER + N: without following),
// SUPER + TAB / SHIFT + SUPER + TAB step through the places of the screen you
// are on, CTRL + SUPER + TAB goes back to the place before. On linked screens
// a place is a view of the whole desk (linkedPlaceAction, at the end). With
// the screens on their own, a place is one screen's worth of canvas and
// belongs to one screen: the one it was first
// visited from, unless canvas:place_monitors names one, until SHIFT + ALT +
// SUPER + arrows move it (and its windows) to another screen. Where places
// are is remembered across restarts. Place N is column N of the canvas, a
// desk-width and more from the next, on the row of its screen, unless it was
// made somewhere else by right-clicking the canvas (placeClick). With
// canvas:tile_places the windows in a place tile to fill its screen, and
// each place remembers which window had focus there.
struct SCanvasPlace {
    PHLWINDOWREF focus;
};
static std::unordered_map<int, SCanvasPlace>                   g_places;
static int                                                     g_previousPlace = 0;
static std::unordered_map<int, std::vector<PHLWINDOWREF>>      g_placeOrder;
static std::unordered_map<const Desktop::View::CWindow*, CBox> g_placeTiled; // where tiling last put each window
static std::unordered_set<const Desktop::View::CWindow*>       g_placeOpened; // new, so they tile last rather than where they were dropped
static std::unordered_set<const Desktop::View::CWindow*>       g_placeFloating; // resized or SUPER + T'd out of the tiling, in a tiled place
static int                                                     g_placeHeldButtons = 0;
static bool                                                    g_placeReleased    = false;

static void notePlaceWindow(const PHLWINDOW& window, bool opened) {
    if (!window)
        return;
    if (opened)
        g_placeOpened.emplace(window.get());
    else {
        g_placeOpened.erase(window.get());
        g_placeTiled.erase(window.get());
        g_placeFloating.erase(window.get());
    }
}

void notePlacePointerButton(bool pressed) {
    g_placeHeldButtons = std::max(0, g_placeHeldButtons + (pressed ? 1 : -1));
    if (!pressed)
        g_placeReleased = true;
}

// "1-5:DP-1 6:HDMI-A-1", separated by spaces or commas.
static std::map<int, std::string> parsePlaceMonitors(std::string text) {
    std::map<int, std::string> names;
    std::ranges::replace(text, ',', ' ');
    std::istringstream stream{text};
    std::string        entry;
    while (stream >> entry) {
        const auto COLON = entry.find(':');
        if (COLON == std::string::npos || COLON + 1 >= entry.size())
            continue;
        const auto RANGE = entry.substr(0, COLON);
        const auto DASH  = RANGE.find('-');
        int        first = 0, last = 0;
        try {
            first = std::stoi(RANGE.substr(0, DASH));
            last  = DASH == std::string::npos ? first : std::stoi(RANGE.substr(DASH + 1));
        } catch (...) { continue; }
        for (int place = std::max(1, first); place <= std::min(10, last); ++place)
            names[place] = entry.substr(COLON + 1);
    }
    return names;
}

static std::filesystem::path placeMonitorsPath() {
    std::filesystem::path base;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        base = state;
    else
        base = std::filesystem::path{std::getenv("HOME") ? std::getenv("HOME") : "/tmp"} / ".local" / "state";
    return base / "spatial-overview" / "place-monitors";
}

// Where places were put while running; wins over canvas:place_monitors. A
// line per place, "N:SCREEN", or "N:SCREEN@X,Y" for one made somewhere of its
// own by right-clicking the canvas (X,Y: its camera).
struct SPlaceState {
    std::map<int, std::string> monitors;
    std::map<int, Vector2D>    cameras;
    std::map<int, bool>        tiling; // " tiled" / " free" at the end of the line: wins over canvas:tile_places
    std::map<int, std::string> names;  // " #name", last on the line
};

static SPlaceState& placeState() {
    static std::optional<SPlaceState> state;
    if (!state) {
        state.emplace();
        std::ifstream in{placeMonitorsPath()};
        std::string   line;
        while (std::getline(in, line)) {
            std::string name;
            if (const auto HASH = line.find(" #"); HASH != std::string::npos) {
                name = line.substr(HASH + 2);
                line.resize(HASH);
            }
            const auto AT = std::min(line.find('@'), line.find(' '));
            for (const auto& [place, name] : parsePlaceMonitors(line.substr(0, AT))) {
                state->monitors[place] = name;
                double x = 0, y = 0;
                if (AT != std::string::npos && line[AT] == '@' && std::sscanf(line.c_str() + AT + 1, "%lf,%lf", &x, &y) == 2)
                    state->cameras[place] = Vector2D{x, y};
                if (line.ends_with(" free") || line.ends_with(" tiled"))
                    state->tiling[place] = line.ends_with(" tiled");
                if (!name.empty())
                    state->names[place] = name;
            }
        }
    }
    return *state;
}

static void savePlaceState() {
    const auto&     STATE = placeState();
    std::error_code error;
    const auto      PATH = placeMonitorsPath();
    std::filesystem::create_directories(PATH.parent_path(), error);
    std::ofstream out{PATH, std::ios::trunc};
    for (const auto& [number, name] : STATE.monitors) {
        out << number << ':' << name;
        if (const auto IT = STATE.cameras.find(number); IT != STATE.cameras.end())
            out << '@' << sc<long>(std::lround(IT->second.x)) << ',' << sc<long>(std::lround(IT->second.y));
        if (const auto IT = STATE.tiling.find(number); IT != STATE.tiling.end())
            out << (IT->second ? " tiled" : " free");
        if (const auto IT = STATE.names.find(number); IT != STATE.names.end() && !IT->second.empty())
            out << " #" << IT->second;
        out << '\n';
    }
}

static std::string placeName(int place) {
    const auto IT = placeState().names.find(place);
    return IT == placeState().names.end() ? std::string{} : IT->second;
}

// "Space 3", or "Space 3 · mail" once it has a name.
static std::string placeLabel(int place) {
    const auto NAME = placeName(place);
    return "Space " + std::to_string(place) + (NAME.empty() ? "" : "  ·  " + NAME);
}

static void setPlaceName(int place, std::string name) {
    std::erase_if(name, [](unsigned char c) { return c < 0x20 || c == 0x7F; });
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.empty())
        placeState().names.erase(place);
    else
        placeState().names[place] = name;
    savePlaceState();
}

static std::map<int, std::string>& assignedPlaceMonitors() {
    return placeState().monitors;
}

static void assignPlaceMonitor(int place, const std::string& monitor) {
    placeState().monitors[place] = monitor;
    savePlaceState();
}

static std::optional<std::string> placeMonitorName(int place) {
    if (const auto IT = assignedPlaceMonitors().find(place); IT != assignedPlaceMonitors().end())
        return IT->second;
    static std::string                config;
    static std::map<int, std::string> configured;
    if (const auto CONFIG = ScrollOverview::Config::getCanvasPlaceMonitors(); CONFIG != config) {
        config     = CONFIG;
        configured = parsePlaceMonitors(CONFIG);
    }
    if (const auto IT = configured.find(place); IT != configured.end())
        return IT->second;
    return std::nullopt;
}

// The screen a place lands on; none for a place never visited that no
// setting names, or whose screen is gone.
static PHLMONITOR placeMonitor(int place) {
    const auto NAME = placeMonitorName(place);
    if (!NAME)
        return nullptr;
    for (const auto& monitor : State::monitorState()->monitors())
        if (monitor && monitor->m_enabled && monitor->m_name == *NAME)
            return monitor;
    return nullptr;
}

// The screens side by side.
static CBox canvasScreensBox() {
    CBox box;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || !monitor->m_enabled)
            continue;
        const auto BOX = monitor->logicalBox();
        if (box.empty())
            box = BOX;
        else {
            const double X0 = std::min(box.x, BOX.x), Y0 = std::min(box.y, BOX.y);
            box             = CBox{X0, Y0, std::max(box.x + box.width, BOX.x + BOX.width) - X0, std::max(box.y + box.height, BOX.y + BOX.height) - Y0};
        }
    }
    return box;
}

// Rounded, so a small change to the screens does not move every place.
static Vector2D placeCamera(int place) {
    if (const auto IT = placeState().cameras.find(place); IT != placeState().cameras.end())
        return IT->second;
    const double PITCH = std::max(1000.0, std::ceil(canvasScreensBox().width * 1.25 / 1000.0) * 1000.0);
    return Vector2D{(place - 1) * PITCH, 0.0};
}

static CBox placeWorldBox(int place, const PHLMONITOR& monitor) {
    return monitor->logicalBox().translate(placeCamera(place));
}

// Screens on their own (canvas:linked_screens off) each have places of their
// own; linked, a place is a view of the whole desk (linkedPlaceAction).
static bool screenPlaces() {
    return ScrollOverview::Config::getCanvasPlaces() && !ScrollOverview::Config::getCanvasLinkedScreens();
}

// Whether a place tiles its windows: its own setting (the place menu), or
// canvas:tile_places.
static bool placeTiling(int place) {
    if (!screenPlaces())
        return false;
    if (const auto IT = placeState().tiling.find(place); IT != placeState().tiling.end())
        return IT->second;
    return ScrollOverview::Config::getCanvasTilePlaces();
}

static int placeAt(const PHLMONITOR& monitor, const Vector2D& world) {
    if (!monitor || !screenPlaces())
        return 0;
    for (int place = 1; place <= 10; ++place)
        if (placeMonitor(place) == monitor && placeWorldBox(place, monitor).containsPoint(world))
            return place;
    return 0;
}

static CBox placeWindowBox(const PHLWINDOW& window) {
    return CBox{window->m_realPosition->goal(), window->m_realSize->goal()};
}

// Filled, fullscreen and pinned windows stand over the tiling.
static bool placeTileable(const PHLWINDOW& window) {
    const auto TARGET = window ? window->layoutTarget() : nullptr;
    return shouldShowOverviewWindow(window) && TARGET && TARGET->floating() && !window->m_pinned && !Fullscreen::controller()->isFullscreen(window) &&
        !(window->m_workspace && window->m_workspace->m_isSpecialWorkspace) && !g_canvasFill.contains(window.get()) &&
        !g_placeFloating.contains(window.get());
}

static std::vector<PHLWINDOW> placeMembers(int place, const PHLMONITOR& monitor) {
    std::vector<PHLWINDOW>                            members;
    std::unordered_set<const Desktop::View::CWindow*> visited;
    const auto                                        AREA = placeWorldBox(place, monitor);
    for (const auto& windowRef : Desktop::windowState()->windows()) {
        const auto WINDOW = getOverviewWindowToShow(windowRef);
        if (placeTileable(WINDOW) && visited.emplace(WINDOW.get()).second && AREA.containsPoint(placeWindowBox(WINDOW).middle()))
            members.push_back(WINDOW);
    }
    return members;
}

// Dwindle: each window takes half of what is left, split across its longer side.
static std::vector<CBox> placeTiles(CBox area, size_t count, double gap) {
    std::vector<CBox> tiles;
    for (size_t i = 0; i < count; ++i) {
        if (i + 1 == count) {
            tiles.push_back(area);
            break;
        }
        if (area.width >= area.height) {
            const double W = std::round((area.width - gap) / 2.0);
            tiles.push_back(CBox{area.x, area.y, W, area.height});
            area = CBox{area.x + W + gap, area.y, area.width - W - gap, area.height};
        } else {
            const double H = std::round((area.height - gap) / 2.0);
            tiles.push_back(CBox{area.x, area.y, area.width, H});
            area = CBox{area.x, area.y + H + gap, area.width, area.height - H - gap};
        }
    }
    return tiles;
}

// dropped: windows were just let go of, so one dropped on another's tile
// takes its turn there.
static void tilePlace(int place, const PHLMONITOR& monitor, bool dropped) {
    if (!placeTiling(place))
        return;
    const auto             MEMBERS = placeMembers(place, monitor);
    auto&                  order   = g_placeOrder[place];
    std::vector<PHLWINDOW> tiled;
    for (const auto& ref : order)
        if (const auto WINDOW = ref.lock(); WINDOW && std::ranges::contains(MEMBERS, WINDOW) && !std::ranges::contains(tiled, WINDOW))
            tiled.push_back(WINDOW);

    const auto tileUnder = [&tiled](const PHLWINDOW& moving) -> int {
        const auto CENTER = placeWindowBox(moving).middle();
        for (size_t i = 0; i < tiled.size(); ++i)
            if (tiled[i] != moving)
                if (const auto IT = g_placeTiled.find(tiled[i].get()); IT != g_placeTiled.end() && IT->second.containsPoint(CENTER))
                    return sc<int>(i);
        return -1;
    };

    if (dropped) {
        for (size_t i = 0; i < tiled.size(); ++i) {
            const auto IT = g_placeTiled.find(tiled[i].get());
            if (IT == g_placeTiled.end() || IT->second.containsPoint(placeWindowBox(tiled[i]).middle()))
                continue;
            if (const int OTHER = tileUnder(tiled[i]); OTHER >= 0)
                std::swap(tiled[i], tiled[OTHER]);
        }
    }

    std::vector<PHLWINDOW> arrived;
    for (const auto& window : MEMBERS)
        if (!std::ranges::contains(tiled, window))
            arrived.push_back(window);
    std::ranges::sort(arrived, [](const PHLWINDOW& a, const PHLWINDOW& b) {
        const auto A = placeWindowBox(a).middle(), B = placeWindowBox(b).middle();
        return A.y != B.y ? A.y < B.y : A.x < B.x;
    });
    for (const auto& window : arrived) {
        const int AT = g_placeOpened.erase(window.get()) ? -1 : tileUnder(window);
        tiled.insert(AT >= 0 ? tiled.begin() + AT : tiled.end(), window);
    }

    order.assign(tiled.begin(), tiled.end());
    if (tiled.empty())
        return;

    const auto GAPS  = ScrollOverview::Config::getCssGapData("general:gaps_in");
    const auto AREA  = canvasFillArea(monitor, nullptr).translate(placeCamera(place));
    const auto TILES = placeTiles(AREA, tiled.size(), 2.0 * std::max<int64_t>(0, GAPS.m_left));
    for (size_t i = 0; i < tiled.size(); ++i) {
        const auto WINDOW = tiled[i];
        const auto TARGET = WINDOW->layoutTarget();
        if (!TARGET)
            continue;
        const double BORDER = WINDOW->getRealBorderSize();
        const CBox   BOX{TILES[i].x + BORDER, TILES[i].y + BORDER, std::max(1.0, TILES[i].width - 2 * BORDER), std::max(1.0, TILES[i].height - 2 * BORDER)};
        g_placeTiled[WINDOW.get()] = BOX;
        const auto NOW             = placeWindowBox(WINDOW);
        if (NOW.pos().distanceSq(BOX.pos()) < 1.0 && NOW.size().distanceSq(BOX.size()) < 1.0)
            continue;
        TARGET->rememberFloatingSize(BOX.size());
        TARGET->setPositionGlobal(BOX);
        WINDOW->sendWindowSize(true);
    }
}

// Any place that tiles.
static bool tilingPlaces() {
    return screenPlaces() && (ScrollOverview::Config::getCanvasTilePlaces() || std::ranges::any_of(placeState().tiling, [](const auto& entry) { return entry.second; }));
}

// Every frame, once for all screens: a place whose windows changed tiles
// again. Nothing moves while a button is down, so a window can be dragged
// through places; once let go, one moved off its tile goes back or swaps.
void CScrollOverview::reconcilePlaces() {
    placeFullscreenCheck();
    if (closing || !isCanvasDesktop() || !tilingPlaces() || g_placeHeldButtons > 0 || g_pInputManager->hasHeldButtons())
        return;
    static Time::steady_tp lastRun;
    const auto             NOW = Time::steadyNow();
    if (NOW - lastRun < std::chrono::milliseconds(8))
        return;
    lastRun            = NOW;
    const bool DROPPED = std::exchange(g_placeReleased, false);

    for (int place = 1; place <= 10; ++place) {
        const auto MONITOR = placeMonitor(place);
        if (!MONITOR || !placeTiling(place))
            continue;
        // Resized away from its tile (by mouse, key or the app), a window
        // leaves the tiling and keeps its size.
        bool changed = false;
        for (const auto& window : placeMembers(place, MONITOR))
            if (const auto IT = g_placeTiled.find(window.get());
                IT != g_placeTiled.end() && (std::abs(placeWindowBox(window).width - IT->second.width) > 2.0 || std::abs(placeWindowBox(window).height - IT->second.height) > 2.0)) {
                g_placeFloating.emplace(window.get());
                g_placeTiled.erase(IT);
                changed = true;
            }
        const auto MEMBERS = placeMembers(place, MONITOR);
        size_t     live    = 0;
        for (const auto& ref : g_placeOrder[place]) {
            const auto WINDOW = ref.lock();
            if (!WINDOW || !std::ranges::contains(MEMBERS, WINDOW))
                changed = true;
            else
                ++live;
        }
        changed = changed || live != MEMBERS.size();
        if (!changed && DROPPED)
            changed = std::ranges::any_of(MEMBERS, [](const PHLWINDOW& window) {
                const auto IT = g_placeTiled.find(window.get());
                return IT == g_placeTiled.end() || placeWindowBox(window).pos().distanceSq(IT->second.pos()) > 4.0;
            });
        if (changed)
            tilePlace(place, MONITOR, DROPPED);
    }
}

// SUPER + T in a tiled place: the window leaves the tiling, floating over the
// middle of the place, or joins it again where it is. False outside one.
bool canvasToggleTiled(PHLWINDOW window) {
    window            = getOverviewWindowToShow(window);
    const auto TARGET = window ? window->layoutTarget() : nullptr;
    if (!TARGET || !tilingPlaces() || !shouldShowOverviewWindow(window) || window->m_pinned || g_canvasFill.contains(window.get()))
        return false;
    const auto CENTER = placeWindowBox(window).middle();
    for (int place = 1; place <= 10; ++place) {
        const auto MONITOR = placeMonitor(place);
        if (!MONITOR || !placeWorldBox(place, MONITOR).containsPoint(CENTER))
            continue;
        if (!placeTiling(place))
            return false;
        if (!g_placeFloating.erase(window.get())) {
            g_placeFloating.emplace(window.get());
            g_placeTiled.erase(window.get());
            const auto AREA = canvasFillArea(MONITOR, nullptr).translate(placeCamera(place));
            const auto SIZE = Vector2D{std::round(AREA.width * 0.7), std::round(AREA.height * 0.6)};
            const CBox BOX{AREA.middle() - SIZE / 2.0, SIZE};
            TARGET->rememberFloatingSize(BOX.size());
            TARGET->setPositionGlobal(BOX);
            window->sendWindowSize(true);
        }
        tilePlace(place, MONITOR, false);
        return true;
    }
    return false;
}

// SUPER + double-click on a window floating in a tiled place snaps it back
// into the tiling, at the tile under it.
static bool canvasSnapDoubleClick(const PHLWINDOW& clicked, uint32_t timeMs) {
    static PHLWINDOWREF lastWindow;
    static uint32_t     lastTime = 0;
    const auto          WINDOW   = getOverviewWindowToShow(clicked);
    const bool          DOUBLE   = WINDOW && lastWindow.lock() == WINDOW && timeMs - lastTime < 400;
    lastWindow                   = WINDOW;
    lastTime                     = timeMs;
    if (!DOUBLE || !tilingPlaces() || !g_placeFloating.contains(WINDOW.get()))
        return false;
    lastWindow.reset();
    return canvasToggleTiled(WINDOW);
}

// SUPER + SHIFT + arrows in a tiled place swap the window with its neighbour
// that way; past the last one, it goes to the place the next screen that way
// is showing.
bool CScrollOverview::nudgeInPlace(PHLWINDOW window, const Vector2D& direction) {
    window = getOverviewWindowToShow(window);
    if (!isCanvasDesktop() || !tilingPlaces() || !placeTileable(window))
        return false;
    const auto CENTER  = placeWindowBox(window).middle();
    int        place   = 0;
    PHLMONITOR monitor = nullptr;
    for (int candidate = 1; candidate <= 10 && !place; ++candidate)
        if (const auto M = placeMonitor(candidate); M && placeWorldBox(candidate, M).containsPoint(CENTER)) {
            place   = candidate;
            monitor = M;
        }
    if (!place || !placeTiling(place))
        return false;

    const auto that_way = [&direction](const Vector2D& delta) {
        const double ALONG  = delta.x * direction.x + delta.y * direction.y;
        const double ACROSS = std::abs(delta.x * direction.y - delta.y * direction.x);
        return ALONG > 1.0 && ACROSS <= ALONG ? delta.size() : -1.0;
    };

    tilePlace(place, monitor, false);
    auto&  order = g_placeOrder[place];
    int    self = -1, best = -1;
    double nearest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < order.size(); ++i) {
        const auto OTHER = order[i].lock();
        if (OTHER == window) {
            self = sc<int>(i);
            continue;
        }
        if (!OTHER)
            continue;
        if (const double DISTANCE = that_way(placeWindowBox(OTHER).middle() - CENTER); DISTANCE >= 0 && DISTANCE < nearest) {
            nearest = DISTANCE;
            best    = sc<int>(i);
        }
    }
    if (self >= 0 && best >= 0) {
        checkpointCanvas();
        std::swap(order[self], order[best]);
        tilePlace(place, monitor, false);
        return true;
    }

    PHLMONITOR next = nullptr;
    nearest         = std::numeric_limits<double>::max();
    for (const auto& other : State::monitorState()->monitors()) {
        if (!other || !other->m_enabled || other == monitor)
            continue;
        if (const double DISTANCE = that_way(other->logicalBox().middle() - monitor->logicalBox().middle()); DISTANCE >= 0 && DISTANCE < nearest) {
            nearest = DISTANCE;
            next    = other;
        }
    }
    auto* canvas = next ? canvasOf(scrollOverviewForMonitor(next)) : nullptr;
    if (!canvas)
        return true;
    const int TO = placeAt(next, next->m_position + next->m_size / 2.0 + canvas->viewOffset->goal());
    if (!TO)
        return true;
    checkpointCanvas();
    const auto AREA   = placeWorldBox(TO, next);
    const auto SIZE   = placeWindowBox(window).size();
    const auto TARGET = window->layoutTarget();
    TARGET->setPositionGlobal(CBox{AREA.middle() - SIZE / 2.0, SIZE});
    TARGET->warpPositionSize();
    std::erase_if(order, [&window](const PHLWINDOWREF& ref) { return ref.lock() == window; });
    g_placeOrder[TO].push_back(window);
    tilePlace(place, monitor, false);
    tilePlace(TO, next, false);
    canvas->canvasAdoptFocus(window);
    return true;
}

// Monitor-local, in pixels: the place's box on this canvas, its number badge
// and the × beside it that takes the place away.
struct SPlaceChrome {
    CBox inner, badge, close;
};

// Pixel width of `text` drawn at `points`.
static float canvasTextWidth(const std::string& text, int points) {
    static std::unordered_map<std::string, float> widths;
    const auto KEY = std::to_string(points) + '\n' + text;
    if (const auto IT = widths.find(KEY); IT != widths.end())
        return IT->second;
    const auto  TEXTURE = g_pHyprRenderer->renderText(text, CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(points, 1));
    const float WIDTH   = TEXTURE && TEXTURE->ok() ? sc<float>(TEXTURE->m_size.x) : sc<float>(text.size()) * points * 0.6F;
    if (widths.size() > 256)
        widths.clear();
    return widths[KEY] = WIDTH;
}

static std::string placeBadgeText(int place) {
    const auto NAME = placeName(place);
    return NAME.empty() ? std::to_string(place) : std::to_string(place) + "  " + NAME;
}

std::optional<SPlaceChrome> CScrollOverview::placeChrome(int place, const PHLMONITOR& monitor) const {
    const auto OWNER = placeMonitor(place);
    if (!OWNER || !monitor)
        return std::nullopt;
    const float SCALE  = std::max(monitor->m_scale, 0.01F);
    const auto  SCREEN = canvasWorldToScreen(placeWorldBox(place, OWNER));
    const CBox  INNER  = CBox{(SCREEN.pos() - monitor->m_position) * SCALE, SCREEN.size() * SCALE}.round();
    const float BADGE  = 40.F * SCALE;
    float       width  = place == 10 ? BADGE * 1.4F : BADGE;
    if (!placeName(place).empty())
        width = std::clamp(canvasTextWidth(placeBadgeText(place), sc<int>(std::round(20.F * SCALE))) + 26.F * SCALE, width,
                           std::max(width, sc<float>(INNER.width) - BADGE - 36.F * SCALE));
    const CBox NUMBER = CBox{INNER.x + 14.F * SCALE, INNER.y + 14.F * SCALE, width, BADGE}.round();
    return SPlaceChrome{INNER, NUMBER, CBox{NUMBER.x + NUMBER.width + 8.F * SCALE, NUMBER.y, BADGE, BADGE}.round()};
}

int CScrollOverview::placeCloseAt(const Vector2D& local) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !screenPlaces() || !isCanvasNavigationActive())
        return 0;
    for (int place = 1; place <= 10; ++place)
        if (const auto CHROME = placeChrome(place, MONITOR); CHROME && CHROME->close.containsPoint(local))
            return place;
    return 0;
}

int CScrollOverview::placeBadgeAt(const Vector2D& local) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !screenPlaces() || !isCanvasNavigationActive())
        return 0;
    for (int place = 1; place <= 10; ++place)
        if (const auto CHROME = placeChrome(place, MONITOR); CHROME && CHROME->badge.containsPoint(local))
            return place;
    return 0;
}

// Its windows stay where they are on the canvas, no longer tiled.
static void removePlace(int place) {
    placeState().monitors.erase(place);
    placeState().cameras.erase(place);
    placeState().tiling.erase(place);
    placeState().names.erase(place);
    savePlaceState();
    g_places.erase(place);
    g_placeOrder.erase(place);
    if (g_previousPlace == place)
        g_previousPlace = 0;
}

// Zoomed out, each place a screen has is outlined and numbered, just outside
// it, since tiled windows cover all of it.
void CScrollOverview::renderPlaceOutlines(PHLMONITOR monitor) {
    if (!monitor || !screenPlaces() || !isCanvasNavigationActive())
        return;
    const float FADE = overviewProgress();
    if (FADE <= 0.01F)
        return;
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const auto  THEME = SpatialOverview::Hud::theme();
    const int HOVERED = placeCloseAt(lastMousePosLocal);
    for (int place = 1; place <= 10; ++place) {
        const auto CHROME = placeChrome(place, monitor);
        if (!CHROME || !CHROME->inner.overlaps(CBox{{}, monitor->m_size * SCALE}))
            continue;
        CBox box = CHROME->inner;
        box.expand(8.F * SCALE).round();
        CBorderPassElement::SBorderData border;
        border.box           = box;
        border.grad1         = Config::CGradientValueData{THEME.text};
        border.a             = 0.6F * FADE;
        border.borderSize    = std::max(2, sc<int>(std::round(2.F * SCALE)));
        border.round         = sc<int>(std::round(14.F * SCALE));
        border.outerRound    = border.round;
        border.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));

        // The number on a badge in the corner, over the windows.
        CRectPassElement::SRectData badge;
        badge.box           = CHROME->badge;
        badge.color         = THEME.accent;
        badge.color.a       = 0.92F * FADE;
        badge.round         = sc<int>(std::round(9.F * SCALE));
        badge.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(badge));
        drawCanvasText(placeBadgeText(place), CBox{badge.box.x + 12.F * SCALE, badge.box.y + 5.F * SCALE, badge.box.width - 18.F * SCALE, badge.box.height},
                       CHyprColor{0.04F, 0.04F, 0.05F, FADE}, sc<int>(std::round(20.F * SCALE)));

        CRectPassElement::SRectData close;
        close.box           = CHROME->close;
        close.color         = HOVERED == place ? CHyprColor{0.86F, 0.3F, 0.32F, 0.95F * FADE} : CHyprColor{0.04F, 0.04F, 0.05F, 0.7F * FADE};
        close.round         = badge.round;
        close.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(close));
        auto cross = THEME.text;
        cross.a    = FADE;
        drawCanvasText("×", CBox{close.box.x + 12.F * SCALE, close.box.y + 3.F * SCALE, close.box.width, close.box.height}, cross, sc<int>(std::round(24.F * SCALE)));
    }
}

bool CScrollOverview::canvasPlaceAction(const std::string& action) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !isCanvasDesktop() || closing)
        return false;
    // Places are off unless canvas:places is set: the workspace keys then do
    // nothing on the canvas (and succeed, so bindings skip their fallback).
    if (!ScrollOverview::Config::getCanvasPlaces())
        return true;
    if (ScrollOverview::Config::getCanvasLinkedScreens())
        return action.starts_with("go ") || action.starts_with("send ") ? linkedPlaceAction(action) : true;

    const auto canvasFor = [this, &MONITOR](const PHLMONITOR& monitor) -> CScrollOverview* {
        return monitor == MONITOR ? this : canvasOf(scrollOverviewForMonitor(monitor));
    };
    // Where a screen's camera is headed, not where it is on the way.
    const auto placeHere = [&MONITOR, this] { return placeAt(MONITOR, MONITOR->m_position + MONITOR->m_size / 2.0 + viewOffset->goal()); };
    const auto claim     = [&MONITOR](int place) {
        auto monitor = placeMonitor(place);
        if (!monitor) {
            monitor = MONITOR;
            assignPlaceMonitor(place, MONITOR->m_name);
        }
        return monitor;
    };

    // Zoomed out, the view glides there and stays zoomed out, on the minimap;
    // at 100% the minimap shows for a moment while you travel.
    const auto go = [&](int place) {
        if (place < 1 || place > 10)
            return false;
        const auto TARGETMONITOR = claim(place);
        auto*      canvas        = canvasFor(TARGETMONITOR);
        if (!canvas || canvas->closing || !canvas->isCanvasDesktop())
            return false;
        const auto FOCUSED = getOverviewWindowToShow(Desktop::focusState()->window());
        if (const int HERE = placeHere(); HERE) {
            g_places[HERE].focus = FOCUSED;
            if (HERE != place)
                g_previousPlace = HERE;
        }
        markCanvasCameraActive(TARGETMONITOR);
        if (!canvas->canvasNavigationActive) {
            const auto NOW = Time::steadyNow();
            if (NOW >= g_minimapFlashUntil)
                g_minimapFlashStart = NOW;
            g_minimapFlashUntil = NOW + std::chrono::milliseconds(1400);
        }
        *canvas->viewOffset = placeCamera(place);
        if (TARGETMONITOR != MONITOR) {
            Desktop::focusState()->rawMonitorFocus(TARGETMONITOR);
            Pointer::pointerController()->warpTo(TARGETMONITOR->logicalBox().middle(), true);
        }

        // Focus what had it there, or else a window there, or nothing.
        const auto VIEW     = placeWorldBox(place, TARGETMONITOR);
        const auto inView   = [&VIEW](const PHLWINDOW& window) { return shouldShowOverviewWindow(window) && VIEW.containsPoint(placeWindowBox(window).middle()); };
        auto       focusing = g_places.contains(place) ? g_places.at(place).focus.lock() : PHLWINDOW{};
        if (!inView(focusing)) {
            focusing = nullptr;
            for (const auto& ref : g_placeOrder[place])
                if (const auto WINDOW = ref.lock(); inView(WINDOW)) {
                    focusing = WINDOW;
                    break;
                }
        }
        if (!focusing)
            for (const auto& windowRef : Desktop::windowState()->windows())
                if (const auto WINDOW = getOverviewWindowToShow(windowRef); inView(WINDOW) && !WINDOW->m_pinned) {
                    focusing = WINDOW;
                    break;
                }
        if (focusing)
            canvas->canvasAdoptFocus(focusing);
        else
            Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        expectPlaceFullscreen(place, TARGETMONITOR);
        canvas->damage();
        damage();
        return true;
    };

    if (action.starts_with("go ")) {
        const auto WHERE = action.substr(3);
        const int  HERE  = placeHere();
        if (WHERE == "back")
            return go(g_previousPlace ? g_previousPlace : (HERE ? HERE : 1));
        // The place next door among this screen's, empty or not.
        if (WHERE == "next" || WHERE == "prev") {
            std::vector<int> mine;
            for (int place = 1; place <= 10; ++place)
                if (placeMonitor(place) == MONITOR)
                    mine.push_back(place);
            if (mine.empty())
                return true;
            const auto IT = std::ranges::find(mine, HERE);
            if (IT == mine.end())
                return go(mine.front());
            const int STEP = WHERE == "next" ? 1 : -1;
            const int SIZE = sc<int>(mine.size());
            return go(mine[((IT - mine.begin()) + STEP + SIZE) % SIZE]);
        }
        int place = 0;
        try { place = std::stoi(WHERE); } catch (...) { return false; }
        return go(place);
    }

    // menu [N]: the menu of place N, or of the place this screen shows, zoomed out.
    if (action == "menu" || action.starts_with("menu ")) {
        int place = placeHere();
        if (action.size() > 5)
            try { place = std::stoi(action.substr(5)); } catch (...) { return false; }
        if (!placeMonitor(place))
            return true;
        if (!canvasNavigationActive)
            toggleCanvasNavigation();
        openPlaceMenu(place, MONITOR->m_size * MONITOR->m_scale / 2.0 - Vector2D{140.0, 120.0} * MONITOR->m_scale, true);
        return true;
    }

    // assign <left|right|up|down|screen>: the place this screen shows moves,
    // with its windows, to that screen, and you go with it. This screen
    // shows the place you were at before, or another of its own.
    if (action.starts_with("assign ")) {
        const auto WHERE = action.substr(7);
        const int  PLACE = placeHere();
        if (!PLACE)
            return true;
        PHLMONITOR to = nullptr;
        if (WHERE == "left" || WHERE == "right" || WHERE == "up" || WHERE == "down") {
            const Vector2D DIRECTION = WHERE == "left" ? Vector2D{-1, 0} : WHERE == "right" ? Vector2D{1, 0} : WHERE == "up" ? Vector2D{0, -1} : Vector2D{0, 1};
            double         nearest   = std::numeric_limits<double>::max();
            for (const auto& other : State::monitorState()->monitors()) {
                if (!other || !other->m_enabled || other == MONITOR)
                    continue;
                const auto   DELTA  = other->logicalBox().middle() - MONITOR->logicalBox().middle();
                const double ALONG  = DELTA.x * DIRECTION.x + DELTA.y * DIRECTION.y;
                const double ACROSS = std::abs(DELTA.x * DIRECTION.y - DELTA.y * DIRECTION.x);
                if (ALONG > 1.0 && ACROSS <= ALONG && DELTA.size() < nearest) {
                    nearest = DELTA.size();
                    to      = other;
                }
            }
        } else
            for (const auto& other : State::monitorState()->monitors())
                if (other && other->m_enabled && other->m_name == WHERE)
                    to = other;
        if (!to || to == MONITOR || !movePlace(PLACE, to))
            return true;

        int stay = g_previousPlace != PLACE && placeMonitor(g_previousPlace) == MONITOR ? g_previousPlace : 0;
        for (int place = 1; place <= 10 && !stay; ++place)
            if (placeMonitor(place) == MONITOR)
                stay = place;
        if (stay)
            *viewOffset = placeCamera(stay);
        return go(PLACE);
    }

    // send N [stay]: the focused window to place N, at the same spot on the
    // screen (tiled places find it a spot of their own).
    int place = 0;
    try { place = std::stoi(action.substr(5)); } catch (...) { return false; }
    const bool STAY   = action.ends_with(" stay");
    const auto WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    const auto TARGET = shouldShowOverviewWindow(WINDOW) && !WINDOW->m_pinned ? WINDOW->layoutTarget() : nullptr;
    const int  HERE   = placeHere();
    if (place < 1 || place > 10 || !TARGET || place == HERE)
        return place >= 1 && place <= 10;
    checkpointCanvas();
    const auto TO   = claim(place);
    const auto DEST = placeWorldBox(place, TO);
    const auto FROM = HERE ? placeWorldBox(HERE, MONITOR) : CBox{MONITOR->m_position + viewOffset->goal(), MONITOR->m_size};
    auto       box  = placeWindowBox(WINDOW);
    box.width       = std::min(box.width, DEST.width);
    box.height      = std::min(box.height, DEST.height);
    box.x           = DEST.x + std::clamp(box.x - FROM.x, 0.0, DEST.width - box.width);
    box.y           = DEST.y + std::clamp(box.y - FROM.y, 0.0, DEST.height - box.height);
    TARGET->setPositionGlobal(box);
    TARGET->warpPositionSize();
    WINDOW->sendWindowSize(true);
    for (auto& [_, order] : g_placeOrder)
        std::erase_if(order, [&WINDOW](const PHLWINDOWREF& ref) { return ref.lock() == WINDOW; });
    g_placeOrder[place].push_back(WINDOW);
    if (STAY) {
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        return true;
    }
    go(place);
    if (auto* canvas = canvasFor(TO))
        canvas->canvasAdoptFocus(WINDOW);
    return true;
}

// The place, with its windows (scaled to the new screen), onto another screen.
bool CScrollOverview::movePlace(int place, const PHLMONITOR& to) {
    const auto FROM = placeMonitor(place);
    if (!FROM || !to || FROM == to)
        return false;
    checkpointCanvas();
    const auto             FROMBOX = placeWorldBox(place, FROM);
    std::vector<PHLWINDOW> moving;
    for (const auto& windowRef : Desktop::windowState()->windows())
        if (const auto WINDOW = getOverviewWindowToShow(windowRef);
            shouldShowOverviewWindow(WINDOW) && !WINDOW->m_pinned && !std::ranges::contains(moving, WINDOW) && FROMBOX.containsPoint(placeWindowBox(WINDOW).middle()))
            moving.push_back(WINDOW);
    assignPlaceMonitor(place, to->m_name);
    const auto     TOBOX = placeWorldBox(place, to);
    const Vector2D RATIO{TOBOX.width / std::max(1.0, FROMBOX.width), TOBOX.height / std::max(1.0, FROMBOX.height)};
    for (const auto& window : moving) {
        const auto TARGET = window->layoutTarget();
        if (!TARGET)
            continue;
        const auto     BOX  = placeWindowBox(window);
        const Vector2D SIZE{std::min(BOX.width, TOBOX.width), std::min(BOX.height, TOBOX.height)};
        Vector2D       pos = TOBOX.pos() + (BOX.pos() - FROMBOX.pos()) * RATIO;
        pos.x              = std::clamp(pos.x, TOBOX.x, TOBOX.x + TOBOX.width - SIZE.x);
        pos.y              = std::clamp(pos.y, TOBOX.y, TOBOX.y + TOBOX.height - SIZE.y);
        TARGET->setPositionGlobal(CBox{pos, SIZE});
        TARGET->warpPositionSize();
        window->sendWindowSize(true);
        g_placeTiled.erase(window.get());
    }
    if (tilingPlaces())
        tilePlace(place, to, false);
    return true;
}

// The place menu's switch: tiled, its windows tile (floated ones too);
// free, they stay where they are and move freely.
static void setPlaceTiling(int place, bool on) {
    const auto OWNER = placeMonitor(place);
    if (!OWNER)
        return;
    placeState().tiling[place] = on;
    savePlaceState();
    const auto BOX = placeWorldBox(place, OWNER);
    for (const auto& windowRef : Desktop::windowState()->windows())
        if (const auto WINDOW = getOverviewWindowToShow(windowRef); WINDOW && BOX.containsPoint(placeWindowBox(WINDOW).middle())) {
            g_placeFloating.erase(WINDOW.get());
            g_placeTiled.erase(WINDOW.get());
        }
    tilePlace(place, OWNER, false);
}

// Place `from` takes number `to`, staying where it is; a place that had `to`
// takes `from`, likewise.
static void renumberPlace(int from, int to) {
    if (from == to || from < 1 || from > 10 || to < 1 || to > 10 || !placeMonitorName(from))
        return;
    auto&      STATE     = placeState();
    const auto FROMNAME  = *placeMonitorName(from);
    const auto FROMCAM   = placeCamera(from);
    const auto TONAME    = placeMonitorName(to);
    const auto TOCAM     = placeCamera(to);
    STATE.monitors[to]   = FROMNAME;
    STATE.cameras[to]    = FROMCAM;
    if (TONAME) {
        STATE.monitors[from] = *TONAME;
        STATE.cameras[from]  = TOCAM;
    } else {
        STATE.monitors.erase(from);
        STATE.cameras.erase(from);
    }
    const auto swapEntries = [from, to](auto& map) {
        auto fromEntry = map.extract(from);
        auto toEntry   = map.extract(to);
        if (fromEntry) {
            fromEntry.key() = to;
            map.insert(std::move(fromEntry));
        }
        if (toEntry) {
            toEntry.key() = from;
            map.insert(std::move(toEntry));
        }
    };
    swapEntries(STATE.tiling);
    swapEntries(STATE.names);
    savePlaceState();
    std::swap(g_places[from], g_places[to]);
    std::swap(g_placeOrder[from], g_placeOrder[to]);
    if (g_previousPlace == from || g_previousPlace == to)
        g_previousPlace = g_previousPlace == from ? to : from;
}

// ---- The place menu: right-click a place, zoomed out ----------------------------------
struct SPlaceMenu {
    int           place = 0; // the place; for a window, the one it is in (or 0)
    PHLWINDOWREF  window;    // set: the menu of a window
    PHLMONITORREF monitor;
    Vector2D      anchor; // monitor-local pixels
    int           page = 0; // place: 0 actions, 1 numbers, 2 screens, 3 name; window: 10 actions, 11 spaces, 12 screens
    int           selected = -1; // the item the keys are on
    Vector2D      keyPointer;    // where the pointer was at the last key: once it moves, it points again
    std::string   draft;         // the name being typed
};
static SPlaceMenu g_placeMenu;

enum ePlaceMenuItem : uint8_t {
    PLACE_MENU_GO,
    PLACE_MENU_NUMBERS,
    PLACE_MENU_SCREENS,
    PLACE_MENU_DELETE,
    PLACE_MENU_TILING,
    PLACE_MENU_RENAME,
    PLACE_MENU_BRING,
    PLACE_MENU_NAME_SAVE,
    PLACE_MENU_NAME_CLEAR,
    WINDOW_MENU_FLOAT,
    PLACE_MENU_BACK,
    PLACE_MENU_NUMBER,
    PLACE_MENU_SCREEN,
    WINDOW_MENU_SPACES,
    WINDOW_MENU_SCREENS,
    WINDOW_MENU_PLACE,
    WINDOW_MENU_SEND_PLACE,
    WINDOW_MENU_SEND_SCREEN,
};

static int placeOfWindow(const PHLWINDOW& window) {
    if (!window)
        return 0;
    const auto CENTER = placeWindowBox(window).middle();
    for (int place = 1; place <= 10; ++place)
        if (const auto OWNER = placeMonitor(place); OWNER && placeWorldBox(place, OWNER).containsPoint(CENTER))
            return place;
    return 0;
}

struct SPlaceMenuItem {
    CBox           box;
    std::string    label;
    ePlaceMenuItem kind;
    int            number = 0;
    PHLMONITOR     screen;
    bool           current = false;
};

struct SPlaceMenuLayout {
    CBox                        frame;
    std::string                 title;
    std::optional<CBox>         field; // the name being typed, on the name page
    std::vector<SPlaceMenuItem> items;
};

static std::string menuWindowTitle(const PHLWINDOW& window, size_t length) {
    auto title = window->m_title.empty() ? window->m_class : window->m_title;
    if (title.size() > length) {
        size_t cut = length - 1;
        while (cut > 0 && (sc<unsigned char>(title[cut]) & 0xC0) == 0x80)
            --cut;
        title = title.substr(0, cut) + "…";
    }
    return title;
}

// The focused window, when it can be brought to `place` from elsewhere.
static PHLWINDOW placeBringable(int place) {
    const auto WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    if (!WINDOW || !shouldShowOverviewWindow(WINDOW) || WINDOW->m_pinned || placeOfWindow(WINDOW) == place)
        return nullptr;
    return WINDOW;
}

static std::optional<SPlaceMenuLayout> placeMenuLayout(const PHLMONITOR& monitor) {
    if (!monitor || g_placeMenu.monitor.lock() != monitor)
        return std::nullopt;
    const auto WINDOW    = g_placeMenu.window.lock();
    const bool FORWINDOW = g_placeMenu.page >= 10;
    if (FORWINDOW && !WINDOW)
        return std::nullopt;
    const auto OWNER = g_placeMenu.place ? placeMonitor(g_placeMenu.place) : nullptr;
    if (!FORWINDOW && !OWNER)
        return std::nullopt;
    const float      SCALE = std::max(monitor->m_scale, 0.01F);
    const float      WIDTH = 280.F * SCALE, ROW = 44.F * SCALE, PAD = 8.F * SCALE, TITLE = 40.F * SCALE;
    SPlaceMenuLayout layout;
    if (FORWINDOW)
        layout.title = menuWindowTitle(WINDOW, 30);
    else if (g_placeMenu.page == 3)
        layout.title = "Name space " + std::to_string(g_placeMenu.place);
    else
        layout.title = placeLabel(g_placeMenu.place) + "  ·  " + OWNER->m_name;
    std::vector<SPlaceMenuItem> rows;
    if (g_placeMenu.page == 10) {
        rows.push_back({.label = "Send to space  ›", .kind = WINDOW_MENU_SPACES});
        if (State::monitorState()->monitors().size() > 1)
            rows.push_back({.label = "Send to screen  ›", .kind = WINDOW_MENU_SCREENS});
        if (OWNER && placeTiling(g_placeMenu.place))
            rows.push_back({.label = g_placeFloating.contains(WINDOW.get()) ? "Tile window" : "Float window", .kind = WINDOW_MENU_FLOAT});
        if (OWNER)
            rows.push_back({.label = placeLabel(g_placeMenu.place) + "  ›", .kind = WINDOW_MENU_PLACE});
    } else if (g_placeMenu.page == 12) {
        rows.push_back({.label = "‹  Back", .kind = PLACE_MENU_BACK});
        const auto HERE = OWNER ? OWNER : WINDOW->m_monitor.lock();
        for (const auto& screen : State::monitorState()->monitors())
            if (screen && screen->m_enabled)
                rows.push_back({.label = screen->m_name + "  " + std::to_string(sc<int>(screen->m_size.x)) + "×" + std::to_string(sc<int>(screen->m_size.y)),
                                .kind  = WINDOW_MENU_SEND_SCREEN, .screen = screen, .current = screen == HERE});
    } else if (g_placeMenu.page == 0) {
        rows.push_back({.label = "Go to space", .kind = PLACE_MENU_GO});
        if (const auto BRING = placeBringable(g_placeMenu.place))
            rows.push_back({.label = "Bring here: " + menuWindowTitle(BRING, 18), .kind = PLACE_MENU_BRING});
        rows.push_back({.label = placeName(g_placeMenu.place).empty() ? "Name…" : "Rename…", .kind = PLACE_MENU_RENAME});
        rows.push_back({.label = "Change number  ›", .kind = PLACE_MENU_NUMBERS});
        if (State::monitorState()->monitors().size() > 1)
            rows.push_back({.label = "Move to screen  ›", .kind = PLACE_MENU_SCREENS});
        rows.push_back({.label = placeTiling(g_placeMenu.place) ? "Tiling  ·  on" : "Tiling  ·  off (free)", .kind = PLACE_MENU_TILING});
        rows.push_back({.label = "Delete space", .kind = PLACE_MENU_DELETE});
    } else if (g_placeMenu.page == 2) {
        rows.push_back({.label = "‹  Back", .kind = PLACE_MENU_BACK});
        for (const auto& screen : State::monitorState()->monitors())
            if (screen && screen->m_enabled)
                rows.push_back({.label = screen->m_name + "  " + std::to_string(sc<int>(screen->m_size.x)) + "×" + std::to_string(sc<int>(screen->m_size.y)),
                                .kind  = PLACE_MENU_SCREEN, .screen = screen, .current = screen == OWNER});
    } else if (g_placeMenu.page == 3) {
        rows.push_back({.label = "Save  (Enter)", .kind = PLACE_MENU_NAME_SAVE});
        if (!placeName(g_placeMenu.place).empty())
            rows.push_back({.label = "Remove name", .kind = PLACE_MENU_NAME_CLEAR});
        rows.push_back({.label = "‹  Back", .kind = PLACE_MENU_BACK});
    } else
        rows.push_back({.label = "‹  Back", .kind = PLACE_MENU_BACK});

    // Numbers: a 5 × 2 grid under the back row; taken ones swap.
    const bool  GRID     = g_placeMenu.page == 1 || g_placeMenu.page == 11;
    const float GRIDROWS = GRID ? 2.F : 0.F;
    const float FIELD    = g_placeMenu.page == 3 ? ROW + PAD : 0.F;
    const float HEIGHT   = TITLE + FIELD + rows.size() * ROW + GRIDROWS * ROW + PAD * 2.F;
    Vector2D    pos      = g_placeMenu.anchor;
    pos.x                = std::clamp(pos.x, 0.0, std::max(0.0, monitor->m_size.x * SCALE - WIDTH));
    pos.y                = std::clamp(pos.y, 0.0, std::max(0.0, monitor->m_size.y * SCALE - HEIGHT));
    layout.frame         = CBox{pos, {WIDTH, HEIGHT}}.round();

    float y = layout.frame.y + PAD + TITLE;
    if (FIELD > 0.F) {
        layout.field = CBox{layout.frame.x + PAD, y, WIDTH - PAD * 2.F, ROW}.round();
        y += FIELD;
    }
    for (auto& row : rows) {
        row.box = CBox{layout.frame.x + PAD, y, WIDTH - PAD * 2.F, ROW}.round();
        y += ROW;
        layout.items.push_back(std::move(row));
    }
    if (GRID) {
        const float CELL = (WIDTH - PAD * 2.F) / 5.F;
        for (int number = 1; number <= 10; ++number) {
            const int INDEX = number - 1;
            layout.items.push_back({.box     = CBox{layout.frame.x + PAD + (INDEX % 5) * CELL, y + (INDEX / 5) * ROW, CELL, ROW}.round(),
                                    .label   = std::to_string(number),
                                    .kind    = FORWINDOW ? WINDOW_MENU_SEND_PLACE : PLACE_MENU_NUMBER,
                                    .number  = number,
                                    .current = !FORWINDOW && (number == g_placeMenu.place || placeMonitorName(number).has_value())});
        }
    }
    return layout;
}

void CScrollOverview::openPlaceMenu(int place, const Vector2D& local, bool keys) {
    g_placeMenu = SPlaceMenu{.place = place, .monitor = pMonitor, .anchor = local, .page = 0, .selected = keys ? 0 : -1, .keyPointer = lastMousePosLocal};
    damage();
}

void CScrollOverview::openWindowMenu(PHLWINDOW window, const Vector2D& local) {
    window      = getOverviewWindowToShow(window);
    g_placeMenu = SPlaceMenu{.place = placeOfWindow(window), .window = window, .monitor = pMonitor, .anchor = local, .page = 10};
    damage();
}

bool CScrollOverview::placeMenuOpen() const {
    return (g_placeMenu.place || !g_placeMenu.window.expired()) && g_placeMenu.monitor.lock() == pMonitor.lock();
}

// The window to place N, at the same spot in it as in the place it was in
// (centered, from outside one); to a screen, into the place that screen
// shows, or the middle of its view. A tiled place tiles it.
bool CScrollOverview::sendWindowTo(PHLWINDOW window, int place, PHLMONITOR screen) {
    window            = getOverviewWindowToShow(window);
    const auto TARGET = shouldShowOverviewWindow(window) && !window->m_pinned ? window->layoutTarget() : nullptr;
    if (!TARGET)
        return false;
    CBox destination;
    if (!place && screen) {
        const auto* CANVAS = canvasOf(scrollOverviewForMonitor(screen));
        const auto  CAMERA = CANVAS ? CANVAS->viewOffset->goal() : Vector2D{};
        place              = placeAt(screen, screen->logicalBox().middle() + CAMERA);
        if (!place)
            destination = CBox{screen->m_position + CAMERA, screen->m_size};
    }
    const int FROM = placeOfWindow(window);
    if (place) {
        if (place == FROM)
            return true;
        auto owner = placeMonitor(place);
        if (!owner) {
            owner = screen ? screen : pMonitor.lock();
            assignPlaceMonitor(place, owner->m_name);
        }
        destination = placeWorldBox(place, owner);
    }
    if (destination.empty())
        return false;
    checkpointCanvas();
    auto box   = placeWindowBox(window);
    box.width  = std::min(box.width, destination.width);
    box.height = std::min(box.height, destination.height);
    if (const auto FROMOWNER = FROM ? placeMonitor(FROM) : nullptr; FROMOWNER) {
        const auto FROMBOX = placeWorldBox(FROM, FROMOWNER);
        box.x              = destination.x + std::clamp(box.x - FROMBOX.x, 0.0, destination.width - box.width);
        box.y              = destination.y + std::clamp(box.y - FROMBOX.y, 0.0, destination.height - box.height);
    } else {
        box.x = destination.x + (destination.width - box.width) / 2.0;
        box.y = destination.y + (destination.height - box.height) / 2.0;
    }
    TARGET->setPositionGlobal(box);
    TARGET->warpPositionSize();
    window->sendWindowSize(true);
    for (auto& [_, order] : g_placeOrder)
        std::erase_if(order, [&window](const PHLWINDOWREF& ref) { return ref.lock() == window; });
    if (place)
        g_placeOrder[place].push_back(window);
    g_placeTiled.erase(window.get());
    for (const auto& overview : scrollOverviews())
        overview->damage();
    return true;
}

// A press while the menu is open runs what it hit; one anywhere else only
// closes the menu. True when the press is used up.
bool CScrollOverview::placeMenuPress(const Vector2D& local, bool main) {
    const auto MONITOR = pMonitor.lock();
    const auto LAYOUT  = placeMenuLayout(MONITOR);
    if (!LAYOUT) {
        g_placeMenu = {};
        return false;
    }
    if (!LAYOUT->frame.containsPoint(local)) {
        g_placeMenu = {};
        damage();
        return true;
    }
    if (!main)
        return true;
    for (const auto& item : LAYOUT->items)
        if (item.box.containsPoint(local)) {
            activatePlaceMenuItem(item, false);
            break;
        }
    damage();
    return true;
}

// A page the keys opened starts on its first item; one the pointer opened, on none.
void CScrollOverview::activatePlaceMenuItem(const SPlaceMenuItem& item, bool keys) {
    const int  PLACE  = g_placeMenu.place;
    const auto WINDOW = g_placeMenu.window.lock();
    const auto page   = [keys](int page) {
        g_placeMenu.page     = page;
        g_placeMenu.selected = keys ? 0 : -1;
    };
    switch (item.kind) {
        case PLACE_MENU_NUMBERS: page(1); break;
        case PLACE_MENU_SCREENS: page(2); break;
        case PLACE_MENU_RENAME:
            g_placeMenu.draft = placeName(PLACE);
            page(3);
            g_placeMenu.selected = -1;
            break;
        case PLACE_MENU_BACK: page(g_placeMenu.page >= 10 ? 10 : 0); break;
        case PLACE_MENU_TILING: setPlaceTiling(PLACE, !placeTiling(PLACE)); break;
        case PLACE_MENU_NAME_SAVE:
            setPlaceName(PLACE, g_placeMenu.draft);
            page(0);
            break;
        case PLACE_MENU_NAME_CLEAR:
            setPlaceName(PLACE, "");
            page(0);
            break;
        case PLACE_MENU_BRING:
            g_placeMenu = {};
            if (const auto BRING = placeBringable(PLACE))
                sendWindowTo(BRING, PLACE, nullptr);
            break;
        case WINDOW_MENU_FLOAT:
            g_placeMenu = {};
            canvasToggleTiled(WINDOW);
            break;
        case WINDOW_MENU_SPACES: page(11); break;
        case WINDOW_MENU_SCREENS: page(12); break;
        case WINDOW_MENU_PLACE: page(0); break;
        case WINDOW_MENU_SEND_PLACE:
            g_placeMenu = {};
            sendWindowTo(WINDOW, item.number, nullptr);
            break;
        case WINDOW_MENU_SEND_SCREEN:
            g_placeMenu = {};
            sendWindowTo(WINDOW, 0, item.screen);
            break;
        case PLACE_MENU_GO:
            g_placeMenu = {};
            canvasPlaceAction("go " + std::to_string(PLACE));
            break;
        case PLACE_MENU_DELETE:
            g_placeMenu = {};
            removePlace(PLACE);
            break;
        case PLACE_MENU_NUMBER:
            g_placeMenu = {};
            renumberPlace(PLACE, item.number);
            break;
        case PLACE_MENU_SCREEN:
            g_placeMenu = {};
            movePlace(PLACE, item.screen);
            break;
    }
    for (const auto& overview : scrollOverviews())
        overview->damage();
}

// While a place menu is open its keys are its own (SUPER chords still reach
// Hyprland): arrows or Tab pick, Enter runs, Left or Backspace go back a
// page, Esc closes; on a number grid a digit picks that number, and on the
// name page typing writes the name.
bool CScrollOverview::placeMenuKey(const IKeyboard::SKeyEvent& event, uint32_t keysym, uint32_t mods) {
    if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED || !placeMenuOpen() || isModifierKeysym(keysym) || (mods & HL_MODIFIER_META))
        return false;
    const auto LAYOUT = placeMenuLayout(pMonitor.lock());
    if (!LAYOUT) {
        g_placeMenu = {};
        return false;
    }
    const auto& ITEMS  = LAYOUT->items;
    const int   COUNT  = sc<int>(ITEMS.size());
    const bool  NAMING = g_placeMenu.page == 3;
    const bool  CTRL   = mods & HL_MODIFIER_CTRL;
    const auto  isCell = [&](int index) { return index >= 0 && index < COUNT && ITEMS[index].number > 0; };
    const int   ROWS   = sc<int>(std::ranges::count_if(ITEMS, [](const SPlaceMenuItem& item) { return item.number == 0; }));
    auto&       selected = g_placeMenu.selected;
    selected             = std::clamp(selected, -1, COUNT - 1);
    g_placeMenu.keyPointer = lastMousePosLocal;

    const auto back = [&] {
        if (g_placeMenu.page == 0 || g_placeMenu.page == 10) {
            if (g_placeMenu.page == 0 && !g_placeMenu.window.expired()) {
                g_placeMenu.page = 10;
                selected         = 0;
            } else
                g_placeMenu = {};
        } else {
            g_placeMenu.page = g_placeMenu.page >= 10 ? 10 : 0;
            selected         = 0;
        }
    };
    // Rows step one by one; on the grid, up and down go a row of five.
    const auto step = [&](int delta, bool vertical) {
        if (COUNT == 0)
            return;
        if (selected < 0)
            selected = delta > 0 ? 0 : COUNT - 1;
        else if (vertical && isCell(selected)) {
            const int NEXT = selected - ROWS + delta * 5;
            if (NEXT < 0)
                selected = ROWS > 0 ? ROWS - 1 : selected;
            else if (NEXT < COUNT - ROWS)
                selected = ROWS + NEXT;
        } else
            selected = (selected + delta + COUNT) % COUNT;
    };
    const auto run = [&](int index) {
        if (index >= 0 && index < COUNT) {
            const auto ITEM = ITEMS[index];
            activatePlaceMenuItem(ITEM, true);
        }
    };

    switch (keysym) {
        case XKB_KEY_Escape:
            if (g_placeMenu.page == 0 || g_placeMenu.page == 10)
                g_placeMenu = {};
            else
                back();
            break;
        case XKB_KEY_Up:
        case XKB_KEY_KP_Up: step(-1, true); break;
        case XKB_KEY_Down:
        case XKB_KEY_KP_Down: step(1, true); break;
        case XKB_KEY_Tab: step((mods & HL_MODIFIER_SHIFT) ? -1 : 1, false); break;
        case XKB_KEY_ISO_Left_Tab: step(-1, false); break;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left:
            if (isCell(selected) && (selected - ROWS) % 5 != 0)
                --selected;
            else if (!NAMING)
                back();
            break;
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right:
            if (isCell(selected) && (selected - ROWS) % 5 != 4 && selected + 1 < COUNT)
                ++selected;
            else if (selected >= 0 &&
                     std::ranges::contains(std::array{PLACE_MENU_NUMBERS, PLACE_MENU_SCREENS, PLACE_MENU_RENAME, WINDOW_MENU_SPACES, WINDOW_MENU_SCREENS, WINDOW_MENU_PLACE},
                                           ITEMS[selected].kind))
                run(selected);
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (NAMING && selected < 0) {
                setPlaceName(g_placeMenu.place, g_placeMenu.draft);
                g_placeMenu.page     = 0;
                selected             = 0;
            } else
                run(selected);
            break;
        case XKB_KEY_BackSpace:
            if (!NAMING)
                back();
            else if (CTRL)
                g_placeMenu.draft.clear();
            else if (!g_placeMenu.draft.empty()) {
                auto& draft = g_placeMenu.draft;
                size_t cut = draft.size() - 1;
                while (cut > 0 && (sc<unsigned char>(draft[cut]) & 0xC0) == 0x80)
                    --cut;
                draft.resize(cut);
            }
            break;
        default: {
            if (NAMING) {
                if (CTRL && (keysym == XKB_KEY_u || keysym == XKB_KEY_U || keysym == XKB_KEY_w || keysym == XKB_KEY_W))
                    g_placeMenu.draft.clear();
                else if (const auto TEXT = CTRL || (mods & HL_MODIFIER_ALT) ? std::string{} : navigatorKeyText(event);
                         !TEXT.empty() && std::ranges::none_of(TEXT, [](unsigned char c) { return c < 0x20 || c == 0x7F; }) && g_placeMenu.draft.size() + TEXT.size() <= 48) {
                    g_placeMenu.draft += TEXT;
                    selected = -1;
                }
            } else if (keysym >= XKB_KEY_0 && keysym <= XKB_KEY_9 && ROWS < COUNT) {
                const int NUMBER = keysym == XKB_KEY_0 ? 10 : sc<int>(keysym - XKB_KEY_0);
                for (int index = ROWS; index < COUNT; ++index)
                    if (ITEMS[index].number == NUMBER) {
                        run(index);
                        break;
                    }
            }
            break;
        }
    }
    SpatialOverview::Navigator::markConsumed(event.keycode);
    for (const auto& overview : scrollOverviews())
        overview->damage();
    return true;
}

void CScrollOverview::renderPlaceMenu(PHLMONITOR monitor) {
    if (g_placeMenu.monitor.lock() == monitor && !isCanvasNavigationActive())
        g_placeMenu = {};
    const auto LAYOUT = placeMenuLayout(monitor);
    if (!LAYOUT)
        return;
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const auto  THEME = SpatialOverview::Hud::theme();
    const int   ROUND = sc<int>(std::round(10.F * SCALE));

    CRectPassElement::SRectData frame;
    frame.box           = LAYOUT->frame;
    frame.color         = CHyprColor{0.05F, 0.05F, 0.07F, 0.94F};
    frame.round         = ROUND;
    frame.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(frame));
    CBorderPassElement::SBorderData border;
    border.box           = LAYOUT->frame;
    border.grad1         = Config::CGradientValueData{THEME.accent};
    border.a             = 0.9F;
    border.borderSize    = std::max(1, sc<int>(std::round(1.5F * SCALE)));
    border.round         = ROUND;
    border.outerRound    = ROUND;
    border.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));

    auto dim = THEME.text;
    dim.a    = 0.6F;
    drawCanvasText(LAYOUT->title, CBox{LAYOUT->frame.x + 18.F * SCALE, LAYOUT->frame.y + 16.F * SCALE, LAYOUT->frame.width - 36.F * SCALE, 24.F * SCALE}, dim,
                   sc<int>(std::round(13.F * SCALE)));

    if (LAYOUT->field) {
        CRectPassElement::SRectData field;
        field.box           = *LAYOUT->field;
        field.color         = CHyprColor{1.F, 1.F, 1.F, 0.08F};
        field.round         = sc<int>(std::round(7.F * SCALE));
        field.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(field));
        const bool EMPTY = g_placeMenu.draft.empty();
        drawCanvasText(EMPTY ? "Type a name" : g_placeMenu.draft + "▏", CBox{field.box.x + 14.F * SCALE, field.box.y + 11.F * SCALE, field.box.width - 28.F * SCALE, field.box.height - 11.F * SCALE},
                       EMPTY ? dim : THEME.text, sc<int>(std::round(15.F * SCALE)));
    }

    // The keys' pick shows until the pointer moves; then what it is over does.
    const bool KEYS = g_placeMenu.selected >= 0 && lastMousePosLocal == g_placeMenu.keyPointer;
    for (size_t index = 0; index < LAYOUT->items.size(); ++index) {
        const auto& item   = LAYOUT->items[index];
        const bool  HOVER  = KEYS ? sc<int>(index) == g_placeMenu.selected : item.box.containsPoint(lastMousePosLocal);
        const bool  DANGER = item.kind == PLACE_MENU_DELETE;
        const bool CELL   = item.kind == PLACE_MENU_NUMBER;
        if (HOVER || (CELL && item.number == g_placeMenu.place)) {
            CRectPassElement::SRectData highlight;
            highlight.box = item.box.copy().expand(-2.F * SCALE).round();
            highlight.color = DANGER ? CHyprColor{0.86F, 0.3F, 0.32F, 0.9F} : THEME.accent;
            highlight.color.a = HOVER ? 0.9F : 0.35F;
            highlight.round = sc<int>(std::round(7.F * SCALE));
            highlight.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(highlight));
        }
        auto color = HOVER ? CHyprColor{0.04F, 0.04F, 0.05F, 1.F} : DANGER ? CHyprColor{0.95F, 0.45F, 0.45F, 1.F} : THEME.text;
        if (!HOVER && CELL && item.current && item.number != g_placeMenu.place)
            color.a = 0.45F; // taken: picking it swaps
        std::string label = item.label;
        if ((item.kind == PLACE_MENU_SCREEN || item.kind == WINDOW_MENU_SEND_SCREEN) && item.current)
            label = "✓  " + label;
        const float INSET = CELL ? item.box.width / 2.F - (item.number == 10 ? 11.F : 6.F) * SCALE : 14.F * SCALE;
        drawCanvasText(label, CBox{item.box.x + INSET, item.box.y + 11.F * SCALE, item.box.width - INSET, item.box.height - 11.F * SCALE}, color,
                       sc<int>(std::round(15.F * SCALE)));
    }
}

// ---- Moving a place with the mouse, zoomed out ------------------------------------
// Grab its number badge or its outline and drag: the place and its windows
// go along. A click on the badge (no drag) goes there.
struct SPlaceDrag {
    int                                           place = 0;
    const CScrollOverview*                        canvas = nullptr;
    bool                                          badge = false, moved = false;
    Vector2D                                      startLocal, startWorld, startCamera;
    std::vector<std::pair<PHLWINDOWREF, CBox>>    windows;
};
static SPlaceDrag g_placeDrag;

struct SPlaceRightClick {
    PHLWINDOWREF           window;
    const CScrollOverview* canvas = nullptr;
    Vector2D               local;
};
static SPlaceRightClick g_placeRightClick;

int CScrollOverview::placeEdgeAt(const Vector2D& local) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !screenPlaces() || !isCanvasNavigationActive())
        return 0;
    const float SCALE = std::max(MONITOR->m_scale, 0.01F);
    for (int place = 1; place <= 10; ++place) {
        const auto CHROME = placeChrome(place, MONITOR);
        if (!CHROME)
            continue;
        const auto OUTER = CHROME->inner.copy().expand(18.F * SCALE);
        const auto INNER = CHROME->inner.copy().expand(-4.F * SCALE);
        if (OUTER.containsPoint(local) && !INNER.containsPoint(local))
            return place;
    }
    return 0;
}

static std::optional<Vector2D> canvasWorldAtLocal(const CScrollOverview* canvas, const PHLMONITOR& monitor, const Vector2D& local) {
    const auto WORLD = canvas->canvasScreenToWorld(CBox{monitor->m_position + local / std::max(monitor->m_scale, 0.01F), {}}, false);
    return WORLD ? std::optional<Vector2D>{WORLD->pos()} : std::nullopt;
}

bool CScrollOverview::placeDragPress(const Vector2D& local) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;
    int        place = placeBadgeAt(local);
    const bool BADGE = place != 0;
    if (!place)
        place = placeEdgeAt(local);
    const auto OWNER = place ? placeMonitor(place) : nullptr;
    const auto WORLD = OWNER ? canvasWorldAtLocal(this, MONITOR, local) : std::nullopt;
    if (!WORLD)
        return false;
    g_placeDrag = SPlaceDrag{.place = place, .canvas = this, .badge = BADGE, .startLocal = local, .startWorld = *WORLD, .startCamera = placeCamera(place)};
    const auto BOX = placeWorldBox(place, OWNER);
    for (const auto& windowRef : Desktop::windowState()->windows())
        if (const auto WINDOW = getOverviewWindowToShow(windowRef); shouldShowOverviewWindow(WINDOW) && !WINDOW->m_pinned && BOX.containsPoint(placeWindowBox(WINDOW).middle()) &&
            std::ranges::none_of(g_placeDrag.windows, [&WINDOW](const auto& entry) { return entry.first.lock() == WINDOW; }))
            g_placeDrag.windows.emplace_back(WINDOW, placeWindowBox(WINDOW));
    return true;
}

bool CScrollOverview::placeDragMotion(const Vector2D& local) {
    if (!g_placeDrag.place || g_placeDrag.canvas != this)
        return false;
    const auto MONITOR = pMonitor.lock();
    const auto WORLD   = MONITOR ? canvasWorldAtLocal(this, MONITOR, local) : std::nullopt;
    if (!WORLD)
        return true;
    if (!g_placeDrag.moved && local.distanceSq(g_placeDrag.startLocal) < std::pow(8.F * std::max(MONITOR->m_scale, 0.01F), 2))
        return true;
    g_placeDrag.moved       = true;
    const Vector2D DELTA    = Vector2D{std::round(WORLD->x - g_placeDrag.startWorld.x), std::round(WORLD->y - g_placeDrag.startWorld.y)};
    placeState().cameras[g_placeDrag.place] = g_placeDrag.startCamera + DELTA;
    for (const auto& [ref, box] : g_placeDrag.windows) {
        const auto WINDOW = ref.lock();
        const auto TARGET = WINDOW ? WINDOW->layoutTarget() : nullptr;
        if (!TARGET)
            continue;
        TARGET->setPositionGlobal(CBox{box.pos() + DELTA, box.size()});
        TARGET->warpPositionSize();
    }
    for (const auto& overview : scrollOverviews())
        overview->damage();
    return true;
}

bool CScrollOverview::placeDragRelease() {
    if (!g_placeDrag.place || g_placeDrag.canvas != this)
        return false;
    const auto DRAG = std::exchange(g_placeDrag, SPlaceDrag{});
    if (DRAG.moved) {
        savePlaceState();
        const auto DELTA = placeCamera(DRAG.place) - DRAG.startCamera;
        for (const auto& [ref, box] : DRAG.windows)
            if (const auto WINDOW = ref.lock(); WINDOW)
                if (const auto IT = g_placeTiled.find(WINDOW.get()); IT != g_placeTiled.end())
                    IT->second.translate(DELTA);
        if (const auto OWNER = placeMonitor(DRAG.place); OWNER && tilingPlaces())
            tilePlace(DRAG.place, OWNER, false);
        noteCanvasLayoutChanged();
    } else if (DRAG.badge)
        canvasPlaceAction("go " + std::to_string(DRAG.place));
    damage();
    return true;
}

void CScrollOverview::placeRightPress(PHLWINDOW window, const Vector2D& local) {
    g_placeRightClick = {};
    if (screenPlaces() && isCanvasNavigationActive() && window)
        g_placeRightClick = {.window = window, .canvas = this, .local = local};
}

bool CScrollOverview::placeRightRelease(const Vector2D& local, bool resized) {
    const auto CLICK   = std::exchange(g_placeRightClick, SPlaceRightClick{});
    const auto MONITOR = pMonitor.lock();
    const auto WINDOW  = CLICK.window.lock();
    if (!WINDOW || CLICK.canvas != this || resized || !MONITOR || local.distanceSq(CLICK.local) > std::pow(8.F * std::max(MONITOR->m_scale, 0.01F), 2))
        return false;
    openWindowMenu(WINDOW, local);
    return true;
}

// Right-click on the canvas, off any window: on empty canvas, the lowest
// place no screen has yet is made there, for this screen, centered on the
// click. Zoomed out, a right-click on a place (or just around its outline)
// opens its menu.
bool CScrollOverview::placeClick(const Vector2D& world, const Vector2D& local) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !isCanvasDesktop() || !screenPlaces())
        return false;

    const double MARGIN = 24.0 / std::max(scale->value(), 0.01F);
    for (int place = 1; place <= 10; ++place) {
        const auto OWNER = placeMonitor(place);
        if (!OWNER || !placeWorldBox(place, OWNER).expand(MARGIN).containsPoint(world))
            continue;
        if (!isCanvasNavigationActive())
            return false;
        openPlaceMenu(place, local);
        return true;
    }

    int place = 1;
    while (place <= 10 && placeMonitorName(place))
        ++place;
    if (place > 10)
        return true;
    const auto CAMERA = world - MONITOR->logicalBox().middle();
    placeState().monitors[place] = MONITOR->m_name;
    placeState().cameras[place]  = Vector2D{std::round(CAMERA.x), std::round(CAMERA.y)};
    savePlaceState();
    return canvasPlaceAction("go " + std::to_string(place));
}

// ---- Fullscreen apps stay at their place -------------------------------------------
// A place key on a screen a fullscreen app has to itself brings the canvas
// back there: the app leaves fullscreen where it was, remembered as its
// place's. Back at that place, at 100%, it is fullscreen again. Leaving
// fullscreen yourself (Super+F) makes the place forget it.
struct SPlaceFullscreen {
    PHLWINDOWREF                window;
    Fullscreen::SFullscreenMode modes;
};
static std::unordered_map<int, SPlaceFullscreen> g_placeFullscreen;
static int                                       g_placeFullscreenPending = 0;

static void notePlaceFullscreen(const PHLWINDOW& window, const Fullscreen::SFullscreenMode& modes) {
    if (const int PLACE = screenPlaces() ? placeOfWindow(window) : 0; PLACE)
        g_placeFullscreen[PLACE] = {.window = window, .modes = modes};
}

static void forgetPlaceFullscreen(const PHLWINDOW& window) {
    std::erase_if(g_placeFullscreen, [&window](const auto& entry) { return !window || entry.second.window.lock() == window; });
}

void CScrollOverview::expectPlaceFullscreen(int place, const PHLMONITOR& monitor) {
    g_placeFullscreenPending = 0;
    const auto IT = g_placeFullscreen.find(place);
    if (IT == g_placeFullscreen.end() || !monitor)
        return;
    const auto WINDOW = IT->second.window.lock();
    if (!validMapped(WINDOW) || !placeWorldBox(place, monitor).containsPoint(placeWindowBox(WINDOW).middle())) {
        g_placeFullscreen.erase(IT);
        return;
    }
    g_placeFullscreenPending = place;
}

// Once the screen has arrived at the place, at 100%.
void CScrollOverview::placeFullscreenCheck() {
    if (!g_placeFullscreenPending)
        return;
    const int  PLACE   = g_placeFullscreenPending;
    const auto MONITOR = placeMonitor(PLACE);
    const auto IT      = g_placeFullscreen.find(PLACE);
    const auto WINDOW  = IT != g_placeFullscreen.end() ? IT->second.window.lock() : nullptr;
    auto*      canvas  = MONITOR ? canvasOf(scrollOverviewForMonitor(MONITOR)) : nullptr;
    if (!canvas || !validMapped(WINDOW) || canvas->viewOffset->goal().distanceSq(placeCamera(PLACE)) > 1.0) {
        g_placeFullscreenPending = 0;
        return;
    }
    if (canvas->closing || canvas->isCanvasNavigationActive() || canvas->viewOffset->value().distanceSq(canvas->viewOffset->goal()) > 1.0)
        return;
    g_placeFullscreenPending = 0;
    if (Fullscreen::controller()->isFullscreen(WINDOW))
        return;
    Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    Fullscreen::controller()->setFullscreenMode(WINDOW, IT->second.modes.internal, IT->second.modes.client);
}

// A place key while the focused screen's canvas stepped aside for a
// fullscreen app (main.cpp). A place on another screen leaves the app alone.
bool canvasPlaceKeyWhileFullscreen(const PHLMONITOR& focused, const std::string& action) {
    if (!focused || !screenPlaces() || !canvasSteppedAside(focused))
        return false;
    if (action.starts_with("go ")) {
        int place = 0;
        try { place = std::stoi(action.substr(3)); } catch (...) { place = 0; }
        if (const auto OWNER = place ? placeMonitor(place) : nullptr; OWNER && OWNER != focused) {
            auto* canvas = canvasOf(scrollOverviewForMonitor(OWNER));
            if (!canvas)
                return false;
            canvas->canvasPlaceAction(action);
            Desktop::focusState()->rawMonitorFocus(OWNER);
            Pointer::pointerController()->warpTo(OWNER->logicalBox().middle(), true);
            return true;
        }
    }
    if (!openCanvasOverview(focused))
        return false;
    auto* canvas = canvasOf(scrollOverviewForMonitor(focused));
    return canvas && canvas->canvasPlaceAction(action);
}

// ---- Places on linked screens ------------------------------------------------------
// A place is a view of the canvas (the camera of all screens, at 100%). At
// first the places are a row, a screen-set apart, with the one numbered after
// the workspace you were on where you are; each then remembers where you left
// its camera and which window had focus there.
struct SLinkedPlace {
    Vector2D     camera;
    PHLWINDOWREF focus;
};
static std::unordered_map<int, SLinkedPlace> g_linkedPlaces;
static int                                   g_linkedPlace = 0, g_linkedPreviousPlace = 0, g_linkedAnchor = 0;
static Vector2D                              g_linkedAnchorCamera;

static Vector2D linkedPlaceCamera(int place) {
    if (const auto IT = g_linkedPlaces.find(place); IT != g_linkedPlaces.end())
        return IT->second.camera;
    return g_linkedAnchorCamera + Vector2D{(place - g_linkedAnchor) * canvasScreensBox().width * 1.5, 0.0};
}

bool CScrollOverview::linkedPlaceAction(const std::string& action) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;
    // You are at the place of the workspace you were on.
    if (g_linkedPlace == 0) {
        const auto ID        = MONITOR->m_activeWorkspace ? MONITOR->m_activeWorkspace->m_id : 1;
        g_linkedPlace        = ID >= 1 && ID <= 10 ? sc<int>(ID) : 1;
        g_linkedAnchor       = g_linkedPlace;
        g_linkedAnchorCamera = viewOffset->goal();
    }

    // Zoomed out, the view glides there and stays zoomed out, on the minimap;
    // at 100% the minimap shows for a moment while you travel.
    const auto go = [this](int place) {
        if (place < 1 || place > 10)
            return false;
        const auto FOCUSED            = getOverviewWindowToShow(Desktop::focusState()->window());
        g_linkedPlaces[g_linkedPlace] = {.camera = viewOffset->goal(), .focus = FOCUSED};
        if (place != g_linkedPlace)
            g_linkedPreviousPlace = g_linkedPlace;
        g_linkedPlace = place;
        markCanvasCameraActive(pMonitor.lock());
        if (!canvasNavigationActive) {
            const auto NOW = Time::steadyNow();
            if (NOW >= g_minimapFlashUntil)
                g_minimapFlashStart = NOW;
            g_minimapFlashUntil = NOW + std::chrono::milliseconds(1400);
        }
        *viewOffset = linkedPlaceCamera(place);
        // Focus what had it there, or nothing that is not there.
        const auto REMEMBERED = g_linkedPlaces.contains(place) ? g_linkedPlaces.at(place).focus.lock() : PHLWINDOW{};
        const auto VIEW       = canvasScreensBox().translate(linkedPlaceCamera(place));
        if (shouldShowOverviewWindow(REMEMBERED) && VIEW.containsPoint(REMEMBERED->m_realPosition->goal() + REMEMBERED->m_realSize->goal() / 2.0))
            canvasAdoptFocus(REMEMBERED);
        else if (FOCUSED && !VIEW.containsPoint(FOCUSED->m_realPosition->goal() + FOCUSED->m_realSize->goal() / 2.0))
            Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        damage();
        return true;
    };

    if (action.starts_with("go ")) {
        const auto WHERE = action.substr(3);
        if (WHERE == "back")
            return go(g_linkedPreviousPlace ? g_linkedPreviousPlace : g_linkedPlace);
        // The place next door, empty or not, like the workspace keys: with
        // all your windows at one place there is still somewhere to go.
        if (WHERE == "next" || WHERE == "prev")
            return go(((g_linkedPlace - 1 + (WHERE == "next" ? 1 : -1)) % 10 + 10) % 10 + 1);
        int place = 0;
        try { place = std::stoi(WHERE); } catch (...) { return false; }
        return go(place);
    }

    // send N [stay]: the focused window to place N, at the same spot on the screen.
    int place = 0;
    try { place = std::stoi(action.substr(5)); } catch (...) { return false; }
    const bool STAY   = action.ends_with(" stay");
    const auto WINDOW = getOverviewWindowToShow(Desktop::focusState()->window());
    const auto TARGET = shouldShowOverviewWindow(WINDOW) && !WINDOW->m_pinned ? WINDOW->layoutTarget() : nullptr;
    if (place < 1 || place > 10 || !TARGET || place == g_linkedPlace)
        return place >= 1 && place <= 10;
    checkpointCanvas();
    const auto DELTA = linkedPlaceCamera(place) - viewOffset->goal();
    TARGET->setPositionGlobal(CBox{WINDOW->m_realPosition->goal() + DELTA, WINDOW->m_realSize->goal()});
    TARGET->warpPositionSize();
    WINDOW->sendWindowSize(true);
    if (STAY) {
        Desktop::focusState()->fullWindowFocus(nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
        return true;
    }
    go(place);
    canvasAdoptFocus(WINDOW);
    return true;
}
