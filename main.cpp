#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <sstream>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

#include <hyprutils/string/ConstVarList.hpp>
using namespace Hyprutils::String;

#include "globals.hpp"
#include "Config.hpp"
#include "PluginVersion.hpp"
#include "scrollOverview.hpp"
#include "BarrelShader.hpp"
#include "Experiments.hpp"
#include "Hud.hpp"
#include "Navigator.hpp"
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include "OverviewGesture.hpp"
#include "FunctionHook.hpp"

// Methods
static COverviewFunctionHook* g_pScrollRenderWorkspaceHook = nullptr;
static COverviewFunctionHook* g_pScrollAddDamageHookA      = nullptr;
static COverviewFunctionHook* g_pScrollAddDamageHookB      = nullptr;
static COverviewFunctionHook* g_pScrollDamageSurfaceHook   = nullptr;
static COverviewFunctionHook* g_pScrollScheduleFrameHook   = nullptr;
static COverviewFunctionHook* g_pScrollSendFrameEventsHook = nullptr;
static COverviewFunctionHook* g_pScrollSurfaceFrameHook    = nullptr;
static COverviewFunctionHook* g_pScrollDrawTexHook         = nullptr;
static COverviewFunctionHook* g_pScrollElementDrawTexHook  = nullptr;
static COverviewFunctionHook* g_pPopupRepositionHook       = nullptr;
static COverviewFunctionHook* g_pBeginDragTargetHook       = nullptr;
static COverviewFunctionHook* g_pX11ConfigureHook          = nullptr;
static COverviewFunctionHook* g_pX11ConfigureRequestHook   = nullptr;
static COverviewFunctionHook* g_pX11ClientMessageHook      = nullptr;

namespace Desktop::View {
    class CPopup;
}
bool canvasRepositionPopup(Desktop::View::CPopup* popup);
bool canvasTakeClientWindowGesture(const PHLWINDOW& window, std::optional<Layout::eRectCorner> resizeEdge);
CBox canvasX11Configure(void* surface, const CBox& box);
CBox canvasX11Request(void* window, CBox box);
void canvasReleaseX11Windows();
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
typedef void (*origScheduleFrame)(void*, Aquamarine::IOutput::scheduleFrameReason);
typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);
typedef void (*origDrawTex)(void*, WP<CTexPassElement>, const CRegion&);
typedef void (*origElementDrawTex)(void*, WP<CTexPassElement>, const CRegion&);

static bool g_unloading = false;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool renderingOverview = false;
static bool damageFromSurface = false;
static PHLMONITORREF renderingOverviewMonitor;
static bool g_scrollOverviewHooksActive = false;

static void failNotif(const std::string& reason);

bool ensureScrollOverviewHooks() {
    if (g_scrollOverviewHooksActive)
        return true;

    bool success = g_pScrollRenderWorkspaceHook->hook();
    success      = success && g_pScrollScheduleFrameHook->hook();
    success      = success && g_pScrollDamageSurfaceHook->hook();
    success      = success && g_pScrollSendFrameEventsHook->hook();
    success      = success && g_pScrollSurfaceFrameHook->hook();
    success      = success && g_pScrollAddDamageHookA->hook();
    success      = success && g_pScrollAddDamageHookB->hook();
    success      = success && g_pScrollDrawTexHook->hook();
    success      = success && g_pScrollElementDrawTexHook->hook();
    success      = success && g_pPopupRepositionHook->hook();
    success      = success && g_pBeginDragTargetHook->hook();
    success      = success && g_pX11ConfigureHook->hook();
    success      = success && g_pX11ConfigureRequestHook->hook();

    if (!success) {
        disableScrollOverviewHooks();
        failNotif("Failed enabling overview hooks (is other overview plugin enabled?)");
        return false;
    }

    g_scrollOverviewHooksActive = true;
    return true;
}

void disableScrollOverviewHooks() {
    if (g_pX11ConfigureRequestHook)
        g_pX11ConfigureRequestHook->unhook();
    if (g_pX11ConfigureHook)
        g_pX11ConfigureHook->unhook();
    if (g_pBeginDragTargetHook)
        g_pBeginDragTargetHook->unhook();
    if (g_pPopupRepositionHook)
        g_pPopupRepositionHook->unhook();
    if (g_pScrollElementDrawTexHook)
        g_pScrollElementDrawTexHook->unhook();
    if (g_pScrollDrawTexHook)
        g_pScrollDrawTexHook->unhook();
    if (g_pScrollAddDamageHookB)
        g_pScrollAddDamageHookB->unhook();
    if (g_pScrollAddDamageHookA)
        g_pScrollAddDamageHookA->unhook();
    if (g_pScrollSurfaceFrameHook)
        g_pScrollSurfaceFrameHook->unhook();
    if (g_pScrollSendFrameEventsHook)
        g_pScrollSendFrameEventsHook->unhook();
    if (g_pScrollDamageSurfaceHook)
        g_pScrollDamageSurfaceHook->unhook();
    if (g_pScrollScheduleFrameHook)
        g_pScrollScheduleFrameHook->unhook();
    if (g_pScrollRenderWorkspaceHook)
        g_pScrollRenderWorkspaceHook->unhook();

    g_scrollOverviewHooksActive = false;
}

