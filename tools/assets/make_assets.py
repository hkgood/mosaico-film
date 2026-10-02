#!/usr/bin/env python3
"""Mosaico Film 素材生成器：把已确认的 v3 样稿材质、字体和开机动画转成设备可直接使用的数据。

素材数据（约 5 MB）不编进固件：设备上它们放在独立的 assets 分区，运行时内存映射（走 Flash 缓存，
不占 PSRAM）；模拟器从同一个文件读取。固件里只保留字形表和绑定表。

输出（均为生成文件，改素材后重新运行本脚本）：
  components/film_assets/assets.bin        素材数据：精灵图（RGB565 + A8）、纹理、图标、字形位图、开机动画
  components/film_assets/assets_table.c    字形表、素材描述与 film_assets_bind()
  components/film_assets/include/film_assets.h
  components/film_assets/boot_anim.bin     开机动画 JPEG 序列（中间文件，--skip-boot 时复用）

依赖：Python 3.10+、Pillow、numpy、Google Chrome（渲染 CSS 材质）、ffmpeg（拆开机视频）。
用法：python tools/assets/make_assets.py [--boot-video PATH] [--skip-boot]
"""
from __future__ import annotations

import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
OUT = PROJECT / "components" / "film_assets"
UI_SOURCES = [PROJECT / "components" / "film_ui", PROJECT / "components" / "film_darkroom", PROJECT / "main"]
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
DEFAULT_BOOT_VIDEO = PROJECT / "release" / "mosaico_film_boot.mp4"

JOST = HERE / "fonts" / "Jost.ttf"
DSEG = HERE / "fonts" / "DSEG7Classic-Regular.ttf"
NOTO = HERE / "fonts" / "NotoSansSC-Regular.otf"

ASCII = "".join(chr(c) for c in range(32, 127))
JOST_EXTRA = "·×"
DSEG_CHARS = " '0123456789-."

# ---------------------------------------------------------------- 字体

@dataclass
class FontSpec:
    name: str
    path: Path
    size: float
    weight: int | None      # 可变字重（Jost），None 表示固定字重
    charset: str
    glow_sigma: float = 0.0  # >0 时输出模糊后的光晕图集，与同名核心字体叠加使用


def jost(name, size, weight=500, glow=0.0):
    return FontSpec(name, JOST, size, weight, ASCII + JOST_EXTRA, glow)


def ui_cjk_chars() -> str:
    """扫描界面源码字符串字面量里的非 Jost 字符，作为中文子集。"""
    chars = set()
    literal = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
    for root in UI_SOURCES:
        for path in list(root.rglob("*.c")) + list(root.rglob("*.h")):
            for text in literal.findall(path.read_text(encoding="utf-8")):
                chars.update(ch for ch in text if ord(ch) > 126 and ch not in JOST_EXTRA)
    chars.update("，。：、“”！？（）·")
    return " " + "".join(sorted(chars))


def font_specs() -> list[FontSpec]:
    cjk = ui_cjk_chars()
    return [
        jost("jost_m8", 7.5), jost("jost_m9", 9), jost("jost_m10", 10), jost("jost_m11", 10.5),
        jost("jost_m12", 12), jost("jost_m14", 14), jost("jost_m18", 18), jost("jost_m19", 19),
        jost("jost_m22", 22), jost("jost_m25", 25),
        jost("jost_r11", 11.5, 400), jost("jost_r12", 12, 400), jost("jost_r16", 16, 400),
        jost("jost_m11_glow", 10.5, glow=2.0), jost("jost_m19_glow", 19, glow=2.5),
        jost("jost_m25_glow", 25, glow=3.0),
        FontSpec("dseg_20", DSEG, 20, None, DSEG_CHARS), FontSpec("dseg_20_glow", DSEG, 20, None, DSEG_CHARS, 2.0),
        FontSpec("dseg_60", DSEG, 60, None, DSEG_CHARS), FontSpec("dseg_60_glow", DSEG, 60, None, DSEG_CHARS, 6.0),
        FontSpec("noto_11", NOTO, 11, None, cjk), FontSpec("noto_12", NOTO, 12, None, cjk),
        FontSpec("noto_13", NOTO, 13, None, cjk),
    ]


def load_font(spec: FontSpec) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(spec.path), spec.size)
    if spec.weight is not None:
        font.set_variation_by_axes([spec.weight])
    return font


