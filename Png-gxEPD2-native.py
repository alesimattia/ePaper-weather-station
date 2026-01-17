# CON WINDOWS USARE INVECE
# py -m pip install pillow
# py .\png_to_gxepd2_7c_gui.py


#!/usr/bin/env python3
"""
PNG -> GxEPD2_7C native (4bpp packed, 2 pixels per byte) -> image.h

- Quantizzazione: nearest-color su palette 7 colori (sempre)
- Dithering: Floyd–Steinberg (opzionale)

GUI: selezioni file input/output e opzioni.
Esempi CLI (se vuoi anche usare il modulo senza GUI, vedi funzione convert_png_to_header):
  - (GUI) python3 png_to_gxepd2_7c_gui.py
Parametri GUI:
  - Width, Height
  - Mode: fit|crop|stretch
  - Dither: on|off
"""

import tkinter as tk
from tkinter import filedialog, messagebox
from PIL import Image
import os

PALETTE = [
    (255, 255, 255),  # 0 WHITE
    (0, 0, 0),        # 1 BLACK
    (255, 0, 0),      # 2 RED
    (0, 255, 0),      # 3 GREEN
    (0, 0, 255),      # 4 BLUE
    (255, 255, 0),    # 5 YELLOW
    (255, 128, 0),    # 6 ORANGE (approx)
]

def clamp8(v: float) -> int:
    if v < 0: return 0
    if v > 255: return 255
    return int(v)

def nearest_index(r: int, g: int, b: int) -> int:
    best_i = 0
    best_d = 10**18
    for i, (pr, pg, pb) in enumerate(PALETTE):
        dr = r - pr
        dg = g - pg
        db = b - pb
        d = dr*dr + dg*dg + db*db
        if d < best_d:
            best_d = d
            best_i = i
    return best_i

def resize_to_canvas(img: Image.Image, width: int, height: int, mode: str) -> Image.Image:
    img = img.convert("RGB")
    if mode == "stretch":
        return img.resize((width, height), Image.Resampling.LANCZOS)

    src_w, src_h = img.size
    src_ar = src_w / src_h
    dst_ar = width / height

    if mode == "fit":
        if src_ar > dst_ar:
            new_w = width
            new_h = round(width / src_ar)
        else:
            new_h = height
            new_w = round(height * src_ar)
        resized = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
        canvas = Image.new("RGB", (width, height), (255, 255, 255))
        ox = (width - new_w) // 2
        oy = (height - new_h) // 2
        canvas.paste(resized, (ox, oy))
        return canvas

    # crop
    if src_ar > dst_ar:
        new_h = height
        new_w = round(height * src_ar)
    else:
        new_w = width
        new_h = round(width / src_ar)
    resized = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
    ox = (new_w - width) // 2
    oy = (new_h - height) // 2
    return resized.crop((ox, oy, ox + width, oy + height))

def quantize_nearest(pixels, w: int, h: int):
    # pixels: list of (r,g,b)
    idx = [0] * (w * h)
    for i, (r, g, b) in enumerate(pixels):
        idx[i] = nearest_index(r, g, b)
    return idx

def dither_floyd_steinberg(pixels, w: int, h: int):
    # pixels: list of (r,g,b) -> indices 0..6
    # Lavoriamo su float per diffondere l'errore
    buf = [[float(c) for c in px] for px in pixels]
    out = [0] * (w * h)

    def add_err(x, y, er, eg, eb, factor):
        if 0 <= x < w and 0 <= y < h:
            j = y * w + x
            buf[j][0] = float(clamp8(buf[j][0] + er * factor))
            buf[j][1] = float(clamp8(buf[j][1] + eg * factor))
            buf[j][2] = float(clamp8(buf[j][2] + eb * factor))

    for y in range(h):
        for x in range(w):
            i = y * w + x
            r, g, b = (int(buf[i][0]), int(buf[i][1]), int(buf[i][2]))
            pi = nearest_index(r, g, b)
            out[i] = pi
            pr, pg, pb = PALETTE[pi]
            er, eg, eb = (r - pr, g - pg, b - pb)

            # Floyd–Steinberg:
            # x+1, y     7/16
            # x-1, y+1   3/16
            # x,   y+1   5/16
            # x+1, y+1   1/16
            add_err(x + 1, y,     er, eg, eb, 7/16)
            add_err(x - 1, y + 1, er, eg, eb, 3/16)
            add_err(x,     y + 1, er, eg, eb, 5/16)
            add_err(x + 1, y + 1, er, eg, eb, 1/16)

    return out

