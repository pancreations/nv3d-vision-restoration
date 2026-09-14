# SPDX-License-Identifier: GPL-3.0-or-later
"""Sender end of Vision Restoration's frame channel (mirrors src/frame_channel.h)."""
import ctypes
import mmap
import os
import threading
from ctypes import c_char, c_int32, c_int64, c_uint32, c_uint64, wintypes

import numpy as np

HEADER_NAME = "Local\\VisionRestoration.FrameChannel.v2"
EVENT_NAME = HEADER_NAME + ".Frame"
MAGIC = 0x4233564E
VERSION = 2
SLOTS = 3
FORMAT_RGBA8_BOTTOM_UP = 0


class Slot(ctypes.Structure):
    _fields_ = [("seq", c_uint32), ("width", c_uint32), ("height", c_uint32), ("packing", c_uint32), ("frame_id", c_uint64)]


class Header(ctypes.Structure):
    _fields_ = [
        ("magic", c_uint32), ("version", c_uint32),
        ("sender_pid", c_uint32), ("generation", c_uint32), ("sender_heartbeat", c_int64),
        ("slot_bytes", c_uint64),
        ("format", c_uint32), ("latest_slot", c_int32),
        ("active", c_int32), ("visible", c_int32),
        ("view_left", c_int32), ("view_top", c_int32), ("view_width", c_int32), ("view_height", c_int32),
        ("slots", Slot * SLOTS),
        ("sender_name", c_char * 64),
        ("host_pid", c_uint32), ("host_state", c_int32), ("host_heartbeat", c_int64),
        ("frames_received", c_uint64), ("frames_torn", c_uint64),
        ("output_running", c_int32), ("emitter_ready", c_int32),
        ("host_message", c_char * 160),
    ]


assert ctypes.sizeof(Slot) == 24 and ctypes.sizeof(Header) == 400

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.CreateEventW.restype = wintypes.HANDLE
_kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
_kernel32.SetEvent.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_frequency = ctypes.c_int64()
_kernel32.QueryPerformanceFrequency(ctypes.byref(_frequency))


def qpc_ticks():
    value = ctypes.c_int64()
    _kernel32.QueryPerformanceCounter(ctypes.byref(value))
    return value.value


class HostInfo:
    def __init__(self, header):
        age = (qpc_ticks() - header.host_heartbeat) / _frequency.value
        self.connected = header.host_pid != 0 and header.host_heartbeat != 0 and 0 <= age < 1.5
        self.output_running = self.connected and bool(header.output_running)
        self.emitter_ready = bool(header.emitter_ready)
        self.frames_received = header.frames_received
        self.frames_torn = header.frames_torn
        self.message = header.host_message.decode("utf-8", "replace") if self.connected else ""


class Sender:
    def __init__(self, name):
        self._header_map = mmap.mmap(-1, ctypes.sizeof(Header), tagname=HEADER_NAME)
        self._h = Header.from_buffer(self._header_map)
        h = self._h
        h.magic, h.version = MAGIC, VERSION
        h.format = FORMAT_RGBA8_BOTTOM_UP
        h.latest_slot = -1
        h.slot_bytes = 0
        h.active = h.visible = 0
        h.generation += 1
        h.sender_name = name.encode("utf-8")[:63]
        h.sender_pid = os.getpid()
        h.sender_heartbeat = qpc_ticks()
        self._data_map = None
        self._data = None
        self._frame_id = 0
        self._event = _kernel32.CreateEventW(None, False, False, EVENT_NAME)
        self._stop = threading.Event()
        # The heartbeat runs off Blender's main thread so a long operation does not drop the link.
        self._beat = threading.Thread(target=self._heartbeat, name="Vision Stereo heartbeat", daemon=True)
        self._beat.start()

    def _heartbeat(self):
        while not self._stop.wait(0.1):
            self._h.sender_heartbeat = qpc_ticks()

    def host(self):
        return HostInfo(self._h)

    def view(self):
        h = self._h
        return bool(h.visible), (h.view_left, h.view_top, h.view_width, h.view_height)

    def set_view(self, active, visible, rect):
        h = self._h
        if rect is None:
            rect, visible = (0, 0, 0, 0), False
        h.view_left, h.view_top, h.view_width, h.view_height = rect
        h.visible = 1 if visible else 0
        h.active = 1 if active else 0

    def _ensure_capacity(self, nbytes):
        h = self._h
        if self._data is not None and nbytes <= h.slot_bytes:
            return
        h.latest_slot = -1
        capacity = (max(nbytes, 1 << 20) + (1 << 20) - 1) & ~((1 << 20) - 1)
        generation = h.generation + 1
        new_map = mmap.mmap(-1, capacity * SLOTS, tagname=f"{HEADER_NAME}.Data.{os.getpid()}.{generation}")
        self._release_data()
        self._data_map = new_map
        self._data = np.frombuffer(new_map, dtype=np.uint8)
        for slot in h.slots:
            slot.seq = 0
        h.slot_bytes = capacity
        h.generation = generation

    def frame(self, eye_width, height):
        """(slot index, writable (height, 2 * eye_width, 4) array) for the next side-by-side pair."""
        width = eye_width * 2
        nbytes = width * height * 4
        self._ensure_capacity(nbytes)
        h = self._h
        index = (h.latest_slot + 1) % SLOTS if h.latest_slot >= 0 else 0
        slot = h.slots[index]
        slot.seq += 1 if slot.seq % 2 == 0 else 2
        start = index * h.slot_bytes
        return index, self._data[start:start + nbytes].reshape(height, width, 4)

    def commit(self, index, eye_width, height):
        h = self._h
        slot = h.slots[index]
        self._frame_id += 1
        slot.width, slot.height, slot.packing = eye_width * 2, height, 0
        slot.frame_id = self._frame_id
        slot.seq += 1
        h.latest_slot = index
        if self._event:
            _kernel32.SetEvent(self._event)

    def _release_data(self):
        self._data = None
        if self._data_map is not None:
            try:
                self._data_map.close()
            except BufferError:
                pass
            self._data_map = None

    def close(self):
        self._stop.set()
        self._beat.join(1.0)
        h = self._h
        h.active = h.visible = 0
        h.latest_slot = -1
        h.sender_heartbeat = 0
        h.sender_pid = 0
        del h
        self._h = None
        self._release_data()
        try:
            self._header_map.close()
        except BufferError:
            pass
        if self._event:
            _kernel32.CloseHandle(self._event)
            self._event = None
