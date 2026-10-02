#!/usr/bin/env python3
"""Turns the promo capture output into Remotion static files under promo/public/.

    clips/<clip>/NNNN.jpg   recorded UI frames (480 x 480)
    clips/boot/NNNN.jpg     the real boot animation, unpacked from boot_anim.bin (MFB1)
    prints/<name>.jpg       real darkroom prints (long side 1080)
    manifest.json           capture manifest plus the boot clip
    fonts/                  the app's own Jost and DSEG7 (date stamp) faces
    fx/grain.png            a tileable film-grain texture

The device itself is rendered in 3D by promo/render3d/ from these clips and prints.

Usage (from the workspace root, needs Pillow):
    python3 projects/mosaico_film/tools/promo/export_assets.py <capture output dir>
"""
from __future__ import annotations

import json
import shutil
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

APP = Path(__file__).resolve().parents[2]
PUBLIC = APP / "promo/public"
BOOT_ANIM = APP / "components/film_assets/boot_anim.bin"
FONTS = [APP / "tools/assets/fonts" / f for f in ("Jost.ttf", "DSEG7Classic-Regular.ttf", "DSEG-LICENSE.txt")]
GRAIN_SIDE = 512
GRAIN_SIGMA = 38                # grey levels around mid-grey; the video blends it with "overlay"
BOOT_SOUND_MS = 1600            # film_boot.c BOOT_SOUND_MS: the boot sound lands on the animation's shutter
PRINT_LONG_SIDE = 1080
JPEG_QUALITY = 92


def export_clips(src: Path, manifest: dict) -> None:
    for name, clip in manifest["clips"].items():
        out = PUBLIC / "clips" / name
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        frames = sorted((src / name).glob("*.ppm"))
        if len(frames) != clip["frames"]:
            sys.exit(f"{name}: manifest says {clip['frames']} frames, found {len(frames)}")
        for i, f in enumerate(frames):
            Image.open(f).convert("RGB").save(out / f"{i:04d}.jpg", quality=JPEG_QUALITY)
        print(f"clip {name}: {len(frames)} frames")


def export_boot() -> dict:
    """Unpacks MFB1: magic, count u16, fps u16, count + 1 file offsets, then the JPEG frames."""
    data = BOOT_ANIM.read_bytes()
    magic, count, fps = struct.unpack_from("<4sHH", data)
    if magic != b"MFB1":
        sys.exit(f"{BOOT_ANIM}: not MFB1")
    offsets = struct.unpack_from(f"<{count + 1}I", data, 8)
    out = PUBLIC / "clips/boot"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for i in range(count):
        (out / f"{i:04d}.jpg").write_bytes(data[offsets[i]:offsets[i + 1]])
    print(f"clip boot: {count} frames at {fps} fps")
    sound = round(BOOT_SOUND_MS * fps / 1000)
    return {"frames": count, "marks": [{"frame": sound, "type": "sfx", "name": "boot", "x": 0, "y": 0}]}


def export_prints(src: Path, manifest: dict) -> None:
    out = PUBLIC / "prints"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    for p in manifest["prints"]:
        im = Image.open(src / "prints" / f"{p['name']}.ppm").convert("RGB")
        im.thumbnail((PRINT_LONG_SIDE, PRINT_LONG_SIDE), Image.LANCZOS)
        im.save(out / f"{p['name']}.jpg", quality=JPEG_QUALITY)
        p["width"], p["height"] = im.size
    print(f"prints: {len(manifest['prints'])}")


def export_static() -> None:
    fonts = PUBLIC / "fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    for f in FONTS:
        shutil.copy2(f, fonts / f.name)
    # Grain: two octaves of gaussian noise, so it is neither pure pixel hiss nor blotchy.
    rng = np.random.default_rng(1977)
    fine = rng.normal(0, 1, (GRAIN_SIDE, GRAIN_SIDE))
    coarse = np.asarray(Image.fromarray(rng.normal(128, 40, (GRAIN_SIDE // 2,) * 2).clip(0, 255).astype(np.uint8))
                        .resize((GRAIN_SIDE,) * 2, Image.BICUBIC), dtype=np.float32) / 40 - 3.2
    grain = (128 + GRAIN_SIGMA * (0.75 * fine + 0.25 * coarse)).clip(0, 255).astype(np.uint8)
    (PUBLIC / "fx").mkdir(parents=True, exist_ok=True)
    Image.fromarray(grain, "L").save(PUBLIC / "fx/grain.png")
    print("static: fonts, grain")


def main() -> None:
    src = Path(sys.argv[1])
    manifest = json.loads((src / "manifest.json").read_text())
    export_clips(src, manifest)
    manifest["clips"]["boot"] = export_boot()
    export_prints(src, manifest)
    export_static()
    (PUBLIC / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
