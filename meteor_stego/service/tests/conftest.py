"""Put the repo root on sys.path so `import meteor_stego.service...` resolves
when pytest is run from anywhere."""
import os
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)