def bake_font(spec: FontSpec):
    font = load_font(spec)
    pad = int(np.ceil(spec.glow_sigma * 3)) if spec.glow_sigma else 1
    glyphs, bitmap = [], bytearray()
    for ch in sorted(set(spec.charset)):
        cp = ord(ch)
        if spec.path != JOST and spec.path != DSEG and cp < 127 and ch != " ":
            continue  # 中文字体只取中文与全角标点，西文统一用 Jost
        advance = font.getlength(ch)
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w, h = r - l, b - t
        if w <= 0 or h <= 0:
            glyphs.append((cp, len(bitmap), 0, 0, 0, 0, round(advance * 16)))
            continue
        im = Image.new("L", (w + 2 * pad, h + 2 * pad), 0)
        ImageDraw.Draw(im).text((pad - l, pad - t), ch, font=font, fill=255, anchor="ls")
        if spec.glow_sigma:
            im = im.filter(ImageFilter.GaussianBlur(spec.glow_sigma))
        arr = np.asarray(im, dtype=np.uint8)
        # 裁掉全零边，减小图集
        ys, xs = np.nonzero(arr)
        if len(xs) == 0:
            glyphs.append((cp, len(bitmap), 0, 0, 0, 0, round(advance * 16)))
            continue
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        arr = arr[y0:y1, x0:x1]
        left, top = l - pad + int(x0), t - pad + int(y0)
        assert arr.shape[0] < 256 and arr.shape[1] < 256 and -128 <= left < 128 and -128 <= top < 128, spec.name
        glyphs.append((cp, len(bitmap), arr.shape[1], arr.shape[0], left, top, round(advance * 16)))
        bitmap += arr.tobytes()
    ref = "H" if spec.path != NOTO else "中"
    _, cap_top, _, _ = font.getbbox(ref, anchor="ls")
    _, descent = font.getmetrics()
    return glyphs, bytes(bitmap), -cap_top, descent


def bake_fonts(specs: list[FontSpec]) -> list[tuple]:
    fonts = []
    for spec in specs:
        glyphs, bitmap, ascent, descent = bake_font(spec)
        fonts.append((spec.name, glyphs, bitmap, ascent, descent))
        print(f"font {spec.name}: {len(glyphs)} glyphs, {len(bitmap)} bytes")
    return fonts

# ---------------------------------------------------------------- 精灵图（Chrome 渲染）

@dataclass
class Sprite:
    name: str
    w: int
    h: int
    pad: int
    html: Callable[[int, int], str]   # 参数为元素框（不含 pad）的左上角
    opaque: bool = False


def box(cls, x, y, w, h, style="", inner=""):
    return f'<div class="a {cls}" style="left:{x}px;top:{y}px;width:{w}px;height:{h}px;{style}">{inner}</div>'


ICON = {
    "back": '<path d="M15 5l-7 7 7 7"/>',
    "close": '<path d="M6 6l12 12M18 6L6 18"/>',
    "trash": '<path d="M5 7h14M10 7V5h4v2M7 7l1 12h8l1-12"/>',
    "phone": '<rect x="7" y="3" width="10" height="18" rx="2"/><path d="M11 18h2"/>',
    "check": '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
    "sun": '<circle cx="12" cy="12" r="3.6"/><path d="M12 3v2.4M12 18.6V21M3 12h2.4M18.6 12H21M5.6 5.6l1.7 1.7'
           'M16.7 16.7l1.7 1.7M5.6 18.4l1.7-1.7M16.7 7.3l1.7-1.7"/>',
    "moon": '<circle cx="12" cy="12" r="5" fill="#fff" stroke="none"/>',
    "wifi": '<path d="M2.5 9a14 14 0 0 1 19 0M5.8 12.4a9.3 9.3 0 0 1 12.4 0M9.1 15.8a4.6 4.6 0 0 1 5.8 0"/>'
            '<circle cx="12" cy="19" r="1.2" fill="#fff" stroke="none"/>',
}


def svg(name, size, stroke=1.6):
    return (f'<svg width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" stroke="#fff" '
            f'stroke-width="{stroke}" stroke-linecap="round" stroke-linejoin="round">{ICON[name]}</svg>')


