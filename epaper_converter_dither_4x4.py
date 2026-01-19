# STRETCH (default): deforma l'immagine a 800x480
#python img_to_gxepd2_7c.py input.png --out /path/assoluto/image.h

# STRETCH a 600x400
#python img_to_gxepd2_7c.py input.jpg --out ./image.h --width 600 --height 400

# FIT: mantiene aspect ratio, letterbox bianco
#python img_to_gxepd2_7c.py input.jpg --out ./image.h --fit

# FIT + dithering
#python img_to_gxepd2_7c.py input.jpg --out ./image.h --fit --dither




#!/usr/bin/env python3
import argparse
from PIL import Image

# ==========================
# CONFIG HARD-CODED (come richiesto)
# ==========================
MAX_W = 800
MAX_H = 480
DEFAULT_W = 800
DEFAULT_H = 480

# Palette 7-color (approssimata)
# Indici:
# 0=BLACK, 1=WHITE, 2=GREEN, 3=BLUE, 4=RED, 5=YELLOW, 6=ORANGE
PALETTE = [
    ("BLACK",  (0, 0, 0)),
    ("WHITE",  (255, 255, 255)),
    ("GREEN",  (0, 255, 0)),
    ("BLUE",   (0, 0, 255)),
    ("RED",    (255, 0, 0)),
    ("YELLOW", (255, 255, 0)),
    ("ORANGE", (255, 165, 0)),
]

# ==========================
# Utility
# ==========================
def clamp_size(w, h):
    w = max(1, min(int(w), MAX_W))
    h = max(1, min(int(h), MAX_H))
    return w, h

def nearest_palette(rgb):
    r, g, b = rgb
    best_i = 0
    best_d = 1e18
    best_rgb = PALETTE[0][1]
    for i, (_, (pr, pg, pb)) in enumerate(PALETTE):
        dr = r - pr
        dg = g - pg
        db = b - pb
        d = dr*dr + dg*dg + db*db
        if d < best_d:
            best_d = d
            best_i = i
            best_rgb = (pr, pg, pb)
    return best_i, best_rgb

def pack_4bpp(indices, w, h):
    # 2 pixel / byte: high nibble = x pari, low nibble = x dispari
    out = bytearray((w * h + 1) // 2)
    k = 0
    for y in range(h):
        row = y * w
        for x in range(w):
            idx = indices[row + x] & 0x0F
            if (x & 1) == 0:
                out[k] = idx << 4
            else:
                out[k] |= idx
                k += 1
        if (w & 1):
            k += 1
    return out

# ==========================
# Quantizzazione
# ==========================
def quantize_no_dither(img):
    w, h = img.size
    px = img.load()
    out = [0] * (w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            out[i], _ = nearest_palette(px[x, y])
            i += 1
    return out

def quantize_floyd_steinberg(img):
    w, h = img.size
    px = img.load()

    buf = [[[float(px[x, y][c]) for c in range(3)] for x in range(w)] for y in range(h)]
    indices = [0] * (w * h)

    def add(x, y, er, eg, eb, f):
        if 0 <= x < w and 0 <= y < h:
            buf[y][x][0] += er * f
            buf[y][x][1] += eg * f
            buf[y][x][2] += eb * f

    for y in range(h):
        for x in range(w):
            r = int(max(0, min(255, round(buf[y][x][0]))))
            g = int(max(0, min(255, round(buf[y][x][1]))))
            b = int(max(0, min(255, round(buf[y][x][2]))))

            idx, (qr, qg, qb) = nearest_palette((r, g, b))
            indices[y * w + x] = idx

            er, eg, eb = r - qr, g - qg, b - qb
            add(x+1, y,   er, eg, eb, 7/16)
            add(x-1, y+1, er, eg, eb, 3/16)
            add(x,   y+1, er, eg, eb, 5/16)
            add(x+1, y+1, er, eg, eb, 1/16)

    return indices

# ==========================
# Output
# ==========================
def write_image_h(path, w, h, data):
    with open(path, "w", encoding="utf-8") as f:
        f.write("#pragma once\n#include <Arduino.h>\n\n")
        f.write(f"static const int IMG_W = {w};\n")
        f.write(f"static const int IMG_H = {h};\n\n")
        f.write("// 4bpp packed, 2 pixel per byte (high nibble = x pari)\n")
        f.write(f"// Size: {len(data)} bytes\n\n")
        f.write("const uint8_t Img_test[] PROGMEM = {\n")
        for i in range(0, len(data), 16):
            line = ", ".join(f"0x{b:02X}" for b in data[i:i+16])
            f.write(f"  {line},\n")
        f.write("};\n")

# ==========================
# Main
# ==========================
def main():
    ap = argparse.ArgumentParser(
        description="PNG/JPG → GxEPD2 7-color image.h (STRETCH default, FIT optional)"
    )
    ap.add_argument("input", help="Input image")
    ap.add_argument("--out", required=True, help="Output image.h (path completo)")
    ap.add_argument("--width", type=int, default=DEFAULT_W, help="Output width (default 800)")
    ap.add_argument("--height", type=int, default=DEFAULT_H, help="Output height (default 480)")
    ap.add_argument("--fit", action="store_true", help="Mantiene aspect ratio con letterbox")
    ap.add_argument("--dither", action="store_true", help="Abilita Floyd–Steinberg dithering")
    ap.add_argument("--resample", choices=["nearest","bilinear","bicubic","lanczos"],
                    default="lanczos", help="Filtro resize")
    args = ap.parse_args()

    out_w, out_h = clamp_size(args.width, args.height)

    img = Image.open(args.input).convert("RGB")

    resample_map = {
        "nearest": Image.NEAREST,
        "bilinear": Image.BILINEAR,
        "bicubic": Image.BICUBIC,
        "lanczos": Image.LANCZOS,
    }
    res = resample_map[args.resample]

    if args.fit:
        # FIT: letterbox
        scale = min(out_w / img.width, out_h / img.height)
        nw = int(img.width * scale)
        nh = int(img.height * scale)
        img_r = img.resize((nw, nh), res)
        canvas = Image.new("RGB", (out_w, out_h), (255, 255, 255))
        canvas.paste(img_r, ((out_w - nw)//2, (out_h - nh)//2))
        img = canvas
    else:
        # STRETCH: deforma intenzionalmente
        img = img.resize((out_w, out_h), res)

    if args.dither:
        indices = quantize_floyd_steinberg(img)
    else:
        indices = quantize_no_dither(img)

    data = pack_4bpp(indices, out_w, out_h)
    write_image_h(args.out, out_w, out_h, data)

    print("OK")
    print(f"  out: {args.out}")
    print(f"  size: {out_w}x{out_h}")
    print(f"  mode: {'FIT' if args.fit else 'STRETCH'}")
    print(f"  dithering: {'ON' if args.dither else 'OFF'}")

if __name__ == "__main__":
    main()
