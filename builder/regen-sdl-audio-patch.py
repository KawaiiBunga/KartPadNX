#!/usr/bin/env python3
"""Preserve SDL AUDOUT edits as a patch against the pinned sdl3-switch driver."""
from pathlib import Path
import difflib

repo = Path(__file__).resolve().parent.parent
base = repo / "ref/sdl3-switch/switch-backend/src/audio/switch"
current = repo / "ref/SDL/src/audio/switch"
parts = []
for name in ("SDL_switchaudio.c", "SDL_switchaudio.h"):
    before = (base / name).read_text().splitlines(keepends=True)
    after = (current / name).read_text().splitlines(keepends=True)
    relative = f"src/audio/switch/{name}"
    parts.extend(difflib.unified_diff(before, after, f"a/{relative}", f"b/{relative}"))
output = repo / "switch/patches/sdl-switch-audio-queue.patch"
output.write_text("".join(parts), newline="\n")
print(f"SDL audio source -> {output} ({output.stat().st_size} bytes)")