def centered(cls, w, h, extra="", inner=""):
    return lambda x, y: (f'<div class="{cls}" style="left:{x + w / 2}px;top:{y + h / 2}px;width:{w}px;height:{h}px;'
                         f'{extra}">{inner}</div>')


SHUTTER_INNER = '<i class="well"></i><i class="knurl"></i><i class="dome"></i><i class="thread"></i>'
KNURLS = '<i class="kn l"></i><i class="kn r"></i>'


def popover(x, y):
    return (box("alu", x, y, 184, 94, "border-radius:10px")
            + f'<div class="pop" style="left:{x}px;top:{y}px;width:184px;height:94px">'
            + '<div class="a plate-light" style="inset:0"></div>'
            + '<div class="a" style="left:16px;right:16px;top:47px;height:1px;background:rgba(0,0,0,.18);'
              'box-shadow:0 1px 0 rgba(255,255,255,.6)"></div></div>'
            + f'<div class="a alu caret" style="left:{x + 128}px;top:{y + 87}px;box-shadow:2px 2px 3px rgba(0,0,0,.25)">'
              '</div>')


def plate(h):
    return lambda x, y: box("alu", x, y, 480, h) + box("plate-light", x, y, 480, h) + box("seam-top", x, y, 480, 2)


SPRITES = [
    Sprite("counter56", 56, 56, 6, centered("counter", 56, 56)),
    Sprite("counter72", 72, 72, 8, centered("counter", 72, 72, "background-size:72px 72px")),
    Sprite("shutter", 84, 84, 10, centered("shutter", 84, 84, inner=SHUTTER_INNER)),
    Sprite("shutter_down", 84, 84, 10, centered("shutter down", 84, 84, inner=SHUTTER_INNER)),
    Sprite("win140_base", 140, 38, 8, centered("window base", 140, 38)),
    Sprite("win140_over", 140, 38, 8, centered("window over", 140, 38, inner=KNURLS)),
    Sprite("win140_glow", 140, 38, 16, centered("window glow", 140, 38)),
    Sprite("win440_base", 440, 62, 8, centered("window base", 440, 62)),
    Sprite("win440_over", 440, 62, 8, centered("window over", 440, 62, inner=KNURLS)),
    Sprite("lever_off", 36, 18, 4, centered("lever", 36, 18)),
    Sprite("lever_on", 36, 18, 4, centered("lever on", 36, 18)),
    Sprite("popover", 184, 101, 34, popover),
    Sprite("rollend", 288, 250, 44, lambda x, y: box(
        "alu", x, y, 288, 250, "border-radius:14px;box-shadow:0 18px 40px rgba(0,0,0,.6),0 0 0 1px rgba(0,0,0,.5),"
        "inset 0 1px 0 rgba(255,255,255,.7)") + box("plate-light", x, y, 288, 250, "border-radius:14px")),
    Sprite("sx_shutter", 80, 80, 8, centered("sx-shutter", 80, 80)),
    Sprite("sx_shutter_down", 80, 80, 8, centered("sx-shutter down", 80, 80)),
    Sprite("pack", 54, 50, 8, centered("pack", 54, 50)),
    Sprite("m6_plate120", 480, 120, 0, plate(120), opaque=True),
    Sprite("m6_plate180", 480, 180, 0, plate(180), opaque=True),
    Sprite("icon_back", 18, 18, 0, lambda x, y: f'<div class="a" style="left:{x}px;top:{y}px">{svg("back", 18)}</div>'),
    Sprite("icon_close", 18, 18, 0, lambda x, y: f'<div class="a" style="left:{x}px;top:{y}px">{svg("close", 18)}</div>'),
    Sprite("icon_trash", 16, 16, 0, lambda x, y: f'<div class="a" style="left:{x}px;top:{y}px">{svg("trash", 16)}</div>'),
    Sprite("icon_phone", 16, 16, 0, lambda x, y: f'<div class="a" style="left:{x}px;top:{y}px">{svg("phone", 16, 1.8)}</div>'),
    Sprite("icon_check", 14, 14, 0, lambda x, y: f'<div class="a" style="left:{x}px;top:{y}px">{svg("check", 14, 2.4)}</div>'),
    Sprite("icon_wifi", 18, 18, 0, lambda x, y: f'<div class="a" style="left:{x}px;top:{y}px">{svg("wifi", 18)}</div>'),
]

