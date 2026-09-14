# SPDX-License-Identifier: GPL-3.0-or-later
"""Renders one 3D viewport for both eyes."""
import gpu
import numpy as np

from . import stereo

# Regions drawn over the viewport stay outside the 3D image, so they remain visible and clickable.
_SHOWN = {
    'TOOLS': "show_region_toolbar",
    'UI': "show_region_ui",
    'HEADER': "show_region_header",
    'TOOL_HEADER': "show_region_tool_header",
    'ASSET_SHELF': "show_region_asset_shelf",
    'ASSET_SHELF_HEADER': "show_region_asset_shelf",
}


def main_region(area):
    return next((region for region in area.regions if region.type == 'WINDOW'), None)


def inner_rect(area, region, space):
    """The main region minus the headers, toolbars and sidebars over it: (x0, y0, x1, y1) in region pixels from the bottom left."""
    x0, y0 = region.x, region.y
    x1, y1 = x0 + region.width, y0 + region.height
    for other in area.regions:
        if other.type == 'WINDOW' or other.width < 2 or other.height < 2:
            continue
        shown = _SHOWN.get(other.type)
        if shown is not None and not getattr(space, shown, True):
            continue
        ox0, oy0 = other.x, other.y
        ox1, oy1 = ox0 + other.width, oy0 + other.height
        if ox1 <= x0 or ox0 >= x1 or oy1 <= y0 or oy0 >= y1:
            continue
        if other.alignment == 'LEFT':
            x0 = max(x0, ox1)
        elif other.alignment == 'RIGHT':
            x1 = min(x1, ox0)
        elif other.alignment == 'TOP':
            y1 = min(y1, oy0)
        elif other.alignment == 'BOTTOM':
            y0 = max(y0, oy1)
    return x0 - region.x, y0 - region.y, x1 - region.x, y1 - region.y


class EyeRenderer:
    def __init__(self):
        self._offscreen = None
        self._buffers = None
        self.size = None

    def free(self):
        if self._offscreen is not None:
            self._offscreen.free()
        self._offscreen = self._buffers = self.size = None

    def render(self, context, region, region_3d, rect, scale, prefs):
        """Left and right (height, width, 4) RGBA images of rect, rows bottom-up. The next call reuses their memory."""
        x0, y0, x1, y1 = rect
        width, height = max(2, round((x1 - x0) * scale)), max(2, round((y1 - y0) * scale))
        if self.size != (width, height):
            self.free()
            self._offscreen = gpu.types.GPUOffScreen(width, height, format='RGBA8')
            # One-dimensional buffers: numpy can wrap them directly (a multi-dimensional read result cannot be).
            self._buffers = [gpu.types.Buffer('UBYTE', width * height * 4) for _ in range(2)]
            self.size = (width, height)
        eyes = []
        for (view, projection), buffer in zip(stereo.eye_matrices(context.scene, region_3d, prefs), self._buffers):
            projection = stereo.crop_projection(projection, region.width, region.height, rect)
            self._offscreen.draw_view3d(context.scene, context.view_layer, context.space_data, region, view, projection,
                                        do_color_management=True)
            with self._offscreen.bind():
                gpu.state.active_framebuffer_get().read_color(0, 0, width, height, 4, 0, 'UBYTE', data=buffer)
            eyes.append(np.frombuffer(buffer, np.uint8).reshape(height, width, 4))
        return eyes
