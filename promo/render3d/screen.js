// 屏幕合成：真实 UI 截图 + 指尖触摸（按住时一个柔光点，抬起时扩散一圈细环），
// 画到一块画布上作为 3D 屏幕贴图，触摸因此会跟着透视一起变形。
// 同时根据采集里的 key_down / key_up 标记给出 AI 键的按下程度。
import * as THREE from 'three';
import { clipAt, clipIndex, marksUpTo } from './timeline.js';

const UI_SIZE = 480;
const TOUCH_TYPES = ['down', 'move', 'up'];
const RIPPLE_FRAMES = 9;
const KEY_EASE_FRAMES = 2;   // 按键按下/回弹用时（片段帧）

const imageCache = new Map();

function loadImage(url) {
    if (imageCache.has(url)) return imageCache.get(url);
    const promise = new Promise((resolve, reject) => {
        const img = new Image();
        img.onload = () => resolve(img);
        img.onerror = () => reject(new Error(`load failed: ${url}`));
        img.src = url;
    });
    imageCache.set(url, promise);
    // 只留最近的几帧，长镜头不至于把内存吃满
    if (imageCache.size > 12) imageCache.delete(imageCache.keys().next().value);
    return promise;
}

/** 指尖：按住时一个柔和的光点，抬起时扩散一圈细环。 */
function drawTouch(ctx, clip, index) {
    const past = marksUpTo(clip, index, TOUCH_TYPES);
    const last = past[past.length - 1];
    if (!last) return;
    const r = 26;
    if (last.type !== 'up') {
        // CSS 版本的渐变半径是外接正方形对角线的一半（√2·r），这里换算成以 r 为半径的色标
        const g = ctx.createRadialGradient(last.x, last.y, 0, last.x, last.y, r);
        g.addColorStop(0, 'rgba(255,246,232,0.55)');
        g.addColorStop(0.78, 'rgba(255,246,232,0.25)');
        g.addColorStop(1, 'rgba(255,246,232,0)');
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(last.x, last.y, r, 0, Math.PI * 2);
        ctx.fill();
        return;
    }
    const age = index - last.frame;
    if (age > RIPPLE_FRAMES) return;
    const t = age / RIPPLE_FRAMES;
    ctx.strokeStyle = `rgba(241,232,216,${(1 - t) * 0.7})`;
    ctx.lineWidth = 3;
    ctx.beginPath();
    ctx.arc(last.x, last.y, r * (1 + t * 1.4), 0, Math.PI * 2);
    ctx.stroke();
}

/** AI 键按下程度：最近一次 key_down 之后逐渐按下，key_up 之后回弹。 */
function keyPressAt(clip, index) {
    const marks = marksUpTo(clip, index, ['key_down', 'key_up']);
    const last = marks[marks.length - 1];
    if (!last) return 0;
    const k = Math.min(1, (index - last.frame + 1) / KEY_EASE_FRAMES);
    return last.type === 'key_down' ? k : 1 - k;
}

/**
 * 屏幕合成器。compose(sceneId, frame, { touches }) 返回 { texture, color, keyPress }：
 *   color —— 画面平均色（屏幕投到台面上的光）
 */
export function createScreenCompositor() {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = UI_SIZE;
    const ctx = canvas.getContext('2d');
    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    texture.anisotropy = 16;
    const probe = document.createElement('canvas');
    probe.width = probe.height = 8;
    const pctx = probe.getContext('2d', { willReadFrequently: true });

    async function compose(sceneId, frame, { touches = true } = {}) {
        const use = clipAt(sceneId, frame);
        const index = clipIndex(use, frame);
        const img = await loadImage(`/public/clips/${use.clip}/${String(index).padStart(4, '0')}.jpg`);
        ctx.drawImage(img, 0, 0, UI_SIZE, UI_SIZE);
        if (touches) drawTouch(ctx, use.clip, index);
        texture.needsUpdate = true;

        pctx.drawImage(canvas, 0, 0, 8, 8);
        const px = pctx.getImageData(0, 0, 8, 8).data;
        const avg = [0, 0, 0];
        for (let i = 0; i < px.length; i += 4) for (let c = 0; c < 3; c++) avg[c] += px[i + c] / 255 / 64;
        const color = new THREE.Color().setRGB(avg[0], avg[1], avg[2], THREE.SRGBColorSpace);
        return { texture, color, keyPress: keyPressAt(use.clip, index) };
    }

    return { compose };
}