# 单独整页渲染的大背景（480×480 坐标系，直接取自样稿布局）
def sx_body_page():
    sun = f'<div class="a" style="left:18px;top:52px;color:#efe3cc">{svg("sun", 19, 1.5).replace("#fff", "#efe3cc")}</div>'
    moon = f'<div class="a" style="left:21px;top:268px">{svg("moon", 13, 0).replace("#fff", "#0f0b08")}</div>'
    return ("<div class='a' style='left:0;top:0;width:480px;height:480px;background:#0b0907'></div>"
            + box("", 60, 0, 360, 360, "background:#000")
            + box("leather", 0, 0, 55, 360) + box("leather", 425, 0, 55, 360, "background-position:-425px 0")
            + box("ribs-v", 55, 0, 5, 360, "box-shadow:1px 0 2px rgba(0,0,0,.5)")
            + box("ribs-v", 420, 0, 5, 360, "box-shadow:-1px 0 2px rgba(0,0,0,.5)")
            + sun
            + '<div class="ev-ring" style="left:27.5px;top:180px;width:22px;height:150px"></div>'
            + box("", 8, 172, 5, 10, "border-radius:1px;background:#ff9f2e;box-shadow:0 0 4px #ff9f2e")
            + moon + box("", 20.5, 267.5, 14, 14, "border-radius:50%;box-shadow:0 0 0 1px rgba(239,227,204,.55)")
            + '<div class="sx-counter" style="left:452px;top:48px"></div>'
            + box("ribs-h", 0, 360, 480, 12, "box-shadow:0 2px 3px rgba(0,0,0,.45)")
            + box("leather", 0, 372, 480, 108, "background-position:0 -360px"))


def sx_drawer_page():
    return box("ribs-h", 0, 0, 480, 12) + box("leather", 0, 12, 480, 150, "background-position:0 -330px")


def dev_bg_page():
    return (box("leather", 0, 0, 480, 480)
            + box("", 0, 0, 480, 480, "background:radial-gradient(ellipse at 50% 35%,rgba(255,220,180,.08),"
                  "rgba(0,0,0,.25) 80%)")
            + box("paper", 60, 22, 360, 24, "border-radius:6px;box-shadow:0 3px 6px rgba(0,0,0,.5),"
                  "inset 0 1px 0 rgba(255,255,255,.72),inset 0 -1px 0 rgba(72,62,46,.18)")
            + box("", 74, 40, 332, 5, "border-radius:3px;background:#070504;box-shadow:inset 0 1px 2px #000"))


def dev_paper_page():
    return ('<div class="a paper" style="left:106px;top:42px;width:268px;height:322px;transform:rotate(-1.6deg);'
            'transform-origin:50% 0;border:1px solid rgba(255,255,255,.72);'
            'box-shadow:0 14px 26px rgba(0,0,0,.5),0 2px 4px rgba(0,0,0,.35),'
            'inset 0 0 0 1px rgba(72,62,46,.10),inset 0 1px 0 rgba(255,255,255,.72)"></div>')


PAGES = [
    # 名称, 页面 HTML, 裁剪框（None=按 alpha 自动裁），是否不透明
    ("sx_body", sx_body_page, (0, 0, 480, 480), True),
    ("sx_drawer", sx_drawer_page, (0, 0, 480, 162), True),
    ("dev_bg", dev_bg_page, (0, 0, 480, 480), True),
    ("dev_paper", dev_paper_page, None, False),
]


def page_html(body: str) -> str:
    return ('<!doctype html><html><head><meta charset="utf-8">'
            f'<link rel="stylesheet" href="{(HERE / "sprites.css").as_uri()}"></head><body>{body}</body></html>')


def chrome_render(html: str, w: int, h: int, png: Path):
    with tempfile.NamedTemporaryFile("w", suffix=".html", dir=HERE, delete=False, encoding="utf-8") as f:
        f.write(html)
        src = Path(f.name)
    try:
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                        "--default-background-color=00000000", f"--window-size={w},{h}",
                        "--allow-file-access-from-files", f"--screenshot={png}", src.as_uri()],
                       check=True, capture_output=True)
    finally:
        src.unlink()