def pack_4bpp(indices, w: int, h: int) -> bytearray:
    if (w * h) % 2 != 0:
        raise ValueError("width*height must be even (need pairs of pixels).")
    out = bytearray((w * h) // 2)
    k = 0
    for y in range(h):
        row = y * w
        for x in range(0, w, 2):
            hi = indices[row + x] & 0x0F
            lo = indices[row + x + 1] & 0x0F
            out[k] = (hi << 4) | lo
            k += 1
    return out

def write_header(out_h_path: str, src_name: str, w: int, h: int, packed: bytes):
    with open(out_h_path, "w", encoding="utf-8") as f:
        f.write("// Auto-generated for GxEPD2_7C (4bpp packed: 2 pixels per byte)\n")
        f.write(f"// Source: {src_name}\n")
        f.write(f"// Size: {w}x{h}\n\n")
        f.write("#pragma once\n#include <Arduino.h>\n\n")
        f.write(f"const uint16_t IMAGE_W = {w};\n")
        f.write(f"const uint16_t IMAGE_H = {h};\n\n")
        f.write("const uint8_t image_data[] PROGMEM = {\n")
        line = []
        for i, b in enumerate(packed):
            line.append(f"0x{b:02X}")
            if len(line) == 16:
                f.write("  " + ", ".join(line) + ",\n")
                line = []
        if line:
            f.write("  " + ", ".join(line) + ",\n")
        f.write("};\n\n")
        f.write("// Use with: display.drawNative(image_data, 0, 0, 0, IMAGE_W, IMAGE_H, false, false, true);\n")

def convert_png_to_header(png_path: str, out_h_path: str, width: int, height: int, mode: str, dither: bool):
    img = Image.open(png_path)
    img2 = resize_to_canvas(img, width, height, mode)
    pixels = list(img2.getdata())  # list of (r,g,b)

    indices = dither_floyd_steinberg(pixels, width, height) if dither else quantize_nearest(pixels, width, height)
    packed = pack_4bpp(indices, width, height)

    write_header(out_h_path, os.path.basename(png_path), width, height, packed)

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("PNG → GxEPD2 7C → image.h (nearest + optional dithering)")
        self.geometry("640x300")

        self.png_path = tk.StringVar()
        self.out_path = tk.StringVar()
        self.mode = tk.StringVar(value="fit")
        self.dither = tk.BooleanVar(value=False)
        self.width = tk.IntVar(value=800)
        self.height = tk.IntVar(value=480)

        frm = tk.Frame(self, padx=12, pady=12)
        frm.pack(fill="both", expand=True)

        tk.Label(frm, text="PNG input:").grid(row=0, column=0, sticky="w")
        tk.Entry(frm, textvariable=self.png_path, width=60).grid(row=0, column=1, sticky="we")
        tk.Button(frm, text="Browse…", command=self.pick_png).grid(row=0, column=2, padx=(8,0))

        tk.Label(frm, text="Output image.h:").grid(row=1, column=0, sticky="w", pady=(10,0))
        tk.Entry(frm, textvariable=self.out_path, width=60).grid(row=1, column=1, sticky="we", pady=(10,0))
        tk.Button(frm, text="Browse…", command=self.pick_out).grid(row=1, column=2, padx=(8,0), pady=(10,0))

        # size
        sizefrm = tk.Frame(frm)
        sizefrm.grid(row=2, column=1, sticky="w", pady=(10,0))
        tk.Label(frm, text="Size:").grid(row=2, column=0, sticky="w", pady=(10,0))
        tk.Label(sizefrm, text="W").pack(side="left")
        tk.Entry(sizefrm, textvariable=self.width, width=7).pack(side="left", padx=(6,12))
        tk.Label(sizefrm, text="H").pack(side="left")
        tk.Entry(sizefrm, textvariable=self.height, width=7).pack(side="left", padx=(6,12))

        # mode
        tk.Label(frm, text="Mode:").grid(row=3, column=0, sticky="w", pady=(10,0))
        modes = tk.Frame(frm)
        modes.grid(row=3, column=1, sticky="w", pady=(10,0))
        for m, label in [("fit","fit (letterbox)"), ("crop","crop (fill)"), ("stretch","stretch")]:
            tk.Radiobutton(modes, text=label, variable=self.mode, value=m).pack(side="left", padx=(0,12))

        # dither
        tk.Label(frm, text="Dithering:").grid(row=4, column=0, sticky="w", pady=(10,0))
        tk.Checkbutton(frm, text="Enable Floyd–Steinberg", variable=self.dither).grid(row=4, column=1, sticky="w", pady=(10,0))

        tk.Button(frm, text="Convert", command=self.convert, height=2).grid(row=5, column=1, sticky="e", pady=(18,0))

        frm.columnconfigure(1, weight=1)

    def pick_png(self):
        path = filedialog.askopenfilename(filetypes=[("PNG images","*.png"), ("All files","*.*")])
        if path:
            self.png_path.set(path)
            if not self.out_path.get():
                self.out_path.set(os.path.join(os.path.dirname(path), "image.h"))

    def pick_out(self):
        path = filedialog.asksaveasfilename(defaultextension=".h", filetypes=[("Header","*.h")])
        if path:
            self.out_path.set(path)

    def convert(self):
        png = self.png_path.get().strip()
        outp = self.out_path.get().strip()

        try:
            w = int(self.width.get())
            h = int(self.height.get())
            if w <= 0 or h <= 0:
                raise ValueError("Width/Height must be > 0")
        except Exception:
            messagebox.showerror("Error", "Width/Height non validi.")
            return

        if not png or not os.path.exists(png):
            messagebox.showerror("Error", "Seleziona un PNG valido.")
            return
        if not outp:
            messagebox.showerror("Error", "Seleziona un file di output (image.h).")
            return

        try:
            convert_png_to_header(png, outp, w, h, self.mode.get(), bool(self.dither.get()))
            messagebox.showinfo("OK", f"Creato:\n{outp}")
        except Exception as e:
            messagebox.showerror("Conversion error", str(e))

if __name__ == "__main__":
    App().mainloop()