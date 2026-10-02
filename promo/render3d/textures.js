// 程序化贴图：机身侧面丝印、顶/底面开孔、背板印刷、按键纹理、工作台表面等。
//
// 机身相关贴图都以“毫米”为单位绘制（PX_PER_MM 像素/毫米），尺寸取自官方 V1.2
// 五视图（以 2.54 mm 排针间距为比例尺量出），见 mosaico_model.js 顶部的尺寸表。
import * as THREE from 'three';
import { rng } from './math.js';

export const PX_PER_MM = 48;
const FONT = 'Jost, Helvetica, Arial, sans-serif';

/** 新建画布，返回 { canvas, ctx }，ctx 已按毫米缩放。 */
function mmCanvas(wMm, hMm) {
    const canvas = document.createElement('canvas');
    canvas.width = Math.round(wMm * PX_PER_MM);
    canvas.height = Math.round(hMm * PX_PER_MM);
    const ctx = canvas.getContext('2d');
    ctx.scale(PX_PER_MM, PX_PER_MM);
    return { canvas, ctx };
}

function toTexture(canvas, { srgb = true, repeat = false } = {}) {
    const tex = new THREE.CanvasTexture(canvas);
    tex.colorSpace = srgb ? THREE.SRGBColorSpace : THREE.NoColorSpace;
    tex.anisotropy = 8;
    if (repeat) tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    return tex;
}

function roundRect(ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.roundRect(x, y, w, h, r);
}

// ---------- 侧面：2×10 排针孔 + 引脚丝印条 ----------

// 引脚丝印：每行 [靠后列, 靠前列]（左侧面）或 [靠前列, 靠后列]（右侧面），
// 与官方五视图中“从该侧看过去”的左右顺序一致。
// 单元格写法：'53' 普通；{ t, kind: 'power' | 'dark', span } 着色或跨行。
const PIN_LABELS = {
    left: [
        ['53', '55'], ['48', '19'], ['13', '18'], ['12', '17'], ['14', '16'],
        ['4', '15'], ['1', { t: 'USJ', span: 2 }], ['0', null],
        [{ t: '5V', kind: 'power' }, { t: 'IN', kind: 'dark' }],
        [{ t: 'GND', kind: 'dark' }, { t: '3V', kind: 'power' }],
    ],
    right: [
        [{ t: '3V', kind: 'power' }, { t: 'GND', kind: 'dark' }],
        [{ t: 'IN', kind: 'dark' }, { t: '5V', kind: 'power' }],
        [{ t: 'UART', span: 2 }, '0'], [null, '1'],
        ['38', '5'], [{ t: 'CODEC', span: 5 }, '39'], [null, '10'], [null, '11'], [null, '47'], [null, '46'],
    ],
};

/**
 * 侧面贴花（透明底）：画布覆盖整个侧面 depth × height，横向为“从该侧看过去的右手方向”。
 * layout：{ depth, height, holesU: [u1,u2], stripU: [u0,u1], pitch, stripH }，单位 mm。
 */