def render_sprites(tmp: Path) -> dict[str, tuple[Image.Image, int, int]]:
    """把所有小精灵排在一张大页上渲染一次，再逐个裁出。返回 名称 → (RGBA 图, 锚点偏移 x, y)。"""
    page_w, x, y, row_h, body, cells = 1100, 0, 0, 0, [], []
    for s in SPRITES:
        cw, ch = s.w + 2 * s.pad, s.h + 2 * s.pad
        if x + cw > page_w:
            x, y, row_h = 0, y + row_h + 4, 0
        body.append(s.html(x + s.pad, y + s.pad))
        cells.append((s, x, y, cw, ch))
        x, row_h = x + cw + 4, max(row_h, ch)
    page_h = y + row_h + 4
    png = tmp / "sheet.png"
    chrome_render(page_html("".join(body)), page_w, page_h, png)
    sheet = Image.open(png).convert("RGBA")
    out = {}
    for s, cx, cy, cw, ch in cells:
        im = sheet.crop((cx, cy, cx + cw, cy + ch))
        out[s.name] = (im.convert("RGB").convert("RGBA") if s.opaque else im, -s.pad, -s.pad)
    for name, fn, crop, opaque in PAGES:
        png = tmp / f"{name}.png"
        chrome_render(page_html(fn()), 480, 480, png)
        im = Image.open(png).convert("RGBA")
        if crop is None:
            x0, y0, x1, y1 = im.getchannel("A").getbbox()
            crop = (x0, y0, x1 - x0, y1 - y0)
        x0, y0, cw, ch = crop
        im = im.crop((x0, y0, x0 + cw, y0 + ch))
        out[name] = (im.convert("RGB").convert("RGBA") if opaque else im, x0, y0)
    # 已确认的 09-camera-picker 产品图。保留为独立素材，选中描边与标签仍由运行时动态绘制。
    picker = HERE / "picker"
    out["picker_m6"] = (Image.open(picker / "picker_m6.png").convert("RGBA"), 0, 0)
    out["picker_sx70"] = (Image.open(picker / "picker_sx70.png").convert("RGBA"), 0, 0)
    return out


