#pragma once

#include "globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace ScrollOverview::Config {

using TDispatcher       = SDispatchResult (*)(std::string);
using TGestureKeyword   = Hyprlang::CParseResult (*)(const char* LHS, const char* RHS);
using TGestureRegistrar   = SDispatchResult (*)(size_t fingerCount, const std::string& direction, const std::string& action, const std::string& mods, float deltaScale,
                                                bool disableInhibit);

enum class ELayout {
    VERTICAL,
    HORIZONTAL,
    GRID,
};

enum class EScrollAction {
    WORKSPACE,
    COLUMN,
};

void registerDispatcher(const std::string& name, TDispatcher dispatcher);
void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword);
void registerConfig();

template <typename T>
CConfigValue<T>& valueRef(const std::string& name) {
    static std::unordered_map<std::string, UP<CConfigValue<T>>> values;

    const auto [it, inserted] = values.try_emplace(name);
    if (inserted)
        it->second = makeUnique<CConfigValue<T>>(name);

    return *it->second;
}

template <typename T>
T getValue(const std::string& name) {
    using TValue = std::decay_t<T>;

	if constexpr (std::is_same_v<TValue, ::Config::CGradientValueData>) {
        auto& ref = valueRef<::Config::IComplexConfigValue>(name);
        if (!ref.good())
            return {};
        return *sc<::Config::CGradientValueData*>(ref.ptr());
    }

    if constexpr (std::is_same_v<TValue, bool>)
        return *valueRef<Hyprlang::INT>(name) != 0;
    else if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>)
        return sc<TValue>(*valueRef<Hyprlang::INT>(name));
    else if constexpr (std::is_floating_point_v<TValue>)
        return sc<TValue>(*valueRef<Hyprlang::FLOAT>(name));
    else
        return *valueRef<TValue>(name);
}

template <typename T>
T* getValuePtr(const std::string& name) {
    return valueRef<T>(name).ptr();
}

template <typename T>
void setValue(const std::string& name, const T& value) {
    using TValue = std::decay_t<T>;

    if constexpr (std::is_same_v<TValue, bool>)
        *getValuePtr<Hyprlang::INT>(name) = value ? 1 : 0;
    else if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>)
        *getValuePtr<Hyprlang::INT>(name) = sc<Hyprlang::INT>(value);
    else if constexpr (std::is_floating_point_v<TValue>)
        *getValuePtr<Hyprlang::FLOAT>(name) = sc<Hyprlang::FLOAT>(value);
    else
        *getValuePtr<TValue>(name) = value;
}

int           getGestureDistance();
float         getScale();
int           getWorkspaceGap();
ELayout       getLayout();
int           getGridColumns();
float         getGridStagger();
float         getPanSensitivity();
bool          getAnimationEnabled();
float         getAnimationSpeed();
std::string   getAnimationBezier();
bool          getBarrelEnabled();
float         getBarrelStrength();
float         getBarrelEdgeScale();
float         getBarrelFeather();
float         getBarrelTransitionPower();
std::string   getBarrelShaderPath();
bool          getWorkspaceOutlineEnabled();
int           getWorkspaceOutlineWidth();
int           getWorkspaceOutlineDropWidth();
int           getWorkspaceOutlineRounding();
float         getWorkspaceOutlineOpacity();
float         getWorkspaceOutlineActiveOpacity();
float         getWorkspaceOutlineDropOpacity();
float         getWorkspaceOutlineDropFillOpacity();
bool          getCanvasEnabled();
bool          getCanvasDesktopMode();
bool          getCanvasPersistent();
bool          getCanvasLinkedScreens();
bool          getCanvasPlaces();
std::string   getCanvasPlaceMonitors();
bool          getCanvasTilePlaces();
float         getCanvasInitialZoom();
float         getCanvasMinZoom();
float         getCanvasMaxZoom();
float         getCanvasZoomStep();
bool          getCanvasAutoFloat();
bool          getCanvasAutoPlace();
int           getCanvasPlacementGap();
bool          getCanvasSpacePan();
bool          getCanvasDirectInput();
bool          getCanvasHoverFocus();
bool          getCanvasMinimapEnabled();
int           getCanvasMinimapWidth();
int           getCanvasMinimapHeight();
int           getCanvasMinimapMargin();
float         getCanvasMinimapOpacity();
bool          getCanvasArrangeContextGrouping();
float         getCanvasArrangeSizeSimilarity();
float         getCanvasArrangeResizeLimit();
bool          getCanvasGridEnabled();
int           getCanvasGridSize();
int           getCanvasGridWidth();
float         getCanvasGridOpacity();
int           getCanvasGridStyle();
float         getCanvasGridDotSize();
float         getCanvasBackgroundDim();
bool          getCanvasViewportEnabled();
int           getCanvasViewportWidth();
int           getCanvasViewportRounding();
float         getCanvasViewportBorderOpacity();
float         getCanvasViewportOutsideOpacity();
bool          getCanvasFloatOnDrag();
bool          getCanvasAllowWindowOverflow();
bool          getCanvasSnapViewportOnPan();
bool          getCanvasCommitViewportOnClose();
bool          getCanvasSnapEnabled();
int           getCanvasSnapSize();
bool          getChromeAnimationEnabled();
std::string   getChromeTopNamespace();
std::string   getChromeBottomNamespace();
float         getChromeTopTravel();
float         getChromeBottomTravel();
float         getChromeTopScale();
float         getChromeBottomScale();
float         getChromeOpacity();
int           getScrollEventDelay();
bool          getLeftHanded();
int           getDragMode();
int           getDragThreshold();
float         getTouchpadScrollFactor();
float         getMouseScrollFactor();
EScrollAction getVerticalScrollAction(ELayout layout);
EScrollAction getHorizontalScrollAction(ELayout layout);
int           getWallpaperMode();
bool          getBlur();
float         getBlurStrength();
::Config::CCssGapData getCssGapData(const std::string& name);
int          getShadowEnabled();
int          getShadowRange();
int          getShadowRenderPower();
std::optional<::Config::CGradientValueData> getShadowColor();
bool          getNavigatorEnabled();
bool          getNavigatorLabels();
float         getNavigatorDimUnmatched();
bool          getNavigatorPointer();
std::string   getNavigatorAccent();
std::string   getNavigatorMonoFont();
bool          getCanvasRememberLayout();

}
