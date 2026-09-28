#include "Config.hpp"
#include "Tuning.hpp"

#include <algorithm>
#include <config/shared/complex/ComplexDataType.hpp>
#include <config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/GradientValue.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <regex>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

ScrollOverview::Config::TGestureRegistrar g_gestureRegistrar   = nullptr;

int dispatcherFactoryLua(lua_State* L, std::string_view name);

using TArgValidator = bool (*)(std::string_view);

struct SDispatcher {
    std::string_view                    name;
    std::regex                          argPattern;
    std::string_view                    defaultArg;
    std::string_view                    typeArgError;
    std::string_view                    invalidArgError;
    TArgValidator                       argValidator = nullptr;
    ScrollOverview::Config::TDispatcher dispatcher   = nullptr;
    lua_CFunction                       luaFunction  = nullptr;

    bool isArgValid(const std::string_view arg) const {
        if (argValidator)
            return argValidator(arg);

        return std::regex_match(arg.begin(), arg.end(), argPattern);
    }
};

SDispatcher* findDispatcher(const std::string_view name) {
    static SDispatcher registrations[] = {
        {
            .name            = "overview",
            .argPattern      = std::regex{R"(^(select|(toggle|on|open|enable|off|close|disable)([ \t]+(all|[^ \t"\\]+))?)$)"},
            .defaultArg      = "toggle",
            .typeArgError    = "expected an optional string argument; did you forget quotes around it?",
            .invalidArgError = "expected select or toggle/open/close [monitor|all] (aliases: on, enable, off, disable)",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "overview"); },
        },
        {
            .name            = "navigate",
            .argPattern      = std::regex{"^(left|right|up|down)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: left, right, up, down",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "navigate"); },
        },
        {
            .name            = "window",
            .argPattern      = std::regex{"^(select|close)$"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected one of: select, close",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "window"); },
        },
        {
            .name            = "canvas",
            .argPattern      = std::regex{R"(^((place|viewport)[ \t]+-?[0-9]+[ \t]+-?[0-9]+|area[ \t]+[1-9][0-9]*|send[ \t]+[1-9][0-9]*([ \t]+stay)?|go[ \t]+([1-9][0-9]*|next|prev|back)|assign[ \t]+[^ \t\"\\]+|menu([ \t]+[1-9][0-9]*)?|refresh|arrange|land|back|frame|undo|redo|fit|summon|tune|fill|float|pin|noop|search([ \t]+[^"\\]+)?|zoom[ \t]+(in|out)|(pan|nudge)[ \t]+(left|right|up|down)|native|maximize|fullscreen|restore|experiment[ \t]+(baseline|landing|labels|alttab|areas|quiet|persist|depth|lens|all|next|prev|status)|alttab[ \t]+(next|prev)|switch[ \t]+(next|prev))$)"},
            .typeArgError    = "expected a string argument",
            .invalidArgError = "expected: search [text] | tune | fill | float | pin | noop | fit | summon | zoom in|out | pan/nudge left|right|up|down | undo | redo | arrange | land | back | frame | place <column> <row> | viewport <x> <y> | area <id> | go <place>|next|prev|back | send <place> [stay] | assign left|right|up|down|<monitor> | menu [place] | refresh | native | maximize | fullscreen | restore | experiment <name> | alttab next|prev | switch next|prev",
            .luaFunction     = [](lua_State* L) { return dispatcherFactoryLua(L, "canvas"); },
        },
    };

    const auto MATCH = std::ranges::find_if(registrations, [name](const auto& registration) { return registration.name == name; });
    return MATCH == std::end(registrations) ? nullptr : &*MATCH;
}

int runDispatcherNow(lua_State* L, const SDispatcher& dispatcher, const char* arg) {
    if (!dispatcher.dispatcher)
        return luaL_error(L, "%s: dispatcher is not registered", dispatcher.name.data());

    const auto result = dispatcher.dispatcher(arg);
    if (!result.success)
        return luaL_error(L, "%s: %s", dispatcher.name.data(), result.error.c_str());

    return 0;
}

void pushDispatcherBindAction(lua_State* L, const char* name, const char* arg) {
    const std::string CODE = "return function() return hl.plugin.spatialoverview._dispatch(\"" + std::string{name} + "\", \"" + std::string{arg} + "\") end";

    if (luaL_loadstring(L, CODE.c_str()) != LUA_OK)
        lua_error(L);

    if (lua_pcall(L, 0, 1, 0) != LUA_OK)
        lua_error(L);
}

