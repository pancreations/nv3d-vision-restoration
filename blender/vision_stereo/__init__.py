# SPDX-License-Identifier: GPL-3.0-or-later
"""Vision Stereo: Blender's 3D viewport in frame-sequential stereo through Vision Restoration.

The 3D button in a viewport header renders that viewport once per eye with an off-axis stereo
camera and hands the pair to Vision Restoration, which shows it on the glasses in a window laid
exactly over the viewport. Mouse and keyboard still reach Blender underneath. The scene camera
gets the same stereo setup, so renders match what the viewport shows.
"""

bl_info = {
    "name": "Vision Stereo",
    "author": "Vision Restoration",
    "version": (1, 1, 0),
    "blender": (5, 1, 0),
    "location": "3D Viewport header > 3D",
    "description": "Shows Blender's 3D viewport on 3D Vision glasses through Vision Restoration",
    "category": "3D View",
}

import time
import traceback

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty
from bpy.types import AddonPreferences, Operator, Panel

from . import channel, stereo, viewport, win32


def prefs():
    return bpy.context.preferences.addons[__package__].preferences


class State:
    def __init__(self):
        self.reset()

    def reset(self):
        self.active = False
        self.window_ptr = 0
        self.area_ptr = 0
        self.hwnd = None
        self.sender = None
        self.renderer = None
        self.draw_handle = None
        self.in_draw = False
        self.connected = False
        self.pending = True
        self.last_frame = 0.0
        self.last_camera_sync = 0.0
        self.frames = 0
        self.fps = 0.0
        self.fps_start = time.perf_counter()
        self.frame_ms = 0.0
        self.size = None
        self.problem = ""
        self.error = ""
        self.status_key = None


state = State()


def _target():
    for window in bpy.context.window_manager.windows:
        if window.as_pointer() == state.window_ptr:
            area = next((a for a in window.screen.areas if a.as_pointer() == state.area_ptr and a.type == 'VIEW_3D'), None)
            return window, area
    return None, None


def _largest_view(window):
    views = [area for area in window.screen.areas if area.type == 'VIEW_3D'] if window is not None else []
    return max(views, key=lambda area: area.width * area.height) if views else None


def _redraw_panels():
    _, area = _target()
    if area is not None:
        for region in area.regions:
            if region.type in {'UI', 'HEADER'}:
                region.tag_redraw()


def _depth_changed(self, context):
    if state.active:
        if self.sync_camera and context.scene is not None:
            stereo.apply_render_setup(context.scene, self)
        state.pending = True


class VisionStereoPreferences(AddonPreferences):
    bl_idname = __package__

    depth_strength: FloatProperty(name="Depth", default=1 / 30, min=0.0, soft_max=0.1, max=0.5, precision=4, step=0.1,
                                  description="Eye separation as a fraction of the screen-plane distance (1/30 is comfortable)",
                                  update=_depth_changed)
    convergence_mode: EnumProperty(
        name="Screen plane",
        items=[
            ('AUTO', "Automatic", "Viewport: the orbit centre sits on the screen. Camera: its focus distance with depth of field, otherwise its own convergence distance"),
            ('FIXED', "Fixed distance", "The same screen-plane distance for the viewport and the camera"),
        ],
        default='AUTO', update=_depth_changed)
    convergence_distance: FloatProperty(name="Screen distance", default=5.0, min=0.001, subtype='DISTANCE', update=_depth_changed)
    # Two full-size 4K eye renders plus readback starve DWM composition: the glasses lost sync ~5 times a second
    # while orbiting at 1.0 and never at 0.5 (measured 2026-09-13, RTX 5070 Ti, 240 Hz).
    resolution: FloatProperty(name="Resolution", default=0.5, min=0.25, max=1.0, subtype='FACTOR',
                              description="Eye image size as a fraction of the viewport. Higher is sharper, but above 0.5 on a 4K viewport the glasses can lose sync while the view moves",
                              update=_depth_changed)
    max_rate: IntProperty(name="Max frames per second", default=30, min=5, max=120,
                          description="Upper limit on new stereo frames while the view changes; each one costs two extra viewport renders")
    sync_camera: BoolProperty(name="Camera uses the same depth", default=True,
                              description="Turn stereo rendering on and give the scene camera this stereo setup, so renders match the viewport",
                              update=_depth_changed)

    def draw(self, context):
        draw_settings(self.layout, self)


