-- Phantomat: a zoomable, infinite-canvas window manager for Hyprland 0.56+ (made on Omarchy).
--
--   SUPER + CTRL + G   the zoomed-out canvas; type to search, Enter to go
--   CTRL + ,           (in the canvas) tune the look live; F1 lists every key
--
-- Edit and run `hyprctl reload`, or tune live: the tuner saves to
-- spatialoverview-tuning.lua next to this file, which is applied last.

local loaded = false
for _, plugin in ipairs(hl.get_loaded_plugins()) do
  if plugin.name == "spatialoverview" then
    loaded = true
    break
  end
end
-- On the first pass Hyprland has only been told to load the plugin; it
-- parses this file again once the plugin is in. Without the plugin (say,
-- right after a Hyprland update, before a rebuild) nothing here applies.
if not loaded then
  return
end

-- Omarchy's o.bind shows bindings in its key guide; elsewhere, plain binds.
local bind = (o and o.bind) or function(keys, _description, action, opts)
  hl.bind(keys, action, opts)
end

hl.config({
  plugin = {
    spatialoverview = {
      -- Camera / tile geometry
      layout = "grid",
      scale = 0.36,
      workspace_gap = 0,
      grid = {
        columns = 3,
        stagger = 0.0,
      },

      -- Input. Middle-drag pans the world; click selects; dragging into empty
      -- grid territory places a floating window without creating a workspace.
      input = {
        pan_sensitivity = 1.0,
        drag_threshold = 10,
        touchpad_scroll_factor = 1.0,
        scroll_event_delay = 160,
      },

      -- Motion. Edit the `spatialOverviewEase` control points below to shape
      -- acceleration/deceleration without recompiling the plugin.
      animation = {
        enabled = true,
        speed = 8.0,
        bezier = "spatialOverviewEase",
      },

      -- Final overview-only lens pass. `strength` is signed, so negative
      -- values reverse the curvature.
      distortion = {
        enabled = true,
        strength = 0.14,
        edge_scale = 1.08,
        feather = 0.025,
        transition_power = 1.0,
        edge_blur = 0.0,        -- lens blur toward the edges, 0..1
        edge_blur_start = 0.45, -- where it begins: 0 center .. 1 corners
        vignette = 0.0,
        chromatic = 0.0,        -- red/blue fringe toward the edges
      },

      workspace_outline = {
        enabled = false,
        width = 2,
        drop_width = 4,
        rounding = 14,
        opacity = 0.34,
        active_opacity = 0.82,
        drop_opacity = 1.0,
        drop_fill_opacity = 0.12,
      },

      canvas = {
        enabled = true,
        desktop_mode = true,
        linked_screens = true,  -- false: each screen has its own camera
        places = false,         -- SUPER + 1…0 go to places on the canvas (experimental)
        place_monitors = "",    -- optional starting screens, e.g. "1-5:DP-1"; SUPER + SHIFT + ALT + arrows move places
        tile_places = false,    -- windows in a place tile to fill its screen
        persistent = true,
        initial_zoom = 0.72,
        min_zoom = 0.15,
        max_zoom = 2.5,
        zoom_step = 0.12,
        auto_float = true,
        auto_place = true,
        placement_gap = 40,
        space_pan = false,
        direct_input = true,
        hover_focus = true,
        minimap_enabled = true,
        minimap_width = 240,
        minimap_height = 150,
        minimap_margin = 24,
        minimap_opacity = 0.72,
        arrange_context_grouping = true,
        arrange_size_similarity = 0.12,
        arrange_resize_limit = 0.15,
        grid_enabled = true,
        grid_size = 80,
        grid_width = 1,
        grid_opacity = 0.16,
        grid_style = 1, -- 0 lines, 1 dots
        grid_dot_size = 3.2,
        background_dim = 0.35,
        viewport_enabled = false,
        viewport_width = 3,
        viewport_rounding = 18,
        viewport_border_opacity = 0.90,
        viewport_outside_opacity = 0.26,
        float_on_drag = true,
        allow_window_overflow = true,
        snap_viewport_on_pan = true,
        commit_viewport_on_close = true,
        snap_enabled = true,
        remember_layout = true, -- windows keep their spots across restarts
      },

      -- Type-to-search palette in the zoomed-out canvas.
      navigator = {
        enabled = true,
        labels = true,          -- window titles on the map
        dim_unmatched = 0.7,    -- how far non-matching windows recede (0..1)
        pointer = true,         -- click lands, drag moves or pans, wheel zooms
        accent = "#ff6b1a",     -- selected row, caret, key names; #rrggbb, a preset (cyan, violet, ...) or "auto" for the theme
        mono_font = "JetBrainsMono Nerd Font", -- the whole HUD is set in this

        -- Look (feature level 3). All of these are also in the live tuner.
        hud_scale = 1.0,
        width = 720,
        top = 0.075,
        rows = 6,
        row_height = 58,
        rounding = 11,
        panel_opacity = 0.94,
        uppercase = true,
        letter_spacing = 1.0,
        query_size = 17,
        title_size = 14,
        detail_size = 10.5,
        corner_size = 12,
        label_size = 10.5,
        corner_labels = true,
        search_height = 60,
        search_border = 1.5,
        search_border_opacity = 0.85,
        search_border_color = "accent",
        search_glow = 0.3,
      },

      -- The wallpaper as a far layer: it follows the camera at a fraction of
      -- the windows' speed and shrinks a little as you zoom out.
      parallax = {
        enabled = true,
        strength = 0.06,
        depth = 0.12,
        desktop = false,
      },

      chrome_animation = {
        enabled = true,
        top_namespace = "omarchy-bar",
        bottom_namespace = "omarchy-dock",
        top_travel = 1.35,
        bottom_travel = 1.08,
        top_scale = 0.82,
        bottom_scale = 0.92,
        opacity = 0.0,
      },

      wallpaper = 0,
      blur = false,
      blur_strength = 0.7,
      shadow = {
        enabled = false,
        range = 36,
      },
    },
  },
})

