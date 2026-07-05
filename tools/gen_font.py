#!/usr/bin/env python3
"""
SmallTV — Turkce GFX font ureteci.

Kullanim:
    pip install freetype-py
    python tools/gen_font.py <font.ttf>

ACIK LISANSLI bir font kullanin (DejaVu Sans Bold, Roboto Bold, Inter vb.).
Cikti: src/trfont.h  — iki GFX font (trFontS=15px etiket, trFontL=22px baslik),
Latin-5 (ISO-8859-9) tek-bayt duzeninde 0x20..0xFF. TFT_eSPI setFreeFont ile uyumlu.
Bit-paketleme Adafruit-GFX fontconvert.c ile birebir (bayt hizali, MSB-first).
"""
import sys, os, freetype

# Latin-5 (ISO-8859-9): sadece 6 konum Latin-1'den farkli (Turkce harfler)
OV = {0xD0: 0x011E, 0xDD: 0x0130, 0xDE: 0x015E, 0xF0: 0x011F, 0xFD: 0x0131, 0xFE: 0x015F}

def cp_for(b):
    if b < 0x80: return b
    if b in OV:  return OV[b]
    if b < 0xA0: return None      # kontrol karakterleri -> bos glif
    return b                       # 0xA0..0xFF = Latin-1 ile ayni

def gen(font_path, px):
    f = freetype.Face(font_path); f.set_pixel_sizes(0, px); yadv = f.size.height >> 6
    bm = bytearray(); gl = []
    for b in range(0x20, 0x100):
        cp = cp_for(b); w = h = adv = xo = yo = 0; start = len(bm)
        if cp is not None and f.get_char_index(cp) != 0:
            f.load_char(chr(cp), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
            g = f.glyph; bp = g.bitmap; w = bp.width; h = bp.rows; pitch = bp.pitch; buf = bp.buffer
            adv = g.advance.x >> 6; xo = g.bitmap_left; yo = 1 - g.bitmap_top
            sm = 0; cnt = 0
            for y in range(h):
                for x in range(w):
                    on = (buf[y * pitch + (x >> 3)] & (0x80 >> (x & 7))) != 0
                    sm = ((sm << 1) | (1 if on else 0)) & 0xFF; cnt += 1
                    if cnt == 8: bm.append(sm); sm = 0; cnt = 0
            if cnt: bm.append((sm << (8 - cnt)) & 0xFF)   # bayt hizasina padle
        else:
            adv = max(3, px // 3)
        gl.append((start, w, h, adv, xo, yo))
    return bm, gl, yadv

def emit(fh, name, font_path, px):
    bm, gl, yadv = gen(font_path, px)
    fh.write(f"// {name}: {os.path.basename(font_path)} {px}px, Latin-5, {len(gl)} glif, {len(bm)} B\n")
    fh.write(f"const uint8_t {name}_Bitmaps[] PROGMEM = {{" + ",".join(str(x) for x in bm) + "};\n")
    fh.write(f"const GFXglyph {name}_Glyphs[] PROGMEM = {{" +
             ",".join(f"{{{o},{w},{h},{a},{xo},{yo}}}" for (o, w, h, a, xo, yo) in gl) + "};\n")
    fh.write(f"const GFXfont {name} PROGMEM = {{(uint8_t*){name}_Bitmaps,(GFXglyph*){name}_Glyphs,0x20,0xFF,{yadv}}};\n\n")
    return len(bm)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Kullanim: python tools/gen_font.py <font.ttf>"); sys.exit(1)
    font = sys.argv[1]
    out = os.path.join(os.path.dirname(__file__), "..", "src", "trfont.h")
    with open(out, "w", encoding="ascii") as fh:
        fh.write(f"// OTOMATIK URETILDI - kaynak: {os.path.basename(font)} (acik lisans)\n")
        fh.write("// Yeniden uretmek icin: python tools/gen_font.py <font.ttf>\n#pragma once\n\n")
        s = emit(fh, "trFontS", font, 15)
        l = emit(fh, "trFontL", font, 22)
    print(f"src/trfont.h yazildi ({os.path.basename(font)}): S={s}B, L={l}B")
