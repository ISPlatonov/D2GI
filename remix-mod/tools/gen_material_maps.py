"""Procedural, tileable detail maps for HT2 material overrides (no game pixels are used).

usage: python gen_material_maps.py <out_dir> [preview_dir]
Writes tangent-space normal maps (DirectX convention, as AperturePBR encoding 2 expects) as uncompressed DDS.
"""
import os
import sys

import numpy as np
from PIL import Image

N = 512


def periodic_noise(seed, power, aniso=(1.0, 1.0), lo=2, hi=None):
    """Tileable noise from a 1/f^power spectrum (periodic by construction: inverse FFT)."""
    rng = np.random.default_rng(seed)
    fy = np.fft.fftfreq(N)[:, None] * N * aniso[1]
    fx = np.fft.fftfreq(N)[None, :] * N * aniso[0]
    f = np.sqrt(fx * fx + fy * fy)
    amp = np.where(f < lo, 0.0, 1.0 / np.maximum(f, 1e-6) ** power)
    if hi is not None:
        amp *= np.exp(-(f / hi) ** 2)
    phase = rng.uniform(0, 2 * np.pi, (N, N))
    h = np.real(np.fft.ifft2(amp * np.exp(1j * phase)))
    h -= h.min()
    return h / max(h.max(), 1e-9)


def to_normal(h, strength):
    # Wrapped central differences keep the map tileable.
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
    nx, ny, nz = -gx, gy, np.ones_like(h)  # +Y down (DirectX)
    l = np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.dstack([nx / l, ny / l, nz / l]) * 0.5 + 0.5
    a = np.full((N, N, 1), 1.0)
    return Image.fromarray((np.dstack([rgb, a]) * 255 + 0.5).astype(np.uint8), "RGBA")


def grass():
    # Fine blades (stretched along V) over soft clumps.
    blades = periodic_noise(11, 0.6, aniso=(1.0, 0.25), lo=24, hi=200)
    clumps = periodic_noise(12, 1.6, lo=2, hi=40)
    return to_normal(blades * 0.7 + clumps * 0.3, 18.0)


def asphalt():
    # Grain and small stones, with gentle large-scale waviness.
    grain = periodic_noise(21, 0.3, lo=30, hi=120)
    stones = np.clip(periodic_noise(22, 0.8, lo=20, hi=120) - 0.55, 0, 1) * 2.0
    wave = periodic_noise(23, 2.0, lo=1, hi=8)
    # Softer than the first version (31.10): near the truck the road looked like crumpled foil
    return to_normal(grain * 0.5 + stones * 0.4 + wave * 0.1, 4.0)


def water():
    # Ripples: directional small waves (stretched across the flow) over a few long swells.
    ripples = periodic_noise(31, 1.2, aniso=(0.5, 1.0), lo=8, hi=60)
    swell = periodic_noise(32, 2.2, lo=1, hi=6)
    return to_normal(ripples * 0.8 + swell * 0.2, 6.0)