export function sideDecal(side, layout) {
    const { depth, height, holesU, stripU, pitch, stripH } = layout;
    const { canvas, ctx } = mmCanvas(depth, height);
    const cy = height / 2;

    // 排针孔：深色方孔，带一圈很淡的内阴影
    const hole = 0.92;
    for (let row = 0; row < 10; row++) {
        const y = cy + (row - 4.5) * pitch;
        for (const u of holesU) {
            ctx.fillStyle = '#0d0d0d';
            roundRect(ctx, u - hole / 2, y - hole / 2, hole, hole, 0.08);
            ctx.fill();
            ctx.strokeStyle = 'rgba(0,0,0,0.25)';
            ctx.lineWidth = 0.12;
            ctx.stroke();
        }
    }

    // 丝印条：2 列 × 10 行，文字旋转 90° 竖排
    const [u0, u1] = stripU;
    const colW = (u1 - u0) / 2;
    const rowH = stripH / 10;
    const top = cy - stripH / 2;
    ctx.fillStyle = '#fbfbfa';
    ctx.fillRect(u0, top, u1 - u0, stripH);
    const rows = PIN_LABELS[side];
    rows.forEach((cells, row) => {
        cells.forEach((cell, col) => {
            if (cell === null) return;   // 被上方跨行单元格覆盖
            const c = typeof cell === 'string' ? { t: cell } : cell;
            const span = c.span || 1;
            const x = u0 + col * colW;
            const y = top + row * rowH;
            const h = rowH * span;
            if (c.kind) {
                ctx.fillStyle = c.kind === 'power' ? '#ee5a24' : '#2e2e2e';
                ctx.fillRect(x, y, colW, h);
            }
            ctx.strokeStyle = '#3b3b3b';
            ctx.lineWidth = 0.09;
            ctx.strokeRect(x, y, colW, h);
            ctx.save();
            ctx.translate(x + colW / 2, y + h / 2);
            ctx.rotate(Math.PI / 2);
            ctx.fillStyle = c.kind ? '#ffffff' : '#2a2a2a';
            ctx.font = `500 ${c.t.length > 3 ? 1.15 : 1.25}px ${FONT}`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(c.t, 0, 0.05);
            ctx.restore();
        });
    });
    ctx.strokeStyle = '#3b3b3b';
    ctx.lineWidth = 0.14;
    ctx.strokeRect(u0, top, u1 - u0, stripH);
    return toTexture(canvas);
}

// ---------- 顶面 / 底面开孔 ----------

/** 顶面贴花：7 条扬声器出音孔。画布覆盖 width × depth，上方为机身背面。 */
export function topDecal({ width, depth, slots }) {
    const { canvas, ctx } = mmCanvas(width, depth);
    const { count, pitch, centerX, w, h } = slots;
    for (let i = 0; i < count; i++) {
        const x = width / 2 + centerX + (i - (count - 1) / 2) * pitch;
        const grad = ctx.createLinearGradient(0, depth / 2 - h / 2, 0, depth / 2 + h / 2);
        grad.addColorStop(0, '#050505');
        grad.addColorStop(1, '#1c1c1c');
        ctx.fillStyle = grad;
        roundRect(ctx, x - w / 2, depth / 2 - h / 2, w, h, 0.18);
        ctx.fill();
    }
    return toTexture(canvas);
}

/** 底面贴花：麦克风孔 + 充电指示灯导光点。画布上方为机身正面。 */
export function bottomDecal({ width, depth, dots }) {
    const { canvas, ctx } = mmCanvas(width, depth);
    const y = dots.fromFront;
    dots.xs.forEach((x, i) => {
        const size = 0.85;
        ctx.fillStyle = i === 0 ? '#0b0b0b' : '#d4d4d2';   // 第一个是麦克风孔
        roundRect(ctx, width / 2 + x - size / 2, y - size / 2, size, size, 0.12);
        ctx.fill();
    });
    return toTexture(canvas);
}

// ---------- 背板印刷 ----------

/** 伪二维码图案（仅作装饰，不编码任何内容）：三个定位角 + 随机模块。 */
function drawQr(ctx, x, y, size) {
    const n = 25;
    const m = size / n;
    const rand = rng(20260);
    ctx.fillStyle = '#f4f4f2';
    ctx.fillRect(x - m, y - m, size + 2 * m, size + 2 * m);
    ctx.fillStyle = '#121212';
    const finder = (fx, fy) => {
        ctx.fillRect(x + fx * m, y + fy * m, 7 * m, 7 * m);
        ctx.fillStyle = '#f4f4f2';
        ctx.fillRect(x + (fx + 1) * m, y + (fy + 1) * m, 5 * m, 5 * m);
        ctx.fillStyle = '#121212';
        ctx.fillRect(x + (fx + 2) * m, y + (fy + 2) * m, 3 * m, 3 * m);
    };
    const inFinder = (i, j) => (i < 8 && j < 8) || (i >= n - 8 && j < 8) || (i < 8 && j >= n - 8);
    for (let j = 0; j < n; j++) {
        for (let i = 0; i < n; i++) {
            if (!inFinder(i, j) && rand() < 0.48) ctx.fillRect(x + i * m, y + j * m, m * 1.02, m * 1.02);
        }
    }
    finder(0, 0);
    finder(n - 7, 0);
    finder(0, n - 7);
}