hl.curve("spatialOverviewEase", {
  type = "bezier",
  points = { { 0.22, 1.0 }, { 0.36, 1.0 } },
})

-- Settings changed with the live tuner (Ctrl+, in the zoomed-out canvas)
-- are saved to spatialoverview-tuning.lua and win over the values above.
-- Delete a line there to hand that setting back to this file.
if hl.plugin.spatialoverview.tuning_file then
  local ok, tuned = pcall(dofile, hl.plugin.spatialoverview.tuning_file())
  if ok and type(tuned) == "table" then
    local tree = {}
    for key, value in pairs(tuned) do
      local node, parts = tree, {}
      for part in string.gmatch(key, "[^:]+") do
        parts[#parts + 1] = part
      end
      for i = 1, #parts - 1 do
        node[parts[i]] = node[parts[i]] or {}
        node = node[parts[i]]
      end
      node[parts[#parts]] = value
    end
    hl.config({ plugin = { spatialoverview = tree } })
  end
end

-- The canvas. The first press of a session creates it and zooms out.
bind("SUPER + CTRL + G", "Zoom out and search", function()
  hl.plugin.spatialoverview.overview("toggle all")
end)

-- Runs a canvas action, or the `fallback` dispatcher when the canvas is not
-- open. (Inside a function, hl.dsp.* only builds a dispatcher; hl.dispatch
-- runs it.)
local function canvas_or(action, fallback)
  return function()
    if not pcall(hl.plugin.spatialoverview.canvas, action) then
      for _, dispatcher in ipairs(fallback) do
        hl.dispatch(dispatcher)
      end
    end
  end
end

-- Alt+Tab walks windows most recently used first: a tap flips to the
-- previous one, holding Alt shows the list. Without the canvas it cycles.
hl.unbind("ALT + TAB")
hl.unbind("ALT + SHIFT + TAB")
bind("ALT + TAB", "Recent windows", canvas_or("switch next", {
  hl.dsp.window.cycle_next(),
  hl.dsp.window.bring_to_top(),
}), { repeating = true })
bind("ALT + SHIFT + TAB", "Recent windows, backwards", canvas_or("switch prev", {
  hl.dsp.window.cycle_next({ next = false }),
  hl.dsp.window.bring_to_top(),
}), { repeating = true })

-- On the canvas every window floats, so SUPER + SHIFT + arrows nudge the
-- focused window a grid step (hold to keep going); tiled desktops swap.
for _, move in ipairs({
  { key = "LEFT", direction = "left", swap = "l" },
  { key = "RIGHT", direction = "right", swap = "r" },
  { key = "UP", direction = "up", swap = "u" },
  { key = "DOWN", direction = "down", swap = "d" },
}) do
  hl.unbind("SUPER + SHIFT + " .. move.key)
  bind("SUPER + SHIFT + " .. move.key, "Move window " .. move.direction, canvas_or("nudge " .. move.direction, {
    hl.dsp.window.swap({ direction = move.swap }),
  }), { repeating = true })
end

-- On the canvas, SUPER + T floats the focused window out of a tiled place
-- (or tiles it again); outside one it fills the screen it is on, and again
-- puts it back. Tiled desktops toggle floating.
hl.unbind("SUPER + T")
bind("SUPER + T", "Float in place / fill screen (canvas) / toggle floating", canvas_or("float", {
  hl.dsp.window.float({ action = "toggle" }),
}))

-- On the canvas, SUPER + M opens the menu of the place this screen shows
-- (arrows pick, Enter runs, Esc closes).
hl.unbind("SUPER + M")
bind("SUPER + M", "Place menu (canvas)", canvas_or("menu", {}))

-- Keys that tile, group, pop windows out or move workspaces between monitors
-- mean something else on the canvas, where every window floats and the
-- screens are one desk: SUPER + O pins the window where it is on the screen
-- (again puts it back on the canvas), SUPER + ALT + F fills the screen like
-- SUPER + T, and the tiling and grouping keys do nothing. Without the canvas
-- they work as before.
local canvas_keys = {
  { "SUPER + O", "Pin to the screen (canvas) / pop window out", "pin", { hl.dsp.exec_cmd("omarchy-hyprland-window-pop") } },
  { "SUPER + ALT + F", "Fill screen (canvas) / full width", "fill", { hl.dsp.window.fullscreen({ mode = "maximized" }) } },
  { "SUPER + J", "Toggle window split", "noop", { hl.dsp.layout("togglesplit") } },
  { "SUPER + P", "Pseudo window", "noop", { hl.dsp.window.pseudo() } },
  { "SUPER + L", "Toggle workspace layout", "noop", { hl.dsp.exec_cmd("omarchy-hyprland-workspace-layout-toggle") } },
  { "SUPER + Home", "Restore window width", "noop", { hl.dsp.exec_cmd("omarchy-hyprland-window-width restore") } },
  { "SUPER + ALT + Home", "Save window width", "noop", { hl.dsp.exec_cmd("omarchy-hyprland-window-width save") } },
}
-- Groups neither: windows on the canvas stack freely, and a group made there
-- crashes Hyprland when it quits. SUPER + ALT + G still takes a window out of
-- a group.
table.insert(canvas_keys, { "SUPER + G", "Toggle window grouping", "noop", { hl.dsp.group.toggle() } })
for _, move in ipairs({ { "LEFT", "l", "left" }, { "RIGHT", "r", "right" }, { "UP", "u", "up" }, { "DOWN", "d", "down" } }) do
  table.insert(canvas_keys, { "SUPER + SHIFT + ALT + " .. move[1], "Move place (canvas) / workspace to " .. move[3] .. " monitor", "assign " .. move[3], { hl.dsp.workspace.move({ monitor = move[2] }) } })
  table.insert(canvas_keys, { "SUPER + ALT + " .. move[1], "Move window to group on " .. move[3], "noop", { hl.dsp.window.move({ into_group = move[2] }) } })
end
for _, key in ipairs(canvas_keys) do
  hl.unbind(key[1])
  bind(key[1], key[2], canvas_or(key[3], key[4]))
end

-- Workspaces are places on the canvas: SUPER + 1…0 go to a place, SHIFT +
-- SUPER + N sends the focused window there and follows it, SHIFT + ALT +
-- SUPER + N sends it without following, SUPER + TAB / SHIFT + SUPER + TAB (and
-- SUPER + scroll) go to the place next door, CTRL + SUPER + TAB goes back
-- to the one you came from. Without the canvas they are workspaces.
for place = 1, 10 do
  local key = "code:" .. tostring(place + 9)
  local ws = tostring(place)
  hl.unbind("SUPER + " .. key)
  bind("SUPER + " .. key, "Go to place " .. place .. " (canvas) / workspace " .. place, canvas_or("go " .. place, { hl.dsp.focus({ workspace = ws }) }))
  hl.unbind("SUPER + SHIFT + " .. key)
  bind("SUPER + SHIFT + " .. key, "Send window to place " .. place .. " (canvas) / workspace " .. place, canvas_or("send " .. place, { hl.dsp.window.move({ workspace = ws }) }))
  hl.unbind("SUPER + SHIFT + ALT + " .. key)
  bind("SUPER + SHIFT + ALT + " .. key, "Send window to place " .. place .. ", stay (canvas) / workspace " .. place .. " silently", canvas_or("send " .. place .. " stay", { hl.dsp.window.move({ workspace = ws, follow = false }) }))
end
for _, step in ipairs({
  { "SUPER + TAB", "Next place (canvas) / workspace", "go next", "e+1" },
  { "SUPER + SHIFT + TAB", "Previous place (canvas) / workspace", "go prev", "e-1" },
  { "SUPER + CTRL + TAB", "Place before (canvas) / former workspace", "go back", "previous" },
  { "SUPER + mouse_down", "Next place (canvas) / workspace", "go next", "e+1" },
  { "SUPER + mouse_up", "Previous place (canvas) / workspace", "go prev", "e-1" },
}) do
  hl.unbind(step[1])
  bind(step[1], step[2], canvas_or(step[3], { hl.dsp.focus({ workspace = step[4] }) }))
end

-- SUPER + arrows focus the nearest window that way; on the canvas the camera
-- follows it.
for _, move in ipairs({
  { key = "LEFT", direction = "left", focus = "l" },
  { key = "RIGHT", direction = "right", focus = "r" },
  { key = "UP", direction = "up", focus = "u" },
  { key = "DOWN", direction = "down", focus = "d" },
}) do
  hl.unbind("SUPER + " .. move.key)
  bind("SUPER + " .. move.key, "Focus window " .. move.direction, function()
    if not pcall(hl.plugin.spatialoverview.navigate, move.direction) then
      hl.dispatch(hl.dsp.focus({ direction = move.focus }))
    end
  end)
end
