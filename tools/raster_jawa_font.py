#!/usr/bin/env python3
"""Rasterize Noto Sans Javanese — dual 48/64 + real pasangan.

Coordinate system (CRITICAL):
  All glyphs share one fixed pen / em crop so sandhangan overlay bases.
  Bases + marks use Pillow (reliable fixed-origin subtract).
  Pasangan use HarfBuzz to select the real .pas glyph, then the ink is
  recentered into a compact lower-center slot of that same em.
  Below vowels on pasangan use OpenType u.ns.pas / uu.ns.pas / keret.ns.alt
  placed with the same FreeType pen as the reference pasangan (ba), so the
  suku hook follows the ending stroke.
"""
from __future__ import annotations

from pathlib import Path

import freetype
import uharfbuzz as hb
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageOps
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "font_jawa.c"

FONT_CANDIDATES = [
    ROOT / "tools" / "NotoSansJavanese-Regular.ttf",
    Path(r"C:\Users\simon\Downloads\Noto_Sans_Javanese\static\NotoSansJavanese-Regular.ttf"),
    Path(r"C:\Users\simon\AppData\Local\Temp\opencode\NotoSansJavanese-Regular.ttf"),
]

JAWA_MIN = 0xA980
JAWA_MAX = 0xA9DF
MARKS = set(range(0xA980, 0xA984)) | set(range(0xA9B3, 0xA9C1))
PASANGAN_CPS = set(range(0xA98F, 0xA9B3))
BASE_ANCHOR = "\uA98F"
PANGKON = "\uA9C0"
SIZES = (48, 64)

# Pillow render — same recipe that previously stacked marks correctly
PIL_PX = 160


def find_font() -> Path:
    for p in FONT_CANDIDATES:
        if p.exists():
            return p
    raise SystemExit("Noto Sans Javanese Regular.ttf not found")


def ink_count(data: list[int]) -> int:
    return sum(bin(b).count("1") for b in data)


