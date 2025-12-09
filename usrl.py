"""
usrl.py - Python Bindings for USRL (Unified System Runtime Library)
Spaceflight-rated, high-performance IPC.
"""

import ctypes
import os
import time
from ctypes import Structure, POINTER, c_char_p, c_int, c_uint32, c_uint64, c_bool, c_void_p

# Load the shared library
# Assumes libusrl.so is in the current directory or LD_LIBRARY_PATH
_lib = ctypes.CDLL(os.path.abspath("./build/core/libusrl_core.so"))

# ============================================================================
# C STRUCTURE MAPPING
# ============================================================================

class UsrlSysConfig(Structure):
    _fields_ = [
        ("app_name", c_char_p),
        ("log_level", c_int),
        ("log_file_path", c_char_p)
    ]

class UsrlPubConfig(Structure):
    _fields_ = [
        ("topic", c_char_p),
        ("ring_type", c_int),  # 0=SWMR, 1=MWMR
        ("slot_count", c_uint32),
        ("slot_size", c_uint32),
        ("rate_limit_hz", c_uint64),
        ("block_on_full", c_bool),
        ("schema_name", c_char_p)
    ]

class UsrlHealth(Structure):
    _fields_ = [
        ("operations", c_uint64),
        ("errors", c_uint64),
        ("rate_hz", c_uint64),
        ("lag", c_uint64),
        ("healthy", c_bool)
    ]

# Opaque Pointers
UsrlCtxPtr = c_void_p
UsrlPubPtr = c_void_p
UsrlSubPtr = c_void_p

# ============================================================================
# C FUNCTION SIGNATURES
# ============================================================================

_lib.usrl_init.argtypes = [POINTER(UsrlSysConfig)]
_lib.usrl_init.restype = UsrlCtxPtr

_lib.usrl_shutdown.argtypes = [UsrlCtxPtr]

_lib.usrl_pub_create.argtypes = [UsrlCtxPtr, POINTER(UsrlPubConfig)]
_lib.usrl_pub_create.restype = UsrlPubPtr

_lib.usrl_pub_send.argtypes = [UsrlPubPtr, c_void_p, c_uint32]
_lib.usrl_pub_send.restype = c_int

_lib.usrl_pub_get_health.argtypes = [UsrlPubPtr, POINTER(UsrlHealth)]

_lib.usrl_pub_destroy.argtypes = [UsrlPubPtr]

_lib.usrl_sub_create.argtypes = [UsrlCtxPtr, c_char_p]
_lib.usrl_sub_create.restype = UsrlSubPtr

_lib.usrl_sub_recv.argtypes = [UsrlSubPtr, c_void_p, c_uint32]
_lib.usrl_sub_recv.restype = c_int

_lib.usrl_sub_get_health.argtypes = [UsrlSubPtr, POINTER(UsrlHealth)]

_lib.usrl_sub_destroy.argtypes = [UsrlSubPtr]

# ============================================================================
# PYTHONIC WRAPPER CLASSES
# ============================================================================

class USRL:
    def __init__(self, app_name="py_usrl", log_level=2): # 2=INFO
        self._cfg = UsrlSysConfig()
        self._cfg.app_name = app_name.encode('utf-8')
        self._cfg.log_level = log_level
        self._cfg.log_file_path = None
        self._ctx = _lib.usrl_init(ctypes.byref(self._cfg))
        if not self._ctx:
            raise RuntimeError("Failed to initialize USRL context")

    def shutdown(self):
        if self._ctx:
            _lib.usrl_shutdown(self._ctx)
            self._ctx = None

    def create_publisher(self, topic, slots=4096, size=1024, rate_hz=0, block=False):
        return Publisher(self._ctx, topic, slots, size, rate_hz, block)

    def create_subscriber(self, topic, buffer_size=1024):
        return Subscriber(self._ctx, topic, buffer_size)

    def __del__(self):
        self.shutdown()

class Publisher:
    def __init__(self, ctx, topic, slots, size, rate_hz, block):
        self._cfg = UsrlPubConfig()
        self._cfg.topic = topic.encode('utf-8')
        self._cfg.ring_type = 0 # SWMR default
        self._cfg.slot_count = slots
        self._cfg.slot_size = size
        self._cfg.rate_limit_hz = rate_hz
        self._cfg.block_on_full = block
        self._cfg.schema_name = None
        
        self._handle = _lib.usrl_pub_create(ctx, ctypes.byref(self._cfg))
        if not self._handle:
            raise RuntimeError(f"Failed to create publisher for {topic}")

    def send(self, payload):
        if isinstance(payload, str):
            payload_bytes = payload.encode('utf-8')
        elif isinstance(payload, (bytes, bytearray)):
            payload_bytes = payload
        else:
            raise TypeError("Send data must be bytes or string")
            
        return _lib.usrl_pub_send(self._handle, payload_bytes, len(payload_bytes))

    def stats(self):
        h = UsrlHealth()
        _lib.usrl_pub_get_health(self._handle, ctypes.byref(h))
        return {
            "ops": h.operations,
            "drops": h.errors,
            "rate": h.rate_hz,
            "healthy": h.healthy
        }

    def destroy(self):
        if self._handle:
            _lib.usrl_pub_destroy(self._handle)
            self._handle = None

    def __del__(self):
        self.destroy()


class Subscriber:
    def __init__(self, ctx, topic, buffer_size=4096):
        self._handle = _lib.usrl_sub_create(ctx, topic.encode('utf-8'))
        if not self._handle:
            raise RuntimeError(f"Failed to create subscriber for {topic}")
        # Pre-allocate read buffer
        self._buf = ctypes.create_string_buffer(buffer_size)
        self._buf_len = buffer_size

    def recv(self):
        """Returns bytes or None"""
        ret = _lib.usrl_sub_recv(self._handle, self._buf, self._buf_len)
        if ret > 0:
            return self._buf[:ret]
        return None

    def stats(self):
        h = UsrlHealth()
        _lib.usrl_sub_get_health(self._handle, ctypes.byref(h))
        return {
            "ops": h.operations,
            "skips": h.errors,
            "rate": h.rate_hz,
            "lag": h.lag,
            "healthy": h.healthy
        }

    def destroy(self):
        if self._handle:
            _lib.usrl_sub_destroy(self._handle)
            self._handle = None

    def __del__(self):
        self.destroy()