static void hkScheduleFrame(void* thisptr, Aquamarine::IOutput::scheduleFrameReason reason) {
    const auto OVERVIEW = scrollOverviewForMonitor(sc<Monitor::CMonitor*>(thisptr)->m_self.lock());
    if (OVERVIEW) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON =
            reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
            reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !OVERVIEW->blockDamageReporting && !OVERVIEW->shouldAllowRealtimePreviewSchedule())
            return;
    }

    rc<origScheduleFrame>(g_pScrollScheduleFrameHook->m_original)(thisptr, reason);
}

//
static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
    const auto OVERVIEW = scrollOverviewForMonitor(pMonitor);
    if (!OVERVIEW || renderingOverview)
        rc<origRenderWorkspace>(g_pScrollRenderWorkspaceHook->m_original)(thisptr, pMonitor, pWorkspace, now, geometry);
    else {
        const bool PREVRENDERINGOVERVIEW = renderingOverview;
        const auto PREVRENDERINGMONITOR  = renderingOverviewMonitor;
        const auto PREVOVERVIEW          = g_pScrollOverview;
        renderingOverview                = true;
        renderingOverviewMonitor         = pMonitor;
        g_pScrollOverview                = OVERVIEW;
        OVERVIEW->render();
        g_pScrollOverview = PREVOVERVIEW;
        renderingOverviewMonitor = PREVRENDERINGMONITOR;
        renderingOverview = PREVRENDERINGOVERVIEW;
    }
}

static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    const bool HANDLED = scrollOverviews().empty() ||
        std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && overview->blockDamageReporting; }) ||
        std::ranges::all_of(scrollOverviews(), [&surface](const auto& overview) { return !overview || overview->shouldHandleSurfaceDamage(surface); });
    if (HANDLED) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = !scrollOverviews().empty();
        rc<origDamageSurface>(g_pScrollDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

static void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (scrollOverviewForMonitor(monitor))
        return;

    rc<origSendFrameEventsToWorkspace>(g_pScrollSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
}

static void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();

    if (std::ranges::any_of(scrollOverviews(), [&SURFACE, &now](const auto& overview) { return overview && !overview->shouldAllowSurfaceFrame(SURFACE, now); }))
        return;

    rc<origSurfaceFrame>(g_pScrollSurfaceFrameHook->m_original)(thisptr, now);
}

static void hkDrawTex(void* thisptr, WP<CTexPassElement> element, const CRegion& damage) {
    if (!renderingOverview || !element || !g_pHyprRenderer) {
        rc<origDrawTex>(g_pScrollDrawTexHook->m_original)(thisptr, element, damage);
        return;
    }

    auto& renderModif = g_pHyprRenderer->m_renderData.renderModif;
    if (!renderModif.enabled || renderModif.modifs.empty()) {
        rc<origDrawTex>(g_pScrollDrawTexHook->m_original)(thisptr, element, damage);
        return;
    }

    // Hyprland applies renderModif to the texture box, but its texture path
    // consumes clipRegion and clipBox as already-transformed framebuffer
    // coordinates. Keep those masks attached to camera-transformed surfaces.
    const auto previousElementClipRegion = element->m_data.clipRegion.copy();
    const auto previousElementClipBox    = element->m_data.clipBox;

    auto transformedDamage = damage.copy();
    if (g_pHyprRenderer->m_renderData.currentWindow && element->m_data.surface) {
        // The backing workspace's visibility calculation may expose only the
        // part of a window that intersected its original monitor. Canvas mode
        // deliberately renders that window elsewhere, so use the complete
        // transformed surface as its authoritative clip and damage region.
        auto transformedSurfaceBox = element->m_data.box;
        renderModif.applyToBox(transformedSurfaceBox);
        transformedSurfaceBox.round();
        element->m_data.clipRegion = CRegion{transformedSurfaceBox};
        element->m_data.clipBox    = {};
        g_pHyprRenderer->m_renderData.clipBox = {};
        transformedDamage                    = CRegion{transformedSurfaceBox};
    } else {
        if (!element->m_data.clipRegion.empty())
            renderModif.applyToRegion(element->m_data.clipRegion);
        if (!element->m_data.clipBox.empty())
            renderModif.applyToBox(element->m_data.clipBox);
        else if (!g_pHyprRenderer->m_renderData.clipBox.empty())
            renderModif.applyToBox(g_pHyprRenderer->m_renderData.clipBox);
    }

    rc<origDrawTex>(g_pScrollDrawTexHook->m_original)(thisptr, element, transformedDamage);

    element->m_data.clipRegion = previousElementClipRegion;
    element->m_data.clipBox    = previousElementClipBox;
}

