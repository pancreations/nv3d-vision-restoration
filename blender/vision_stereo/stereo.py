# SPDX-License-Identifier: GPL-3.0-or-later
"""One stereo setup for the viewport and for renders.

Off-axis projection: each eye moves sideways by half the interocular distance and its frustum
is sheared so the convergence distance lands on the screen plane. Blender's OFFAXIS stereo camera
does the same, so looking through the camera in the viewport matches the rendered pair.
"""
from mathutils import Matrix, Vector


def camera_convergence(camera_object, prefs):
    if prefs.convergence_mode == 'FIXED':
        return prefs.convergence_distance
    data = camera_object.data
    if data.dof.use_dof:
        target = data.dof.focus_object
        if target is not None:
            axis = camera_object.matrix_world.to_3x3() @ Vector((0.0, 0.0, -1.0))
            offset = target.matrix_world.to_translation() - camera_object.matrix_world.to_translation()
            return max(1e-3, offset.dot(axis.normalized()))
        return max(1e-3, data.dof.focus_distance)
    return data.stereo.convergence_distance


def _set(owner, name, value, tolerance=None):
    current = getattr(owner, name)
    if current != value if tolerance is None else abs(current - value) > tolerance:
        setattr(owner, name, value)


def apply_render_setup(scene, prefs):
    """Turn stereo rendering on and give the scene camera the stereo setup the viewport uses."""
    render = scene.render
    _set(render, "use_multiview", True)
    _set(render, "views_format", 'STEREO_3D')
    camera = scene.camera
    if camera is None or camera.type != 'CAMERA':
        return
    stereo = camera.data.stereo
    _set(stereo, "convergence_mode", 'OFFAXIS')
    convergence = camera_convergence(camera, prefs)
    _set(stereo, "convergence_distance", convergence, 1e-5)
    _set(stereo, "interocular_distance", convergence * prefs.depth_strength, 1e-6)


def _offsets(pivot, interocular):
    if pivot == 'LEFT':
        return (0.0, interocular)
    if pivot == 'RIGHT':
        return (-interocular, 0.0)
    return (-interocular / 2, interocular / 2)


def eye_matrices(scene, region_3d, prefs):
    """[(view, projection) for the left eye, then the right eye]."""
    view, projection = region_3d.view_matrix, region_3d.window_matrix
    camera = scene.camera
    if region_3d.view_perspective == 'CAMERA' and camera is not None and camera.type == 'CAMERA':
        stereo = camera.data.stereo
        convergence = stereo.convergence_distance
        offsets = _offsets(stereo.pivot, stereo.interocular_distance)
        off_axis = stereo.convergence_mode != 'PARALLEL'
    else:
        convergence = region_3d.view_distance if prefs.convergence_mode == 'AUTO' else prefs.convergence_distance
        offsets = _offsets('CENTER', convergence * prefs.depth_strength)
        off_axis = True
    convergence = max(convergence, 1e-4)
    eyes = []
    for offset in offsets:
        eye_projection = projection.copy()
        if region_3d.is_perspective:
            eye_view = Matrix.Translation((-offset, 0.0, 0.0)) @ view
            if off_axis:
                eye_projection[0][2] -= projection[0][0] * offset / convergence
        else:
            shear = Matrix.Identity(4)
            shear[0][3] = -offset
            if off_axis:
                shear[0][2] = -offset / convergence
            eye_view = shear @ view
        eyes.append((eye_view, eye_projection))
    return eyes


def crop_projection(projection, width, height, rect):
    """Restrict a region's projection to rect (x0, y0, x1, y1 in region pixels) so it fills an image of that size."""
    x0, y0, x1, y1 = rect
    left, right = 2 * x0 / width - 1, 2 * x1 / width - 1
    bottom, top = 2 * y0 / height - 1, 2 * y1 / height - 1
    crop = Matrix.Identity(4)
    crop[0][0], crop[0][3] = 2 / (right - left), -(right + left) / (right - left)
    crop[1][1], crop[1][3] = 2 / (top - bottom), -(top + bottom) / (top - bottom)
    return crop @ projection
