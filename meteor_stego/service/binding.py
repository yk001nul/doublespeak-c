"""
Locate and import the refreshed Python ctypes binding (meteor.py).

The service lives at meteor_stego/service/ and the binding at
meteor_stego/bindings/python/meteor.py. Rather than require the caller to set
PYTHONPATH, add that directory to sys.path here so `from .binding import
Meteor, ...` just works. The binding itself finds meteor.dll/.so relative to
its own location (out/build/... on Windows).
"""
import os
import sys

_BINDING_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "bindings", "python")
)
if _BINDING_DIR not in sys.path:
    sys.path.insert(0, _BINDING_DIR)

# Re-export the public binding surface the service needs.
from meteor import (  # noqa: E402
    Meteor,
    MeteorStyle,
    MeteorError,
    MeteorLLMError,
    MeteorCapacityError,
    MeteorDecodeError,
    MeteorTimeoutError,
    MeteorConfigError,
)

__all__ = [
    "Meteor", "MeteorStyle", "MeteorError", "MeteorLLMError",
    "MeteorCapacityError", "MeteorDecodeError", "MeteorTimeoutError",
    "MeteorConfigError",
]
