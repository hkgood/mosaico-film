// 暗房场景：工作台、红色安全灯、暖色主光、平放的相纸成片，以及屏幕投到台面上的光。
// 世界单位为厘米；机身放在原点，屏幕朝 +Z。
import * as THREE from 'three';
import { RectAreaLightUniformsLib } from 'three/addons/lights/RectAreaLightUniformsLib.js';
import * as TEX from './textures.js';
import { easeOutCubic, rng } from './math.js';

const SAFELIGHT = 0xff2a12;
const DROP_FRAMES = 9;     // 相纸下落用时（帧）
const DROP_HEIGHT = 2.2;   // 下落起点高度（厘米）
const DROP_SLIDE = 4;      // 下落起点向镜头方向的偏移（厘米）
const KEY_COLOR = 0xffeedd;

/**
 * 环境反射贴图：暗房间 + 暖色柔光箱 + 红色安全灯 + 顶部细长灯条。
 * 玻璃与亮面塑料主要反射这些光板，用 PMREM 预过滤，只生成一次。
 */
function darkroomEnvironment(renderer) {
    const env = new THREE.Scene();
    env.add(new THREE.Mesh(new THREE.BoxGeometry(60, 40, 60),
                           new THREE.MeshBasicMaterial({ color: 0x070504, side: THREE.BackSide })));
    const panel = (w, h, color, strength, pos) => {
        const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h),
                                 new THREE.MeshBasicMaterial({ color: new THREE.Color(color).multiplyScalar(strength),
                                                               side: THREE.DoubleSide }));
        m.position.set(...pos);
        m.lookAt(0, 0, 0);
        env.add(m);
    };
    panel(16, 12, KEY_COLOR, 3.2, [-14, 16, 16]);      // 主光柔光箱（左前上）
    panel(30, 1.6, 0xfff1e0, 2.2, [4, 19, 4]);          // 顶部细长灯条：玻璃上的一道高光
    panel(5, 3, SAFELIGHT, 2.0, [-18, 12, -18]);        // 安全灯（左后）：小面积，只在亮面上留一点红
    panel(10, 7, 0xf4f2ee, 1.6, [20, 10, 18]);          // 右前方中性柔光
    panel(40, 30, 0x1a1512, 0.5, [0, -16, 0]);          // 台面反光
    // 观众身后上方的大块渐变柔光：正面玻璃上一层很淡的上亮下暗光泽，不压 UI 对比度
    const sheen = document.createElement('canvas');
    sheen.width = 4;
    sheen.height = 256;
    const sctx = sheen.getContext('2d');
    const grad = sctx.createLinearGradient(0, 0, 0, 256);
    grad.addColorStop(0, '#ffffff');
    grad.addColorStop(0.55, '#3a3a3a');
    grad.addColorStop(1, '#000000');
    sctx.fillStyle = grad;
    sctx.fillRect(0, 0, 4, 256);
    const sheenTex = new THREE.CanvasTexture(sheen);
    const sheenPanel = new THREE.Mesh(new THREE.PlaneGeometry(34, 22),
                                      new THREE.MeshBasicMaterial({ map: sheenTex, color: new THREE.Color(1.3, 1.25, 1.2),
                                                                    side: THREE.DoubleSide }));
    sheenPanel.position.set(-4, 10, 24);
    sheenPanel.lookAt(0, 0, 0);
    env.add(sheenPanel);
    const pmrem = new THREE.PMREMGenerator(renderer);
    const tex = pmrem.fromScene(env, 0.02).texture;
    pmrem.dispose();
    return tex;
}

/** 给 35mm 成片加一圈相纸白边；拍立得成片自带相框，原样使用。 */
function paperCanvas(img, instant) {
    if (instant) return img;
    const border = Math.round(img.width * 0.045);
    const canvas = document.createElement('canvas');
    canvas.width = img.width + border * 2;
    canvas.height = img.height + border * 2;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#f3ede2';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(img, border, border);
    return canvas;
}

function loadImage(url) {
    return new Promise((resolve, reject) => {
        const img = new Image();
        img.onload = () => resolve(img);
        img.onerror = () => reject(new Error(`load failed: ${url}`));
        img.src = url;
    });
}

/**
 * 一张平放在台面上的相纸：轻微卷边（两侧略翘），接收阴影。
 * spec：{ name, x, z, yaw(弧度), width(厘米), curl(厘米), lift }
 */