def to_rgb565(im: Image.Image) -> list[int]:
    a = np.asarray(im.convert("RGB"), dtype=np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return v.flatten().tolist()


def textures() -> dict[str, Image.Image]:
    tex = HERE / "tex"
    return {
        "tex_alu": Image.open(tex / "alu.png").convert("RGB").resize((480, 480), Image.LANCZOS),
        "tex_vulc": Image.open(tex / "vulcanite.png").convert("RGB").resize((240, 240), Image.LANCZOS),
        "tex_paper": Image.open(tex / "paper.png").convert("RGB").resize((120, 142), Image.LANCZOS),
        "tex_leather": Image.open(tex / "leather.png").convert("RGB").resize((240, 240), Image.LANCZOS),
    }


def collect_images(sprites, texs) -> list[tuple]:
    """返回 (名称, 宽, 高, RGB565 字节或 None, A8 字节或 None, ox, oy)"""
    images = []
    for name, (im, ox, oy) in sprites.items():
        w, h = im.size
        alpha = np.asarray(im.getchannel("A"), dtype=np.uint8)
        if name.startswith("icon_"):
            images.append((name, w, h, None, alpha.tobytes(), ox, oy))
            continue
        px = np.asarray(to_rgb565(im), dtype="<u2").tobytes()
        images.append((name, w, h, px, None if alpha.min() == 255 else alpha.tobytes(), ox, oy))
    for name, im in texs.items():
        w, h = im.size
        images.append((name, w, h, np.asarray(to_rgb565(im), dtype="<u2").tobytes(), None, 0, 0))
    return images

# ---------------------------------------------------------------- 开机动画

def pack_boot(video: Path, tmp: Path):
    frames_dir = tmp / "boot"
    frames_dir.mkdir()
    subprocess.run(["ffmpeg", "-v", "error", "-i", str(video), "-pix_fmt", "yuvj420p", "-q:v", "5",
                    str(frames_dir / "f%03d.jpg")], check=True)
    files = sorted(frames_dir.glob("f*.jpg"))
    # 片尾定格帧完全相同，只保留到最后一张有变化的帧
    decoded = [np.asarray(Image.open(f).convert("L"), dtype=np.int16) for f in files]
    last = len(files) - 1
    while last > 0 and np.abs(decoded[last] - decoded[last - 1]).mean() < 0.15:
        last -= 1
    files = files[:last + 1]
    blobs = [f.read_bytes() for f in files]
    header = struct.pack("<4sHH", b"MFB1", len(blobs), 30)
    offsets, pos = [], 8 + 4 * (len(blobs) + 1)
    for b in blobs:
        offsets.append(pos)
        pos += len(b)
    offsets.append(pos)
    data = header + struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(blobs)
    (OUT / "boot_anim.bin").write_bytes(data)
    print(f"boot: {len(blobs)} frames, {len(data) / 1024:.0f} KB")
    return data

# ---------------------------------------------------------------- 输出

HEADER = "/* 由 tools/assets/make_assets.py 生成，请勿手改。 */\n"
BLOB_MAGIC = b"MFA1"
BLOB_TRAILER = b"MFAE"
BLOB_VERSION = 1
BLOB_HEADER_LEN = 32
NONE = 0xFFFFFFFF


class Blob:
    """按 4 字节对齐追加数据，记录偏移"""

    def __init__(self):
        self.data = bytearray(BLOB_HEADER_LEN)

    def add(self, chunk: bytes | None) -> int:
        if chunk is None:
            return NONE
        while len(self.data) % 4:
            self.data.append(0)
        off = len(self.data)
        self.data += chunk
        return off


def write_outputs(fonts, images, boot: bytes):
    blob = Blob()
    font_rows = [(n, glyphs, blob.add(bitmap), len(bitmap), asc, desc) for n, glyphs, bitmap, asc, desc in fonts]
    image_rows = [(n, w, h, blob.add(px), blob.add(a), ox, oy) for n, w, h, px, a, ox, oy in images]
    boot_off = blob.add(boot)
    while len(blob.data) % 4:
        blob.data.append(0)
    blob.data += BLOB_TRAILER
    # 布局编号：素材名称、尺寸与偏移的摘要。固件与 assets 分区必须来自同一次生成
    layout = repr((font_rows and [(r[0], r[2], r[3]) for r in font_rows], [r[:5] for r in image_rows],
                   boot_off, len(boot), len(blob.data))).encode()
    layout_id = int.from_bytes(hashlib.sha1(layout).digest()[:4], "little")
    struct.pack_into("<4sIIIII", blob.data, 0, BLOB_MAGIC, BLOB_VERSION, layout_id, len(blob.data), boot_off,
                     len(boot))
    (OUT / "assets.bin").write_bytes(bytes(blob.data))

    # 头文件
    lines = [HEADER, "#pragma once\n", "#include <stdbool.h>", "#include <stddef.h>", "#include <stdint.h>\n", '#include "esp_err.h"',
             '#include "film_gfx.h"\n', "#ifdef __cplusplus\nextern \"C\" {\n#endif\n",
             "/*",
             " * 素材在 film_assets_bind() 之前不可用（位图指针为 NULL）。",
             " * 绑定后各结构体只读；素材数据的内存（设备上为 Flash 映射）必须在整个运行期间有效。",
             " */",
             f"#define FILM_ASSETS_LAYOUT_ID 0x{layout_id:08x}u",
             f"#define FILM_ASSETS_SIZE {len(blob.data)}u\n",
             "/** 检查素材数据的版本与布局，并把全部字体、图片指向其中 */",
             "esp_err_t film_assets_bind(const void *data, size_t size);",
             "/** 开机动画（MFB1 格式），未绑定时返回 false */",
             "bool film_assets_boot_anim(const uint8_t **ret_data, size_t *ret_size);\n"]
    lines += [f"extern gfx_font_t font_{r[0]};" for r in font_rows]
    lines.append("")
    lines.append("/* 精灵图；*_OX/_OY 为位图左上角相对元素框左上角的偏移（含阴影外扩） */")
    for n, w, h, px, a, ox, oy in image_rows:
        lines.append(f"extern gfx_image_t img_{n};")
        if ox or oy:
            lines.append(f"#define IMG_{n.upper()}_OX ({ox})")
            lines.append(f"#define IMG_{n.upper()}_OY ({oy})")
    lines.append("\n#ifdef __cplusplus\n}\n#endif")
    (OUT / "include").mkdir(parents=True, exist_ok=True)
    (OUT / "include" / "film_assets.h").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # 字形表与绑定表
    parts = [HEADER, "#include <string.h>\n", '#include "film_assets.h"\n',
             f"#define BLOB_VERSION {BLOB_VERSION}u",
             f"#define NONE 0x{NONE:08x}u\n"]
    for n, glyphs, off, size, asc, desc in font_rows:
        rows = ",\n".join(f"    {{ 0x{cp:x}, {o}, {w}, {h}, {lft}, {top}, {adv} }}"
                          for cp, o, w, h, lft, top, adv in glyphs)
        parts.append(f"static const gfx_glyph_t s_{n}_glyphs[] = {{\n{rows}\n}};")
        parts.append(f"gfx_font_t font_{n} = {{ s_{n}_glyphs, NULL, {len(glyphs)}, {asc}, {desc} }};\n")
    for n, w, h, px, a, ox, oy in image_rows:
        parts.append(f"gfx_image_t img_{n} = {{ NULL, NULL, {w}, {h} }};")
    parts.append("""
typedef struct {
    gfx_font_t *font;
    uint32_t offset;
    uint32_t size;
} font_binding_t;

typedef struct {
    gfx_image_t *image;
    uint32_t pixels;    /*!< RGB565 偏移，NONE 表示没有 */
    uint32_t alpha;     /*!< A8 偏移，NONE 表示不透明 */
} image_binding_t;
""")
    parts.append("static const font_binding_t s_fonts[] = {\n" + ",\n".join(
        f"    {{ &font_{n}, {off}u, {size}u }}" for n, glyphs, off, size, asc, desc in font_rows) + "\n};\n")
    parts.append("static const image_binding_t s_images[] = {\n" + ",\n".join(
        f"    {{ &img_{n}, 0x{px:08x}u, 0x{a:08x}u }}" for n, w, h, px, a, ox, oy in image_rows) + "\n};\n")
    parts.append(f"""static const uint8_t *s_boot;
static size_t s_boot_size;

static uint32_t read_u32(const uint8_t *p)
{{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}}

esp_err_t film_assets_bind(const void *data, size_t size)
{{
    const uint8_t *base = data;
    if (!base || size < FILM_ASSETS_SIZE || memcmp(base, "MFA1", 4) != 0) {{
        return ESP_ERR_NOT_FOUND;
    }}
    if (read_u32(base + 4) != BLOB_VERSION || read_u32(base + 8) != FILM_ASSETS_LAYOUT_ID ||
        read_u32(base + 12) != FILM_ASSETS_SIZE || memcmp(base + FILM_ASSETS_SIZE - 4, "MFAE", 4) != 0) {{
        return ESP_ERR_INVALID_VERSION;
    }}
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); ++i) {{
        s_fonts[i].font->bitmap = base + s_fonts[i].offset;
    }}
    for (size_t i = 0; i < sizeof(s_images) / sizeof(s_images[0]); ++i) {{
        const image_binding_t *b = &s_images[i];
        b->image->pixels = b->pixels == NONE ? NULL : (const uint16_t *)(const void *)(base + b->pixels);
        b->image->alpha = b->alpha == NONE ? NULL : base + b->alpha;
    }}
    s_boot = base + read_u32(base + 16);
    s_boot_size = read_u32(base + 20);
    return ESP_OK;
}}

bool film_assets_boot_anim(const uint8_t **ret_data, size_t *ret_size)
{{
    if (!s_boot || !s_boot_size) {{
        return false;
    }}
    *ret_data = s_boot;
    *ret_size = s_boot_size;
    return true;
}}
""")
    (OUT / "assets_table.c").write_text("\n".join(parts), encoding="utf-8")
    print(f"assets.bin {len(blob.data) / 1024 / 1024:.2f} MB, layout 0x{layout_id:08x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot-video", type=Path, default=DEFAULT_BOOT_VIDEO)
    ap.add_argument("--skip-boot", action="store_true", help="复用上次生成的 boot_anim.bin")
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    fonts = bake_fonts(font_specs())
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        images = collect_images(render_sprites(tmp), textures())
        boot = (OUT / "boot_anim.bin").read_bytes() if args.skip_boot else pack_boot(args.boot_video, tmp)
    write_outputs(fonts, images, boot)
    for old in ("assets_fonts.c", "assets_images.c"):
        (OUT / old).unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