int runDispatcherActionLua(lua_State* L) {
    if (lua_gettop(L) < 2 || lua_isnoneornil(L, 1) || lua_isnoneornil(L, 2))
        return luaL_error(L, "_dispatch: expected dispatcher name and argument");

    if (!lua_isstring(L, 1) || !lua_isstring(L, 2))
        return luaL_error(L, "_dispatch: expected string arguments");

    const char* name = lua_tostring(L, 1);
    const char* arg  = lua_tostring(L, 2);
    const auto  DISPATCHER = findDispatcher(name);

    if (!DISPATCHER)
        return luaL_error(L, "_dispatch: unknown dispatcher '%s'", name);
    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", name, arg, DISPATCHER->invalidArgError.data());

    return runDispatcherNow(L, *DISPATCHER, arg);
}

int dispatcherFactoryLua(lua_State* L, std::string_view name) {
    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return luaL_error(L, "%s: dispatcher metadata is not registered", name.data());

    const char* arg = DISPATCHER->defaultArg.empty() ? nullptr : DISPATCHER->defaultArg.data();

    if (!arg && (lua_gettop(L) < 1 || lua_isnoneornil(L, 1)))
        return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

    if (lua_gettop(L) >= 1) {
        if (lua_isnoneornil(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        if (!lua_isstring(L, 1))
            return luaL_error(L, "%s: %s", DISPATCHER->name.data(), DISPATCHER->typeArgError.data());

        arg = lua_tostring(L, 1);
    }

    if (!DISPATCHER->isArgValid(arg))
        return luaL_error(L, "%s: invalid argument '%s', %s", DISPATCHER->name.data(), arg, DISPATCHER->invalidArgError.data());

    if (g_pKeybindManager && g_pKeybindManager->m_currentKeybind && g_pKeybindManager->m_currentKeybind->handler == "__lua") {
        return runDispatcherNow(L, *DISPATCHER, arg);
    }

    pushDispatcherBindAction(L, DISPATCHER->name.data(), arg);

    return 1;
}

int configureLua(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "configure: expected a table");

    const int CONFIG = lua_absindex(L, 1);

    lua_getglobal(L, "hl");
    if (!lua_istable(L, -1))
        return luaL_error(L, "configure: global hl table is not available");

    lua_getfield(L, -1, "config");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "configure: hl.config is not available");
    }

    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(L, CONFIG);
    lua_setfield(L, -2, "spatialoverview");
    lua_setfield(L, -2, "plugin");

    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        lua_pop(L, 2);
        return luaL_error(L, "configure: %s", err ? err : "hl.config failed");
    }

    lua_pop(L, 1);

    return 0;
}

int gestureLua(lua_State* L) {
    if (!g_gestureRegistrar)
        return luaL_error(L, "gesture: registrar is not registered");

    if (!lua_istable(L, 1))
        return luaL_error(L, "gesture: expected a table, e.g. { fingers = 3, direction = \"up\" }");

    lua_getfield(L, 1, "fingers");
    if (!lua_isinteger(L, -1))
        return luaL_error(L, "gesture: 'fingers' (integer) is required");
    const size_t FINGERS = sc<size_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "direction");
    if (!lua_isstring(L, -1))
        return luaL_error(L, "gesture: 'direction' (string) is required");
    const std::string DIRECTION = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "action");
    const std::string ACTION = lua_isstring(L, -1) ? lua_tostring(L, -1) : "overview";
    lua_pop(L, 1);

    // accept either "mods" or "mod"
    lua_getfield(L, 1, "mods");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "mod");
    }
    const std::string MODS = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    lua_getfield(L, 1, "scale");
    const float SCALE = lua_isnumber(L, -1) ? sc<float>(lua_tonumber(L, -1)) : 1.F;
    lua_pop(L, 1);

    lua_getfield(L, 1, "disable_inhibit");
    const bool DISABLE_INHIBIT = lua_toboolean(L, -1);
    lua_pop(L, 1);

    const auto result = g_gestureRegistrar(FINGERS, DIRECTION, ACTION, MODS, SCALE, DISABLE_INHIBIT);
    if (!result.success)
        return luaL_error(L, "gesture: %s", result.error.c_str());

    return 0;
}

}

