"""
Python ctypes binding for the Meteor stego library.
Usage:
    m = Meteor(key_input=b"my-secret", salt=os.urandom(32))
    covertext = m.encode(b"hello", starting_context="The report stated")
    message   = m.decode(covertext, starting_context="The report stated")

Style mode:
    m = Meteor(key_input=b"my-secret", salt=os.urandom(32),
               style=MeteorStyle.FORMAL_EMAIL)

Progress reporting (encode):
    def on_step(step, bits_done, total_bits):
        print(f"step {step}: {bits_done}/{total_bits} bits")
    m.encode(b"hello", "topic", progress=on_step)
"""
import ctypes
import enum
import os
import platform


# ── Style enum (mirrors MeteorStyle in meteor.h) ────────────────────────────

class MeteorStyle(enum.IntEnum):
    NONE          = 0   # legacy mode: starting_context echoed verbatim
    INFORMAL_CHAT = 1   # casual instant / mobile messaging
    FORMAL_EMAIL  = 2   # professional business email prose
    CASUAL_BLOG   = 3   # relaxed first-person blog writing
    NEWS_ARTICLE  = 4   # neutral third-person news prose


# ── Error codes + exception hierarchy (mirrors METEOR_ERR_* in meteor.h) ────

METEOR_OK           = 0
METEOR_ERR_CONFIG   = 1
METEOR_ERR_LLM      = 2
METEOR_ERR_CAPACITY = 3
METEOR_ERR_DECODE   = 4
METEOR_ERR_DICT     = 5
METEOR_ERR_OOM      = 6
METEOR_ERR_TIMEOUT  = 7
METEOR_ERR_CRYPTO   = 8


class MeteorError(RuntimeError):
    """Base for all library-reported errors. Carries the numeric .code."""
    code = None

    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


class MeteorConfigError(MeteorError):    pass  # METEOR_ERR_CONFIG
class MeteorLLMError(MeteorError):       pass  # METEOR_ERR_LLM
class MeteorCapacityError(MeteorError):  pass  # METEOR_ERR_CAPACITY
class MeteorDecodeError(MeteorError):    pass  # METEOR_ERR_DECODE
class MeteorDictError(MeteorError):      pass  # METEOR_ERR_DICT
class MeteorOOMError(MeteorError):       pass  # METEOR_ERR_OOM
class MeteorTimeoutError(MeteorError):   pass  # METEOR_ERR_TIMEOUT
class MeteorCryptoError(MeteorError):    pass  # METEOR_ERR_CRYPTO

_ERR_CLASS = {
    METEOR_ERR_CONFIG:   (MeteorConfigError,   "invalid configuration"),
    METEOR_ERR_LLM:      (MeteorLLMError,      "LLM server unreachable or bad response"),
    METEOR_ERR_CAPACITY: (MeteorCapacityError, "message too long for max_steps"),
    METEOR_ERR_DECODE:   (MeteorDecodeError,   "syllabification or distribution mismatch"),
    METEOR_ERR_DICT:     (MeteorDictError,     "hyphenation dictionary not found"),
    METEOR_ERR_OOM:      (MeteorOOMError,      "memory allocation failed"),
    METEOR_ERR_TIMEOUT:  (MeteorTimeoutError,  "LLM call timed out"),
    METEOR_ERR_CRYPTO:   (MeteorCryptoError,   "libsodium init or HKDF failure"),
}


def _raise_for_code(code, where):
    """Raise the mapped exception for a non-OK library error code."""
    if code == METEOR_OK:
        return
    cls, desc = _ERR_CLASS.get(code, (MeteorError, "unknown error"))
    raise cls(f"{where}: {desc} (code {code})", code)


# ── Library loading ─────────────────────────────────────────────────────────

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
                       os.path.join(root, "out", "build", "x64-release"),
                       os.path.join(root, "out", "build", "x64-debug")]:
            path = os.path.join(search, name)
            if os.path.exists(path):
                return ctypes.CDLL(path)

    raise FileNotFoundError(
        f"Could not find meteor shared library ({names[0]}). "
        "Build the project first: cmake --build <build-dir>"
    )

_lib = _load_library()


