"""Redump-style CUE/BIN reader for GD-ROM dumps.

Redump publishes Dreamcast discs as a .cue plus one .bin per track::

    REM SINGLE-DENSITY AREA
    FILE "Game (Track 1).bin" BINARY
      TRACK 01 MODE1/2352
        INDEX 01 00:00:00
    FILE "Game (Track 2).bin" BINARY
      TRACK 02 AUDIO
        INDEX 00 00:00:00
        INDEX 01 00:02:00
    REM HIGH-DENSITY AREA
    FILE "Game (Track 3).bin" BINARY
      TRACK 03 MODE1/2352
        INDEX 01 00:00:00

Files are laid out back to back from LBA 0; ``REM HIGH-DENSITY AREA`` restarts the layout at LBA
45000. A track's LBA is its INDEX 01; an audio pregap (INDEX 00) stored before it is skipped with a
file offset, exactly how a GDI describes the same disc. The runtime's C++ reader
(runtime/src/gdrom/disc.cpp, ``open_cue``) implements the same rules.
"""

from __future__ import annotations

import os
import re

from .gdi import GdiImage
from .image import HD_AREA_LBA, Track


def _msf(s: str) -> int:
    m, sec, f = (int(x) for x in s.split(":"))
    return (m * 60 + sec) * 75 + f


class CueImage(GdiImage):
    def _parse(self) -> None:
        file_path, file_lba, next_lba = None, 0, 0
        pending: list[dict] = []
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.strip()
                up = line.upper()
                if up.startswith("REM"):
                    if "HIGH-DENSITY AREA" in up:
                        next_lba = HD_AREA_LBA
                elif up.startswith("FILE"):
                    m = re.match(r'FILE\s+"(.+)"', line) or re.match(r"FILE\s+(\S+)", line)
                    file_path = os.path.join(self.dir, m.group(1))
                    if not os.path.exists(file_path):
                        raise FileNotFoundError(f"cue references missing track file {m.group(1)}")
                    file_lba = next_lba
                    next_lba += os.path.getsize(file_path) // 2352
                elif up.startswith("TRACK"):
                    _, num, mode = line.split()[:3]
                    pending.append({
                        "number": int(num),
                        "data": mode.upper() != "AUDIO",
                        "ssize": 2048 if "2048" in mode else 2352,
                        "file": file_path,
                        "file_lba": file_lba,
                        "index01": 0,
                    })
                elif up.startswith("INDEX") and pending:
                    _, idx, at = line.split()[:3]
                    if int(idx) == 1:
                        pending[-1]["index01"] = _msf(at)
        if not pending:
            raise ValueError("cue lists no tracks")
        for p in pending:
            offset = p["index01"] * p["ssize"]
            size = os.path.getsize(p["file"]) - offset
            self.tracks.append(Track(p["number"], p["file_lba"] + p["index01"], size // p["ssize"],
                                     p["ssize"], p["data"]))
            self.files[p["number"]] = p["file"]
            self.offsets[p["number"]] = offset
        self.tracks.sort(key=lambda t: t.number)
