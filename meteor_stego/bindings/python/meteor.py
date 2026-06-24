"""
Python ctypes binding for the Meteor stego library.
Usage:
    m = Meteor(key_input=b"my-secret", salt=os.urandom(32))
    covertext = m.encode(b"hello", starting_context="The report stated")
    message   = m.decode(covertext, starting_context="The report stated")
"""
import ctypes
import os
import platform

def _load_library():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    system = platform.system()
    if system == "Windows":
        names = ["meteor.dll"]
    elif system == "Darwin":
        names = ["libmeteor.dylib"]
    else:
        names = ["libmeteor.so"]

    for name in names:
        for search in [here, root, os.path.join(root, "build", "bin"),
                       os.path.join(root, "out", "build", "x64-release")]:
            path = os.path.join(search, name)
            if os.path.exists(path):
                return ctypes.CDLL(path)

    raise FileNotFoundError(
        f"Could not find meteor shared library ({names[0]}). "
        "Build the project first: cmake --build <build-dir>"
    )

_lib = _load_library()

class _MeteorConfig(ctypes.Structure):
    _fields_ = [
        ("key_raw",        ctypes.c_char_p),
        ("key_input",      ctypes.c_char_p),
        ("key_input_len",  ctypes.c_size_t),
        ("salt",           ctypes.c_char_p),
        ("salt_len",       ctypes.c_size_t),
        ("beta",           ctypes.c_int),
        ("num_candidates", ctypes.c_int),
        ("llm_url",        ctypes.c_char_p),
        ("hyphen_dict",    ctypes.c_char_p),
        ("max_steps",      ctypes.c_int),
        ("llm_timeout_ms", ctypes.c_int),
    ]

_lib.meteor_create.restype   = ctypes.c_void_p
_lib.meteor_create.argtypes  = [ctypes.POINTER(_MeteorConfig)]

_lib.meteor_destroy.restype  = None
_lib.meteor_destroy.argtypes = [ctypes.c_void_p]

_lib.meteor_encode.restype   = ctypes.c_char_p
_lib.meteor_encode.argtypes  = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)
]

_lib.meteor_decode.restype   = ctypes.POINTER(ctypes.c_uint8)
_lib.meteor_decode.argtypes  = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_int)
]

_lib.meteor_free.restype    = None
_lib.meteor_free.argtypes   = [ctypes.c_void_p]

_lib.meteor_llm_health.restype  = ctypes.c_int
_lib.meteor_llm_health.argtypes = [ctypes.c_void_p]

_lib.meteor_syllabify_word.restype  = ctypes.c_char_p
_lib.meteor_syllabify_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p]


class Meteor:
    """
    High-level Python wrapper for the Meteor stego library.

    Args:
        key_input:      Key material (bytes). Derived via HKDF-SHA256 internally.
        salt:           32-byte random salt (shared out-of-band). None = zero salt.
        beta:           Bits per Meteor step (2–5, default 3).
        num_candidates: Syllable candidates per LLM call (4–8, default 6).
        llm_url:        URL of the running llama-server.
        hyphen_dict:    Path to hyph_en_US.dic, or None for heuristic fallback.
    """

    def __init__(self,
                 key_input: bytes,
                 salt: bytes = None,
                 beta: int = 3,
                 num_candidates: int = 6,
                 llm_url: str = "http://127.0.0.1:8080",
                 hyphen_dict: str = None):
        cfg = _MeteorConfig(
            key_raw        = None,
            key_input      = key_input,
            key_input_len  = len(key_input),
            salt           = salt,
            salt_len       = len(salt) if salt else 0,
            beta           = beta,
            num_candidates = num_candidates,
            llm_url        = llm_url.encode(),
            hyphen_dict    = hyphen_dict.encode() if hyphen_dict else None,
            max_steps      = 256,
            llm_timeout_ms = 30000,
        )
        self._ctx = _lib.meteor_create(ctypes.byref(cfg))
        if not self._ctx:
            raise RuntimeError("meteor_create() failed — check libsodium and config.")

    def encode(self, message: bytes, starting_context: str) -> str:
        """Encode message into LLM-generated covertext."""
        err = ctypes.c_int(0)
        ptr = _lib.meteor_encode(
            self._ctx, message, len(message),
            starting_context.encode(), ctypes.byref(err)
        )
        if err.value != 0:
            raise RuntimeError(f"meteor_encode error {err.value}")
        result = ptr.decode("utf-8")
        _lib.meteor_free(ptr)
        return result

    def decode(self, covertext: str, starting_context: str) -> bytes:
        """Recover message bytes from covertext."""
        err     = ctypes.c_int(0)
        msg_len = ctypes.c_size_t(0)
        ptr = _lib.meteor_decode(
            self._ctx, covertext.encode(), starting_context.encode(),
            ctypes.byref(msg_len), ctypes.byref(err)
        )
        if err.value != 0:
            raise RuntimeError(f"meteor_decode error {err.value}")
        out = bytes(ptr[:msg_len.value])
        _lib.meteor_free(ptr)
        return out

    def syllabify(self, word: str) -> str:
        """Return syllabified word joined by middle-dot (e.g. 're·mark·a·ble')."""
        ptr = _lib.meteor_syllabify_word(self._ctx, word.encode())
        if not ptr:
            return word
        result = ptr.decode("utf-8")
        _lib.meteor_free(ptr)
        return result

    def health(self) -> bool:
        """Return True if the LLM server is reachable."""
        return bool(_lib.meteor_llm_health(self._ctx))

    def __del__(self):
        if self._ctx:
            _lib.meteor_destroy(self._ctx)
            self._ctx = None