def pack_bits(bits: list[list[int]], size: int) -> list[int]:
    row_bytes = (size + 7) // 8
    thick = [[0] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            if bits[y][x]:
                thick[y][x] = 1
                if x + 1 < size:
                    thick[y][x + 1] = 1
    rows: list[int] = []
    for y in range(size):
        rb = [0] * row_bytes
        for x in range(size):
            if thick[y][x]:
                rb[x >> 3] |= 1 << (x & 7)
        rows.extend(rb)
    return rows


def threshold_bits(img: Image.Image, size: int) -> list[list[int]]:
    small = img.resize((size, size), Image.Resampling.LANCZOS)
    small = ImageOps.autocontrast(small, cutoff=1)
    px = small.load()
    dark = [px[x, y] for y in range(size) for x in range(size) if px[x, y] < 248]
    thr = 175
    if dark:
        thr = max(50, min(200, int(sum(dark) / len(dark) + 35)))
    bits = [[0] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            if px[x, y] < thr:
                bits[y][x] = 1
    return bits


def ink_advance(data: list[int], size: int) -> int:
    row_bytes = (size + 7) // 8
    last = -1
    for y in range(size):
        for x in range(size):
            if (data[y * row_bytes + (x >> 3)] >> (x & 7)) & 1:
                if x > last:
                    last = x
    if last < 0:
        return 0
    gap = max(4, size * 6 // 64)
    lo = max(12, size * 20 // 64)
    return max(lo, min(size, last + gap))


def empty_bitmap(size: int) -> list[int]:
    return [0] * (size * ((size + 7) // 8))


def to_bitmap(img: Image.Image, size: int) -> list[int]:
    return pack_bits(threshold_bits(img, size), size)


def ink_bbox(img: Image.Image, thr: int = 200):
    px = img.load()
    w, h = img.size
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            if px[x, y] < thr:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


class PillowEm:
    """Fixed-origin Pillow renderer — bases and marks share this em."""

    def __init__(self, font_path: Path):
        self.font = ImageFont.truetype(str(font_path), PIL_PX)
        ascent, descent = self.font.getmetrics()
        self.em_h = ascent + descent
        self.em_w = self.em_h
        self.pad = PIL_PX // 2
        # Same pen for every glyph so marks overlay bases.
        self.pen_x = self.pad + self.em_w // 8
        self.pen_y = self.pad

    def _canvas(self) -> tuple[Image.Image, ImageDraw.ImageDraw]:
        img = Image.new("L", (self.em_w + self.pad * 2, self.em_h + self.pad * 2), 255)
        return img, ImageDraw.Draw(img)

    def crop_em(self, img: Image.Image) -> Image.Image:
        return img.crop(
            (self.pad, self.pad, self.pad + self.em_w, self.pad + self.em_h)
        )

    def render_base(self, cp: int) -> Image.Image:
        img, d = self._canvas()
        d.text((self.pen_x, self.pen_y), chr(cp), font=self.font, fill=0)
        return self.crop_em(img)

    def render_mark(self, cp: int) -> Image.Image:
        img, d = self._canvas()
        d.text((self.pen_x, self.pen_y), BASE_ANCHOR, font=self.font, fill=0)
        base_bin = img.point(lambda p: 255 if p < 200 else 0)

        img2, d2 = self._canvas()
        d2.text((self.pen_x, self.pen_y), BASE_ANCHOR + chr(cp), font=self.font, fill=0)
        clus_bin = img2.point(lambda p: 255 if p < 200 else 0)

        base_d = base_bin.filter(ImageFilter.MaxFilter(5))
        mark = ImageOps.invert(ImageChops.subtract(clus_bin, base_d))
        return self.crop_em(mark)


# Below-vowel CPs that have pasangan-attached OpenType forms
PAS_BELOW_CPS = {
    0xA9B8: "u.ns.pas",       # suku
    0xA9B9: "uu.ns.pas",      # suku mendut
    0xA9BD: "keret.ns.alt",   # keret (on pasangan)
}
# Reference pasangan for shared-pen placement of pas-below marks (ba)
PAS_BELOW_REF = 0xA9A7


class PasanganExtractor:
    """Pull real .pas glyphs via HarfBuzz, place into the Pillow em box.

    Pasangan sit in a compact lower-center slot. Below vowels that attach to
    pasangan (u.ns.pas / uu.ns.pas / keret.ns.alt) are placed with the same
    FreeType pen as the reference pasangan so the suku hook follows the
    ending stroke — not a crude runtime offset of normal u.ns.
    """

    def __init__(self, font_path: Path, em_w: int, em_h: int):
        self.em_w = em_w
        self.em_h = em_h
        self.names = TTFont(str(font_path)).getGlyphOrder()
        self.name_to_gid = {n: i for i, n in enumerate(self.names)}
        data = font_path.read_bytes()
        self.hb_font = hb.Font(hb.Face(data))
        self.ft = freetype.Face(str(font_path))
        # High-res FreeType render of the pasangan glyph alone
        self.ft_px = 200
        self.ft.set_char_size(self.ft_px * 64)
        # Slot geometry — short enough that u.ns.pas (~1.6× pasangan tall)
        # still fits inside the em when attached at the pasangan top.
        self.slot_top = int(self.em_h * 0.50)
        self.slot_bot = int(self.em_h * 0.78)
        self.slot_h = max(8, self.slot_bot - self.slot_top)
        self.slot_w = int(self.em_w * 0.42)
        base_cx = int(self.em_w * 0.35)
        self.slot_left = base_cx - self.slot_w // 2
        if self.slot_left < 0:
            self.slot_left = 0
        if self.slot_left + self.slot_w > self.em_w:
            self.slot_left = self.em_w - self.slot_w
        # Filled by render_pasangan(PAS_BELOW_REF)
        self._ref_pen: tuple[float, float, float] | None = None  # pen_x, pen_y, scale

    def pasangan_gid(self, cp: int) -> int | None:
        buf = hb.Buffer()
        buf.add_str(BASE_ANCHOR + PANGKON + chr(cp))
        buf.guess_segment_properties()
        hb.shape(self.hb_font, buf)
        for info in buf.glyph_infos:
            name = self.names[info.codepoint]
            if ".pas" in name and not name.startswith("pangkon"):
                return info.codepoint
        return None

    def _ft_glyph_img(self, gid: int) -> tuple[Image.Image, int, int, int, int] | None:
        """Return (L-image black-on-white, bitmap_left, bitmap_top, w, h)."""
        self.ft.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        bmp = self.ft.glyph.bitmap
        if bmp.width == 0 or bmp.rows == 0:
            return None
        gimg = Image.new("L", (bmp.width, bmp.rows), 255)
        buf = bytes(bmp.buffer)
        gpx = gimg.load()
        for row in range(bmp.rows):
            for col in range(bmp.width):
                v = buf[row * bmp.pitch + col]
                if v:
                    gpx[col, row] = 255 - v
        return gimg, self.ft.glyph.bitmap_left, self.ft.glyph.bitmap_top, bmp.width, bmp.rows

    def render_pasangan(self, cp: int) -> Image.Image:
        """Return em-sized image with pasangan ink in the lower-center band."""
        out = Image.new("L", (self.em_w, self.em_h), 255)
        gid = self.pasangan_gid(cp)
        if gid is None:
            return out

        loaded = self._ft_glyph_img(gid)
        if loaded is None:
            return out
        gimg, bl, bt, bw, bh = loaded

        scale = min(self.slot_w / bw, self.slot_h / bh)
        nw = max(1, int(bw * scale))
        nh = max(1, int(bh * scale))
        fitted = gimg.resize((nw, nh), Image.Resampling.LANCZOS)

        ox = self.slot_left + (self.slot_w - nw) // 2
        oy = self.slot_top + (self.slot_h - nh) // 2
        out.paste(fitted, (ox, oy))

        # Reconstruct FreeType pen in em coords so pas-below marks can share it.
        # Bitmap is drawn at (pen_x + bl*S, pen_y - bt*S).
        pen_x = ox - bl * scale
        pen_y = oy + bt * scale
        if cp == PAS_BELOW_REF:
            self._ref_pen = (pen_x, pen_y, scale)

        return out.point(lambda p: 0 if p < 180 else 255)

    def render_pas_below(self, mark_cp: int) -> Image.Image:
        """Place pasangan-attached below vowel using the reference pasangan pen."""
        out = Image.new("L", (self.em_w, self.em_h), 255)
        gname = PAS_BELOW_CPS.get(mark_cp)
        if gname is None:
            return out
        if self._ref_pen is None:
            # Ensure reference pasangan placement is computed
            self.render_pasangan(PAS_BELOW_REF)
        if self._ref_pen is None:
            return out

        gid = self.name_to_gid.get(gname)
        if gid is None:
            return out
        loaded = self._ft_glyph_img(gid)
        if loaded is None:
            return out
        gimg, bl, bt, bw, bh = loaded

        pen_x, pen_y, scale = self._ref_pen
        nw = max(1, int(bw * scale))
        nh = max(1, int(bh * scale))
        ox = int(round(pen_x + bl * scale))
        oy = int(round(pen_y - bt * scale))

        # Clip into em — prefer keeping the attach point (top) if we must crop.
        fitted = gimg.resize((nw, nh), Image.Resampling.LANCZOS)
        if oy + nh > self.em_h:
            # Scale down slightly so the J-hook stays inside the em
            room = self.em_h - oy - 1
            if room < 4:
                oy = max(0, self.em_h - nh - 1)
                room = self.em_h - oy - 1
            if room < nh and room >= 4:
                s2 = room / nh
                nw = max(1, int(nw * s2))
                nh = max(1, int(nh * s2))
                fitted = gimg.resize((nw, nh), Image.Resampling.LANCZOS)

        # Paste with clipping
        px0, py0 = max(0, ox), max(0, oy)
        sx0, sy0 = px0 - ox, py0 - oy
        sx1 = min(nw, self.em_w - ox)
        sy1 = min(nh, self.em_h - oy)
        if sx1 > sx0 and sy1 > sy0:
            crop = fitted.crop((sx0, sy0, sx1, sy1))
            out.paste(crop, (px0, py0))
        return out.point(lambda p: 0 if p < 180 else 255)


def emit_array(lines: list[str], name: str, size: int, table: dict[int, list[int]]):
    rb = (size + 7) // 8
    bpg = size * rb
    lines.append(f"const uint8_t {name}[JAWA_COUNT][{bpg}] = {{")
    nonempty = total = 0
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        data = table.get(cp) or empty_bitmap(size)
        assert len(data) == bpg
        ic = ink_count(data)
        if ic:
            nonempty += 1
            total += ic
        hx = ",".join(f"0x{b:02X}" for b in data)
        lines.append(f"    {{{hx}}}, /* U+{cp:04X} */")
    lines.append("};")
    lines.append("")
    return nonempty, total // max(nonempty, 1)


def emit_adv(lines: list[str], name: str, size: int, table: dict[int, list[int]]):
    lines.append(f"const uint8_t {name}[JAWA_COUNT] = {{")
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        adv = 0 if cp in MARKS else ink_advance(table.get(cp) or empty_bitmap(size), size)
        lines.append(f"    {adv}, /* U+{cp:04X} */")
    lines.append("};")
    lines.append("")


def write_preview(table: dict[int, list[int]], size: int, path: Path, cps: list[int]):
    cols = min(10, len(cps))
    rows_n = (len(cps) + cols - 1) // cols
    preview = Image.new("L", (size * cols, size * rows_n), 255)
    rb = (size + 7) // 8
    for i, cp in enumerate(cps):
        data = table.get(cp) or empty_bitmap(size)
        cell = Image.new("L", (size, size), 255)
        px = cell.load()
        for row in range(size):
            for x in range(size):
                if (data[row * rb + (x >> 3)] >> (x & 7)) & 1:
                    px[x, row] = 0
        r, c = divmod(i, cols)
        preview.paste(cell, (c * size, r * size))
    preview.resize((size * cols * 2, size * rows_n * 2), Image.Resampling.NEAREST).save(path)


def composite_check(base_hi: dict, pas_hi: dict, pas_below_hi: dict, path: Path):
    """ma + pasangan ba + u.ns.pas — sanity check attachment to ending stroke."""
    ma = base_hi[0xA9A9].convert("L")
    ba_pas = pas_hi[0xA9A7].convert("L")
    suku_pas = pas_below_hi[0xA9B8].convert("L")
    comp = ImageChops.darker(ma, ba_pas)
    comp = ImageChops.darker(comp, suku_pas)
    comp.resize((ma.size[0] * 3, ma.size[1] * 3), Image.Resampling.NEAREST).save(path)
    print(f"composite check -> {path}")
    print("  ma", ink_bbox(ma), "pas ba", ink_bbox(ba_pas), "u.ns.pas", ink_bbox(suku_pas))


def main():
    font_path = find_font()
    print(f"font: {font_path}")

    pil = PillowEm(font_path)
    pas_ex = PasanganExtractor(font_path, pil.em_w, pil.em_h)

    base_hi: dict[int, Image.Image] = {}
    pas_hi: dict[int, Image.Image] = {}
    pas_below_hi: dict[int, Image.Image] = {}

    # Pasangan first so the ba reference pen is ready for pas-below marks.
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        if cp in PASANGAN_CPS:
            pas_hi[cp] = pas_ex.render_pasangan(cp)
            bb = ink_bbox(pas_hi[cp])
            gid = pas_ex.pasangan_gid(cp)
            name = pas_ex.names[gid] if gid is not None else "?"
            print(f"  pas U+{cp:04X} {name} bbox={bb}")
        else:
            pas_hi[cp] = Image.new("L", (pil.em_w, pil.em_h), 255)

    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        if cp in MARKS:
            base_hi[cp] = pil.render_mark(cp)
        else:
            base_hi[cp] = pil.render_base(cp)

        if cp in PAS_BELOW_CPS:
            pas_below_hi[cp] = pas_ex.render_pas_below(cp)
            print(f"  pas-below U+{cp:04X} {PAS_BELOW_CPS[cp]} bbox={ink_bbox(pas_below_hi[cp])}")
        else:
            pas_below_hi[cp] = Image.new("L", (pil.em_w, pil.em_h), 255)

    # Sanity: marks should have decent ink in expected regions
    for cp, label in [(0xA9B6, "wulu"), (0xA9B8, "suku"), (0xA9BA, "taling")]:
        print(f"  mark {label} bbox={ink_bbox(base_hi[cp])}")

    composite_check(base_hi, pas_hi, pas_below_hi, ROOT / "tools" / "_composite_mbu.png")

    all_base = {s: {} for s in SIZES}
    all_pas = {s: {} for s in SIZES}
    all_pas_below = {s: {} for s in SIZES}
    for size in SIZES:
        for cp in range(JAWA_MIN, JAWA_MAX + 1):
            all_base[size][cp] = to_bitmap(base_hi[cp], size)
            all_pas[size][cp] = to_bitmap(pas_hi[cp], size)
            all_pas_below[size][cp] = to_bitmap(pas_below_hi[cp], size)
        print(f"rasterized {size}x{size}")

    lines = [
        "/* Auto-rasterized from Noto Sans Javanese — dual 48/64 + real pasangan */",
        '#include "font_jawa.h"',
        "#include <stdbool.h>",
        "",
        "/* ---- base + mark bitmaps (fixed-origin Pillow) ---- */",
        "",
    ]
    for size in SIZES:
        n, avg = emit_array(lines, f"font_jawa{size}", size, all_base[size])
        print(f"  base{size}: nonempty={n} avg_ink={avg}")
    lines.append("/* ---- pasangan (OpenType .pas, lower-center of em) ---- */")
    lines.append("")
    for size in SIZES:
        n, avg = emit_array(lines, f"font_jawa_pas{size}", size, all_pas[size])
        print(f"  pas{size}: nonempty={n} avg_ink={avg}")
    lines.append("/* ---- below vowels on pasangan (u.ns.pas / uu.ns.pas / keret.ns.alt) ---- */")
    lines.append("")
    for size in SIZES:
        n, avg = emit_array(lines, f"font_jawa_pas_below{size}", size, all_pas_below[size])
        print(f"  pas_below{size}: nonempty={n} avg_ink={avg}")

    lines.append("/* ---- advances (marks = 0) ---- */")
    lines.append("")
    for size in SIZES:
        emit_adv(lines, f"font_jawa_adv{size}", size, all_base[size])

    lines.append("const uint8_t font_jawa_has_pas[JAWA_COUNT] = {")
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        has = 1 if ink_count(all_pas[64][cp]) > 20 else 0
        lines.append(f"    {has}, /* U+{cp:04X} */")
    lines.append("};")
    lines.append("")

    lines.append("const uint8_t font_jawa_has_pas_below[JAWA_COUNT] = {")
    for cp in range(JAWA_MIN, JAWA_MAX + 1):
        has = 1 if ink_count(all_pas_below[64][cp]) > 10 else 0
        lines.append(f"    {has}, /* U+{cp:04X} */")
    lines.append("};")
    lines.append("")

    lines += [
        "static int jawa_px = 64;",
        "",
        "void font_jawa_set_px(int px) { jawa_px = (px <= 48) ? 48 : 64; }",
        "int font_jawa_px(void) { return jawa_px; }",
        "int font_jawa_w(void)  { return jawa_px; }",
        "int font_jawa_h(void)  { return jawa_px; }",
        "int font_jawa_row_bytes(void) { return (jawa_px + 7) / 8; }",
        "int font_jawa_pasangan_dy(void) { return 0; } /* ink already in lower em */",
        "/* Pas-below ink is baked into the em; small pad for the J-hook. */",
        "int font_jawa_pasangan_extra(void) { return jawa_px * 6 / 64; }",
        "",
        "const uint8_t *font_jawa_glyph(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return 0;",
        "    unsigned i = cp - JAWA_CP_MIN;",
        "    return (jawa_px == 48) ? font_jawa48[i] : font_jawa64[i];",
        "}",
        "",
        "const uint8_t *font_jawa_pasangan(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return 0;",
        "    unsigned i = cp - JAWA_CP_MIN;",
        "    if (!font_jawa_has_pas[i]) return 0;",
        "    return (jawa_px == 48) ? font_jawa_pas48[i] : font_jawa_pas64[i];",
        "}",
        "",
        "const uint8_t *font_jawa_pas_below(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return 0;",
        "    unsigned i = cp - JAWA_CP_MIN;",
        "    if (!font_jawa_has_pas_below[i]) return 0;",
        "    return (jawa_px == 48) ? font_jawa_pas_below48[i] : font_jawa_pas_below64[i];",
        "}",
        "",
        "bool font_jawa_is_mark(uint32_t cp) {",
        "    return (cp >= 0xA980 && cp <= 0xA983) ||",
        "           (cp >= 0xA9B3 && cp <= 0xA9C0);",
        "}",
        "bool font_jawa_is_below_vowel(uint32_t cp) {",
        "    return cp == 0xA9B8 || cp == 0xA9B9 || cp == 0xA9BD;",
        "}",
        "bool font_jawa_is_medial(uint32_t cp) {",
        "    return cp == 0xA9BE || cp == 0xA9BF;",
        "}",
        "bool font_jawa_is_below_mark(uint32_t cp) {",
        "    return font_jawa_is_below_vowel(cp) || font_jawa_is_medial(cp);",
        "}",
        "bool font_jawa_is_base(uint32_t cp) {",
        "    return cp >= JAWA_CP_MIN && cp <= JAWA_CP_MAX && !font_jawa_is_mark(cp);",
        "}",
        "bool font_jawa_can_pasangan(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return false;",
        "    return font_jawa_has_pas[cp - JAWA_CP_MIN] != 0;",
        "}",
        "int font_jawa_advance(uint32_t cp) {",
        "    if (cp < JAWA_CP_MIN || cp > JAWA_CP_MAX) return 0;",
        "    if (font_jawa_is_mark(cp)) return 0;",
        "    unsigned i = cp - JAWA_CP_MIN;",
        "    return (jawa_px == 48) ? font_jawa_adv48[i] : font_jawa_adv64[i];",
        "}",
        "",
    ]

    OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KB)")

    carakan = [
        0xA9B2, 0xA9A4, 0xA995, 0xA9AB, 0xA98F, 0xA9A2, 0xA9A0, 0xA9B1, 0xA9AE, 0xA9AD,
        0xA9A5, 0xA99D, 0xA997, 0xA9AA, 0xA99A, 0xA9A9, 0xA992, 0xA9A7, 0xA99B, 0xA994,
    ]
    marks = [0xA9B6, 0xA9B8, 0xA9BA, 0xA9B4, 0xA9BC, 0xA9BF, 0xA9BE, 0xA9BD, 0xA9C0, 0xA981]
    write_preview(all_base[64], 64, ROOT / "tools" / "jawa_preview_64.png", carakan + marks)
    write_preview(all_pas[64], 64, ROOT / "tools" / "jawa_preview_pas_64.png", carakan)
    write_preview(all_base[48], 48, ROOT / "tools" / "jawa_preview_48.png", carakan + marks)
    write_preview(all_pas[48], 48, ROOT / "tools" / "jawa_preview_pas_48.png", carakan)
    write_preview(all_pas_below[64], 64, ROOT / "tools" / "jawa_preview_pas_below_64.png",
                  [0xA9B8, 0xA9B9, 0xA9BD])

    # Print 64px bboxes for the critical glyphs
    def bb64(data):
        rb = 8
        xs, ys = [], []
        for y in range(64):
            for x in range(64):
                if (data[y * rb + (x >> 3)] >> (x & 7)) & 1:
                    xs.append(x); ys.append(y)
        if not xs:
            return "empty"
        return f"({min(xs)},{min(ys)})-({max(xs)},{max(ys)})"

    print("64px bboxes:")
    for label, cp, tab in [
        ("ka", 0xA98F, all_base), ("ma", 0xA9A9, all_base), ("suku", 0xA9B8, all_base),
        ("wulu", 0xA9B6, all_base), ("ba.pas", 0xA9A7, all_pas), ("la.pas", 0xA9AD, all_pas),
        ("u.ns.pas", 0xA9B8, all_pas_below), ("uu.ns.pas", 0xA9B9, all_pas_below),
        ("keret.pas", 0xA9BD, all_pas_below),
    ]:
        print(f"  {label:10} {bb64(tab[64][cp])}")


if __name__ == "__main__":
    main()