async function createPrint(spec, manifest) {
    const info = manifest.find((p) => p.name === spec.name);
    const img = await loadImage(`/public/prints/${spec.name}.jpg`);
    const canvas = paperCanvas(img, info && info.instant);
    const tex = new THREE.CanvasTexture(canvas);
    tex.colorSpace = THREE.SRGBColorSpace;
    tex.anisotropy = 8;

    const w = spec.width;
    const h = (w * canvas.height) / canvas.width;
    const geo = new THREE.PlaneGeometry(w, h, 32, 8);
    const pos = geo.attributes.position;
    const curl = spec.curl ?? 0.12;
    for (let i = 0; i < pos.count; i++) {
        const u = pos.getX(i) / (w / 2);
        pos.setZ(i, curl * u * u);   // 平面局部 Z 在放平后朝上
    }
    geo.computeVertexNormals();
    const mat = new THREE.MeshPhysicalMaterial({
        map: tex, roughness: 0.38, clearcoat: 0.35, clearcoatRoughness: 0.3, side: THREE.DoubleSide,
    });
    const mesh = new THREE.Mesh(geo, mat);
    mesh.rotation.order = 'YXZ';
    mesh.rotation.set(-Math.PI / 2, spec.yaw || 0, 0);
    mesh.position.set(spec.x, 0.02 + (spec.lift || 0), spec.z);
    mesh.castShadow = true;
    mesh.receiveShadow = true;
    return mesh;
}

/**
 * 搭建暗房。返回：
 *   key            —— 主光（每个子采样在柔光面上抖动，得到软阴影）
 *   screenLight    —— 屏幕投射到台面的面光源，颜色随画面平均色变化
 *   setPrints(list) —— 换一组台面成片（按镜头切换）
 */