def draw_settings(layout, p):
    col = layout.column()
    col.prop(p, "depth_strength", slider=True)
    col.prop(p, "convergence_mode")
    if p.convergence_mode == 'FIXED':
        col.prop(p, "convergence_distance")
    col.separator()
    col.prop(p, "resolution", slider=True)
    col.prop(p, "max_rate")
    col.prop(p, "sync_camera")


def _draw():
    if not state.active or state.in_draw or not state.connected:
        return
    context = bpy.context
    window, area, region = context.window, context.area, context.region
    if window is None or area is None or region is None or area.as_pointer() != state.area_ptr or window.as_pointer() != state.window_ptr:
        return
    space = context.space_data
    if len(space.region_quadviews):
        state.problem = "Quad view is not supported"
        return
    p = prefs()
    now = time.perf_counter()
    if now - state.last_frame < 1.0 / p.max_rate:
        state.pending = True
        return
    rect = viewport.inner_rect(area, region, space)
    if rect[2] - rect[0] < 32 or rect[3] - rect[1] < 32:
        return
    state.in_draw = True
    try:
        left, right = state.renderer.render(context, region, context.region_data or space.region_3d, rect, p.resolution, p)
        height, width = left.shape[:2]
        index, packed = state.sender.frame(width, height)
        packed[:, :width] = left
        packed[:, width:] = right
        del packed, left, right
        state.sender.commit(index, width, height)
        state.frame_ms = (time.perf_counter() - now) * 1000
        state.size = (width, height)
        state.frames += 1
        state.problem = ""
    except Exception:
        state.error = traceback.format_exc(limit=4)
        traceback.print_exc()
    finally:
        state.last_frame = now
        state.pending = False
        state.in_draw = False


def _tick():
    if not state.active:
        return None
    try:
        p = prefs()
        now = time.perf_counter()
        window, area = _target()
        if window is None and bpy.context.window_manager.windows:
            window = bpy.context.window_manager.windows[0]
            state.window_ptr, state.hwnd = window.as_pointer(), None
        if window is not None and area is None:
            area = _largest_view(window)
            if area is not None:
                state.area_ptr, state.pending = area.as_pointer(), True
        host = state.sender.host()
        if host.connected and not state.connected:
            state.pending = True
        state.connected = host.connected
        region = viewport.main_region(area) if area is not None else None
        rect, visible = None, False
        if region is not None:
            if win32.client_size(state.hwnd) != (window.width, window.height):
                state.hwnd = win32.find_blender_window(window.width, window.height)
            origin = win32.client_origin(state.hwnd)
            x0, y0, x1, y1 = viewport.inner_rect(area, region, area.spaces.active)
            if origin is not None and x1 - x0 >= 32 and y1 - y0 >= 32:
                rect = (origin[0] + region.x + x0, origin[1] + window.height - (region.y + y1), x1 - x0, y1 - y0)
                visible = win32.uncovered(state.hwnd, rect)
        state.sender.set_view(True, visible, rect)
        if state.connected and area is not None and state.pending and now - state.last_frame >= 1.0 / p.max_rate:
            area.tag_redraw()
        if p.sync_camera and window is not None and now - state.last_camera_sync > 0.5:
            state.last_camera_sync = now
            stereo.apply_render_setup(window.scene, p)
        if now - state.fps_start >= 1.0:
            state.fps, state.frames, state.fps_start = state.frames / (now - state.fps_start), 0, now
        key = (host.connected, host.output_running, host.message, round(state.fps), state.problem, state.error, area is None)
        if key != state.status_key:
            state.status_key = key
            _redraw_panels()
        return 0.05
    except Exception:
        state.error = traceback.format_exc(limit=4)
        return 0.5