namespace ScrollOverview::Config {

void registerDispatcher(const std::string& name, TDispatcher dispatcher) {
    HyprlandAPI::addDispatcherV2(SCROLLOVERVIEW_HANDLE, "spatialoverview:" + name, dispatcher);

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    const auto DISPATCHER = findDispatcher(name);
    if (!DISPATCHER)
        return;

    DISPATCHER->dispatcher = dispatcher;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "spatialoverview", std::string{DISPATCHER->name}, DISPATCHER->luaFunction);
}

void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword) {
    HyprlandAPI::addConfigKeyword(SCROLLOVERVIEW_HANDLE, "spatialoverview-gesture", gestureKeyword, {});

    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    g_gestureRegistrar = gestureRegistrar;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "spatialoverview", "gesture", ::gestureLua);
}

static void registerLuaFunctions() {
    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "spatialoverview", "_dispatch", ::runDispatcherActionLua);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "spatialoverview", "configure", ::configureLua);
    // Lets a config apply newer settings only when the loaded build knows
    // them: 2 = navigator, switcher, layout memory; 3 = live tuner, HUD and
    // lens settings, parallax.
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "spatialoverview", "features", [](lua_State* L) {
        lua_pushinteger(L, 3);
        return 1;
    });
    // Where the live tuner saves, so the config can apply it (level 3).
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "spatialoverview", "tuning_file", [](lua_State* L) {
        lua_pushstring(L, SpatialOverview::Tuning::filePath().c_str());
        return 1;
    });
}

