#!/usr/bin/env python3
"""
生成 STRAY 3200 / SEPIA 100 两款 135 胶卷暗盒图（film_cans/stray.png、film_cans/sepia.png）。

已有的 8 张是离线 3D 渲染（228x232，透明底，带投影）。新款没有 3D 源文件，所以沿用现成渲染的圆柱体：
  1. 抹掉原来的标题/型号/小字（按列用上下两端的空白车身色插值，高光条纹和边缘暗角保留）
  2. 换色：SEPIA 把橙色车身映射到褐色色阶，STRAY 在黑色车身上叠一层暗红
  3. 把新文字排在"展开的圆柱面"上，再按圆柱投影卷回去（边缘字形变窄、基线中间低两边高），
     亮度跟随原文字在这条车身上的光照（高光条纹、边缘暗角）

用法：python3 tools/assets/gen_film_cans.py [--check]
  --check  只把 SILVER 400 的文字重新排一遍输出到 /tmp，用来和原图对比位置、字号、弧度
需要 Pillow 与 numpy（macOS 自带的 python3 没有 PIL，用 /opt/homebrew/bin/python3）。
"""
from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
CANS = HERE / "film_cans"
FONT_PATH = HERE / "fonts" / "Jost.ttf"

# ---- 车身几何（228x232 原图坐标，来自对现有渲染的测量）
CX = 76.0            # 车身中心列
R = 55.0             # 车身半径（像素）
SAG = 0.153          # 基线弧度：距标题上沿每多 1 px，边缘比中间高 SAG px
SAG_Y0 = 78.7
SS = 4               # 文字超采样倍数

# 擦除用的空白车身行：标题上方（MOSAICO 与标题之间）、小字下方
ANCHOR_TOP = (73, 77)
ERASE_Y0, ERASE_Y1 = 76, 171

CREAM = np.array([226.0, 218.0, 204.0])

# 文字版式：(基线中心行, 字高(大写字母高度 px), 字重, 字距(em))
TITLE = (110.0, 25.9, 600, 0.09)
NUMBER = (140.0, 16.0, 600, 0.08)
SUBLINE = (155.7, 4.6, 560, 0.14)
TITLE_MAX_FLAT = 106.0    # 展开宽度上限（对应屏幕上约 92 px）
SUB_FLAT = 96.0          # 小字展开宽度（屏幕上约 88 px）


@dataclass
class Line:
    text: str
    layout: tuple[float, float, int, float]
    color: np.ndarray
    fit_width: float | None = None   # 给定时按展开宽度反推字距（小字用）


# ---------------------------------------------------------------- 文字排版

def _font(weight: int, cap_px: float) -> ImageFont.FreeTypeFont:
    """按大写字母高度选字号（Jost 大写字高约 0.7 em），在超采样尺度下加载"""
    probe = ImageFont.truetype(str(FONT_PATH), 100)
    left, top, right, bottom = probe.getbbox("H")
    cap_per_size = (bottom - top) / 100.0
    font = ImageFont.truetype(str(FONT_PATH), max(1, round(cap_px * SS / cap_per_size)))
    font.set_variation_by_axes([weight])
    return font


def _flat_line(canvas: Image.Image, line: Line) -> None:
    """把一行字画在展开的平面上（居中于 CX，基线在 layout 指定行）"""
    baseline, cap, weight, track = line.layout
    font = _font(weight, cap)
    chars = list(line.text)
    adv = [font.getlength(c) for c in chars]
    gaps = len(chars) - 1
    if line.fit_width is not None:
        spacing = (line.fit_width * SS - sum(adv)) / max(1, gaps)
    else:
        spacing = track * font.size
        width = sum(adv) + spacing * gaps
        if width > TITLE_MAX_FLAT * SS:           # 太宽：收紧字距，仍不够再整体缩小
            spacing = max(0.0, (TITLE_MAX_FLAT * SS - sum(adv)) / max(1, gaps))
    total = sum(adv) + spacing * gaps
    x = CX * SS - total / 2
    draw = ImageDraw.Draw(canvas)
    for c, a in zip(chars, adv):
        draw.text((x, baseline * SS), c, fill=255, font=font, anchor="ls")
        x += a + spacing


