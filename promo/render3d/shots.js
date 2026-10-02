// 镜头表：宣传片每个场景（timeline.json 的 scenes）对应一个 3D 镜头。
//
// 每个镜头：
//   scene      —— 场景 id：帧数、屏幕上的 UI 片段都从 timeline.json 取，和字幕、配乐对齐
//   prints()   —— 台面成片的摆放（厘米，yaw 为弧度；drop 为落到台面的帧）
//   key        —— 主光方位 { yaw, elev }（度）
//   touches    —— 屏幕上是否画指尖
//   at(f)      —— 第 f 帧（可为小数，用于运动模糊）的状态：
//                 view { position, target, focus, aperture, fov, roll }，以及可选的 keyPress / exposure / device
// 运镜（已确认）：转场灵动，功能镜头稳住看 UI——机身偏离正面不超过 ~16°。
import * as THREE from 'three';
import { deg, easeInOutCubic, easeInOutSine, easeOutCubic, lerp, seg, smooth } from './math.js';
import { clipsOf, clipInfo, sceneFrameOf, sceneOf } from './timeline.js';

/** 屏幕中心（厘米）：机身底面在 y=0。 */
const C = new THREE.Vector3(0, 2.3, 0);
/** AI 键顶面中心与微距机位。 */
const KEY = new THREE.Vector3(1.29, 4.65, 0);
const KEY_MACRO = new THREE.Vector3(5.2, 8.6, 6.8);

const v = (x, y, z) => new THREE.Vector3(x, y, z);

/** 以 target 为中心的球面机位：yaw 从 +Z 往 +X 转，elev 向上抬（度）。 */
export function orbit(target, yawDeg, elevDeg, dist) {
    const yaw = deg(yawDeg);
    const elev = deg(elevDeg);
    return new THREE.Vector3(
        target.x + dist * Math.sin(yaw) * Math.cos(elev),
        target.y + dist * Math.sin(elev),
        target.z + dist * Math.cos(yaw) * Math.cos(elev),
    );
}

/**
 * 常用构图：绕屏幕中心取景。lift 把注视点上移（机身在画面里下移，给上方字幕留位置），
 * shift 把注视点右移（机身在画面里左移）。
 */
function frameDevice({ yaw, elev, dist, lift = 0, shift = 0, aperture = 0.24, fov = 30, roll = 0 }) {
    const target = C.clone().add(v(shift, lift, 0));
    return {
        position: orbit(target, yaw, elev, dist),
        target,
        focus: v(C.x, C.y, 0.6),
        aperture, fov, roll,
    };
}

/** 两个机位按 k 插值（位置、注视点、焦点、光圈、视场）。 */
function mixView(a, b, k) {
    return {
        position: a.position.clone().lerp(b.position, k),
        target: a.target.clone().lerp(b.target, k),
        focus: a.focus.clone().lerp(b.focus, k),
        aperture: lerp(a.aperture, b.aperture, k),
        fov: lerp(a.fov, b.fov, k),
        roll: lerp(a.roll || 0, b.roll || 0, k),
    };
}

/** 背景里散放的几张成片：虚化成暖色块，给台面纵深。 */
const BACKDROP = [
    { name: 'neon', x: -9, z: -8, yaw: -0.18, width: 8 },
    { name: 'tokyo', x: 8, z: -10, yaw: 0.4, width: 8 },
    { name: 'sx_cafe', x: -2, z: -15, yaw: 0.15, width: 7 },
];

// ---------- ① 开机：黑暗中亮起，从背面绕到正面 ----------
function bootShot() {
    const path = new THREE.CatmullRomCurve3([
        orbit(C, 158, 4, 29), orbit(C, 96, 8, 26), orbit(C, 36, 10, 23), orbit(C, 14, 8, 21),
    ], false, 'centripetal');
    const end = frameDevice({ yaw: 14, elev: 8, dist: 21 });
    return {
        scene: 'boot',
        touches: false,
        prints: () => [{ name: 'films_0', x: -6.5, z: 7, yaw: 0.32, width: 8 }, { name: 'sx_beach', x: 6.5, z: 9, yaw: -0.22, width: 7 }, ...BACKDROP],
        at(f) {
            const k = easeInOutCubic(seg(f, 0, 58));
            const push = easeOutCubic(seg(f, 58, 90));
            const position = path.getPointAt(k).lerp(C, 0.06 * push);
            return {
                view: { ...end, position, roll: deg(2) * Math.sin(Math.PI * k) },
                exposure: smooth(seg(f, 0, 36)),
            };
        },
    };
}