static void registerConfigValues() {
    using namespace ::Config::Values;

    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:gesture_distance", "gesture distance in pixels", 200, SIntValueOptions{.min = 1}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:scale", "overview scale", 0.36F, SFloatValueOptions{.min = 0.1F, .max = 0.9F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:workspace_gap", "gap between overview workspaces", 28, SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:layout", "overview layout", Hyprlang::STRING{"grid"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:grid:columns", "number of workspace columns", 3, SIntValueOptions{.min = 1, .max = 12}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:grid:stagger", "alternating row offset as a fraction of tile width", 0.12F,
                                                          SFloatValueOptions{.min = -0.5F, .max = 0.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:input:pan_sensitivity", "free camera drag sensitivity", 1.F,
                                                          SFloatValueOptions{.min = 0.05F, .max = 5.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:animation:enabled", "animate overview camera transitions", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:animation:speed", "overview camera animation speed", 8.F,
                                                          SFloatValueOptions{.min = 0.1F, .max = 30.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:animation:bezier", "overview camera animation curve", Hyprlang::STRING{"default"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, makeShared<CBoolValue>("plugin:spatialoverview:distortion:enabled", "enable barrel distortion", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:distortion:strength", "barrel distortion strength", 0.14F,
                                                          SFloatValueOptions{.min = -0.5F, .max = 0.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:distortion:edge_scale", "distortion edge overscan", 1.08F,
                                                          SFloatValueOptions{.min = 1.F, .max = 1.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:distortion:feather", "soft edge width", 0.025F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.25F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:distortion:transition_power", "distortion transition response", 1.F,
                                                          SFloatValueOptions{.min = 0.25F, .max = 4.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:distortion:shader_path", "lens shader file; empty uses the one built into the plugin", Hyprlang::STRING{""}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:workspace_outline:enabled", "draw workspace card outlines", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:workspace_outline:width", "workspace outline width", 2,
                                                        SIntValueOptions{.min = 1, .max = 12}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:workspace_outline:drop_width", "workspace drop-target outline width", 4,
                                                        SIntValueOptions{.min = 1, .max = 16}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:workspace_outline:rounding", "workspace outline corner radius", 14,
                                                        SIntValueOptions{.min = 0, .max = 64}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:workspace_outline:opacity", "inactive workspace outline opacity", 0.34F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:workspace_outline:active_opacity", "active workspace outline opacity", 0.82F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:workspace_outline:drop_opacity", "drop-target outline opacity", 1.F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:workspace_outline:drop_fill_opacity", "drop-target fill opacity", 0.12F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:enabled", "enable continuous canvas presentation", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:desktop_mode", "render one shared window canvas instead of workspace cards", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:persistent", "keep the canvas renderer active at normal zoom", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:linked_screens", "screens show adjacent parts of the canvas and move together, like one wide desk", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:places", "workspace keys go to places on the canvas (experimental); off, they do nothing on the canvas", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:canvas:place_monitors",
                                                           "starting screens for places, as \"1-5:DP-1 6-10:HDMI-A-1\"; a place is otherwise claimed by the screen it is first visited from, and canvas \"assign\" moves it for good",
                                                           Hyprlang::STRING{""}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:tile_places", "windows in a place tile to fill its screen", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:initial_zoom", "initial shared-canvas camera zoom", 0.72F,
                                                          SFloatValueOptions{.min = 0.1F, .max = 2.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:min_zoom", "minimum shared-canvas camera zoom", 0.15F,
                                                          SFloatValueOptions{.min = 0.05F, .max = 2.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:max_zoom", "maximum shared-canvas camera zoom", 2.5F,
                                                          SFloatValueOptions{.min = 0.1F, .max = 8.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:zoom_step", "wheel zoom strength", 0.12F,
                                                          SFloatValueOptions{.min = 0.01F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:auto_float", "detach canvas windows from tiling", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:auto_place", "place newly opened windows on the canvas grid", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:placement_gap", "minimum gap used by automatic canvas placement", 40,
                                                        SIntValueOptions{.min = 0, .max = 512}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:space_pan", "hold Space and left-drag to move the camera", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:direct_input", "forward input into scaled canvas windows", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:hover_focus", "focus canvas windows when the pointer enters them", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:minimap_enabled", "show a world minimap in navigation mode", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:minimap_width", "minimap width in logical pixels", 240,
                                                        SIntValueOptions{.min = 120, .max = 640}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:minimap_height", "minimap height in logical pixels", 150,
                                                        SIntValueOptions{.min = 80, .max = 480}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:minimap_margin", "minimap edge margin in logical pixels", 24,
                                                        SIntValueOptions{.min = 0, .max = 128}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:minimap_opacity", "minimap background opacity", 0.72F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:arrange_context_grouping",
                                                         "group related canvas windows using local app/title context", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:arrange_size_similarity",
                                                          "relative size difference eligible for matching during arrangement", 0.12F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:arrange_resize_limit",
                                                          "maximum relative size change during canvas arrangement", 0.15F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:grid_enabled", "draw the continuous canvas grid", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:grid_size", "canvas grid spacing in logical pixels", 160,
                                                        SIntValueOptions{.min = 24, .max = 1024}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:grid_width", "canvas grid line width", 1,
                                                        SIntValueOptions{.min = 1, .max = 8}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:grid_opacity", "canvas grid opacity", 0.16F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.5F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:grid_style", "canvas grid style: 0 lines, 1 dots", 1,
                                                        SIntValueOptions{.min = 0, .max = 1}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:grid_dot_size", "canvas grid dot diameter in logical pixels", 3.2F,
                                                          SFloatValueOptions{.min = 2.F, .max = 16.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:background_dim", "navigation-mode canvas background dimming", 0.35F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.9F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:viewport_enabled", "draw the selected screen viewport", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:viewport_width", "viewport border width", 3,
                                                        SIntValueOptions{.min = 1, .max = 16}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:viewport_rounding", "viewport corner radius", 18,
                                                        SIntValueOptions{.min = 0, .max = 96}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:viewport_border_opacity", "viewport border opacity", 0.9F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:canvas:viewport_outside_opacity", "opacity of the area outside the viewport", 0.26F,
                                                          SFloatValueOptions{.min = 0.F, .max = 0.8F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:float_on_drag", "detach tiled windows for free canvas placement", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:allow_window_overflow", "allow canvas windows beyond the workspace footprint", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:snap_viewport_on_pan", "select the workspace nearest the viewport after panning", true));
    HyprlandAPI::addConfigValueV2(
        SCROLLOVERVIEW_HANDLE,
        makeShared<CBoolValue>("plugin:spatialoverview:canvas:commit_viewport_on_close", "make the freely panned viewport the normal desktop view when overview closes", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:snap_enabled", "snap canvas window positions to the world grid", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:canvas:snap_size", "canvas window position snap interval", 40,
                                                        SIntValueOptions{.min = 1, .max = 512}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:chrome_animation:enabled", "animate desktop chrome out of canvas mode", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:chrome_animation:top_namespace", "top chrome layer namespace", Hyprlang::STRING{"omarchy-bar"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:chrome_animation:bottom_namespace", "bottom chrome layer namespace", Hyprlang::STRING{"omarchy-dock"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:chrome_animation:top_travel", "top chrome travel multiplier", 1.35F,
                                                          SFloatValueOptions{.min = 0.F, .max = 4.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:chrome_animation:bottom_travel", "bottom chrome travel multiplier", 1.08F,
                                                          SFloatValueOptions{.min = 0.F, .max = 4.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:chrome_animation:top_scale", "top chrome final scale", 0.82F,
                                                          SFloatValueOptions{.min = 0.1F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:chrome_animation:bottom_scale", "bottom chrome final scale", 0.92F,
                                                          SFloatValueOptions{.min = 0.1F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:chrome_animation:opacity", "desktop chrome opacity at full overview", 0.F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:input:scroll_event_delay", "minimum delay (ms) between discrete scroll steps (wheel workspace nav and trackpad focus stepping)", 200, SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:input:touchpad_scroll_factor", "overview touchpad scroll factor", 1.F,
                                                          SFloatValueOptions{.min = 0.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:input:mouse_scroll_factor", "canvas window mouse-wheel scroll factor", 1.F,
                                                          SFloatValueOptions{.min = 0.05F, .max = 5.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:input:left_handed", "overview left handed mouse buttons, 2 follows input:left_handed", 2,
                                                        SIntValueOptions{.min = 0, .max = 2}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:input:scrolling_mode", "overview mouse wheel behavior", 0,
                                                        SIntValueOptions{.min = 0, .max = 3}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:input:drag_mode", "overview mouse drag behavior", 0,
                                                        SIntValueOptions{.min = 0, .max = 1}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:input:drag_threshold", "overview drag threshold", 10,
                                                        SIntValueOptions{.min = 0}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:wallpaper", "wallpaper mode", 0, SIntValueOptions{.min = 0, .max = 2}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, makeShared<CBoolValue>("plugin:spatialoverview:blur", "blur the overview wallpaper", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:blur_strength", "navigation-mode wallpaper blur blend", 0.7F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:shadow:enabled", "draw a shadow around each workspace card", false));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:shadow:range", "workspace card shadow range", -1));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CIntValue>("plugin:spatialoverview:shadow:render_power", "workspace card shadow render power", -1));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CGradientValue>("plugin:spatialoverview:shadow:color", "workspace card shadow color", -1));

    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:navigator:enabled", "type-to-search palette in the zoomed-out canvas", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:navigator:labels", "draw window titles on the zoomed-out canvas", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CFloatValue>("plugin:spatialoverview:navigator:dim_unmatched", "dimming of windows that do not match the search", 0.7F,
                                                          SFloatValueOptions{.min = 0.F, .max = 1.F}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:navigator:pointer", "in the zoomed-out canvas, click goes to a window, drag moves it or pans, wheel zooms", true));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:navigator:accent", "HUD accent color (#rrggbb), or auto to follow the active border", Hyprlang::STRING{"#ff6b1a"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:navigator:highlight", "unused; matches take the accent (kept so older configs load)", Hyprlang::STRING{""}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:navigator:font", "unused; the HUD is set in mono_font (kept so older configs load)", Hyprlang::STRING{""}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CStringValue>("plugin:spatialoverview:navigator:mono_font", "HUD readout font", Hyprlang::STRING{"JetBrainsMono Nerd Font"}));
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE,
                                  makeShared<CBoolValue>("plugin:spatialoverview:canvas:remember_layout", "remember where windows live across restarts and reopenings", false));

    SpatialOverview::Tuning::registerConfig();
}

void registerConfig() {
    registerLuaFunctions();
    registerConfigValues();
    HyprlandAPI::reloadConfig();
}

int getGestureDistance() {
    return std::max<int>(1, getValue<int>("plugin:spatialoverview:gesture_distance"));
}

float getScale() {
    return std::clamp(getValue<float>("plugin:spatialoverview:scale"), 0.1F, 0.9F);
}

int getWorkspaceGap() {
    return std::max<int>(0, getValue<int>("plugin:spatialoverview:workspace_gap"));
}

ELayout getLayout() {
    const auto LAYOUT = getValue<std::string>("plugin:spatialoverview:layout");
    if (LAYOUT == "horizontal")
        return ELayout::HORIZONTAL;
    if (LAYOUT == "vertical")
        return ELayout::VERTICAL;
    return ELayout::GRID;
}

int getGridColumns() {
    return std::clamp(getValue<int>("plugin:spatialoverview:grid:columns"), 1, 12);
}

float getGridStagger() {
    return std::clamp(getValue<float>("plugin:spatialoverview:grid:stagger"), -0.5F, 0.5F);
}

float getPanSensitivity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:input:pan_sensitivity"), 0.05F, 5.F);
}

bool getAnimationEnabled() {
    return getValue<bool>("plugin:spatialoverview:animation:enabled");
}

float getAnimationSpeed() {
    return std::clamp(getValue<float>("plugin:spatialoverview:animation:speed"), 0.1F, 30.F);
}

std::string getAnimationBezier() {
    return getValue<std::string>("plugin:spatialoverview:animation:bezier");
}

bool getBarrelEnabled() {
    return getValue<bool>("plugin:spatialoverview:distortion:enabled");
}

float getBarrelStrength() {
    return std::clamp(getValue<float>("plugin:spatialoverview:distortion:strength"), -0.5F, 0.5F);
}

float getBarrelEdgeScale() {
    return std::clamp(getValue<float>("plugin:spatialoverview:distortion:edge_scale"), 1.F, 1.5F);
}

float getBarrelFeather() {
    return std::clamp(getValue<float>("plugin:spatialoverview:distortion:feather"), 0.F, 0.25F);
}

float getBarrelTransitionPower() {
    return std::clamp(getValue<float>("plugin:spatialoverview:distortion:transition_power"), 0.25F, 4.F);
}

std::string getBarrelShaderPath() {
    return getValue<std::string>("plugin:spatialoverview:distortion:shader_path");
}

bool getWorkspaceOutlineEnabled() {
    return getValue<bool>("plugin:spatialoverview:workspace_outline:enabled");
}

int getWorkspaceOutlineWidth() {
    return std::clamp(getValue<int>("plugin:spatialoverview:workspace_outline:width"), 1, 12);
}

int getWorkspaceOutlineDropWidth() {
    return std::clamp(getValue<int>("plugin:spatialoverview:workspace_outline:drop_width"), 1, 16);
}

int getWorkspaceOutlineRounding() {
    return std::clamp(getValue<int>("plugin:spatialoverview:workspace_outline:rounding"), 0, 64);
}

float getWorkspaceOutlineOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:workspace_outline:opacity"), 0.F, 1.F);
}

float getWorkspaceOutlineActiveOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:workspace_outline:active_opacity"), 0.F, 1.F);
}

float getWorkspaceOutlineDropOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:workspace_outline:drop_opacity"), 0.F, 1.F);
}

float getWorkspaceOutlineDropFillOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:workspace_outline:drop_fill_opacity"), 0.F, 0.5F);
}

bool getCanvasEnabled() {
    return getValue<bool>("plugin:spatialoverview:canvas:enabled");
}

bool getCanvasDesktopMode() {
    return getValue<bool>("plugin:spatialoverview:canvas:desktop_mode");
}

bool getCanvasPlaces() {
    return getValue<bool>("plugin:spatialoverview:canvas:places");
}

std::string getCanvasPlaceMonitors() {
    return getValue<std::string>("plugin:spatialoverview:canvas:place_monitors");
}

bool getCanvasTilePlaces() {
    return getValue<bool>("plugin:spatialoverview:canvas:tile_places");
}

bool getCanvasLinkedScreens() {
    return getValue<bool>("plugin:spatialoverview:canvas:linked_screens");
}

bool getCanvasPersistent() {
    return getValue<bool>("plugin:spatialoverview:canvas:persistent");
}

float getCanvasInitialZoom() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:initial_zoom"), getCanvasMinZoom(), getCanvasMaxZoom());
}

float getCanvasMinZoom() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:min_zoom"), 0.05F, 2.F);
}

float getCanvasMaxZoom() {
    return std::max(getCanvasMinZoom(), std::clamp(getValue<float>("plugin:spatialoverview:canvas:max_zoom"), 0.1F, 8.F));
}

float getCanvasZoomStep() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:zoom_step"), 0.01F, 1.F);
}

bool getCanvasAutoFloat() {
    return getValue<bool>("plugin:spatialoverview:canvas:auto_float");
}

bool getCanvasAutoPlace() {
    return getValue<bool>("plugin:spatialoverview:canvas:auto_place");
}

int getCanvasPlacementGap() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:placement_gap"), 0, 512);
}

bool getCanvasSpacePan() {
    return getValue<bool>("plugin:spatialoverview:canvas:space_pan");
}

bool getCanvasDirectInput() {
    return getValue<bool>("plugin:spatialoverview:canvas:direct_input");
}

bool getCanvasHoverFocus() {
    return getValue<bool>("plugin:spatialoverview:canvas:hover_focus");
}

bool getCanvasMinimapEnabled() {
    return getValue<bool>("plugin:spatialoverview:canvas:minimap_enabled");
}

int getCanvasMinimapWidth() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:minimap_width"), 120, 640);
}

int getCanvasMinimapHeight() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:minimap_height"), 80, 480);
}

int getCanvasMinimapMargin() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:minimap_margin"), 0, 128);
}

float getCanvasMinimapOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:minimap_opacity"), 0.F, 1.F);
}

bool getCanvasArrangeContextGrouping() {
    return getValue<bool>("plugin:spatialoverview:canvas:arrange_context_grouping");
}

float getCanvasArrangeSizeSimilarity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:arrange_size_similarity"), 0.F, 0.5F);
}

float getCanvasArrangeResizeLimit() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:arrange_resize_limit"), 0.F, 0.5F);
}

bool getCanvasGridEnabled() {
    return getValue<bool>("plugin:spatialoverview:canvas:grid_enabled");
}

int getCanvasGridSize() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:grid_size"), 24, 1024);
}

int getCanvasGridWidth() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:grid_width"), 1, 8);
}

float getCanvasGridOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:grid_opacity"), 0.F, 0.5F);
}

int getCanvasGridStyle() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:grid_style"), 0, 1);
}

