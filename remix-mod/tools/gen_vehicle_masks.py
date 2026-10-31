"""Roughness masks for vehicle texture atlases (one-off authoring tool, not run at build time).

usage: python gen_vehicle_masks.py <dir with <HASH>.png decoded game atlases> <out dir>
The game's vehicle atlases (256x256 RGB565, from a Remix capture: rtx-remix/captures/textures/<HASH>.dds) hold body
paint, glass, lights and dark trim side by side. Classify each pixel by color into glossy paint, glass and dark trim
and write a grayscale roughness mask <HASH>_rough.png (white = rough). Only the mask is kept, never game pixels.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

PAINT, GLASS, TRIM = 0.18, 0.04, 0.55

# Atlases that are not car bodies, with a constant roughness instead of the classifier
CONSTANT = {
    "B6A7783CF8E3FEAC": 0.85,  # tarpaulin trailer
    "0BCA2E90F90E4B71": 0.75,  # curtain-side trailer
    "8211DABB7F356637": 0.5,   # engine and underside
    "9A0FF7C84F68B3F2": 0.5,   # engine and underside
    "15DD14BA147F9B4C": 0.5,   # engine and underside
    "2914FAA416D42F2E": 0.5,   # engine and underside
}


def classify(rgb):
    a = rgb.astype(np.float32) / 255.0
    mx, mn = a.max(2), a.min(2)
    lum = a @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    sat = np.where(mx > 1e-3, (mx - mn) / np.maximum(mx, 1e-3), 0.0)
    r = np.full(lum.shape, PAINT, np.float32)
    grey = sat < 0.3
    glass = grey & (lum > 0.06) & (lum < 0.45)
    trim = grey & (lum <= 0.06)
    r[glass] = GLASS
    r[trim] = TRIM
    return r


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name in sorted(os.listdir(src)):
        h = name[:16]
        img = Image.open(os.path.join(src, name)).convert("RGB")
        if h in CONSTANT:
            r = np.full((img.height, img.width), CONSTANT[h], np.float32)
        else:
            r = classify(np.asarray(img))
        m = Image.fromarray((r * 255 + 0.5).astype(np.uint8), "L").filter(ImageFilter.MedianFilter(5))
        m.save(os.path.join(out, f"{h}_rough.png"), optimize=True)
        print(h)


if __name__ == "__main__":
    main()
