#!/usr/bin/env python3
from pathlib import Path
import struct

root = Path(__file__).resolve().parent.parent
out_bin = root / "mods/High-Quality Sound Mixing/source/m4a_hq_mixer_no_fix.bin"
if out_bin.exists() and out_bin.stat().st_size == 0x87C:
    raise SystemExit(0)

ups_files = list((root / "mods/High-Quality Sound Mixing").rglob("*.ups"))
if not ups_files:
    raise SystemExit("No .ups patch found in mods/High-Quality Sound Mixing")

data = ups_files[0].read_bytes()
# Extract the 0x087C mixer payload block embedded in the UPS patch
idx = data.rfind(b"\x00\xb5")  # Thumb push {lr} / mixer entry signature fallback
out_bin.parent.mkdir(parents=True, exist_ok=True)
if not out_bin.exists():
    # Decode standard UPS XOR patch against a zero buffer at target offset 0x087C bytes
    buf = bytearray(0x2000000)
    pos = 4
    def read_vuint(b, p):
        v, shift = 0, 0
        while True:
            x = b[p]; p += 1
            if x & 0x80:
                return v + ((x & 0x7f) << shift), p
            v += (x | 0x80) << shift
            shift += 7
    _, pos = read_vuint(data, pos)
    _, pos = read_vuint(data, pos)
    off = 0
    while pos < len(data) - 12:
        skip, pos = read_vuint(data, pos)
        off += skip
        while data[pos] != 0:
            buf[off] ^= data[pos]
            off += 1
            pos += 1
        off += 1
        pos += 1
    # Locate the 0x087C-byte mixer routine inside the patched region
    nonzero = [i for i, b in enumerate(buf) if b != 0]
    start = nonzero[0]
    out_bin.write_bytes(buf[start:start + 0x87C])
print(f"Extracted {out_bin} ({out_bin.stat().st_size} bytes)")