export async function createDarkroom(scene, renderer) {
    RectAreaLightUniformsLib.init();
    scene.environment = darkroomEnvironment(renderer);
    scene.environmentIntensity = 0.55;
    scene.background = new THREE.Color(0x000000);
    scene.fog = new THREE.FogExp2(0x000000, 0.0065);
    const manifest = (await (await fetch('/public/manifest.json')).json()).prints;

    // 工作台：深色哑光台面
    const surface = TEX.benchSurface();
    for (const t of [surface.map, surface.roughness]) t.repeat.set(4, 4);
    const bench = new THREE.Mesh(new THREE.PlaneGeometry(240, 240),
                                 new THREE.MeshPhysicalMaterial({
                                     // 哑光、低镜面：否则掠射角下安全灯的红色高光会铺满整张台面
                                     map: surface.map, roughnessMap: surface.roughness, roughness: 0.85,
                                     specularIntensity: 0.3,
                                 }));
    bench.rotation.x = -Math.PI / 2;
    bench.position.z = -60;
    bench.receiveShadow = true;
    scene.add(bench);

    // 接触阴影：机身底部贴地处的柔和暗区
    const contact = new THREE.Mesh(new THREE.PlaneGeometry(7.4, 4.3),
                                   new THREE.MeshBasicMaterial({
                                       map: TEX.contactShadow(), color: 0x000000, transparent: true,
                                       opacity: 0.75, depthWrite: false,
                                   }));
    contact.rotation.x = -Math.PI / 2;
    contact.position.y = 0.005;
    scene.add(contact);

    // 主光：暖色聚光，位置随镜头设定（见 placeKey），userData.soft 为柔光面半径
    const key = new THREE.SpotLight(KEY_COLOR, 9000, 0, 0.42, 0.85, 2);
    key.target.position.set(0, 2.5, 0);
    key.castShadow = true;
    key.shadow.mapSize.set(2048, 2048);
    key.shadow.camera.near = 20;
    key.shadow.camera.far = 140;
    // 偏移量按厘米场景调过：太小会在机身圆角上出现横纹（阴影痤疮）
    key.shadow.bias = -0.0006;
    key.shadow.normalBias = 0.06;
    key.userData.base = new THREE.Vector3();
    key.userData.soft = 4.5;
    scene.add(key, key.target);

    /** 主光放在以机身为中心的球面上：yaw 从 +Z 往 +X 转（度），elev 仰角（度）。 */
    function placeKey({ yaw = -43, elev = 38, dist = 58 } = {}) {
        const y = (yaw * Math.PI) / 180;
        const e = (elev * Math.PI) / 180;
        key.userData.base.set(dist * Math.sin(y) * Math.cos(e), dist * Math.sin(e), dist * Math.cos(y) * Math.cos(e));
        key.position.copy(key.userData.base);
    }
    placeKey();

    // 红色安全灯：左后上方，给白色机身勾一道红色轮廓
    // 只勾轮廓，不染红台面：放在机身后上方，强度很低
    const safe = new THREE.RectAreaLight(SAFELIGHT, 1.0, 18, 10);
    safe.position.set(-26, 26, -40);
    safe.lookAt(0, 3, 0);
    scene.add(safe);
    // 右前方中性柔光：让白色机身读起来是“白”，而不是被暖光染成米色
    const fill = new THREE.RectAreaLight(0xf4f2ee, 1.8, 30, 30);
    fill.position.set(30, 18, 38);
    fill.lookAt(0, 2.5, 0);
    scene.add(fill);
    // 极弱的环境补光，保留浓重黑位
    scene.add(new THREE.HemisphereLight(0x2a2220, 0x000000, 0.25));

    // 屏幕光：贴在屏幕前方，朝 +Z 照亮台面和前景相纸
    const screenLight = new THREE.RectAreaLight(0xffffff, 0, 3.9, 3.9);
    scene.add(screenLight);

    // 背景：远处的安全灯灯罩（大块红色散景）+ 墙面红晕 + 几颗计时器指示灯
    const glow = TEX.radialGlow(0.55);
    const soft = TEX.radialGlow(0.0);
    const additive = (map, color, strength) => new THREE.MeshBasicMaterial({
        map, color: new THREE.Color(color).multiplyScalar(strength), transparent: true,
        blending: THREE.AdditiveBlending, depthWrite: false, fog: false,
    });
    const lamp = new THREE.Mesh(new THREE.PlaneGeometry(5, 5), additive(glow, SAFELIGHT, 2.6));
    lamp.position.set(-30, 18, -80);
    // 灯罩周围的一圈红晕：只覆盖背景上方，不能叠到台面上，否则整个画面发红
    const wash = new THREE.Mesh(new THREE.PlaneGeometry(46, 30), additive(soft, 0x8a1206, 0.09));
    wash.position.set(-30, 22, -85);
    scene.add(lamp, wash);
    const rand = rng(9);
    // 光点要够大够柔：太小的亮点在有限采样下会散成一簇小方块，而不是圆形散景
    for (let i = 0; i < 3; i++) {
        const dot = new THREE.Mesh(new THREE.CircleGeometry(1.1 + rand() * 0.5, 32),
                                   additive(soft, i % 2 ? 0xff7a2a : 0xff3a1a, 0.9 + rand() * 0.6));
        dot.position.set(14 + rand() * 26, 5 + rand() * 9, -60 - rand() * 20);
        scene.add(dot);
    }

    // 台面成片
    const printGroup = new THREE.Group();
    scene.add(printGroup);
    const printCache = new Map();
    let placed = [];   // [{ spec, mesh }]
    async function setPrints(list) {
        printGroup.clear();
        placed = [];
        for (const spec of list) {
            const id = JSON.stringify(spec);
            if (!printCache.has(id)) printCache.set(id, await createPrint(spec, manifest));
            const mesh = printCache.get(id);
            printGroup.add(mesh);
            placed.push({ spec, mesh });
        }
    }

    /**
     * 让带 drop 的相纸在第 drop 帧从上方落到台面：DROP_FRAMES 帧内下落并转正，之前不可见。
     * 与 2D 版 Films 场景的落片节奏一致（每次换卷的“咔哒”声即落片时刻）。
     */
    function updatePrints(frame) {
        for (const { spec, mesh } of placed) {
            if (spec.drop === undefined) continue;
            const k = Math.min(1, Math.max(0, (frame - spec.drop) / DROP_FRAMES));
            mesh.visible = frame >= spec.drop;
            // 从镜头一侧低低地“发牌”过来：既有下落感，又不会从机身前面挡过去
            const fall = 1 - easeOutCubic(k);
            mesh.position.y = 0.02 + (spec.lift || 0) + DROP_HEIGHT * fall;
            mesh.position.z = spec.z + DROP_SLIDE * fall;
            mesh.rotation.y = (spec.yaw || 0) + 0.35 * fall;
        }
    }

    // 调试：scene.html?off=key,safe,fill,hemi,env,back 逐个关掉光源，排查色偏来源
    const off = new URLSearchParams(location.search).get('off');
    if (off) {
        const parts = { key, safe, fill, hemi: scene.children.find((o) => o.isHemisphereLight), screen: screenLight };
        for (const name of off.split(',')) {
            if (parts[name]) parts[name].visible = false;
            if (name === 'env') scene.environmentIntensity = 0;
            if (name === 'back') lamp.visible = wash.visible = false;
        }
    }

    return { key, screenLight, setPrints, updatePrints, placeKey };
}