float getCanvasGridDotSize() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:grid_dot_size"), 2.F, 16.F);
}

float getCanvasBackgroundDim() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:background_dim"), 0.F, 0.9F);
}

bool getCanvasViewportEnabled() {
    return getValue<bool>("plugin:spatialoverview:canvas:viewport_enabled");
}

int getCanvasViewportWidth() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:viewport_width"), 1, 16);
}

int getCanvasViewportRounding() {
    return std::clamp(getValue<int>("plugin:spatialoverview:canvas:viewport_rounding"), 0, 96);
}

float getCanvasViewportBorderOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:viewport_border_opacity"), 0.F, 1.F);
}

float getCanvasViewportOutsideOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:canvas:viewport_outside_opacity"), 0.F, 0.8F);
}

bool getCanvasFloatOnDrag() {
    return getValue<bool>("plugin:spatialoverview:canvas:float_on_drag");
}

bool getCanvasAllowWindowOverflow() {
    return getValue<bool>("plugin:spatialoverview:canvas:allow_window_overflow");
}

bool getCanvasSnapViewportOnPan() {
    return getValue<bool>("plugin:spatialoverview:canvas:snap_viewport_on_pan");
}

bool getCanvasCommitViewportOnClose() {
    return getValue<bool>("plugin:spatialoverview:canvas:commit_viewport_on_close");
}