// ---------- ② 换卷：机身在上，成片随每次“咔哒”落到前方台面 ----------
const FILM_PRINTS = ['films_0', 'films_1', 'films_2', 'films_3', 'films_4', 'films_5', 'films_6', 'films_7'];

/** 换卷片段里每个 detent 声音对应的场景帧：第 i 张成片落下的时刻（第 0 张在开场）。 */
function detentFrames() {
    const [use] = clipsOf('films');
    return clipInfo(use.clip).marks
        .filter((m) => m.type === 'sfx' && m.name === 'detent')
        .map((m) => sceneFrameOf(use, m.frame));
}

function filmsShot() {
    // 构图对齐 2D 版：机身在上（约画面 38% 高度），成片堆在下方，最底下留给胶卷名。
    // 绕着靠近成片堆的点转，横移时成片堆基本不动、不会被画面边缘切掉
    const target = v(0, 2.0, 5.4);
    const view = (yaw) => ({
        position: orbit(target, yaw, 27, 26),
        target,
        focus: v(0, 1.3, 3.4),
        aperture: 0.1, fov: 30, roll: 0,
    });
    return {
        scene: 'films',
        prints() {
            const lands = [0, ...detentFrames()];
            return FILM_PRINTS.map((name, i) => ({
                name, width: 6.4, drop: lands[i], lift: i * 0.015,
                x: Math.sin(i * 2.1) * 0.35, z: 7.6 + Math.cos(i * 1.7) * 0.25, yaw: Math.sin(i * 1.3) * 0.1,
            }));
        },
        at(f) {
            // 每次换卷，机位向右轻推一小步（出去不回来），节奏和咔哒声一致
            const steps = detentFrames().reduce((sum, d) => sum + easeOutCubic(seg(f, d, d + 10)), 0);
            return { view: view(-5 + 10 * (steps / FILM_PRINTS.length)) };
        },
    };
}

// ---------- ③ 红键：3/4 角一口气推到 AI 键，按下；卡点切到正面看快门 ----------
const PRESS = 26;   // 与 2D 版、配乐里的按键声一致
function shutterShot() {
    const cut = clipsOf('shutter')[0].at;
    const macroStart = {
        position: orbit(C, 34, 16, 19), target: C.clone(), focus: C.clone(), aperture: 0.26, fov: 30, roll: 0,
    };
    const macroEnd = {
        position: KEY_MACRO, target: v(1.15, 4.4, 0.1), focus: KEY.clone(), aperture: 0.13, fov: 25, roll: deg(-2),
    };
    return {
        scene: 'shutter',
        prints: () => [{ name: 'films_0', x: -6.5, z: 6.5, yaw: 0.32, width: 8 }, ...BACKDROP],
        at(f) {
            if (f < cut) {
                const k = easeInOutCubic(seg(f, 0, PRESS - 2));
                return {
                    view: mixView(macroStart, macroEnd, k),
                    keyPress: easeOutCubic(seg(f, PRESS, PRESS + 3)),
                };
            }
            const push = easeOutCubic(seg(f, cut, 90));
            return { view: frameDevice({ yaw: lerp(9, 5, push), elev: 6, dist: lerp(20.5, 19, push), lift: -0.15 }) };
        },
    };
}

// ---------- ④ 两台机身：机位从左侧横穿到右侧，正好在换机身时经过正面 ----------
function bodiesShot() {
    return {
        scene: 'bodies',
        prints: () => BACKDROP,
        at(f) {
            const k = easeInOutCubic(seg(f, 0, 90));
            return { view: frameDevice({ yaw: lerp(-16, 16, k), elev: lerp(9, 6, k), dist: 20.5, lift: -0.15 }) };
        },
    };
}

