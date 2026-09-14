#!/usr/bin/env python3
"""Regenerate cpp/launcher/src/ai.cpp for Amalgam's bundled AI runtimes."""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "cpp" / "launcher" / "src" / "ai.cpp"
source = OUT.read_text(encoding="utf-8")
# Keep this generator as a safe, reproducible guard: the generated source is
# maintained in-tree, and this script refuses to overwrite it from a stale
# developer-specific template. Runtime changes should be made in ai.cpp and
# reviewed there, then this script validates the output path.
if "CreateProcessW" not in source or "llama-completion" not in source:
    raise SystemExit("ai.cpp does not contain the native bundled-runtime backend")
if "C:\\Users\\" in source or "C:/Users/" in source:
    raise SystemExit("ai.cpp contains a hardcoded developer path")
print(f"Validated {OUT} ({OUT.stat().st_size} bytes)")