/** Mosaico 标志的简化版：红色圆饼，左上缺一角并留出竖条。 */
function drawLogoMark(ctx, cx, cy, r) {
    ctx.save();
    ctx.fillStyle = '#e8402a';
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fill();
    ctx.globalCompositeOperation = 'destination-out';
    const gap = r * 0.16;
    ctx.fillRect(cx - r, cy - r, r - gap / 2, r - gap / 2);           // 左上缺角
    ctx.fillRect(cx - r, cy - gap / 2, 2 * r, gap);                    // 横向缝
    ctx.fillRect(cx - gap / 2 - r * 0.38, cy - r, gap, r);             // 竖向缝
    ctx.restore();
    ctx.fillStyle = '#e8402a';
    ctx.fillRect(cx - r * 0.38 + gap / 2, cy - r * 0.92, r * 0.38 - gap, r * 0.92 - gap / 2);
}

/**
 * 背板印刷：标志、二维码、7 个金手指焊盘的丝印文字（焊盘本身是独立的金属网格）。
 * 画布按“从背面看过去”绘制。pads：{ labels, pitch, y, w, h }。
 */
export function backPrint({ width, height, pads }) {
    const { canvas, ctx } = mmCanvas(width, height);
    ctx.fillStyle = '#141414';
    ctx.fillRect(0, 0, width, height);

    // 标志 + 文字
    const logoY = height * 0.2;
    ctx.font = `600 2.6px ${FONT}`;
    ctx.textBaseline = 'middle';
    const title = 'ESP-MOSAICO';
    const tw = ctx.measureText(title).width;
    const markR = 1.25;
    const x0 = width / 2 - (tw + markR * 2 + 0.9) / 2;
    drawLogoMark(ctx, x0 + markR, logoY, markR);
    ctx.fillStyle = '#e8402a';
    ctx.fillText('ESP', x0 + markR * 2 + 0.9, logoY);
    const espW = ctx.measureText('ESP').width;
    ctx.fillStyle = '#f2f2f0';
    ctx.fillText('-MOSAICO', x0 + markR * 2 + 0.9 + espW, logoY);
    ctx.font = `500 1.15px ${FONT}`;
    ctx.fillStyle = '#d8d8d6';
    ctx.fillText('Powered by Espressif', x0 + markR * 2 + 0.9 + 0.15, logoY + 2.2);

    // 二维码
    const qr = 8.4;
    drawQr(ctx, width / 2 - qr / 2, height * 0.43 - qr / 2, qr);

    // 焊盘丝印
    ctx.font = `500 1.05px ${FONT}`;
    ctx.fillStyle = '#e6e6e4';
    ctx.textAlign = 'center';
    pads.labels.forEach((label, i) => {
        const x = width / 2 + (i - (pads.labels.length - 1) / 2) * pads.pitch;
        ctx.fillText(label, x, pads.y + pads.h / 2 + 1.5);
    });
    return toTexture(canvas);
}

// ---------- 按键 / 拨动开关的竖纹 ----------