// ---------- ⑤ 显影：贴着台面的低机位横移，前景拍立得掠过 ----------
function developShot() {
    return {
        scene: 'develop',
        key: { yaw: -30, elev: 34 },
        prints: () => [
            { name: 'sx_beach', x: 4.5, z: 8.5, yaw: -0.3, width: 7 },
            { name: 'sx_sunflower', x: -4.5, z: 9.5, yaw: 0.25, width: 7 },
            ...BACKDROP,
        ],
        at(f) {
            const k = easeInOutSine(seg(f, 0, 90));
            return { view: frameDevice({ yaw: lerp(13, -9, k), elev: lerp(3, 6, k), dist: lerp(20, 18.6, k), lift: -0.15, aperture: 0.3 }) };
        },
    };
}
// ---------- ⑥ 重新冲洗：稳住的正面慢推 + 微微环绕 ----------
function redevelopShot() {
    return {
        scene: 'redevelop',
        prints: () => [{ name: 'tokyo', x: 6.5, z: 8.5, yaw: -0.2, width: 8 }, ...BACKDROP.slice(0, 1)],
        at(f) {
            const k = easeInOutCubic(seg(f, 0, 90));
            return { view: frameDevice({ yaw: lerp(4, -5, k), elev: lerp(5, 8, k), dist: lerp(20, 18.8, k), lift: -0.15 }) };
        },
    };
}

// ---------- ⑦ 发送到手机：机身退到画面左侧并转向右边的手机 ----------
function shareShot() {
    const phoneIn = 40;   // 与 Lab.tsx 的 PHONE_IN 一致
    return {
        scene: 'share',
        prints: () => BACKDROP,
        at(f) {
            const k = easeInOutCubic(seg(f, phoneIn - 6, phoneIn + 14));
            return {
                // 终点：机身退到画面左侧约 1/3 处，给右侧升起的手机让出位置
                view: frameDevice({
                    yaw: lerp(2, -12, k), elev: lerp(6, 9, k), dist: lerp(20.5, 24, k),
                    lift: lerp(-0.15, 0.5, k), shift: lerp(0, 1.45, k),
                }),
            };
        },
    };
}

// ---------- ⑧ 数字：俯拍台面上铺开的一整版成片，缓缓漂移 ----------
function numbersShot() {
    return {
        scene: 'numbers',
        device: false,
        key: { yaw: -30, elev: 62 },
        prints() {
            const names = ['films_0', 'sunflower', 'arles', 'tokyo', 'beach', 'neon', 'films_3', 'films_6', 'arles_bw',
                           'films_1', 'films_5', 'films_7', 'films_2', 'films_4', 'shared_0', 'shared_2', 'films_0', 'tokyo'];
            const cols = 3;
            const w = 8;
            const gap = 0.7;
            return names.map((name, i) => ({
                name, width: w, curl: 0.05,
                x: ((i % cols) - 1) * (w + gap),
                z: (Math.floor(i / cols) - 2.5) * (w * 0.75 + gap * 2.2),
                yaw: Math.sin(i * 2.7) * 0.03,
            }));
        },
        at(f) {
            const drift = lerp(-6, 6, f / 90);
            const target = v(0, 0, drift);
            return {
                view: {
                    position: v(3, 36, drift + 9), target, focus: target.clone(), aperture: 0.2, fov: 30, roll: deg(-6),
                },
            };
        },
    };
}

// ---------- ⑨ 收尾：从低处升到 3/4 英雄角，片名浮现，灯灭 ----------
function finaleShot() {
    // 终点：机身在画面中下部，上方留给片名
    const from = frameDevice({ yaw: -24, elev: 2, dist: 21, lift: 0.6 });
    const to = frameDevice({ yaw: 28, elev: 13, dist: 21, lift: 1.5, aperture: 0.3 });
    return {
        scene: 'finale',
        touches: false,
        prints: () => [
            { name: 'films_0', x: -6.5, z: 6.5, yaw: 0.32, width: 8 },
            { name: 'sx_beach', x: 5.8, z: 8.5, yaw: -0.22, width: 7 },
            ...BACKDROP,
        ],
        at(f) {
            const k = easeInOutCubic(seg(f, 0, 64));
            const view = mixView(from, to, k);
            // 升起时走一段弧线：中途机位更高一点，再落回英雄角
            view.position.y += 2.5 * Math.sin(Math.PI * k);
            view.position.lerp(to.target, 0.04 * easeOutCubic(seg(f, 64, 90)));
            return { view };
        },
    };
}

/** 所有镜头，键为场景 id。frames 取自 timeline.json，需在 loadTimeline() 之后调用。 */
export function buildShots() {
    const shots = {
        boot: bootShot(),
        films: filmsShot(),
        shutter: shutterShot(),
        bodies: bodiesShot(),
        develop: developShot(),
        redevelop: redevelopShot(),
        share: shareShot(),
        numbers: numbersShot(),
        finale: finaleShot(),
    };
    for (const [id, shot] of Object.entries(shots)) shot.frames = sceneOf(id).duration;
    return shots;
}