def _warp_mask(flat: Image.Image, y0: int, y1: int) -> np.ndarray:
    """把展开平面上的字卷到圆柱面上，返回 (232, 228) 的覆盖度 0..1（已降采样）"""
    src = np.asarray(flat, dtype=np.float32) / 255.0
    h, w = 232 * SS, 228 * SS
    ys = (np.arange(y0 * SS, y1 * SS) + 0.5) / SS
    xs = (np.arange(w) + 0.5) / SS
    X, Y = np.meshgrid(xs, ys)
    t = np.clip((X - CX) / R, -0.995, 0.995)
    u = CX + R * np.arcsin(t)
    yc = Y.copy()
    for _ in range(3):                           # 反解：中心线行 = 屏幕行 + 弧度 * t^2
        yc = Y + SAG * np.maximum(yc - SAG_Y0, 0.0) * t * t
    fx = np.clip(u * SS - 0.5, 0, w - 1.001)
    fy = np.clip(yc * SS - 0.5, 0, h - 1.001)
    x0, y0i = fx.astype(int), fy.astype(int)
    ax, ay = fx - x0, fy - y0i
    val = (src[y0i, x0] * (1 - ax) * (1 - ay) + src[y0i, x0 + 1] * ax * (1 - ay)
           + src[y0i + 1, x0] * (1 - ax) * ay + src[y0i + 1, x0 + 1] * ax * ay)
    val[np.abs((X - CX) / R) > 0.985] = 0        # 车身最边缘不放字
    out = np.zeros((232 * SS, w), dtype=np.float32)
    out[y0 * SS:y1 * SS] = val
    return out.reshape(232, SS, 228, SS).mean(axis=(1, 3))


# ---------------------------------------------------------------- 光照与擦除

def _light_profile(src: np.ndarray, rows: tuple[int, int]) -> np.ndarray:
    """原图文字行里每一列的亮度包络 / 奶油色亮度：高光条纹处 >1，边缘暗角处 <1"""
    lum = src[rows[0]:rows[1], :, :3].mean(axis=2)
    env = lum.max(axis=0)
    k = 5
    padded = np.pad(env, k, mode="edge")
    env = np.max(np.stack([padded[i:i + len(env)] for i in range(2 * k + 1)]), axis=0)
    env = np.convolve(np.pad(env, 3, mode="edge"), np.ones(7) / 7, mode="valid")
    return np.clip(env / CREAM.mean(), 0.25, 1.15)


def _erase(src: np.ndarray, y0: int, y1: int, bottom: tuple[int, int] | None) -> np.ndarray:
    """按列把 [y0, y1] 填成空白车身色：只有上锚点时整列同色，有下锚点时在两端之间线性过渡"""
    out = src.copy()
    top = np.median(src[ANCHOR_TOP[0]:ANCHOR_TOP[1] + 1, :, :3], axis=0)
    if bottom:
        bot = np.median(src[bottom[0]:bottom[1] + 1, :, :3], axis=0)
        y_top = (ANCHOR_TOP[0] + ANCHOR_TOP[1]) / 2
        y_bot = (bottom[0] + bottom[1]) / 2
    for y in range(y0, y1 + 1):
        if bottom:
            f = np.clip((y - y_top) / (y_bot - y_top), 0, 1)
            out[y, :, :3] = top * (1 - f) + bot * f
        else:
            out[y, :, :3] = top
    cols = np.abs((np.arange(228) + 0.5 - CX) / R) > 0.985   # 车身以外的列保持原样
    out[y0:y1 + 1, cols, :] = src[y0:y1 + 1, cols, :]
    return out


def _smoothstep(a: float, b: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------- 合成

def _composite_text(img: np.ndarray, lines: list[Line], profile: np.ndarray, y0: int, y1: int) -> np.ndarray:
    out = img.copy()
    for line in lines:
        flat = Image.new("L", (228 * SS, 232 * SS), 0)
        _flat_line(flat, line)
        cover = _warp_mask(flat, y0, y1)[..., None]
        color = line.color[None, None, :] * profile[None, :, None]
        out[..., :3] = out[..., :3] * (1 - cover) + np.minimum(color, 255) * cover
        out[..., 3] = np.maximum(out[..., 3], cover[..., 0] * 255)
    return out


def _warp_rule(img: np.ndarray, baseline: float, thick: float, color: np.ndarray, profile: np.ndarray,
               half_width: float) -> np.ndarray:
    """沿圆柱面画一条水平细线（随车身弯曲）"""
    flat = Image.new("L", (228 * SS, 232 * SS), 0)
    d = ImageDraw.Draw(flat)
    d.rectangle([(CX - half_width) * SS, (baseline - thick / 2) * SS, (CX + half_width) * SS,
                 (baseline + thick / 2) * SS], fill=255)
    cover = _warp_mask(flat, int(baseline) - 6, int(baseline) + 8)[..., None]
    out = img.copy()
    out[..., :3] = out[..., :3] * (1 - cover) + np.minimum(color[None, None, :] * profile[None, :, None], 255) * cover
    return out


def make_sepia() -> Image.Image:
    src = np.asarray(Image.open(CANS / "faded.png").convert("RGBA"), dtype=np.float32)
    profile = _light_profile(src, (80, 110))
    img = _erase(src, ERASE_Y0, 150, None)          # 橙色车身上下基本同色；色条从 151 行起，保留

    # 橙/黄/褐 -> 褐色色阶（按亮度映射），奶油色文字与金属件饱和度低，不受影响
    rgb = img[..., :3]
    mx, mn = rgb.max(axis=2), rgb.min(axis=2)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1), 0)
    lum = (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]) / 255.0
    stops = np.array([0.0, 0.2, 0.45, 0.65, 0.85, 1.0])
    ramp = np.array([[14, 8, 4], [52, 31, 15], [104, 66, 32], [158, 109, 62], [206, 163, 108], [240, 222, 190]],
                    dtype=np.float32)
    toned = np.stack([np.interp(lum, stops, ramp[:, c]) for c in range(3)], axis=-1)
    w = _smoothstep(0.18, 0.5, sat)[..., None]
    img[..., :3] = rgb * (1 - w) + toned * w

    lines = [
        Line("SEPIA", TITLE, CREAM, None),
        Line("100", NUMBER, CREAM, None),
        Line("135 \u00b7 36 EXP \u00b7 TONED MONOCHROME", SUBLINE, CREAM * 0.9, SUB_FLAT),
    ]
    img = _composite_text(img, lines, profile, 76, 156)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")