def activate(window, area):
    if state.active:
        state.window_ptr, state.area_ptr, state.pending = window.as_pointer(), area.as_pointer(), True
        area.tag_redraw()
        return
    state.reset()
    state.window_ptr, state.area_ptr = window.as_pointer(), area.as_pointer()
    state.sender = channel.Sender(f"Blender {bpy.app.version_string}")
    state.active = True
    state.renderer = viewport.EyeRenderer()
    state.hwnd = win32.find_blender_window(window.width, window.height)
    state.draw_handle = bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'WINDOW', 'POST_PIXEL')
    p = prefs()
    if p.sync_camera:
        stereo.apply_render_setup(window.scene, p)
    bpy.app.timers.register(_tick, first_interval=0.01)
    area.tag_redraw()


def deactivate():
    if not state.active:
        return
    _, area = _target()
    state.active = False
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if state.draw_handle is not None:
        bpy.types.SpaceView3D.draw_handler_remove(state.draw_handle, 'WINDOW')
    if state.sender is not None:
        state.sender.close()
    if state.renderer is not None:
        state.renderer.free()
    state.reset()
    if area is not None:
        area.tag_redraw()


class VISION_STEREO_OT_toggle(Operator):
    bl_idname = "vision_stereo.toggle"
    bl_label = "3D Viewport"
    bl_description = "Show this viewport in 3D on the glasses through Vision Restoration"

    @classmethod
    def poll(cls, context):
        return context.window is not None

    def execute(self, context):
        area = context.area if context.area is not None and context.area.type == 'VIEW_3D' else _largest_view(context.window)
        if area is None:
            self.report({'ERROR'}, "No 3D viewport to show")
            return {'CANCELLED'}
        try:
            if state.active and area.as_pointer() == state.area_ptr:
                deactivate()
            else:
                activate(context.window, area)
        except Exception as error:
            deactivate()
            self.report({'ERROR'}, f"Vision Stereo: {error}")
            return {'CANCELLED'}
        return {'FINISHED'}


def _is_on(area):
    return state.active and area is not None and area.as_pointer() == state.area_ptr


def _header_button(self, context):
    self.layout.operator(VISION_STEREO_OT_toggle.bl_idname, text="3D", icon='CAMERA_STEREO', depress=_is_on(context.area))


class VISION_STEREO_PT_panel(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Stereo"
    bl_label = "Vision Stereo"

    def draw(self, context):
        layout = self.layout
        on = _is_on(context.area)
        layout.operator(VISION_STEREO_OT_toggle.bl_idname, text="Turn 3D off" if on else "Turn 3D on", icon='CAMERA_STEREO', depress=on)
        if state.active and state.sender is not None:
            host = state.sender.host()
            col = layout.column(align=True)
            if not host.connected:
                col.label(text="Start Vision Restoration", icon='UNLINKED')
            else:
                col.label(text=host.message, icon='HIDE_OFF' if host.output_running else 'LINKED')
            if state.size is not None:
                col.label(text=f"{state.size[0]} x {state.size[1]} per eye, {state.fps:.0f} fps, {state.frame_ms:.0f} ms")
            if state.problem:
                col.label(text=state.problem, icon='ERROR')
            col.label(text="Menus opened over the viewport are hidden while 3D is on", icon='INFO')
        if state.error:
            layout.label(text=state.error.strip().splitlines()[-1], icon='ERROR')
        draw_settings(layout, prefs())


_classes = (VisionStereoPreferences, VISION_STEREO_OT_toggle, VISION_STEREO_PT_panel)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.VIEW3D_HT_header.append(_header_button)


def unregister():
    deactivate()
    bpy.types.VIEW3D_HT_header.remove(_header_button)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