/** 竖向条纹凹凸贴图：用于 AI 键和 BOOT / ON-OFF 拨动开关的防滑纹。 */
export function ribBump(stripes) {
    const canvas = document.createElement('canvas');
    canvas.width = 512;
    canvas.height = 64;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, 512, 64);
    for (let i = 1; i < stripes; i++) {
        const x = (i / stripes) * 512;
        const g = ctx.createLinearGradient(x - 9, 0, x + 9, 0);
        g.addColorStop(0, '#ffffff');
        g.addColorStop(0.5, '#000000');
        g.addColorStop(1, '#ffffff');
        ctx.fillStyle = g;
        ctx.fillRect(x - 9, 0, 18, 64);
    }
    return toTexture(canvas, { srgb: false });
}

// ---------- 场景 ----------

/** 暗房工作台：深色哑光台面，带细微颗粒和使用痕迹。返回 { map, roughness }。 */
export function benchSurface() {
    const size = 1024;
    const make = (fn) => {
        const canvas = document.createElement('canvas');
        canvas.width = canvas.height = size;
        const ctx = canvas.getContext('2d');
        fn(ctx);
        return canvas;
    };
    const rand = rng(77);
    const albedo = make((ctx) => {
        ctx.fillStyle = '#16120f';
        ctx.fillRect(0, 0, size, size);
        // 大块色斑：台面不是完全均匀的
        for (let i = 0; i < 60; i++) {
            const x = rand() * size;
            const y = rand() * size;
            const r = 60 + rand() * 220;
            const g = ctx.createRadialGradient(x, y, 0, x, y, r);
            const tone = rand() < 0.5 ? '30,24,19' : '10,8,7';
            g.addColorStop(0, `rgba(${tone},0.35)`);
            g.addColorStop(1, `rgba(${tone},0)`);
            ctx.fillStyle = g;
            ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
        }
        // 细颗粒
        const img = ctx.getImageData(0, 0, size, size);
        for (let i = 0; i < img.data.length; i += 4) {
            const n = (rand() - 0.5) * 7;
            img.data[i] += n;
            img.data[i + 1] += n;
            img.data[i + 2] += n;
        }
        ctx.putImageData(img, 0, 0);
    });
    // 粗糙度：整体哑光，叠一层柔和的斑驳变化（不画划痕：放大后会像裂缝）
    const rough = make((ctx) => {
        ctx.fillStyle = '#a8a8a8';
        ctx.fillRect(0, 0, size, size);
        for (let i = 0; i < 80; i++) {
            const x = rand() * size;
            const y = rand() * size;
            const r = 40 + rand() * 160;
            const g = ctx.createRadialGradient(x, y, 0, x, y, r);
            g.addColorStop(0, 'rgba(120,120,120,0.35)');
            g.addColorStop(1, 'rgba(120,120,120,0)');
            ctx.fillStyle = g;
            ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
        }
    });
    const map = toTexture(albedo, { repeat: true });
    const roughness = toTexture(rough, { srgb: false, repeat: true });
    return { map, roughness };
}

/** 径向光斑：中心实、边缘柔，用于安全灯光晕和背景散景。 */
export function radialGlow(hard = 0.0) {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 256;
    const ctx = canvas.getContext('2d');
    const g = ctx.createRadialGradient(128, 128, 0, 128, 128, 128);
    g.addColorStop(0, 'rgba(255,255,255,1)');
    g.addColorStop(Math.max(0.01, hard), 'rgba(255,255,255,1)');
    g.addColorStop(1, 'rgba(255,255,255,0)');
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, 256, 256);
    return toTexture(canvas, { srgb: false });
}

/** 接触阴影：机身底部贴地的柔和暗区（圆角矩形高斯模糊），让机身“落地”。 */
export function contactShadow() {
    const canvas = document.createElement('canvas');
    canvas.width = 512;
    canvas.height = 256;
    const ctx = canvas.getContext('2d');
    ctx.filter = 'blur(22px)';
    ctx.fillStyle = 'rgba(0,0,0,1)';
    ctx.beginPath();
    ctx.roundRect(96, 92, 320, 72, 30);
    ctx.fill();
    return toTexture(canvas, { srgb: false });
}
