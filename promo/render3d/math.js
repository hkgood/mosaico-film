// 镜头动画用的小工具：区间进度、缓动、低差异序列。
// 所有函数都是纯函数，便于在不同镜头之间复用。

export const clamp01 = (x) => Math.min(1, Math.max(0, x));
/** 把 t 映射到区间 [a, b] 内的 0..1 进度。 */
export const seg = (t, a, b) => clamp01((t - a) / (b - a));
export const lerp = (a, b, k) => a + (b - a) * k;
export const smooth = (k) => k * k * (3 - 2 * k);
export const easeInOutCubic = (k) => (k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2);
export const easeOutCubic = (k) => 1 - Math.pow(1 - k, 3);
export const easeInOutSine = (k) => -(Math.cos(Math.PI * k) - 1) / 2;
export const deg = (d) => (d * Math.PI) / 180;

/** Halton 低差异序列，用于抗锯齿抖动、光圈采样和软阴影。 */
export function halton(index, base) {
    let f = 1;
    let r = 0;
    let i = index;
    while (i > 0) {
        f /= base;
        r += f * (i % base);
        i = Math.floor(i / base);
    }
    return r;
}

/** 可复现的伪随机数（mulberry32），保证每次渲染结果一致。 */
export function rng(seed) {
    let s = seed >>> 0;
    return () => {
        s = (s + 0x6d2b79f5) >>> 0;
        let t = s;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
