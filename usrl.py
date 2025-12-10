"""
usrl.py - Unified System Runtime Library (Python Bindings)
Spaceflight-rated, high-performance IPC.
"""

import ctypes
import os
from ctypes import Structure, POINTER, c_char_p, c_int, c_uint32, c_uint64, c_bool, c_void_p

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
            try: return ctypes.CDLL(path)
            except OSError: continue
    # Fallback to standard load (LD_LIBRARY_PATH)
    try: return ctypes.CDLL("libusrl_core.so")
    except OSError: pass
    
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
# C BINDINGS
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

if hasattr(_lib, 'usrl_schema_validate'):
    _lib.usrl_schema_validate.argtypes = [c_void_p, c_char_p, c_void_p, c_uint32]
    _lib.usrl_schema_validate.restype = c_int

# ============================================================================
# PYTHON API
# ============================================================================

class USRL:
    def __init__(self, app_name="py_usrl", log_level=1):
        self._cfg = UsrlSysConfig(app_name.encode('utf-8'), log_level, None)
        self._ctx = _lib.usrl_init(ctypes.byref(self._cfg))
        if not self._ctx: raise RuntimeError("Failed to initialize USRL context")
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
        for p in self.publishers: p.destroy()
        for s in self.subscribers: s.destroy()
        if self._ctx:
            _lib.usrl_shutdown(self._ctx)
            self._ctx = None

    def __enter__(self): return self
    def __exit__(self, exc_type, exc_val, exc_tb): self.shutdown()
    def __del__(self): self.shutdown()


class Publisher:
    def __init__(self, ctx, topic, slots, size, rate_hz, block, mwmr, schema):
        self._cfg = UsrlPubConfig()
        self._cfg.topic = topic.encode('utf-8')
        self._cfg.ring_type = USRL_RING_MWMR if mwmr else USRL_RING_SWMR
        self._cfg.slot_count = slots
        self._cfg.slot_size = size
        self._cfg.rate_limit_hz = rate_hz
        self._cfg.block_on_full = block
        self._cfg.schema_name = schema.encode('utf-8') if schema else None
        
        self._handle = _lib.usrl_pub_create(ctx, ctypes.byref(self._cfg))
        if not self._handle: raise RuntimeError(f"Failed to create publisher for {topic}")

    def send(self, payload):
        """
        Send bytes, string, or buffer.
        """
        # 1. String -> Bytes
        if isinstance(payload, str):
            payload = payload.encode('utf-8')
            return _lib.usrl_pub_send(self._handle, payload, len(payload))

        # 2. Immutable Bytes (Fast Path)
        if isinstance(payload, bytes):
            return _lib.usrl_pub_send(self._handle, payload, len(payload))
            
        # 3. NumPy (High Performance Zero Copy)
        if hasattr(payload, 'ctypes') and hasattr(payload, 'nbytes'):
             if hasattr(payload, 'flags') and not payload.flags['C_CONTIGUOUS']:
                 payload = payload.copy() 
             c_ptr = payload.ctypes.data_as(c_void_p)
             return _lib.usrl_pub_send(self._handle, c_ptr, payload.nbytes)

        # 4. Mutable Buffer (bytearray) -> Zero Copy
        if isinstance(payload, bytearray):
             c_type = ctypes.c_char * len(payload)
             c_ptr = c_type.from_buffer(payload)
             return _lib.usrl_pub_send(self._handle, c_ptr, len(payload))

        # 5. Generic Buffer Protocol / Fallback
        # FIX: Explicitly check memoryview() to reject integers (bytes(int) is valid but wrong here)
        try:
            # This raises TypeError for int, dict, None, etc.
            mv = memoryview(payload) 
            
            # Now safe to convert to bytes (handles read-only memoryviews safely)
            data = bytes(payload)
            return _lib.usrl_pub_send(self._handle, data, len(data))
        except TypeError:
             raise TypeError(f"Invalid type: {type(payload)}. Must support buffer protocol.")


    def stats(self):
        h = UsrlHealth()
        _lib.usrl_pub_get_health(self._handle, ctypes.byref(h))
        return {
            "ops": h.operations, "drops": h.errors,
            "rate": h.rate_hz, "healthy": h.healthy
        }

    def destroy(self):
        if self._handle:
            _lib.usrl_pub_destroy(self._handle)
            self._handle = None
    def __del__(self): self.destroy()


class Subscriber:
    def __init__(self, ctx, topic, buffer_size=None):
        self._handle = _lib.usrl_sub_create(ctx, topic.encode('utf-8'))
        if not self._handle: raise RuntimeError(f"Failed to create subscriber for {topic}")
        if buffer_size is None: buffer_size = 1024 * 1024 
        self._buf = ctypes.create_string_buffer(buffer_size)
        self._buf_len = buffer_size

    def recv(self):
        """Returns bytes or None (No Data)."""
        ret = _lib.usrl_sub_recv(self._handle, self._buf, self._buf_len)
        if ret >= 0: return self._buf[:ret]
        if ret == USRL_RING_NO_DATA: return None
        return None

    def recv_into(self, buffer):
        """
        Zero-copy receive into buffer.
        Returns bytes read or None.
        """
        c_ptr = None
        length = 0
        
        # NumPy Optimization
        if hasattr(buffer, 'ctypes') and hasattr(buffer, 'nbytes'):
            if hasattr(buffer, 'flags') and not buffer.flags['C_CONTIGUOUS']:
                raise ValueError("NumPy array must be C_CONTIGUOUS")
            c_ptr = buffer.ctypes.data_as(c_void_p)
            length = buffer.nbytes
        else:
            # Standard Buffer Protocol
            try:
                mv = memoryview(buffer)
            except TypeError:
                raise TypeError("Buffer must be writable and support buffer protocol")

            if not mv.contiguous: raise ValueError("Buffer must be contiguous")
            if mv.readonly: raise ValueError("Buffer must be writable")

            c_ptr = (ctypes.c_char * len(mv)).from_buffer(mv)
            length = len(mv)
            
        ret = _lib.usrl_sub_recv(self._handle, c_ptr, length)
        
        if ret >= 0: return ret
        if ret == USRL_RING_NO_DATA: return None
        return None

    def stats(self):
        h = UsrlHealth()
        _lib.usrl_sub_get_health(self._handle, ctypes.byref(h))
        return {
            "ops": h.operations, "skips": h.errors,
            "lag": h.lag, "healthy": h.healthy
        }

    def destroy(self):
        if self._handle:
            _lib.usrl_sub_destroy(self._handle)
            self._handle = None
    def __del__(self): self.destroy()

if __name__ == "__main__":
    print("USRL Python Bindings Loaded")