static void hkElementDrawTex(void* thisptr, WP<CTexPassElement> element, const CRegion& damage) {
    if (!renderingOverview || !element || !g_pHyprRenderer) {
        rc<origElementDrawTex>(g_pScrollElementDrawTexHook->m_original)(thisptr, element, damage);
        return;
    }

    auto& renderModif = g_pHyprRenderer->m_renderData.renderModif;
    if (!renderModif.enabled || renderModif.modifs.empty() || !g_pHyprRenderer->m_renderData.currentWindow || !element->m_data.surface) {
        rc<origElementDrawTex>(g_pScrollElementDrawTexHook->m_original)(thisptr, element, damage);
        return;
    }

    // SurfacePassElement and the blur path both inspect the texture box before
    // CGLElementRenderer::draw (and therefore hkDrawTex) is reached. A window
    // shown through another monitor's canvas camera can otherwise be rejected
    // at its source coordinates, leaving only plugin-rendered decorations.
    // Materialize the camera transform for this texture while it passes through
    // that early stage, then suppress the later transform to avoid applying it
    // twice. All queued element state is restored synchronously afterwards.
    const auto previousRenderModif       = renderModif;
    const auto previousElementBox        = element->m_data.box;
    const auto previousElementClipRegion = element->m_data.clipRegion.copy();
    const auto previousElementClipBox    = element->m_data.clipBox;
    const auto previousRendererClipBox   = g_pHyprRenderer->m_renderData.clipBox;

    renderModif.applyToBox(element->m_data.box);
    element->m_data.box.round();
    if (!element->m_data.clipRegion.empty())
        renderModif.applyToRegion(element->m_data.clipRegion);
    if (!element->m_data.clipBox.empty())
        renderModif.applyToBox(element->m_data.clipBox);

    const CRegion transformedDamage{element->m_data.box};
    g_pHyprRenderer->m_renderData.clipBox = {};
    renderModif                           = {};

    rc<origElementDrawTex>(g_pScrollElementDrawTexHook->m_original)(thisptr, element, transformedDamage);

    renderModif                              = previousRenderModif;
    element->m_data.box                      = previousElementBox;
    element->m_data.clipRegion               = previousElementClipRegion;
    element->m_data.clipBox                  = previousElementClipBox;
    g_pHyprRenderer->m_renderData.clipBox     = previousRendererClipBox;
}

// Title-bar drags of apps that draw their own (xdg move/resize requests) run
// as canvas drags; see CScrollOverview::beginClientWindowGesture. Hyprland
// inlines the request handlers, so the drag start is where to catch them.
typedef void (*origBeginDragTarget)(void*, SP<Layout::ITarget>, eMouseBindMode, std::optional<Layout::eRectCorner>, bool);
static void hkBeginDragTarget(void* thisptr, SP<Layout::ITarget> target, eMouseBindMode mode, std::optional<Layout::eRectCorner> edge, bool exclusiveGrab) {
    if (target && target->window()) {
        const bool MOVE   = mode == MBIND_MOVE;
        const bool RESIZE = mode == MBIND_RESIZE && edge && *edge != Layout::CORNER_NONE;
        if ((MOVE || RESIZE) && canvasTakeClientWindowGesture(target->window(), RESIZE ? edge : std::nullopt))
            return;
    }
    rc<origBeginDragTarget>(g_pBeginDragTargetHook->m_original)(thisptr, target, mode, edge, exclusiveGrab);
}

// X11 windows report where the canvas draws them; see canvasX11Configure.
typedef void (*origX11Configure)(void*, const CBox&);
static void hkX11Configure(void* thisptr, const CBox& box) {
    rc<origX11Configure>(g_pX11ConfigureHook->m_original)(thisptr, canvasX11Configure(thisptr, box));
}

typedef void (*origX11ConfigureRequest)(void*, CBox);
static void hkX11ConfigureRequest(void* thisptr, CBox box) {
    rc<origX11ConfigureRequest>(g_pX11ConfigureRequestHook->m_original)(thisptr, canvasX11Request(thisptr, box));
}

// Hyprland re-applies an X11 app's last fullscreen request on every
// _NET_WM_STATE message, including ones about something else: a game
// flashing for attention when combat starts would drop out of the fullscreen
// the user chose with SUPER + F. Only messages about fullscreen may change it.
// (Not tied to the canvas: always hooked.)
typedef void (*origX11ClientMessage)(void*, xcb_client_message_event_t*);
static void hkX11ClientMessage(void* thisptr, xcb_client_message_event_t* e) {
    const auto CALL = [&] { rc<origX11ClientMessage>(g_pX11ClientMessageHook->m_original)(thisptr, e); };
    const auto NETWMSTATE = HYPRATOMS.find("_NET_WM_STATE");
    const auto FULLSCREEN = HYPRATOMS.find("_NET_WM_STATE_FULLSCREEN");
    if (!e || e->format != 32 || NETWMSTATE == HYPRATOMS.end() || FULLSCREEN == HYPRATOMS.end() || !FULLSCREEN->second || e->type != NETWMSTATE->second ||
        e->data.data32[1] == FULLSCREEN->second || e->data.data32[2] == FULLSCREEN->second)
        return CALL();

    SP<CXWaylandSurface> surface;
    for (const auto& window : Desktop::windowState()->windows()) {
        if (window && window->m_isX11 && window->m_xwaylandSurface && window->m_xwaylandSurface->m_xID == e->window) {
            surface = window->m_xwaylandSurface.lock();
            break;
        }
    }
    if (!surface)
        return CALL();

    const auto REQUESTED = surface->m_state.requestsFullscreen;
    surface->m_state.requestsFullscreen.reset();
    CALL();
    surface->m_state.requestsFullscreen = REQUESTED;
}