def make_stray() -> Image.Image:
    src = np.asarray(Image.open(CANS / "silver.png").convert("RGBA"), dtype=np.float32)
    profile = _light_profile(src, (80, 110))
    img = _erase(src, ERASE_Y0, ERASE_Y1, (172, 186))

    # 黑色车身叠一层暗红（越暗叠得越多，高光条纹保持亮白）；只作用在车身的字区与下部
    rgb = img[..., :3]
    lum = rgb.mean(axis=2) / 255.0
    add = np.array([46.0, 6.0, 8.0])[None, None, :] * ((1 - lum) ** 2)[..., None]
    # 纵向渐入渐出：肩部与底圈的金属件不染色，字区不出现硬边
    rows = np.interp(np.arange(232), [38, 58, 182, 194], [0.0, 1.0, 1.0, 0.0]).astype(np.float32)[:, None, None]
    cols = (np.abs((np.arange(228) + 0.5 - CX) / R) <= 0.99).astype(np.float32)[None, :, None]
    img[..., :3] = rgb + add * rows * cols

    red = np.array([214.0, 62.0, 44.0])
    lines = [
        Line("STRAY", TITLE, CREAM, None),
        Line("3200", NUMBER, red, None),
        Line("135 \u00b7 36 EXP \u00b7 PUSHED B&W", SUBLINE, CREAM * 0.9, SUB_FLAT),
    ]
    img = _composite_text(img, lines, profile, 76, 156)

    # 型号下面两条红线（随圆柱弯曲）代替 SILVER 的银环
    img = _warp_rule(img, 161.0, 1.6, red * 0.9, profile, 47.0)
    img = _warp_rule(img, 166.0, 0.9, red * 0.7, profile, 47.0)

    # 粗颗粒：下半截车身撒一些亮点，暗示"推冲、颗粒粗"
    rng = np.random.default_rng(3200)
    for _ in range(170):
        y = int(rng.integers(171, 188))
        t = rng.uniform(-0.9, 0.9)
        x = int(round(CX + R * t))
        a = rng.uniform(0.25, 0.8)
        tint = np.array([236.0, 226.0, 210.0]) if rng.random() < 0.7 else red
        sh = float(np.clip(profile[x], 0.3, 1.0))
        img[y, x, :3] = img[y, x, :3] * (1 - a) + tint * sh * a
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")


def check_silver() -> None:
    """把 SILVER 400 的文字按同样的版式重新排一遍，输出对比图，用来校准位置、字号与弧度"""
    src = np.asarray(Image.open(CANS / "silver.png").convert("RGBA"), dtype=np.float32)
    profile = _light_profile(src, (80, 110))
    img = _erase(src, ERASE_Y0, ERASE_Y1, (172, 186))
    lines = [
        Line("SILVER", TITLE, CREAM, None),
        Line("400", NUMBER, CREAM, None),
        Line("135 \u00b7 36 EXP \u00b7 BLACK & WHITE", SUBLINE, CREAM * 0.9, SUB_FLAT),
    ]
    img = _composite_text(img, lines, profile, 76, 156)
    mine = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGBA")
    orig = Image.open(CANS / "silver.png").convert("RGBA")
    sheet = Image.new("RGBA", (228 * 6, 232 * 3), (40, 40, 44, 255))
    for i, im in enumerate((orig, mine)):
        big = im.resize((228 * 3, 232 * 3), Image.LANCZOS)
        sheet.alpha_composite(big, (i * 228 * 3, 0))
    sheet.crop((0, 0, 228 * 6, 232 * 3)).convert("RGB").save("/tmp/check_silver.png")
    print("wrote /tmp/check_silver.png (左：原图，右：重排)")


def main() -> None:
    if "--check" in sys.argv:
        check_silver()
        return
    for name, make in (("sepia", make_sepia), ("stray", make_stray)):
        make().save(CANS / f"{name}.png")
        print(f"wrote {CANS / (name + '.png')}")


if __name__ == "__main__":
    main()
