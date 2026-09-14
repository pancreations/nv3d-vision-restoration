# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender's window on the Windows desktop: where it is, and whether another window covers the viewport."""
import ctypes
import os
from ctypes import wintypes

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_dwmapi = ctypes.WinDLL("dwmapi")

GW_HWNDPREV = 3
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x20
DWMWA_EXTENDED_FRAME_BOUNDS = 9
DWMWA_CLOAKED = 14
# Click-through overlays and the taskbar never hide Blender's viewport.
_IGNORED_CLASSES = {"Shell_TrayWnd", "Shell_SecondaryTrayWnd", "VisionRestorationOutput"}
_WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

_user32.EnumWindows.argtypes = [_WNDENUMPROC, wintypes.LPARAM]
_user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_user32.IsWindow.argtypes = [wintypes.HWND]
_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.IsIconic.argtypes = [wintypes.HWND]
_user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
_user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
_user32.GetForegroundWindow.restype = wintypes.HWND
_user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
_user32.GetWindow.restype = wintypes.HWND
_user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.GetWindowLongW.restype = wintypes.LONG
_user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
_user32.IntersectRect.argtypes = [ctypes.POINTER(wintypes.RECT), ctypes.POINTER(wintypes.RECT), ctypes.POINTER(wintypes.RECT)]
_dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.UINT]


def _class_name(hwnd):
    name = ctypes.create_unicode_buffer(64)
    _user32.GetClassNameW(hwnd, name, 64)
    return name.value


def is_window(hwnd):
    return bool(hwnd) and bool(_user32.IsWindow(hwnd))


def client_size(hwnd):
    rect = wintypes.RECT()
    if not is_window(hwnd) or not _user32.GetClientRect(hwnd, ctypes.byref(rect)):
        return None
    return rect.right - rect.left, rect.bottom - rect.top


def client_origin(hwnd):
    if not is_window(hwnd):
        return None
    point = wintypes.POINT(0, 0)
    if not _user32.ClientToScreen(hwnd, ctypes.byref(point)):
        return None
    return point.x, point.y


def find_blender_window(width, height):
    """This process's GHOST window whose client area has Blender's window size."""
    pid, found = os.getpid(), []

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd) and _class_name(hwnd).startswith("GHOST"):
            found.append(hwnd)
        return True

    _user32.EnumWindows(_WNDENUMPROC(visit), 0)
    sized = [hwnd for hwnd in found if client_size(hwnd) == (width, height)] or found
    if not sized:
        return None
    foreground = _user32.GetForegroundWindow()
    return next((hwnd for hwnd in sized if hwnd == foreground), sized[0])


def _bounds(hwnd):
    rect = wintypes.RECT()
    if _dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(rect), ctypes.sizeof(rect)) == 0:
        return rect
    return rect if _user32.GetWindowRect(hwnd, ctypes.byref(rect)) else None


def _cloaked(hwnd):
    value = ctypes.c_int(0)
    return _dwmapi.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(value), ctypes.sizeof(value)) == 0 and value.value != 0


def uncovered(hwnd, rect):
    """True when Blender is not minimised and no visible window above it overlaps rect (left, top, width, height)."""
    if not is_window(hwnd) or _user32.IsIconic(hwnd):
        return False
    left, top, width, height = rect
    target = wintypes.RECT(left, top, left + width, top + height)
    above = _user32.GetWindow(hwnd, GW_HWNDPREV)
    for _ in range(1024):
        if not above:
            return True
        if (_user32.IsWindowVisible(above) and not (_user32.GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TRANSPARENT)
                and _class_name(above) not in _IGNORED_CLASSES and not _cloaked(above)):
            bounds, overlap = _bounds(above), wintypes.RECT()
            if bounds is not None and _user32.IntersectRect(ctypes.byref(overlap), ctypes.byref(target), ctypes.byref(bounds)):
                return False
        above = _user32.GetWindow(above, GW_HWNDPREV)
    return False