typedef void (*origPopupReposition)(void*);
static void hkPopupReposition(void* thisptr) {
    if (!canvasRepositionPopup(rc<Desktop::View::CPopup*>(thisptr)))
        rc<origPopupReposition>(g_pPopupRepositionHook->m_original)(thisptr);
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = sc<Monitor::CMonitor*>( thisptr );
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());

    if (OVERVIEW && renderingOverviewMonitor.lock() == PMONITOR->m_self.lock() && !damageFromSurface && OVERVIEW->shouldSuppressRenderDamage()) {
        return;
    }

    if (!OVERVIEW || OVERVIEW->blockDamageReporting || damageFromSurface) {
        rc<origAddDamageA>(g_pScrollAddDamageHookA->m_original)(thisptr, box);
        return;
    }

    OVERVIEW->onDamageReported();
    rc<origAddDamageA>(g_pScrollAddDamageHookA->m_original)(thisptr, box);
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = sc<Monitor::CMonitor*>(thisptr);
    const auto OVERVIEW = scrollOverviewForMonitor(PMONITOR->m_self.lock());

    if (OVERVIEW && renderingOverviewMonitor.lock() == PMONITOR->m_self.lock() && !damageFromSurface && OVERVIEW->shouldSuppressRenderDamage()) {
        return;
    }

    if (!OVERVIEW || OVERVIEW->blockDamageReporting || damageFromSurface) {
        rc<origAddDamageB>(g_pScrollAddDamageHookB->m_original)(thisptr, rg);
        return;
    }

    OVERVIEW->onDamageReported();
    rc<origAddDamageB>(g_pScrollAddDamageHookB->m_original)(thisptr, rg);
}

static std::pair<std::string, std::string> splitOverviewArg(const std::string& arg) {
    const auto FIRST = arg.find_first_not_of(" \t");
    if (FIRST == std::string::npos)
        return {"on", ""};

    const auto SEPARATOR = arg.find_first_of(" \t", FIRST);
    if (SEPARATOR == std::string::npos)
        return {arg.substr(FIRST), ""};

    const auto TARGET = arg.find_first_not_of(" \t", SEPARATOR);
    if (TARGET == std::string::npos)
        return {arg.substr(FIRST, SEPARATOR - FIRST), ""};

    const auto LAST = arg.find_last_not_of(" \t");
    return {arg.substr(FIRST, SEPARATOR - FIRST), arg.substr(TARGET, LAST - TARGET + 1)};
}

static std::vector<PHLMONITOR> overviewTargetMonitors(const std::string& target) {
    if (target == "all")
        return State::monitorState()->monitors();

    if (!target.empty()) {
        const auto& MONITORS = State::monitorState()->monitors();
        const auto  IT       = std::ranges::find_if(MONITORS, [&target](const auto& monitor) { return monitor && monitor->m_name == target; });
        return IT == MONITORS.end() ? std::vector<PHLMONITOR>{} : std::vector<PHLMONITOR>{*IT};
    }

    const auto MONITOR = Desktop::focusState()->monitor();
    return MONITOR ? std::vector<PHLMONITOR>{MONITOR} : std::vector<PHLMONITOR>{};
}

static SP<IOverview> dispatcherOverview() {
    const auto CURRENTKEYBIND = g_pKeybindManager ? g_pKeybindManager->m_currentKeybind : SP<SKeybind>{};
    if (CURRENTKEYBIND && CURRENTKEYBIND->key.starts_with("mouse"))
        return g_pInputManager ? scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()) : SP<IOverview>{};

    return activeScrollOverview();
}

void canvasReclaimScreen(const PHLMONITOR& monitor); // scrollOverview.cpp

static bool openOverview(PHLMONITOR monitor) {
    if (!monitor || scrollOverviewForMonitor(monitor))
        return true;

    if (!ensureScrollOverviewHooks())
        return false;

    const bool PREVRENDERINGOVERVIEW = renderingOverview;
    renderingOverview                = true;
    auto overview                    = makeShared<CScrollOverview>(monitor->m_activeWorkspace, false, monitor);
    registerScrollOverview(overview);
    renderingOverview = PREVRENDERINGOVERVIEW;
    canvasReclaimScreen(monitor);
    return true;
}

bool openCanvasOverview(PHLMONITOR monitor) {
    return openOverview(monitor);
}

void canvasFullscreenEvent(PHLWINDOW window); // scrollOverview.cpp
bool canvasPointerMoved();
std::string canvasStateJson();
static SP<SHyprCtlCommand> g_stateCommand;
void canvasFullscreenReset();
bool canvasToggleFill(PHLWINDOW window);
bool canvasTogglePin(PHLWINDOW window);

