"""
usrl.py - Unified System Runtime Library (Python Bindings)
Spaceflight-rated, high-performance IPC.
"""

import ctypes
import os
from ctypes import (
    Structure, POINTER, c_char_p, c_int, c_uint32, c_uint64, c_bool, c_void_p,
    c_size_t, c_char, cast, byref
)

# ============================================================================
# LIBRARY LOADING
# ============================================================================

def _load_library():
    search_paths = [
        os.path.abspath("./build/core/libusrl_core.so"),
        os.path.abspath("./libusrl_core.so"),
        "/usr/local/lib/libusrl_core.so",
        "/usr/lib/libusrl_core.so"
    ]
    if os.environ.get("USRL_LIB_PATH"):
        search_paths.insert(0, os.environ["USRL_LIB_PATH"])

    for path in search_paths:
        if os.path.exists(path):
            try:
                return ctypes.CDLL(path)
            except OSError:
                continue
    # Fallback to standard load (LD_LIBRARY_PATH)
    try:
        return ctypes.CDLL("libusrl_core.so")
    except OSError:
        pass

    raise RuntimeError("Could not load libusrl_core.so. Set USRL_LIB_PATH.")

_lib = _load_library()

# ============================================================================
# CONSTANTS & TYPES
# ============================================================================

USRL_RING_SWMR = 0
USRL_RING_MWMR = 1
USRL_RING_NO_DATA = -11

class UsrlSysConfig(Structure):
    _fields_ = [("app_name", c_char_p), ("log_level", c_int), ("log_file_path", c_char_p)]

class UsrlPubConfig(Structure):
    _fields_ = [
        ("topic", c_char_p), ("ring_type", c_int),
        ("slot_count", c_uint32), ("slot_size", c_uint32),
        ("rate_limit_hz", c_uint64), ("block_on_full", c_bool),
        ("schema_name", c_char_p)
    ]

class UsrlHealth(Structure):
    _fields_ = [
        ("operations", c_uint64), ("errors", c_uint64),
        ("rate_hz", c_uint64), ("lag", c_uint64), ("healthy", c_bool)
    ]

# Opaque Handles
UsrlCtxPtr = c_void_p
UsrlPubPtr = c_void_p
UsrlSubPtr = c_void_p

# ============================================================================
# C BINDINGS (argtypes/restype)
# ============================================================================

# usrl_init: usrl_ctx_t *usrl_init(const usrl_sys_config_t *config);
_lib.usrl_init.argtypes = [POINTER(UsrlSysConfig)]
_lib.usrl_init.restype = UsrlCtxPtr

# void usrl_shutdown(usrl_ctx_t *ctx);
_lib.usrl_shutdown.argtypes = [UsrlCtxPtr]
_lib.usrl_shutdown.restype = None

# usrl_pub_create/usrl_pub_destroy/usrl_pub_send/etc
_lib.usrl_pub_create.argtypes = [UsrlCtxPtr, POINTER(UsrlPubConfig)]
_lib.usrl_pub_create.restype = UsrlPubPtr

# int usrl_pub_send(usrl_pub_t *pub, const void *data, uint32_t len);
# Use c_void_p for the data pointer (we'll make sure to cast bytes -> c_char_p when needed)
_lib.usrl_pub_send.argtypes = [UsrlPubPtr, c_void_p, c_uint32]
_lib.usrl_pub_send.restype = c_int

_lib.usrl_pub_get_health.argtypes = [UsrlPubPtr, POINTER(UsrlHealth)]
_lib.usrl_pub_get_health.restype = None

_lib.usrl_pub_destroy.argtypes = [UsrlPubPtr]
_lib.usrl_pub_destroy.restype = None

# Subscriber bindings
_lib.usrl_sub_create.argtypes = [UsrlCtxPtr, c_char_p]
_lib.usrl_sub_create.restype = UsrlSubPtr

_lib.usrl_sub_recv.argtypes = [UsrlSubPtr, c_void_p, c_uint32]
_lib.usrl_sub_recv.restype = c_int

_lib.usrl_sub_get_health.argtypes = [UsrlSubPtr, POINTER(UsrlHealth)]
_lib.usrl_sub_get_health.restype = None