# ── Struct definitions (must byte-match meteor.h exactly) ────────────────────

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
        # Must stay LAST, matching MeteorConfig's field order in meteor.h.
        # Without it, meteor_create() reads config->style past the end of this
        # struct — an out-of-bounds read.
        ("style",          ctypes.c_int),
    ]


class _MeteorCapacityEstimate(ctypes.Structure):
    # Mirrors MeteorCapacityEstimate in meteor.h.
    _fields_ = [
        ("estimated_bits",    ctypes.c_int),
        ("estimated_bytes",   ctypes.c_int),
        ("estimated_words",   ctypes.c_int),
        ("avg_bits_per_word", ctypes.c_float),
        ("sample_steps_used", ctypes.c_int),
    ]


# Progress callback prototype — matches MeteorProgressFn in meteor.h:
#   void (*)(void* userdata, int step, int bits_done, int total_bits)
_ProgressFn = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int
)


# ── Function prototypes ──────────────────────────────────────────────────────

_lib.meteor_create.restype   = ctypes.c_void_p
_lib.meteor_create.argtypes  = [ctypes.POINTER(_MeteorConfig)]

_lib.meteor_destroy.restype  = None
_lib.meteor_destroy.argtypes = [ctypes.c_void_p]

# NOTE: string-returning functions use c_void_p (NOT c_char_p) restype. With
# c_char_p, ctypes auto-converts the return into a Python bytes and the original
# heap pointer is lost — calling meteor_free() on the bytes object then frees
# Python's own buffer instead of the C allocation, corrupting the heap. Keeping
# the raw pointer lets us string_at() it and free the correct address.
_lib.meteor_encode.restype   = ctypes.c_void_p
_lib.meteor_encode.argtypes  = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)
]

_lib.meteor_encode_ex.restype  = ctypes.c_void_p
_lib.meteor_encode_ex.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
    _ProgressFn, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)
]

_lib.meteor_decode.restype   = ctypes.POINTER(ctypes.c_uint8)
_lib.meteor_decode.argtypes  = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_int)
]

_lib.meteor_estimate_capacity.restype  = ctypes.c_int
_lib.meteor_estimate_capacity.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
    ctypes.POINTER(_MeteorCapacityEstimate)
]

_lib.meteor_free.restype    = None
_lib.meteor_free.argtypes   = [ctypes.c_void_p]

_lib.meteor_llm_health.restype  = ctypes.c_int
_lib.meteor_llm_health.argtypes = [ctypes.c_void_p]

_lib.meteor_syllabify_word.restype  = ctypes.c_void_p   # see restype note above
_lib.meteor_syllabify_word.argtypes = [ctypes.c_void_p, ctypes.c_char_p]