static SDispatchResult onOverviewDispatcher(std::string arg) {
    const auto [ACTION, TARGET] = splitOverviewArg(arg);

    if (ACTION == "select") {
        const auto OVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
        if (OVERVIEW && OVERVIEW->m_isSwiping)
            return {.success = false, .error = "already swiping"};
        if (OVERVIEW)
            OVERVIEW->selectHoveredWorkspace();
        return {};
    }

    const auto ACTIVE = dispatcherOverview();

    if (ACTIVE && ACTIVE->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (ACTION == "off" || ACTION == "close" || ACTION == "disable") {
        if (TARGET.empty() || TARGET == "all") {
            closeAll();
            return {};
        }

        const auto MONITORS = overviewTargetMonitors(TARGET);
        if (MONITORS.empty())
            return {.success = false, .error = "monitor not found: " + TARGET};
        if (const auto overview = scrollOverviewForMonitor(MONITORS.front()))
            overview->close();
        return {};
    }

    if (ACTION != "toggle" && ACTION != "on" && ACTION != "open" && ACTION != "enable")
        return {.success = false, .error = "invalid arg. expected toggle|open|close|select [monitor|all]"};

    const auto MONITORS = overviewTargetMonitors(TARGET);
    if (MONITORS.empty())
        return {.success = false, .error = TARGET.empty() ? "no active monitor" : "monitor not found: " + TARGET};

    const bool ALL_OPEN = std::ranges::all_of(MONITORS, [](const auto& monitor) { return !!scrollOverviewForMonitor(monitor); });
    if (ACTION == "toggle" && ALL_OPEN) {
        for (const auto& monitor : MONITORS) {
            if (const auto overview = scrollOverviewForMonitor(monitor)) {
                if (overview->isClosing())
                    overview->reopen();
                else if (auto* canvas = dynamic_cast<CScrollOverview*>(overview.get()); canvas && canvas->isPersistentCanvas())
                    canvas->toggleCanvasNavigation();
                else
                    overview->close();
            }
        }
        return {};
    }

    for (const auto& monitor : MONITORS) {
        if (!openOverview(monitor))
            return {.success = false, .error = "failed enabling overview hooks (is other overview plugin enabled?)"};
    }

    // A persistent canvas opens at 100%. When the toggle key created it, the
    // user asked to see the map, so continue straight into navigation.
    if (ACTION == "toggle") {
        for (const auto& monitor : MONITORS) {
            auto* canvas = dynamic_cast<CScrollOverview*>(scrollOverviewForMonitor(monitor).get());
            if (canvas && canvas->isPersistentCanvas() && !canvas->isCanvasNavigationActive() && !canvas->isClosing())
                canvas->toggleCanvasNavigation();
        }
    }
    return {};
}

static SDispatchResult onNavigateDispatcher(std::string arg) {
    // Window ownership is only a surface-storage detail in the shared canvas.
    // Route arrows through the last camera the user actually interacted with,
    // and keep that output locked for the duration of the Super key hold.
    const auto OVERVIEW = canvasNavigationOverview();

    if (!OVERVIEW)
        return {};

    if (arg != "left" && arg != "right" && arg != "up" && arg != "down")
        return {.success = false, .error = "invalid arg. expected left|right|up|down"};

    OVERVIEW->moveSelection(arg);
    return {};
}

static SDispatchResult onWindowDispatcher(std::string arg) {
    const auto OVERVIEW = scrollOverviewAt(g_pInputManager->getMouseCoordsInternal());
    if (!OVERVIEW)
        return {};

    if (arg != "select" && arg != "close")
        return {.success = false, .error = "invalid arg. expected select|close"};

    OVERVIEW->windowDispatcherAction(arg);
    return {};
}

static PHLWINDOWREF g_flightDeckNativeWindow;
static Fullscreen::SFullscreenMode g_flightDeckPreviousModes;
static bool g_flightDeckNativePending = false;
struct SFlightDeckRequest { PHLWINDOWREF window; Fullscreen::eFullscreenMode mode; };
static wl_event_source* g_flightDeckIdle = nullptr;
static std::unique_ptr<SFlightDeckRequest> g_flightDeckRequest;

void requestFlightDeckNative(PHLWINDOW window, Fullscreen::eFullscreenMode mode) {
    if (g_flightDeckNativePending) return;
    g_flightDeckNativePending = true;
    g_flightDeckRequest = std::make_unique<SFlightDeckRequest>(window, mode);
    g_flightDeckIdle = wl_event_loop_add_idle(g_pCompositor->m_wlEventLoop, [](void*) {
        g_flightDeckIdle = nullptr;
        const auto request = std::move(g_flightDeckRequest);
        clearScrollOverviews();
        disableScrollOverviewHooks();
        const auto w = request->window.lock();
        if (validMapped(w)) {
            g_flightDeckNativeWindow = w;
            g_flightDeckPreviousModes = Fullscreen::controller()->getFullscreenModes(w);
            Desktop::focusState()->fullWindowFocus(w, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
            if (request->mode != Fullscreen::FSMODE_NONE)
                Fullscreen::controller()->setFullscreenMode(w, request->mode, request->mode);
        }
        g_flightDeckNativePending = false;
    }, nullptr);
    if (!g_flightDeckIdle) {
        g_flightDeckRequest.reset();
        g_flightDeckNativePending = false;
    }
}

static SDispatchResult onCanvasDispatcher(std::string arg) {
    if (arg.starts_with("experiment ")) {
        const auto PREVIOUS = SpatialOverview::Experiments::id();
        const auto NAME = arg.substr(11);
        if (!SpatialOverview::Experiments::setByName(NAME))
            return {.success = false, .error = "unknown experiment"};
        onCanvasExperimentChanged(PREVIOUS);
        HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, SpatialOverview::Experiments::title() + ". " + SpatialOverview::Experiments::help(), CHyprColor{0.55F, 0.9F, 1.F, 1.F}, 7000);
        return {};
    }
    if (arg == "switch next" || arg == "switch prev") {
        auto* CANVAS = dynamic_cast<CScrollOverview*>(dispatcherOverview().get());
        if (!CANVAS)
            return {.success = false, .error = "Open the canvas before using this action"};
        return CANVAS->switchWindow(arg == "switch next" ? 1 : -1) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "No window to switch to"};
    }
    if (arg == "alttab next" || arg == "alttab prev") {
        if (!SpatialOverview::Experiments::on(ECanvasExperiment::AltTab))
            return {.success = false, .error = "Alt-Tab experiment is not selected"};
        const auto OVERVIEW = dispatcherOverview();
        auto* CANVAS = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr;
        if (!CANVAS)
            return {.success = false, .error = "Open the canvas before using this action"};
        return CANVAS->cycleAltTab(arg == "alttab next" ? 1 : -1) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "No window to cycle"};
    }

    const auto OVERVIEW = dispatcherOverview();
    auto*      CANVAS   = OVERVIEW ? dynamic_cast<CScrollOverview*>(OVERVIEW.get()) : nullptr;
    if (arg == "restore") {
        if (const auto w = g_flightDeckNativeWindow.lock(); validMapped(w))
            Fullscreen::controller()->setFullscreenMode(w, g_flightDeckPreviousModes.internal, g_flightDeckPreviousModes.client);
        return {};
    }
    if (arg == "native" || arg == "maximize" || arg == "fullscreen") {
        requestFlightDeckNative(Desktop::focusState()->window(), arg == "fullscreen" ? Fullscreen::FSMODE_FULLSCREEN : arg == "maximize" ? Fullscreen::FSMODE_MAXIMIZED : Fullscreen::FSMODE_NONE);
        return {};
    }
    if (arg == "fill")
        return canvasToggleFill(Desktop::focusState()->window()) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    if (arg == "pin")
        return canvasTogglePin(Desktop::focusState()->window()) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    // For keys that have nothing to do on the canvas (tiling): succeeds, so
    // canvas_or() bindings skip their fallback, only while a canvas is open.
    if (arg == "noop")
        return std::ranges::any_of(scrollOverviews(), [](const auto& overview) { return overview && !overview->isClosing(); }) ? SDispatchResult{}
                                                                                                                                : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    if (!CANVAS)
        return arg == "refresh" ? SDispatchResult{} : SDispatchResult{.success = false, .error = "Open the canvas before using this action"};
    if (arg == "back" || arg == "land" || arg == "frame" || arg == "undo" || arg == "redo" || arg == "fit" || arg == "summon" || arg == "search" || arg == "tune" ||
        arg.starts_with("search ") || arg.starts_with("zoom ") || arg.starts_with("pan ") || arg.starts_with("nudge ") || arg.starts_with("area ") || arg.starts_with("go ") ||
        arg.starts_with("send "))
        return CANVAS->flightDeckAction(arg) ? SDispatchResult{} : SDispatchResult{.success = false, .error = "No matching window, area or undo state"};

    std::istringstream stream{arg};
    std::string        action;
    stream >> action;

    if (action == "refresh") {
        CANVAS->refreshCanvasSettings();
        return {};
    }

    if (action == "arrange") {
        if (!CANVAS->arrangeCanvasWindows())
            return {.success = false, .error = CANVAS->canvasPlacementError().empty() ? "canvas arrangement failed" : CANVAS->canvasPlacementError()};
        return {};
    }

    int                column = 0;
    int                row    = 0;
    stream >> column >> row;
    if ((action != "place" && action != "viewport") || stream.fail())
        return {.success = false, .error = "expected: place <column> <row> | viewport <x> <y> | refresh | arrange"};

    const bool SUCCESS = action == "place" ? CANVAS->placeWindowOnCanvasCell(column, row) : CANVAS->setCanvasViewport(column, row);
    if (!SUCCESS)
        return {.success = false, .error = CANVAS->canvasPlacementError().empty() ? "canvas action failed" : CANVAS->canvasPlacementError()};

    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, "[spatialoverview] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