_lib.usrl_sub_destroy.argtypes = [UsrlSubPtr]
_lib.usrl_sub_destroy.restype = None

# Optional schema validation (if exported)
if hasattr(_lib, 'usrl_schema_validate'):
    _lib.usrl_schema_validate.argtypes = [c_void_p, c_char_p, c_void_p, c_uint32]
    _lib.usrl_schema_validate.restype = c_int

# ============================================================================
# PYTHON API
# ============================================================================

class USRL:
    def __init__(self, app_name="py_usrl", log_level=1, log_file_path=None):
        self._cfg = UsrlSysConfig(
            app_name.encode('utf-8') if app_name is not None else None,
            int(log_level),
            log_file_path.encode('utf-8') if log_file_path else None
        )
        self._ctx = _lib.usrl_init(byref(self._cfg))
        if not self._ctx:
            raise RuntimeError("Failed to initialize USRL context")
        self.publishers = []
        self.subscribers = []

    def publisher(self, topic, slots=4096, size=1024, rate_hz=0, block=False, mwmr=False, schema=None):
        pub = Publisher(self._ctx, topic, slots, size, rate_hz, block, mwmr, schema)
        self.publishers.append(pub)
        return pub

    def subscriber(self, topic, buffer_size=None):
        sub = Subscriber(self._ctx, topic, buffer_size)
        self.subscribers.append(sub)
        return sub

    def shutdown(self):
        for p in list(self.publishers):
            try:
                p.destroy()
            except Exception:
                pass
        for s in list(self.subscribers):
            try:
                s.destroy()
            except Exception:
                pass
        if self._ctx:
            _lib.usrl_shutdown(self._ctx)
            self._ctx = None

    def __enter__(self): return self
    def __exit__(self, exc_type, exc_val, exc_tb): self.shutdown()
    def __del__(self): 
        try:
            self.shutdown()
        except Exception:
            pass


class Publisher:
    def __init__(self, ctx, topic, slots, size, rate_hz, block, mwmr, schema):
        self._cfg = UsrlPubConfig()
        # store bytes so they remain alive while the C call uses the pointer ephemeral buffer
        self._topic_b = topic.encode('utf-8')
        self._cfg.topic = self._topic_b
        self._cfg.ring_type = USRL_RING_MWMR if mwmr else USRL_RING_SWMR
        self._cfg.slot_count = int(slots)
        self._cfg.slot_size = int(size)
        self._cfg.rate_limit_hz = int(rate_hz)
        self._cfg.block_on_full = bool(block)
        self._cfg.schema_name = schema.encode('utf-8') if schema else None

        self._handle = _lib.usrl_pub_create(ctx, byref(self._cfg))
        if not self._handle:
            raise RuntimeError(f"Failed to create publisher for {topic}")

    def send(self, payload):
        """
        Send bytes, string, or buffer.
        Returns the integer return code from usrl_pub_send (0 == success).
        """
        # Strings -> bytes
        if isinstance(payload, str):
            b = payload.encode('utf-8')
            # create c_char_p and cast to void*
            c_ptr = ctypes.c_char_p(b)
            return _lib.usrl_pub_send(self._handle, cast(c_ptr, c_void_p), len(b))

        # Immutable bytes (fast)
        if isinstance(payload, bytes):
            # c_char_p holds a pointer to internal buffer of bytes
            c_ptr = ctypes.c_char_p(payload)
            return _lib.usrl_pub_send(self._handle, cast(c_ptr, c_void_p), len(payload))

        # NumPy array / object exposing ctypes and nbytes
        if hasattr(payload, 'ctypes') and hasattr(payload, 'nbytes'):
            # ensure contiguous
            if hasattr(payload, 'flags') and not payload.flags['C_CONTIGUOUS']:
                payload = payload.copy()
            # get pointer and cast
            c_ptr = payload.ctypes.data_as(c_void_p)
            return _lib.usrl_pub_send(self._handle, c_ptr, payload.nbytes)

        # bytearray -> from_buffer zero-copy
        if isinstance(payload, bytearray):
            buf_type = (c_char * len(payload))
            buf = buf_type.from_buffer(payload)
            return _lib.usrl_pub_send(self._handle, cast(buf, c_void_p), len(payload))

        # generic buffer protocol fallback (memoryview)
        try:
            mv = memoryview(payload)
            if mv.readonly:
                # create bytes copy for read-only memoryviews
                b = bytes(mv)
                c_ptr = ctypes.c_char_p(b)
                return _lib.usrl_pub_send(self._handle, cast(c_ptr, c_void_p), len(b))
            # writable contiguous buffer -> from_buffer
            if not mv.contiguous:
                # make contiguous copy
                b = bytes(mv)
                c_ptr = ctypes.c_char_p(b)
                return _lib.usrl_pub_send(self._handle, cast(c_ptr, c_void_p), len(b))
            # create pointer from buffer
            buf = (c_char * mv.nbytes).from_buffer(mv)
            return _lib.usrl_pub_send(self._handle, cast(buf, c_void_p), mv.nbytes)
        except TypeError:
            raise TypeError(f"Invalid payload type {type(payload)}; must be str/bytes/bytearray/NumPy/memoryview")

    def stats(self):
        h = UsrlHealth()
        _lib.usrl_pub_get_health(self._handle, byref(h))
        return {
            "ops": int(h.operations), "drops": int(h.errors),
            "rate": int(h.rate_hz), "healthy": bool(h.healthy)
        }

    def destroy(self):
        if getattr(self, "_handle", None):
            _lib.usrl_pub_destroy(self._handle)
            self._handle = None

    def __del__(self): 
        try:
            self.destroy()
        except Exception:
            pass


