"""1ST_READ.BIN scrambling.

The BIOS scrambles the boot binary only when booting from a CD-R (the MIL-CD path): the binary on
such a disc has its 32-byte slices permuted and the BIOS descrambles it while loading. **A GD-ROM
stores 1ST_READ.BIN in plain form**, so dumps of retail GD-ROMs (GDI, CHD) need no descrambling;
self-boot CD-R images (CDI) do: descrambling a retail GD-ROM's boot binary destroys it.

The permutation is driven by a 16-bit LCG seeded with the file size, applied over progressively
smaller windows (2 MB down to 32 bytes), then any final sub-slice remainder is copied verbatim. This
is the algorithm from Marcus Comstedt's ``scramble.c``, reimplemented; a scrambled file and its plain
form are the same length. Descramble reads the scrambled input sequentially and scatters slices to
their permuted positions.
"""

from __future__ import annotations

MAXCHUNK = 2048 * 1024
SLICE = 32


class _Rng:
    def __init__(self, seed: int):
        self.seed = seed & 0xFFFF

    def next(self) -> int:
        self.seed = (self.seed * 2109 + 2) & 0xFFFF
        return self.seed


def _permutation(rng: _Rng, slices: int) -> list[int]:
    """Order in which sequential input slices land in the output window."""
    idx = list(range(slices))
    order = []
    for i in range(slices - 1, -1, -1):
        x = (rng.next() * i) >> 16
        idx[i], idx[x] = idx[x], idx[i]
        order.append(idx[i])
    return order


def descramble(data: bytes) -> bytes:
    size = len(data)
    out = bytearray(size)
    rng = _Rng(size)
    in_pos = 0
    out_pos = 0
    remaining = size
    chunk = MAXCHUNK
    while chunk >= SLICE:
        while remaining >= chunk:
            for target in _permutation(rng, chunk // SLICE):
                dst = out_pos + target * SLICE
                out[dst:dst + SLICE] = data[in_pos:in_pos + SLICE]
                in_pos += SLICE
            out_pos += chunk
            remaining -= chunk
        chunk >>= 1
    if remaining:
        out[out_pos:out_pos + remaining] = data[in_pos:in_pos + remaining]
    return bytes(out)