bool getCanvasSnapEnabled() {
    return getValue<bool>("plugin:spatialoverview:canvas:snap_enabled");
}

int getCanvasSnapSize() {
    // Grid rendering and window placement share one world-space lattice. Keep
    // accepting the old snap_size key for config compatibility, but never let
    // it diverge from the dots/lines the user can actually see.
    return getCanvasGridSize();
}

bool getChromeAnimationEnabled() {
    return getValue<bool>("plugin:spatialoverview:chrome_animation:enabled");
}

std::string getChromeTopNamespace() {
    return getValue<std::string>("plugin:spatialoverview:chrome_animation:top_namespace");
}

std::string getChromeBottomNamespace() {
    return getValue<std::string>("plugin:spatialoverview:chrome_animation:bottom_namespace");
}

float getChromeTopTravel() {
    return std::clamp(getValue<float>("plugin:spatialoverview:chrome_animation:top_travel"), 0.F, 4.F);
}

float getChromeBottomTravel() {
    return std::clamp(getValue<float>("plugin:spatialoverview:chrome_animation:bottom_travel"), 0.F, 4.F);
}

float getChromeTopScale() {
    return std::clamp(getValue<float>("plugin:spatialoverview:chrome_animation:top_scale"), 0.1F, 1.F);
}

float getChromeBottomScale() {
    return std::clamp(getValue<float>("plugin:spatialoverview:chrome_animation:bottom_scale"), 0.1F, 1.F);
}

