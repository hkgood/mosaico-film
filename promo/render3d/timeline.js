// 渲染端的时间轴：与 Remotion（src/lib/timeline.ts、src/lib/clip.tsx）读同一份 timeline.json 和 manifest.json，
// 保证 3D 屏幕上播放的 UI 帧、触摸、按键与 2D 字幕和配乐逐帧对齐。

let timeline;
let manifest;

/** 启动时加载一次（服务根目录为 promo/）。 */
export async function loadTimeline() {
    timeline = await (await fetch('/src/timeline.json')).json();
    manifest = await (await fetch('/public/manifest.json')).json();
    return { timeline, manifest };
}

/** 每拍帧数（120 BPM、30 fps 时为 15）。 */
export const beat = () => (60 / timeline.bpm) * timeline.fps;

export const sceneOf = (id) => timeline.scenes[id];

export const clipInfo = (clip) => {
    const info = manifest.clips[clip];
    if (!info) throw new Error(`clip ${clip} is missing; run tools/promo/export_assets.py`);
    return info;
};

/** 场景的片段摆放（与 clipsOf 相同）。 */
export const clipsOf = (id) => timeline.clips.filter((c) => c.scene === id);

/** 场景第 frame 帧生效的片段摆放：最后一个已开始的。 */
export function clipAt(id, frame) {
    const uses = clipsOf(id);
    if (uses.length === 0) return null;
    return uses.filter((u) => u.at <= frame).pop() ?? uses[0];
}

/** 摆放 use 在场景第 frame 帧显示的片段帧号（片段外保持首/末帧）。 */
export function clipIndex(use, frame) {
    const raw = Math.floor(use.src + Math.max(0, frame - use.at) * use.rate);
    return Math.min(clipInfo(use.clip).frames - 1, Math.max(0, raw));
}

/** 片段中帧号 ≤ index、类型属于 types 的标记，按时间先后。 */
export const marksUpTo = (clip, index, types) =>
    clipInfo(clip).marks.filter((m) => m.frame <= index && types.includes(m.type));

/** 片段帧号 → 场景帧（用于把采集里的声音/触摸标记换算到镜头时间上）。 */
export const sceneFrameOf = (use, clipFrame) => use.at + (clipFrame - use.src) / use.rate;

export const printInfo = (name) => manifest.prints.find((p) => p.name === name);