class Meteor:
    """
    High-level Python wrapper for the Meteor stego library.

    Key material — supply EXACTLY ONE of:
        key_input: arbitrary bytes, derived via HKDF-SHA256 internally, or
        key_raw:   a pre-derived 32-byte key (e.g. from ECDH).

    Args:
        key_input:      Key material (bytes). Mutually exclusive with key_raw.
        key_raw:        Pre-derived 32-byte key. Mutually exclusive with key_input.
        salt:           32-byte random salt (shared out-of-band). None = zero salt.
        beta:           Bits per Meteor step (2–5, default 3).
        num_candidates: Candidates per LLM call (4–8, default 6).
        llm_url:        URL of the running llama-server.
        hyphen_dict:    Path to hyph_en_US.dic, or None for heuristic fallback.
        max_steps:      Max steps before giving up (default 256).
        llm_timeout_ms: HTTP timeout per LLM call (default 30000).
        style:          MeteorStyle (default NONE = legacy mode).
    """

    def __init__(self,
                 key_input: bytes = None,
                 salt: bytes = None,
                 beta: int = 3,
                 num_candidates: int = 6,
                 llm_url: str = "http://127.0.0.1:8080",
                 hyphen_dict: str = None,
                 max_steps: int = 256,
                 llm_timeout_ms: int = 30000,
                 style: MeteorStyle = MeteorStyle.NONE,
                 key_raw: bytes = None):
        if (key_input is None) == (key_raw is None):
            raise ValueError("supply exactly one of key_input or key_raw")
        if key_raw is not None and len(key_raw) != 32:
            raise ValueError(f"key_raw must be exactly 32 bytes, got {len(key_raw)}")

        cfg = _MeteorConfig(
            key_raw        = key_raw,
            key_input      = key_input,
            key_input_len  = len(key_input) if key_input is not None else 0,
            salt           = salt,
            salt_len       = len(salt) if salt else 0,
            beta           = beta,
            num_candidates = num_candidates,
            llm_url        = llm_url.encode(),
            hyphen_dict    = hyphen_dict.encode() if hyphen_dict else None,
            max_steps      = max_steps,
            llm_timeout_ms = llm_timeout_ms,
            style          = int(style),
        )
        self._ctx = _lib.meteor_create(ctypes.byref(cfg))
        if not self._ctx:
            raise MeteorConfigError(
                "meteor_create() failed — check libsodium and config.",
                METEOR_ERR_CONFIG,
            )

    def encode(self, message: bytes, starting_context: str, progress=None) -> str:
        """
        Encode message into LLM-generated covertext.

        progress: optional callable(step, bits_done, total_bits) invoked
                  synchronously once per encode step. Passing None takes the
                  plain meteor_encode_ex(..., NULL, NULL, ...) path.
        """
        err = ctypes.c_int(0)

        # Retain the CFUNCTYPE trampoline in a local for the whole blocking
        # call. If it were garbage-collected before meteor_encode_ex returns,
        # the C side would call a freed function pointer and segfault.
        # A ctypes CFUNCTYPE argument will NOT accept Python None as NULL — it
        # raises ArgumentError. To pass a NULL callback, cast None to the
        # pointer type, which yields a valid null _ProgressFn instance.
        if progress is None:
            cb = ctypes.cast(None, _ProgressFn)
        else:
            def _trampoline(_userdata, step, bits_done, total_bits):
                progress(step, bits_done, total_bits)
            cb = _ProgressFn(_trampoline)

        ptr = _lib.meteor_encode_ex(
            self._ctx, message, len(message),
            starting_context.encode(), cb, None, ctypes.byref(err)
        )
        _raise_for_code(err.value, "meteor_encode")
        if not ptr:
            raise MeteorError("meteor_encode returned NULL without error code",
                              METEOR_ERR_OOM)
        result = ctypes.string_at(ptr).decode("utf-8")
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
        _raise_for_code(err.value, "meteor_decode")
        out = bytes(ptr[:msg_len.value])
        _lib.meteor_free(ptr)
        return out

    def estimate_capacity(self, context: str, sample_steps: int = 0) -> dict:
        """
        Estimate how many bits/bytes can be embedded using `context` as the
        style-mode paraphrase source.

        sample_steps == 0: fast heuristic, no LLM calls (returns in microseconds).
        sample_steps  > 0: accurate — makes this many LLM calls (server required).

        Returns a dict with keys: estimated_bits, estimated_bytes,
        estimated_words, avg_bits_per_word, sample_steps_used.
        """
        est = _MeteorCapacityEstimate()
        rc = _lib.meteor_estimate_capacity(
            self._ctx, context.encode(), sample_steps, ctypes.byref(est)
        )
        _raise_for_code(rc, "meteor_estimate_capacity")
        return {
            "estimated_bits":    est.estimated_bits,
            "estimated_bytes":   est.estimated_bytes,
            "estimated_words":   est.estimated_words,
            "avg_bits_per_word": est.avg_bits_per_word,
            "sample_steps_used": est.sample_steps_used,
        }

    def syllabify(self, word: str) -> str:
        """Return syllabified word joined by middle-dot (e.g. 're·mark·a·ble')."""
        ptr = _lib.meteor_syllabify_word(self._ctx, word.encode())
        if not ptr:
            return word
        result = ctypes.string_at(ptr).decode("utf-8")
        _lib.meteor_free(ptr)
        return result

    def health(self) -> bool:
        """Return True if the LLM server is reachable."""
        return bool(_lib.meteor_llm_health(self._ctx))

    def __del__(self):
        if getattr(self, "_ctx", None):
            _lib.meteor_destroy(self._ctx)
            self._ctx = None