// Helper function to find a function by name and ensure it contains one of the requested substrings in its demangled name (to disambiguate overloads).
static void* findFnOrThrow(const std::string& name, std::initializer_list<std::string_view> demangledNeedles) {
    auto fns = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name);
    if (fns.empty()) {
        failNotif(std::format("no fns for hook {}", name));
        throw std::runtime_error(std::format("[spatialoverview] No fns for hook {}", name));
    }

    if (demangledNeedles.size() == 0 || (demangledNeedles.size() == 1 && demangledNeedles.begin()->empty()))
        return fns[0].address;

    std::vector<SFunctionMatch> matches;
    matches.reserve(fns.size());
    for (const auto& fn : fns) {
        for (const auto& needle : demangledNeedles) {
            if (needle.empty() || fn.demangled.find(needle) != std::string::npos) {
                matches.push_back(fn);
                break;
            }
        }
    }

    if (matches.empty()) {
        failNotif(std::format("no matching overload for hook {}", name));
        throw std::runtime_error(std::format("[spatialoverview] No matching overload for hook {}", name));
    }

    if (matches.size() > 1) {
        failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
        throw std::runtime_error(std::format("[spatialoverview] Ambiguous overload for hook {}", name));
    }

    return matches[0].address;
}

// shared core: register / unregister the overview trackpad gesture. used by both the hyprlang
// keyword and the Lua `spatialoverview.gesture` function so both configure the same gesture system.
static std::expected<void, std::string> applyOverviewGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, uint32_t modMask,
                                                             float deltaScale, bool disableInhibit) {
    if (fingerCount <= 1 || fingerCount >= 10)
        return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return std::unexpected("Invalid direction");

    if (action == "overview")
        return g_pTrackpadGestures->addGesture(makeUnique<COverviewGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);

    if (action == "unset")
        return g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);

    return std::unexpected(std::format("Invalid gesture: {}", action));
}

