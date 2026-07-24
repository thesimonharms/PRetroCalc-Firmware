#!/usr/bin/env python3
"""Rasterize Noto Sans Javanese into 64x64 bitmaps.

All glyphs share a FIXED pen origin (left-baseline) so sandhangan stack on
the same coordinates as their base aksara. Advance = rightmost ink + gap.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageOps, ImageFilter, ImageChops

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "font_jawa.c"
FONT = Path(r"C:\Users\simon\AppData\Local\Temp\opencode\NotoSansJavanese-Regular.ttf")

W = H = 64
ROW_BYTES = (W + 7) // 8  # 8
JAWA_MIN = 0xA980
JAWA_MAX = 0xA9DF
MARKS = set(range(0xA980, 0xA984)) | set(range(0xA9B3, 0xA9C1))
BASE_ANCHOR = "\uA98F"  # KA — positioning anchor for mark extraction


def render_cp(cp: int, font_px: int = 160):
    f = ImageFont.truetype(str(FONT), font_px)
    ascent, descent = f.getmetrics()
    em_h = ascent + descent
    em_w = em_h
    pad = font_px // 2

    # Fixed pen: same for every glyph so marks overlay bases correctly.
    # Slight left inset so taling (pre-base) still fits in the cell.
    pen_x = pad + em_w // 8
    pen_y = pad  # Pillow draws with top of em near y; we use metric crop

    img = Image.new("L", (em_w + pad * 2, em_h + pad * 2), 255)
    d = ImageDraw.Draw(img)

    if cp in MARKS:
        # cluster at fixed pen, subtract dilated base → mark-only ink
        d.text((pen_x, pen_y), BASE_ANCHOR, font=f, fill=0)
        base_bin = img.point(lambda p: 255 if p < 200 else 0)
        img2 = Image.new("L", img.size, 255)
        d2 = ImageDraw.Draw(img2)
        d2.text((pen_x, pen_y), BASE_ANCHOR + chr(cp), font=f, fill=0)
        clus_bin = img2.point(lambda p: 255 if p < 200 else 0)
        base_d = base_bin.filter(ImageFilter.MaxFilter(5))
        mark = ImageChops.subtract(clus_bin, base_d)
        img = ImageOps.invert(mark)
    else:
        d.text((pen_x, pen_y), chr(cp), font=f, fill=0)

    glyph = img.crop((pad, pad, pad + em_w, pad + em_h))
    small = glyph.resize((W, H), Image.Resampling.LANCZOS)
    small = ImageOps.autocontrast(small, cutoff=1)
    px = small.load()

    dark = [px[x, y] for y in range(H) for x in range(W) if px[x, y] < 248]
    thr = 175
    if dark:
        thr = max(50, min(200, int(sum(dark) / len(dark) + 35)))

    bits = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if px[x, y] < thr:
                bits[y][x] = 1

    # light thicken for LCD
    thick = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if bits[y][x]:
                thick[y][x] = 1
                if x + 1 < W:
                    thick[y][x + 1] = 1

    rows = []
    for y in range(H):
        rowbytes = [0] * ROW_BYTES
        for x in range(W):
            if thick[y][x]:
                rowbytes[x >> 3] |= 1 << (x & 7)
        rows.extend(rowbytes)
    return rows


def ink_advance(data):
    """Advance from origin to rightmost ink + bearing (marks stay 0)."""
    last = -1
    for y in range(H):
        for x in range(W):
            if (data[y * ROW_BYTES + (x >> 3)] >> (x & 7)) & 1:
                if x > last:
                    last = x
    if last < 0:
        return 0
    return max(20, min(W, last + 6))


def ink_count(data):
    return sum(bin(b).count("1") for b in data)


def main():
    if not FONT.exists():
        raise SystemExit(f"missing font: {FONT}")

    lines = [
        "/* Auto-rasterized from Noto Sans Javanese — 64x64 fixed-origin, bit0=left */",
        '#include "font_jawa.h"',
        "#include <stdbool.h>",
        "",
        "const uint8_t font_jawa[JAWA_COUNT][JAWA_BYTES] = {",
    ]
    nonempty = total_ink = 0
    all_data = {}
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        data = render_cp(cp)
        assert len(data) == H * ROW_BYTES
        all_data[cp] = data
        ic = ink_count(data)
        if ic:
            nonempty += 1
            total_ink += ic
        hx = ",".join(f"0x{b:02X}" for b in data)
        lines.append(f"    {{{hx}}}, /* U+{cp:04X} */")
    lines += [
        "};",
        "",
        "/* Per-glyph advance (origin→right ink + bearing); 0 for marks. */",
        "const uint8_t font_jawa_adv[JAWA_COUNT] = {",
    ]
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        adv = 0 if cp in MARKS else ink_advance(all_data[cp])
        lines.append(f"    {adv}, /* U+{cp:04X} */")
    lines += [
        "};",
        "",
        "const uint8_t *font_jawa_glyph(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return 0;",
        "    return font_jawa[cp - JAWA_CP_MIN];",
        "}",
        "",
        "bool font_jawa_is_mark(uint32_t cp) {",
        "    return (cp >= 0xA980 && cp <= 0xA983) ||",
        "           (cp >= 0xA9B3 && cp <= 0xA9C0);",
        "}",
        "",
        "bool font_jawa_is_base(uint32_t cp) {",
        "    return cp >= JAWA_CP_MIN && cp <= JAWA_CP_MAX && !font_jawa_is_mark(cp);",
        "}",
        "",
        "int font_jawa_advance(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return 0;",
        "    if (font_jawa_is_mark(cp)) return 0;",
        "    return font_jawa_adv[cp - JAWA_CP_MIN];",
        "}",
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT} nonempty={nonempty} avg_ink={total_ink // max(nonempty, 1)}")

    cols, rows_n = 10, 4
    preview = Image.new("L", (W * cols, H * rows_n), 255)
    cps = [
        0xA9B2, 0xA9A4, 0xA995, 0xA9AB, 0xA98F, 0xA9A2, 0xA9A0, 0xA9B1, 0xA9AE, 0xA9AD,
        0xA9A5, 0xA99D, 0xA997, 0xA9AA, 0xA99A, 0xA9A9, 0xA992, 0xA9A7, 0xA99B, 0xA994,
        0xA9B6, 0xA9B8, 0xA9BA, 0xA9B4, 0xA9BC, 0xA9C0, 0xA984, 0xA986, 0xA988, 0xA98E,
        0xA9D0, 0xA9D1, 0xA9D2, 0xA9D3, 0xA9D4, 0xA9D5, 0xA9D6, 0xA9D7, 0xA9D8, 0xA9D9,
    ]
    for i, cp in enumerate(cps):
        data = all_data[cp]
        cell = Image.new("L", (W, H), 255)
        px = cell.load()
        for row in range(H):
            for x in range(W):
                b = data[row * ROW_BYTES + (x >> 3)]
                if (b >> (x & 7)) & 1:
                    px[x, row] = 0
        r, c = divmod(i, cols)
        preview.paste(cell, (c * W, r * H))
    prev_path = ROOT / "tools" / "jawa_preview.png"
    preview.resize((W * cols * 2, H * rows_n * 2), Image.Resampling.NEAREST).save(prev_path)
    print(f"preview {prev_path}")


if __name__ == "__main__":
    main()