class Subscriber:
    def __init__(self, ctx, topic, buffer_size=None):
        self._topic_b = topic.encode('utf-8')
        self._handle = _lib.usrl_sub_create(ctx, self._topic_b)
        if not self._handle:
            raise RuntimeError(f"Failed to create subscriber for {topic}")
        if buffer_size is None: buffer_size = 1024 * 1024
        self._buf_len = int(buffer_size)
        self._buf = ctypes.create_string_buffer(self._buf_len)

    def recv(self):
        """Returns bytes or None (No Data)."""
        ret = _lib.usrl_sub_recv(self._handle, cast(self._buf, c_void_p), self._buf_len)
        if ret >= 0:
            return bytes(self._buf[:ret])
        if ret == USRL_RING_NO_DATA:
            return None
        return None

    def recv_into(self, buffer):
        """
        Zero-copy receive into buffer.
        Returns number of bytes read (int) or None if no data.
        """
        # NumPy optimization
        if hasattr(buffer, 'ctypes') and hasattr(buffer, 'nbytes'):
            if hasattr(buffer, 'flags') and not buffer.flags['C_CONTIGUOUS']:
                raise ValueError("NumPy array must be C_CONTIGUOUS")
            c_ptr = buffer.ctypes.data_as(c_void_p)
            length = buffer.nbytes
        else:
            try:
                mv = memoryview(buffer)
            except TypeError:
                raise TypeError("Buffer must be writable and support buffer protocol")
            if mv.readonly:
                raise ValueError("Buffer must be writable")
            if not mv.contiguous:
                raise ValueError("Buffer must be contiguous")
            buf = (c_char * mv.nbytes).from_buffer(mv)
            c_ptr = cast(buf, c_void_p)
            length = mv.nbytes

        ret = _lib.usrl_sub_recv(self._handle, c_ptr, length)
        if ret >= 0:
            return int(ret)
        if ret == USRL_RING_NO_DATA:
            return None
        return None

    def stats(self):
        h = UsrlHealth()
        _lib.usrl_sub_get_health(self._handle, byref(h))
        return {
            "ops": int(h.operations), "skips": int(h.errors),
            "lag": int(h.lag), "healthy": bool(h.healthy)
        }

    def destroy(self):
        if getattr(self, "_handle", None):
            _lib.usrl_sub_destroy(self._handle)
            self._handle = None

    def __del__(self): 
        try:
            self.destroy()
        except Exception:
            pass


if __name__ == "__main__":
    print("USRL Python Bindings Loaded")
