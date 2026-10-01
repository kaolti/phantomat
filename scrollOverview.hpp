#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/types.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config.hpp"
#include "DropIndicator.hpp"
#include "IOverview.hpp"

class CMonitor;
struct wl_event_source;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false, PHLMONITOR monitor = {});
    ~CScrollOverview() override;

    void         render() override;
    void         damage() override;
    void         requestInputFrame() override;
    void         markBlurDirty();
    void         markBackdropBlurDirty();
    void         onDamageReported() override;
    bool         shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) override;
    bool         shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) override;
    bool         shouldAllowRealtimePreviewSchedule() override;
    bool         shouldSuppressRenderDamage() const override;
    void         onPreRender() override;

    void         setClosing(bool closing) override;

    void         resetSwipe() override;
    void         onSwipeUpdate(double delta) override;
    void         onSwipeEnd() override;

    // close without a selection
    void         close() override;
    bool         isClosing() const override;
    void         reopen() override;
    void         selectHoveredWorkspace() override;
    bool         moveSelection(const std::string& direction) override;
    bool         windowDispatcherAction(const std::string& action) override;
    bool         placeWindowOnCanvasCell(int x, int y);
    bool         setCanvasViewport(int x, int y);
    bool         arrangeCanvasWindows();
    const std::string& canvasPlacementError() const;
    float        overviewProgress() const;
    float        distortionProgress() const;
    bool         isCanvasDesktop() const;
    bool         isPersistentCanvas() const;
    bool         isCanvasNavigationActive() const;
    void         toggleCanvasNavigation();
    void         refreshCanvasSettings();
    bool         flightDeckAction(const std::string& action);
    bool         canvasPlaceAction(const std::string& action);
    bool   tunerKeyAction(uint32_t keysym, uint32_t mods, const std::string& text);
    bool         navigatorKeyAction(uint32_t keysym, uint32_t mods, const std::string& text, bool repeat);
    bool         openNavigator(const std::string& query = {});
    void         landOnWindow(PHLWINDOW window);
    void         summonWindow(PHLWINDOW window);
    void         revertAllNavigation();
    void         fitAllWindows();
    void         panCamera(const Vector2D& direction);
    void         zoomCameraBy(float factor);
    bool         nudgeWindow(PHLWINDOW window, const Vector2D& direction, bool checkpoint);
    bool         canvasRedo();
    bool         switchWindow(int direction);
    void         revealSwitcher();
    void         finishSwitcher(bool cancel = false);
    bool         cycleAltTab(int direction);
    void         noteCanvasLayoutChanged();
    Vector2D     restingCameraOffset() const;
    void         warpCameraOffset(const Vector2D& offset);
    bool         followCanvasWindow(PHLWINDOW window, bool syncFocus, bool animate = true);
    void         saveSharedCanvasLayout();
    void         loadSharedCanvasLayout();
    void         checkpointCanvas();
    Vector2D     navigationReturnOffset{};
    bool         hasNavigationReturn = false;

    void         fullRender() override;
    void         syncAnimationConfig() override;

    // Popups: Hyprland places and fades them in the window's real
    // coordinates, which on the canvas are not where the window is drawn.
    // The screen box (global logical) as this canvas shows it, in those
    // real coordinates; nullopt when this is not a canvas desktop.
    std::optional<CBox> canvasScreenToWorld(const CBox& screenGlobal, bool atGoal = false) const;
    CBox                canvasWorldToScreen(const CBox& world) const;
    bool                canvasDrawsWindow(const PHLWINDOW& window) const;
    static const CScrollOverview* canvasFrameOwner(const PHLWINDOW& window);
    bool                canvasPopupFading() const;
    // X11 windows: where this canvas draws a window (global logical), and
    // making this canvas the one an X11 window reports its position from.
    CBox                canvasDrawnGlobalBox(const PHLWINDOW& window) const;
    static bool         canvasWindowOnScreen(const PHLWINDOW& window);
    // Make a window this canvas's selection and focus it, camera untouched.
    void                canvasAdoptFocus(const PHLWINDOW& window);
    // At 100%, where windows are drawn 1:1.
    bool                canvasAtRestZoom() const;
    float               canvasZoom() const;
    void                followLinkedCamera();
    // Take `leader`'s camera from now on (its own camera change is not a lead).
    void                followLinkedLeader(const CScrollOverview* leader);
    // Go on to where `from` (a leader going away) was heading.
    void                inheritLinkedCamera(const CScrollOverview* from);
    void                syncCanvasWindowScreens();
    void                canvasClaimX11Window(const PHLWINDOW& window);
    // A client asked to be moved (or resized from an edge) by its own
    // title bar; run the canvas's drag with the button the app was pressed with.
    bool                beginClientWindowGesture(PHLWINDOW window, std::optional<Layout::eRectCorner> resizeEdge);
    PHLMONITOR          canvasMonitor() const {
        return pMonitor.lock();
    }

  private:
    void   rebuildWorkspaceImages();
    void   seedRememberedSelections();
    void   redrawAll(bool forcelowres = false);
    void   onWorkspaceChange();
    void   renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F);
    // Parallax: the wallpaper as a far layer, shifted by a fraction of the
    // camera's movement and scaled with the zoom. Inactive leaves it untouched.
    struct SBackdropTransform {
        Vector2D offset; // device px
        float    scale  = 1.F;
        bool     active = false;
    };
    SBackdropTransform canvasBackdropTransform(PHLMONITOR monitor, float renderScale) const;
    bool   updateBackdropSharpCache(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderBackdropTiled(PHLMONITOR monitor, const SP<Render::ITexture>& texture, float alpha, const SBackdropTransform& transform);
    void   updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now);
    void   renderBackdropBlurCache(PHLMONITOR monitor, float alpha, const SBackdropTransform& transform);
    void   renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderWorkspaceOutline(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale);
    void   renderCanvasBackgroundDim(PHLMONITOR monitor);
    void   renderCanvasGrid(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale);
    void   renderCanvasMinimap(PHLMONITOR monitor);
    void   renderExperimentChrome(PHLMONITOR monitor);
    bool   handleExperimentKey(uint32_t keysym, uint32_t mods);
    bool   handleExperimentClick(uint32_t button, uint32_t state, const Vector2D& rawLocal);
    void   updateExperimentDrag();
    bool   jumpCanvasMinimap(const Vector2D& rawLocal);
    CBox   currentCanvasViewportWorld() const;
    void   commitLanding();
    void   revertNavigation();
    bool   handleNavigatorKey(const IKeyboard::SKeyEvent& event, uint32_t keysym, uint32_t mods);
    void   followNavigatorSelection();
    bool   navigatorOwnsPointer() const;
    bool   showsNavigatorHud() const;
    void   updateNavigatorHover();
    void   setCanvasCursor(const std::string& shape);
    void   leaveNavigatorPointer();
    Vector2D canvasCameraOffsetFor(const PHLWINDOW& window, float zoom, bool navigating) const;
    void   renderNavigatorWindowOverlay(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox);
    void   renderNavigatorReticle(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderNavigatorHud(PHLMONITOR monitor);
    CBox   canvasMinimapPanelBox() const;
    CBox   canvasArrangeButtonBox() const;
    void   renderCanvasViewport(PHLMONITOR monitor, float renderScale);
    void   renderCanvasDesktopScene(PHLMONITOR monitor, float renderScale, const Time::steady_tp& now);
    void   renderChromeLayers(PHLMONITOR monitor, const Time::steady_tp& now);
    bool   hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const;
    void   renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr,
                             bool dragged = false);
    void   renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now);
    void   renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now);
    void   moveViewportWorkspace(bool up);
    void   moveViewportWorkspaceTo(size_t targetIndex);
    void   trackpadSwipeLayout(const PHLWORKSPACE target, const double delta);
    void   trackpadSwipeWorkspace(const double delta);
    void   finishWorkspaceScrollFollow();
    double trackpadWorkspaceScrollOffset(PHLMONITOR monitor, float renderScale);
    bool   scrollStepAllowed(uint32_t timeMs);
    bool   selectOverviewWindow(PHLWINDOW window, size_t workspaceIdx, bool syncFocus = false);
    bool   selectWindowAtOverviewCursor(bool syncFocus = false);
    PHLWINDOW windowClosestToWorkspaceCenter(size_t workspaceIdx) const;
    void   rememberSelection(PHLWINDOW window);
    void   syncSelectionToViewport();
    void   syncFocusedSelection();
    size_t dragWorkspaceIndex(PHLWINDOW window) const;
    void   updateWorkspaceOverflow();
    CBox   workspaceOverviewVisibleBox(size_t workspaceIdx, const CBox& workspaceBox, float renderScale, PHLMONITOR monitor) const;
    float      workspaceOverviewAxisOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    Vector2D   workspaceOverviewOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewLogicalOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    Vector2D   workspaceOverviewLogicalVectorOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewAlpha(size_t workspaceIdx) const;
    PHLWINDOW windowAtOverviewPoint(const Vector2D& point, size_t* workspaceIdx = nullptr) const;
    PHLWINDOW windowAtOverviewCursor(size_t* workspaceIdx = nullptr);
    PHLWINDOW windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr, CBox* windowBox = nullptr) const;
    CDropIndicator::SDropAnchor dropAnchorAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr,
                                                                      CScrollOverview* dragContext = nullptr);
    PHLWORKSPACE workspaceAtOverviewPoint(const Vector2D& point, size_t* workspaceIdx = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewDropPoint(const Vector2D& point, size_t* workspaceIdx = nullptr, const PHLWINDOW& draggedWindow = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewCursor(size_t* workspaceIdx = nullptr) const;
    CBox         canvasDesktopWindowBox(const PHLWINDOW& window) const;
    PHLWINDOW    canvasDesktopWindowAtPoint(const Vector2D& point, CBox* renderedBox = nullptr, Vector2D* surfaceLocal = nullptr) const;
    void         zoomCanvasAt(const Vector2D& point, float requestedZoom, bool animate = false);
    bool         manageCanvasWindow(PHLWINDOW window, bool placeNew);
    void         ensureCanvasKeyboardFocus(PHLWINDOW window = {});
    void         seedCanvasWindows();
    void         forwardCanvasPointerMotion(uint32_t timeMs = 0);
    bool         forwardCanvasPointerButton(const IPointer::SButtonEvent& event);
    bool         forwardCanvasPointerAxis(const IPointer::SAxisEvent& event);
    Vector2D     canvasCellForWorkspaceIndex(size_t workspaceIdx) const;
    Vector2D     canvasCellAtOverviewPoint(const Vector2D& point) const;
    CBox         snapCanvasWindowBox(const CBox& box, PHLMONITOR monitor) const;
    Vector2D  overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const;
    CBox      draggedWindowBox(size_t workspaceIdx) const;
    CBox      draggedWindowBoxFor(PHLWINDOW window, size_t workspaceIdx, const Vector2D& pointLocal, const Vector2D& grabRatio) const;
    CBox      draggedWindowGlobalBox() const;
    void      refreshDragOriginalOverviewBoxes();
    void      clearDragPending();
    void      beginWindowDrag(PHLWINDOW window);
    void      updateWindowDrag();
    void      endWindowDrag();
    CBox      resizedWindowBox() const;
    void      beginWindowResize();
    void      updateWindowResize();
    void      endWindowResize();
    void      updateScrollingPan();
    void      beginScrollingPan(PHLWORKSPACE workspace);
    void      endScrollingPan();
    void      updateViewportWorkspaceFromCanvasCenter();
    bool      commitCanvasViewport(size_t workspaceIdx);
    void      focusMostVisibleScrollingWindow(const PHLWORKSPACE& workspace);
    bool      moveScrollingColumnSelection(bool next);
    bool      moveScrollingStackSelection(bool next);
    void   forceSurfaceVisibility(SP<CWLSurfaceResource> surface);
    void   forceWindowSurfaceVisibility(PHLWINDOW window);
    void   forceWindowVisible(PHLWINDOW window);
    void   forceLayersAboveFullscreen();
    void   restoreForcedSurfaceVisibility();
    void   restoreForcedWindowVisibility();
    void   restoreForcedLayerVisibility();
    void   applyWorkspaceAnimationOverrides();
    void   restoreWorkspaceAnimationOverrides();
    void   forceWorkspaceAlphaVisible();
    void   forceWorkspaceWindowsDecoRecalc(const PHLWORKSPACE& workspace);
    void   emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen);
    void   applyInputConfigOverrides();
    void   restoreInputConfigOverrides();
    void   transferSharedStateOwnership();
    size_t activeWorkspaceIndex() const;
    bool   isSelectedWorkspace(const PHLWORKSPACE& workspace) const;
    void   sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool   isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const;
    bool   hasRunningWorkspaceAnimation() const;
    bool   shouldAllowRealtimePreviewFrame() const;
    void   scheduleMinimumPreviewFrame();
    void   schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void   scheduleRealtimePreviewFrame();
    void   releaseInputListeners();
    void   activateSubmapIfConfigured();
    void   restoreSubmapIfActive();
    bool   dispatchSubmapMouseClick(uint32_t button);
    static int realtimePreviewTimerCallback(void* data);

    size_t viewportCurrentWorkspace = 0;
    std::string canvasPlacementFailure;
    bool   rebuildPending           = false;
    bool   overviewBlurDirty        = true;
    bool   backdropBlurDirty        = true;
    bool   overviewBlurStateValid   = false;
    float  lastOverviewBlurScale    = 1.F;
    int    lastBackdropWallpaperMode = -1;
    float  lastBackdropBlurStrength = -1.F;
    bool   lastBackdropBlurEnabled  = false;
    Vector2D lastOverviewBlurViewOffset = Vector2D{};
    SP<Render::IFramebuffer> backdropBlurFB;
    SP<Render::IFramebuffer> backdropSharpFB;
    bool   backdropSharpDirty       = true;

    struct SWorkspaceImage {
        PHLWORKSPACE              pWorkspace;
        std::vector<PHLWINDOWREF> windows;
        float                     overflowLeft   = 0.F;
        float                     overflowRight  = 0.F;
        float                     overflowTop    = 0.F;
        float                     overflowBottom = 0.F;
    };

    struct SWorkspaceInsertTransition {
        bool                             active              = false;
        WORKSPACEID                      transitionWorkspaceID = WORKSPACE_INVALID;
        bool                             transitionFadeIn   = true;
        std::unordered_map<WORKSPACEID, float> oldRelativeOffsets;
        std::unordered_map<WORKSPACEID, float> newRelativeOffsets;
        float                            transitionOldRelativeOffset = 0.F;
    };

    Vector2D                         lastMousePosLocal = Vector2D{}; // monitor-local pixel space

    PHLWINDOWREF                     closeOnWindow;
    PHLWINDOWREF                     dragActiveWindow;
    PHLWORKSPACEREF                  dragOriginalWorkspace;
    PHLWORKSPACEREF                  scrollingPanWorkspace;
    PHLWINDOWREF                     scrollingPanInitialWindow;
    PHLWINDOWREF                     resizePendingWindow;
    PHLWINDOWREF                     resizeActiveWindow;

    Vector2D                         dragStartMouseLocal   = Vector2D{};
    Vector2D                         dragGrabOffsetLocal   = Vector2D{};
    Vector2D                         dragGrabRatio         = Vector2D{0.5, 0.5};
    Vector2D                         dragOriginalFloatSize = Vector2D{};
    Vector2D                         dragOriginalTapeTranslation = Vector2D{};
    Vector2D                         resizeStartMouseLocal = Vector2D{};
    Vector2D                         resizeLastMouseLocal  = Vector2D{};
    Vector2D                         scrollingPanLastMouseLocal = Vector2D{};
    CBox                             dragOriginalBox            = CBox{};
    CBox                             dragOriginalVisualBox      = CBox{};
    CBox                             dragOriginalOverviewBox    = CBox{};
    CBox                             dragOriginalOverviewHitbox = CBox{};
    CBox                             resizeOriginalBox          = CBox{};
    WORKSPACEID                      focusSyncedFromWorkspaceID = WORKSPACE_INVALID;
    size_t                           resizeWorkspaceIdx     = 0;
    Layout::eRectCorner              resizeCorner           = Layout::CORNER_NONE;
    bool                             dragPendingPrimary    = false;
    bool                             resizePointerDown     = false;
    bool                             scrollingPanPointerDown = false;
    bool                             submapMouseClickPending = false;
    bool                             dragStartedTiled      = false;
    bool                             emittingFullscreenVisibilityState = false;
    bool                             inputConfigOverridden = false;
    bool                             realtimePreviewTimerArmed = false;
    bool                             realtimePreviewFrameQueued = false;
    bool                             selectedWorkspaceFramePending = false;
    bool                             inputFramePending = false;
    bool                             sendingOverviewFrameCallbacks = false;
    bool                             usesSubmapKeybinds = false;
    bool                             submapActive = false;
    uint32_t                         submapMouseClickButton = 0;
    std::string                      previousSubmapName;
    int                              previousNoWarps = 0;
    int                              previousWarpOnChangeWorkspace = 0;
    int                              previousWarpOnToggleSpecial = 0;
    int                              previousWarpBackAfterNonMouseInput = 0;
    int                              previousFollowMouse = 0;

    std::vector<SP<SWorkspaceImage>> images;
    std::vector<PHLWINDOWREF>        pinnedFloatingWindows;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF> rememberedSelection;
    SWorkspaceInsertTransition       workspaceInsertTransition;
    PHLWORKSPACEREF                  pendingRemovedWorkspace;
    ScrollOverview::Config::ELayout  layout = ScrollOverview::Config::ELayout::VERTICAL;

    struct SForcedSurfaceVisibility {
        WP<CWLSurfaceResource> surface;
        CRegion               visibleRegion;
    };
    std::vector<SForcedSurfaceVisibility> forcedSurfaceVisibility;

    struct SForcedWindowVisibility {
        PHLWINDOWREF window;
        bool         hidden = false;
    };
    std::vector<SForcedWindowVisibility> forcedWindowVisibility;

    struct SForcedLayerVisibility {
        PHLLSREF layer;
        bool     aboveFullscreen = true;
        float    alpha           = 1.F;
    };
    std::vector<SForcedLayerVisibility> forcedLayerVisibility;

    struct SWorkspaceAnimationConfig {
        std::string                                      name;
        Hyprutils::Animation::SAnimationPropertyConfig   config;
    };
    std::vector<SWorkspaceAnimationConfig> savedWorkspaceAnimationConfigs;
    bool                                   workspaceAnimationsOverridden = false;
    bool                                   sharedStateOwner = false;

    PHLWORKSPACE                     startedOn;

    PHLANIMVAR<float>                scale;
    PHLANIMVAR<float>                transitionProgress;
    PHLANIMVAR<Vector2D>             viewOffset;
    // Linked screens: the camera this canvas last took from the leader (see
    // followLinkedCamera); a goal that differs means its own code moved it.
    Vector2D                         linkedAppliedOffset;
    float                            linkedAppliedScale = -1.F;
    bool                             minimapShown = false; // the lens shader was last given a visible minimap
    PHLANIMVAR<float>                workspaceInsertProgress;
    PHLANIMVAR<float>                workspaceInsertFadeProgress;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> overviewAnimationConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceInsertFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceRemoveFadeConfig;
    Time::steady_tp                  lastRealtimePreviewFrame = {};
    Time::steady_tp                  realtimePreviewTimerDue = {};
    wl_event_source*                 realtimePreviewTimer = nullptr;

    bool                             closing = false;
    bool                             closeApplied = false; // close() has run its teardown; guards against double-invocation
    bool                             spacePanHeld = false;
    bool                             canvasPinching = false;
    bool                             canvasArrangeButtonPressed = false;
    bool                             canvasNavigationActive = true;
    bool                             landingDrag = false;
    bool                             hasLandingDestination = false;
    Vector2D                         landingDragLast{};
    CBox                             landingDestinationWorld{};
    float                            canvasPinchStartZoom = 1.F;
    PHLWINDOWREF                     navigatorHoverWindow;
    std::unordered_set<uint32_t>     navigatorSwallowedButtons;
    // After a keyboard jump the pointer is usually resting on some other
    // window; hover focus waits until it actually enters a different one.
    bool                             hoverFocusSettling   = false;
    bool                             hoverFocusSettleSeen = false;
    PHLWINDOWREF                     hoverFocusSettleWindow;
    PHLWINDOWREF                     canvasForwardedPointerWindow;
    WP<CWLSurfaceResource>           canvasForwardedPointerSurface;
    std::unordered_set<uint32_t>     canvasForwardedPointerButtons;
    uint32_t                         clientGestureButton = 0; // held button of a move/resize the app asked for
    // Leftover high-resolution wheel units, in 1/120 of a notch.
    int32_t                          canvasWheelValue120Accum = 0;
    wl_pointer_axis                  canvasWheelAccumAxis    = WL_POINTER_AXIS_VERTICAL_SCROLL;
    uint32_t                         canvasWheelAccumTimeMs  = 0;

    CHyprSignalListener             mouseMoveHook;
    CHyprSignalListener             mouseButtonHook;
    CHyprSignalListener             touchMoveHook;
    CHyprSignalListener             touchDownHook;
    CHyprSignalListener             mouseAxisHook;
    CHyprSignalListener             pinchBeginHook;
    CHyprSignalListener             pinchUpdateHook;
    CHyprSignalListener             pinchEndHook;
    CHyprSignalListener             windowOpenHook;
    CHyprSignalListener             windowCloseHook;
    CHyprSignalListener             windowMoveHook;
    CHyprSignalListener             windowActiveHook;
    CHyprSignalListener             windowFullscreenHook;
    CHyprSignalListener             keyboardKeyHook;
    CHyprSignalListener             workspaceCreatedHook;
    CHyprSignalListener             workspaceRemovedHook;

    bool                             swipe = false;

    uint32_t                         lastScrollStepTimeMs = 0;      // event time of the last discrete scroll step, for scroll_event_delay throttling

    double                           trackpadScrollAccum          = 0.0;
    bool                             trackpadWorkspaceFollowing   = false;
    bool                             trackpadTapeFollowing        = false;
    bool                             trackpadGestureSettlePending = false;
    double                           trackpadGestureSettleOffset  = 0.0;

    friend class CScrollOverviewPassElement;
};

// The shared canvas decouples a window's Hyprland owner from the output whose
// camera the user is operating. Directional key dispatch must resolve through
// this interaction camera before looking at ordinary window focus.
SP<IOverview> canvasNavigationOverview();
void          disarmCanvasSwitcher();
void          disarmCanvasTimers();
void          onCanvasExperimentChanged(std::string_view previous);