float getChromeOpacity() {
    return std::clamp(getValue<float>("plugin:spatialoverview:chrome_animation:opacity"), 0.F, 1.F);
}

bool getLeftHanded() {
    const auto LEFT_HANDED = getValue<int>("plugin:spatialoverview:input:left_handed");
    if (LEFT_HANDED <= 1)
        return LEFT_HANDED != 0;

    return getValue<bool>("input:left_handed");
}

int getDragMode() {
    return std::clamp(getValue<int>("plugin:spatialoverview:input:drag_mode"), 0, 1);
}

int getDragThreshold() {
    return std::max<int>(0, getValue<int>("plugin:spatialoverview:input:drag_threshold"));
}

float getTouchpadScrollFactor() {
    static constexpr float OVERVIEWTOUCHPADSCROLLFACTOR = 1.5F;

    return OVERVIEWTOUCHPADSCROLLFACTOR * std::max<float>(0.F, getValue<float>("input:touchpad:scroll_factor")) *
        std::max<float>(0.F, getValue<float>("plugin:spatialoverview:input:touchpad_scroll_factor"));
}

float getMouseScrollFactor() {
    return std::clamp(getValue<float>("plugin:spatialoverview:input:mouse_scroll_factor"), 0.05F, 5.F) *
        std::max<float>(0.F, getValue<float>("input:scroll_factor"));
}

