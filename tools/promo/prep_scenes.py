#!/usr/bin/env python3
"""Prepares the Unsplash photos that stand in for the camera sensor in the promo capture.

The sensor is mounted rotated, so the upright "viewfinder direction" frame is portrait 3:4:
M6 sees its middle 9/16 strip (a landscape 4:3 band), SX-70 its middle square.
Each source photo is cropped to 3:4 around its subject and written twice as upright
binary PPM: <name>.ppm (1350 x 1800, windowed to 1080 x 1440 for stills) and
<name>_vf.ppm (960 x 1280, panned by a 768 x 1024 window for the live viewfinder).
promo_capture rotates each window into the sensor's orientation.

Usage (from the workspace root, needs Pillow):
    python3 projects/mosaico_film/tools/promo/prep_scenes.py <output dir>
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image

PROMO = Path(__file__).resolve().parents[2] / "promo"
SOURCES = PROMO / "assets/unsplash"
STILL = (1350, 1800)
VIEWFINDER = (960, 1280)


def crop_3x4(im: Image.Image, fx: float, fy: float) -> Image.Image:
    """Largest portrait 3:4 crop whose center sits as close to (fx, fy) as the frame allows."""
    w, h = im.size
    cw, ch = (w, round(w * 4 / 3)) if w * 4 <= h * 3 else (round(h * 3 / 4), h)
    left = min(max(round(fx * w - cw / 2), 0), w - cw)
    top = min(max(round(fy * h - ch / 2), 0), h - ch)
    return im.crop((left, top, left + cw, top + ch))


def main() -> None:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    credits = json.loads((SOURCES / "credits.json").read_text())
    for name, info in credits.items():
        im = Image.open(SOURCES / f"{name}.jpg").convert("RGB")
        scene = crop_3x4(im, *info["focus"])
        scene.resize(STILL, Image.LANCZOS).save(out / f"{name}.ppm")
        scene.resize(VIEWFINDER, Image.LANCZOS).save(out / f"{name}_vf.ppm")
        print(f"{name}: {im.size} -> {scene.size}")


if __name__ == "__main__":
    main()
