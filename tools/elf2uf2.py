#!/usr/bin/env python3
"""Minimal ELF->UF2 converter for RP2040/RP2350 (replaces picotool).

Builds a contiguous flash image from PT_LOAD segments, padding gaps and
segment tails with 0xFF, then slices into 256-byte UF2 blocks aligned on
256-byte boundaries (required by strict loaders like uf2loader)."""
import struct, sys

ELF = sys.argv[1] if len(sys.argv) > 1 else "pretrocalc.elf"
UF2 = sys.argv[2] if len(sys.argv) > 2 else "pretrocalc.uf2"
FAMILY = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0xE48BFF56  # RP2040 default

with open(ELF, "rb") as f:
    data = f.read()

assert data[:4] == b"\x7fELF", "not an ELF"
is64 = data[4] == 2
if is64:
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    ph_fmt = "<IIQQQQQQ"
else:
    e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
    e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
    ph_fmt = "<IIIIIIII"

segments = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    vals = struct.unpack_from(ph_fmt, data, off)
    if is64:
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = vals
    else:
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = vals
    if p_type == 1 and p_filesz > 0:  # PT_LOAD with content
        addr = p_paddr or p_vaddr
        segments.append((addr, data[p_offset:p_offset + p_filesz]))

segments.sort(key=lambda s: s[0])

# Merge into one contiguous image, padding gaps with 0xFF
base = segments[0][0]
image = bytearray()
for addr, blob in segments:
    if addr < base + len(image):
        raise SystemExit(f"overlapping segments at 0x{addr:08x}")
    image.extend(b"\xff" * (addr - (base + len(image))))
    image.extend(blob)

PAYLOAD = 256
# Round image up to a multiple of PAYLOAD with 0xFF
if len(image) % PAYLOAD:
    image.extend(b"\xff" * (PAYLOAD - len(image) % PAYLOAD))

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30

total = len(image) // PAYLOAD
with open(UF2, "wb") as out:
    for i in range(total):
        addr = base + i * PAYLOAD
        chunk = bytes(image[i * PAYLOAD:(i + 1) * PAYLOAD])
        hdr = struct.pack("<IIIIIIII",
                          UF2_MAGIC_START0, UF2_MAGIC_START1, 0x00002000,
                          addr, PAYLOAD, i, total, FAMILY)
        out.write(hdr + chunk + b"\x00" * (476 - PAYLOAD) + struct.pack("<I", UF2_MAGIC_END))

print(f"Wrote {UF2}: {total} blocks, base 0x{base:08x}, {len(segments)} segments, family 0x{FAMILY:08x}")