def water_flow():
    """Animated ripples: an 8x8 sprite sheet of 128x128 frames (Remix plays it with sprite_sheet_*, 8 fps).
    Periodic layers roll by whole periods per 64-frame loop, so the 8 s loop is seamless; 2 px per frame keeps
    the motion smooth. 31.10: the first version (16 frames, 2 s) flowed about 4x too fast for the owner."""
    global N
    n_old, N = N, 128
    try:
        a = periodic_noise(41, 1.2, aniso=(0.5, 1.0), lo=2, hi=15)
        b = periodic_noise(42, 1.0, aniso=(0.7, 1.0), lo=4, hi=25)
        swell = periodic_noise(43, 2.2, lo=1, hi=2)
        frames, cols = 64, 8
        sheet = Image.new("RGBA", (cols * N, cols * N))
        for f in range(frames):
            s = (f * N) // frames                     # one period along V per loop
            h = (np.roll(a, s, 0) * 0.55 + np.roll(np.roll(b, s, 0), -s, 1) * 0.3
                 + np.roll(swell, s, 0) * 0.15)
            sheet.paste(to_normal(h, 2.5), ((f % cols) * N, (f // cols) * N))
        return sheet
    finally:
        N = n_old


def waterfall():
    """Falling water (dam spillway): 4x4 sprite sheet of 128x128 frames, 16 frames at 16 fps (1 s loop).
    Streaks stretched along V fall by one period per loop. Returns (albedo sheet, normal sheet): dark water with
    white foam streaks, and matching relief."""
    global N
    n_old, N = N, 128
    try:
        streaks = periodic_noise(51, 0.9, aniso=(0.12, 1.0), lo=3, hi=20)
        foam = periodic_noise(52, 1.1, aniso=(0.3, 1.0), lo=4, hi=15)
        cols, frames = 4, 16
        col = Image.new("RGBA", (cols * N, cols * N))
        nrm = Image.new("RGBA", (cols * N, cols * N))
        for f in range(frames):
            s = (f * N) // frames
            h = np.roll(streaks, s, 0) * 0.7 + np.roll(foam, 2 * s, 0) * 0.3
            w = np.clip((h - 0.45) * 2.5, 0, 1)          # foam amount 0..1
            base = np.array([20, 44, 46], np.float32)
            white = np.array([225, 235, 238], np.float32)
            rgb = base[None, None, :] * (1 - w[..., None]) + white[None, None, :] * w[..., None]
            a = np.full((N, N, 1), 235, np.float32)
            col.paste(Image.fromarray(np.dstack([rgb, a]).astype(np.uint8), "RGBA"), ((f % cols) * N, (f // cols) * N))
            nrm.paste(to_normal(h, 4.0), ((f % cols) * N, (f // cols) * N))
        return col, nrm
    finally:
        N = n_old


def wet_road():
    """Wet road: 4x4 sprite sheets of 128x128 frames, 16 frames at 16 fps (1 s loop), tiled in the road texture's UV.
    Roughness: puddles 0.03, wet asphalt 0.22. Normal: fine asphalt grain, and in puddles flat water with expanding
    raindrop rings (each drop starts at a fixed frame of the loop, so the loop is seamless)."""
    global N
    n_old, N = N, 128
    try:
        rng = np.random.default_rng(61)
        mask = periodic_noise(62, 2.0, lo=1, hi=6)
        puddle = np.clip((mask - 0.58) * 12.0, 0, 1)              # soft-edged blobs, ~25% of the area
        grain = periodic_noise(63, 0.4, lo=20, hi=60)
        rough = 0.22 * (1 - puddle) + 0.03 * puddle
        frames, cols = 16, 4
        drops = [(rng.uniform(0, N), rng.uniform(0, N), int(rng.integers(0, frames))) for _ in range(40)]
        yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
        rs = Image.new("RGBA", (cols * N, cols * N))
        ns = Image.new("RGBA", (cols * N, cols * N))
        rimg = Image.fromarray((np.dstack([rough] * 3 + [np.ones_like(rough)]) * 255 + 0.5).astype(np.uint8), "RGBA")
        for f in range(frames):
            h = grain * 0.6 * (1 - puddle)
            for (x, y, f0) in drops:
                age = (f - f0) % frames
                if age > 7:
                    continue
                radius = 1.5 + age * 1.6
                dx = (xx - x + N / 2) % N - N / 2                    # wrap so rings tile
                dy = (yy - y + N / 2) % N - N / 2
                dist = np.sqrt(dx * dx + dy * dy)
                ring = np.exp(-((dist - radius) / 0.9) ** 2) * (1 - age / 8.0)
                h = h + ring * 0.8 * puddle
            ns.paste(to_normal(h, 3.0), ((f % cols) * N, (f // cols) * N))
            rs.paste(rimg, ((f % cols) * N, (f // cols) * N))
        return rs, ns
    finally:
        N = n_old


def water_color():
    # Real water is dark; what you see is mostly reflection. Deep green-blue, 4x4 constant (the game's turquoise
    # texture made rivers look like a pool).
    a = np.zeros((4, 4, 4), np.uint8)
    a[..., 0], a[..., 1], a[..., 2], a[..., 3] = 14, 34, 36, 255
    return Image.fromarray(a, "RGBA")


def main():
    out = sys.argv[1]
    prev = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    for name, fn in (("grass_normal", grass), ("asphalt_normal", asphalt), ("water_normal", water), ("water_flow", water_flow),
                     ("water_color", water_color)):
        img = fn()
        img.save(os.path.join(out, name + ".dds"))
        if prev:
            os.makedirs(prev, exist_ok=True)
            img.convert("RGB").save(os.path.join(prev, name + ".png"))
        print(name, img.size)
    rs, ns = wet_road()
    rs.save(os.path.join(out, "wet_road_rough.dds"))
    ns.save(os.path.join(out, "wet_road_normal.dds"))
    if prev:
        ns.convert("RGB").save(os.path.join(prev, "wet_road_normal.png"))
        rs.convert("RGB").save(os.path.join(prev, "wet_road_rough.png"))
    col, nrm = waterfall()
    col.save(os.path.join(out, "waterfall_color.dds"))
    nrm.save(os.path.join(out, "waterfall_normal.dds"))
    if prev:
        col.convert("RGB").save(os.path.join(prev, "waterfall_color.png"))
    print("waterfall", col.size)


if __name__ == "__main__":
    main()