static EScrollAction defaultVerticalScrollAction(ELayout layout) {
    return layout == ELayout::HORIZONTAL ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

EScrollAction getVerticalScrollAction(ELayout layout) {
    const auto MODE = std::clamp(getValue<int>("plugin:spatialoverview:input:scrolling_mode"), 0, 3);

    switch (MODE) {
        case 1: return defaultVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
        case 2: return EScrollAction::WORKSPACE;
        case 3: return EScrollAction::COLUMN;
        case 0:
        default: return defaultVerticalScrollAction(layout);
    }
}

EScrollAction getHorizontalScrollAction(ELayout layout) {
    return getVerticalScrollAction(layout) == EScrollAction::WORKSPACE ? EScrollAction::COLUMN : EScrollAction::WORKSPACE;
}

int getScrollEventDelay() {
    return std::max<int>(0, getValue<int>("plugin:spatialoverview:input:scroll_event_delay"));
}

int getWallpaperMode() {
    return std::clamp<int>(getValue<int>("plugin:spatialoverview:wallpaper"), 0, 2);
}

bool getBlur() {
    return getValue<bool>("plugin:spatialoverview:blur");
}

float getBlurStrength() {
    return std::clamp(getValue<float>("plugin:spatialoverview:blur_strength"), 0.F, 1.F);
}

::Config::CCssGapData getCssGapData(const std::string& name) {
    auto& VALUE = valueRef<::Config::IComplexConfigValue>(name);
    if (!VALUE.good())
        return {};

    auto* const GAPS = dc<::Config::CCssGapData*>(VALUE.ptr());
    if (!GAPS)
        return {};

    return *GAPS;
}

int getShadowEnabled() {
    return getValue<bool>("plugin:spatialoverview:shadow:enabled") ? 1 : 0;
}

int getShadowRange() {
    return getValue<int>("plugin:spatialoverview:shadow:range");
}

int getShadowRenderPower() {
    return getValue<int>("plugin:spatialoverview:shadow:render_power");
}

std::optional<::Config::CGradientValueData> getShadowColor() {
    constexpr auto NAME = "plugin:spatialoverview:shadow:color";

    if (!::Config::mgr()->getConfigValue(NAME).setByUser)
        return std::nullopt;

    return getValue<::Config::CGradientValueData>(NAME);
}

bool getNavigatorEnabled() {
    return getValue<bool>("plugin:spatialoverview:navigator:enabled");
}

bool getNavigatorLabels() {
    return getValue<bool>("plugin:spatialoverview:navigator:labels");
}

float getNavigatorDimUnmatched() {
    return std::clamp(getValue<float>("plugin:spatialoverview:navigator:dim_unmatched"), 0.F, 1.F);
}

bool getNavigatorPointer() {
    return getValue<bool>("plugin:spatialoverview:navigator:pointer");
}

std::string getNavigatorAccent() {
    return getValue<std::string>("plugin:spatialoverview:navigator:accent");
}

std::string getNavigatorMonoFont() {
    return getValue<std::string>("plugin:spatialoverview:navigator:mono_font");
}

bool getCanvasRememberLayout() {
    return getValue<bool>("plugin:spatialoverview:canvas:remember_layout");
}

}