// Lua-facing registrar (spatialoverview.gesture). takes the direction/mods as strings and resolves
// them the same way the keyword does, then defers to applyOverviewGesture.
static SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale,
                                                 bool disableInhibit) {
    if (g_unloading)
        return {};

    const auto direction = g_pTrackpadGestures->dirForString(directionStr);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", directionStr)};

    uint32_t modMask = 0;
    if (!mods.empty() && g_pKeybindManager)
        modMask = g_pKeybindManager->stringToModMask(mods);

    const auto res = applyOverviewGesture(fingerCount, direction, action, modMask, std::clamp(deltaScale, 0.1F, 10.F), disableInhibit);
    if (!res)
        return {.success = false, .error = res.error()};

    return {};
}

static Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    if (g_unloading)
        return result;

    CConstVarList             data(RHS);

    size_t                    fingerCount = 0;

    try {
        fingerCount = std::stoul(std::string{data[0]});
    } catch (...) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    const auto direction = g_pTrackpadGestures->dirForString(data[1]);

    if (direction == TRACKPAD_GESTURE_DIR_NONE) {
        result.setError(std::format("Invalid direction: {}", data[1]).c_str());
        return result;
    }

    int      startDataIdx   = 2;
    uint32_t modMask        = 0;
    float    deltaScale     = 1.F;
    bool     disableInhibit = false;

    while (true) {

        if (data[startDataIdx].starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(std::string{data[startDataIdx].substr(4)});
            startDataIdx++;
            continue;
        } else if (data[startDataIdx].starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(std::string{data[startDataIdx].substr(6)}), 0.1F, 10.F);
                startDataIdx++;
                continue;
            } catch (...) {
                result.setError(std::format("Invalid delta scale: {}", std::string{data[startDataIdx].substr(6)}).c_str());
                return result;
            }
        } else if (data[startDataIdx] == "disable_inhibit") {
            disableInhibit = true;
            startDataIdx++;
            continue;
        }

        break;
    }

    const auto resultFromGesture = applyOverviewGesture(fingerCount, direction, std::string{data[startDataIdx]}, modMask, deltaScale, disableInhibit);

    if (!resultFromGesture)
        result.setError(resultFromGesture.error().c_str());

    return result;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    SCROLLOVERVIEW_HANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        // The usual cause: Hyprland was updated after this build.
        failNotif("this build is for a different Hyprland version (Hyprland was probably updated). Rebuild it: run scripts/install.sh in the "
                  "Phantomat source folder");
        throw std::runtime_error("[he] Version mismatch");
    }

    g_pScrollRenderWorkspaceHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("renderWorkspace", {"CHyprRenderer::renderWorkspace(", "IHyprRenderer::renderWorkspace("}),
        rc<void*>(hkRenderWorkspace));

    g_pScrollScheduleFrameHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE, 
        findFnOrThrow("_ZN7Monitor8CMonitor13scheduleFrameEN10Aquamarine7IOutput19scheduleFrameReasonE", {""}),
        rc<void*>(hkScheduleFrame));

    g_pScrollDamageSurfaceHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("damageSurface", {"CHyprRenderer::damageSurface(", "IHyprRenderer::damageSurface("}),
        rc<void*>(hkDamageSurface));

    g_pScrollSendFrameEventsHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("sendFrameEventsToWorkspace", {"CHyprRenderer::sendFrameEventsToWorkspace(", "IHyprRenderer::sendFrameEventsToWorkspace("}),
        rc<void*>(hkSendFrameEventsToWorkspace));

    g_pScrollSurfaceFrameHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE", {""}),
        rc<void*>(hkSurfaceFrame));

    g_pScrollAddDamageHookB = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("addDamageEPK15pixman_region32", {"CMonitor::addDamage"}),
        rc<void*>(hkAddDamageB));

    g_pScrollAddDamageHookA = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN7Monitor8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", {""}),
        rc<void*>(hkAddDamageA));

    g_pScrollDrawTexHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("draw", {"CGLElementRenderer::draw(Hyprutils::Memory::CWeakPointer<CTexPassElement>"}),
        rc<void*>(hkDrawTex));

    g_pScrollElementDrawTexHook = createOverviewFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("drawTex", {"IElementRenderer::drawTex(Hyprutils::Memory::CWeakPointer<CTexPassElement>"}),
        rc<void*>(hkElementDrawTex));

    g_pBeginDragTargetHook = createOverviewFunctionHook(SCROLLOVERVIEW_HANDLE,
                                                             findFnOrThrow("beginDragTarget", {"CLayoutManager::beginDragTarget("}),
                                                             rc<void*>(hkBeginDragTarget));

    g_pX11ConfigureHook = createOverviewFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow("configure", {"CXWaylandSurface::configure("}),
                                                          rc<void*>(hkX11Configure));
    g_pX11ConfigureRequestHook = createOverviewFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow("onX11ConfigureRequest", {"CWindow::onX11ConfigureRequest("}),
                                                                 rc<void*>(hkX11ConfigureRequest));

    g_pX11ClientMessageHook = createOverviewFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow("handleClientMessage", {"CXWM::handleClientMessage("}),
                                                              rc<void*>(hkX11ClientMessage));
    if (!g_pX11ClientMessageHook || !g_pX11ClientMessageHook->hook())
        Log::logger->log(Log::WARN, "[spatialoverview] could not hook X11 client messages; X11 apps flashing for attention may leave fullscreen");

    // Popups of canvas windows are kept on screen as the canvas shows them.
    g_pPopupRepositionHook = createOverviewFunctionHook(SCROLLOVERVIEW_HANDLE, findFnOrThrow("reposition", {"Desktop::View::CPopup::reposition()"}),
                                                             rc<void*>(hkPopupReposition));

    static auto P = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (const auto overview = scrollOverviewForMonitor(monitor))
            overview->onPreRender();
    });
    static auto HUDSTAGE = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_LAST_MOMENT)
            SpatialOverview::BarrelShader::applyPendingHud();
    });
    const auto dropMonitorOverview = [](PHLMONITOR monitor) {
        g_overviewMonitorTeardown = true;
        unregisterScrollOverviewForMonitor(monitor);
        g_overviewMonitorTeardown = false;
    };
    static auto MONITORREMOVED   = Event::bus()->m_events.monitor.removed.listen(dropMonitorOverview);
    static auto MONITORDESTROYED = Event::bus()->m_events.monitor.destroyMon.listen(dropMonitorOverview);

    SpatialOverview::Experiments::load();

    // Fullscreen on the canvas: only the screen showing the window steps
    // aside, and comes back after (scrollOverview.cpp).
    static auto CANVASFULLSCREEN = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW window) {
        if (!g_unloading)
            canvasFullscreenEvent(window);
    });
    static auto CANVASFULLSCREENCLOSE = Event::bus()->m_events.window.close.listen([](PHLWINDOW) {
        if (!g_unloading)
            canvasFullscreenEvent(nullptr);
    });
    static auto CANVASFULLSCREENMOVE = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) {
        if (!g_unloading)
            canvasFullscreenEvent(nullptr);
    });
    static auto CANVASFULLSCREENWORKSPACE = Event::bus()->m_events.workspace.active.listen([](PHLWORKSPACE) {
        if (!g_unloading)
            canvasFullscreenEvent(nullptr);
    });
    // Screens given to a fullscreen window: a game hiding its cursor keeps
    // the pointer on its screen, and the pointer moving onto one focuses its
    // window. Registered before any canvas, so the canvases see the pointer
    // where it stays.
    static auto CANVASFULLSCREENPOINTER = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo& info) {
        if (!g_unloading && canvasPointerMoved())
            info.cancelled = true;
    });

    // Recency for the navigator is tracked for the whole session, not only
    // while the canvas is open, so "recent first" means what it says.
    static auto NAVIGATORFOCUS = Event::bus()->m_events.window.active.listen([](PHLWINDOW window, Desktop::eFocusReason) { SpatialOverview::Navigator::noteFocus(window); });
    static auto NAVIGATORCLOSE = Event::bus()->m_events.window.close.listen([](PHLWINDOW window) { SpatialOverview::Navigator::forget(window); });
    SpatialOverview::Navigator::noteFocus(Desktop::focusState()->window());
    SpatialOverview::Navigator::setDamageCallback([] {
        if (const auto overview = activeScrollOverview())
            overview->damage();
    });

    // hyprctl spatialoverview: the canvases' state as JSON, for scripts and tests.
    g_stateCommand = HyprlandAPI::registerHyprCtlCommand(SCROLLOVERVIEW_HANDLE, SHyprCtlCommand{.name = "spatialoverview", .exact = true, .fn = [](eHyprCtlOutputFormat, std::string) {
                                                             return canvasStateJson();
                                                         }});

    ScrollOverview::Config::registerDispatcher("overview", ::onOverviewDispatcher);
    ScrollOverview::Config::registerDispatcher("navigate", ::onNavigateDispatcher);
    ScrollOverview::Config::registerDispatcher("window", ::onWindowDispatcher);
    ScrollOverview::Config::registerDispatcher("canvas", ::onCanvasDispatcher);
    ScrollOverview::Config::registerGesture(::onRegisterOverviewGesture, ::overviewGestureKeyword);
    ScrollOverview::Config::registerConfig();

    return {"spatialoverview", "Phantomat: a zoomable, infinite-canvas window manager for Hyprland", "Kaolti, yayuuu, Vaxry", SCROLLOVERVIEW_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_flightDeckIdle) wl_event_source_remove(g_flightDeckIdle);
    g_flightDeckIdle = nullptr;
    g_flightDeckRequest.reset();
    g_flightDeckNativePending = false;
    g_pHyprRenderer->m_renderPass.removeAllOfType("CScrollOverviewPassElement");

    g_unloading = true;
    if (g_stateCommand)
        HyprlandAPI::unregisterHyprCtlCommand(SCROLLOVERVIEW_HANDLE, g_stateCommand);
    g_stateCommand.reset();
    canvasFullscreenReset();
    clearScrollOverviews();
    disableScrollOverviewHooks();
    canvasReleaseX11Windows();
    releaseOverviewFunctionHooks();

    disarmCanvasTimers();
    SpatialOverview::Navigator::shutdown();
    if (const auto OPENGL = g_pHyprRenderer ? g_pHyprRenderer->glBackend() : WP<Render::GL::CHyprOpenGLImpl>{})
        OPENGL->makeEGLCurrent();
    SpatialOverview::BarrelShader::discardPendingHud();
    SpatialOverview::Hud::shutdown();

    if (g_pTrackpadGestures)
        g_pTrackpadGestures->clearGestures();

    HyprlandAPI::reloadConfig(); // re-adds built-in gestures cleared above
}
