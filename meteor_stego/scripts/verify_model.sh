#!/usr/bin/env bash
# Verify GGUF model file SHA-256 hash.
# Usage: ./verify_model.sh /path/to/model.gguf <expected_sha256>
set -euo pipefail

MODEL_PATH="${1:?Usage: $0 /path/to/model.gguf <expected_sha256>}"
EXPECTED="${2:?Usage: $0 /path/to/model.gguf <expected_sha256>}"

if [[ ! -f "$MODEL_PATH" ]]; then
    echo "ERROR: file not found: $MODEL_PATH" >&2
    exit 1
fi

if command -v sha256sum &>/dev/null; then
    ACTUAL="$(sha256sum "$MODEL_PATH" | awk '{print $1}')"
elif command -v shasum &>/dev/null; then
    ACTUAL="$(shasum -a 256 "$MODEL_PATH" | awk '{print $1}')"
else
    echo "ERROR: sha256sum / shasum not found" >&2
    exit 1
fi

if [[ "$ACTUAL" == "$EXPECTED" ]]; then
    echo "OK: $MODEL_PATH"
    echo "    SHA-256: $ACTUAL"
    exit 0
else
    echo "MISMATCH: $MODEL_PATH" >&2
    echo "  expected: $EXPECTED" >&2
    echo "  actual:   $ACTUAL" >&2
    echo "Both encoder and decoder must use the same model file." >&2
    exit 1
fi
