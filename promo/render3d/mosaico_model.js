// ESP-Mosaico V1.2 程序化模型（毫米建模，整体缩放到厘米放进场景）。
//
// 尺寸取自官方五视图（https://mosaico.espressif.com/ 硬件页），以侧面 2.54 mm 排针间距为比例尺：
//   机身 44.5 W × 45 H × 11.2 D，正面黑色玻璃几乎铺满（约 1.3 mm 白边），屏幕可视区约 38.8 mm 见方
//   顶面：7 条出音孔（中心偏左 2.5 mm、间距 2.53 mm）+ 橙色 AI 键（5.9 × 5.8 mm，中心偏右 12.9 mm）
//   底面：麦克风孔 + 6 个指示灯导光点（靠前），BOOT / USB-C / ON-OFF（靠后）
//   左右侧：2×10 方孔（靠后）+ 引脚丝印条（靠前）
//   背面：黑色背板，4 颗螺丝、标志、二维码、7 个金手指焊盘
// 坐标：原点在机身中心，+Z 为屏幕朝向，+Y 向上。
import * as THREE from 'three';
import { RoundedBoxGeometry } from 'three/addons/geometries/RoundedBoxGeometry.js';
import { mergeVertices } from 'three/addons/utils/BufferGeometryUtils.js';
import * as TEX from './textures.js';

const MM_TO_CM = 0.1;

export const DIMS = {
    width: 44.5,
    height: 45,
    depth: 11.2,
    cornerR: 4.2,        // 正视圆角
    edgeR: 1.2,          // 前后棱边圆角
    glassRim: 1.3,       // 玻璃四周白边
    glassR: 3.6,
    screen: 38.8,
    screenR: 2.6,
    pitch: 2.54,
};

// 顶面出音孔与 AI 键
const TOP = {
    slots: { count: 7, pitch: 2.53, centerX: -2.5, w: 1.0, h: 2.4 },
    key: { x: 12.9, w: 5.9, d: 5.8, proud: 0.7, travel: 0.45 },
};
// 底面：z 为相对机身中心的前后位置
const BOTTOM = {
    dots: { fromFront: 3.7, xs: [-6.9, -4.6, -2.35, 0.14, 2.4, 4.6, 6.9] },
    switchX: 10.1, switchW: 8.3, switchD: 2.3, switchZ: -1.4,
    usbW: 8.8, usbD: 2.9,
};
// 背板：焊盘 y 为距背板上沿的距离
const BACK = {
    width: 40.5, height: 41, r: 3.0,
    pads: { labels: ['GND', 'BOOT', 'RST', 'RX', 'TX', '5V', 'GND'], pitch: 2.7, y: 30.2, w: 2.1, h: 3.2 },
    screws: { x: 16.6, y: 17.0, r: 1.25 },
};

/** 以中心为原点的圆角矩形 Shape。 */
function roundedShape(w, h, r) {
    const s = new THREE.Shape();
    const x = -w / 2;
    const y = -h / 2;
    s.moveTo(x + r, y);
    s.lineTo(x + w - r, y);
    s.quadraticCurveTo(x + w, y, x + w, y + r);
    s.lineTo(x + w, y + h - r);
    s.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
    s.lineTo(x + r, y + h);
    s.quadraticCurveTo(x, y + h, x, y + h - r);
    s.lineTo(x, y + r);
    s.quadraticCurveTo(x, y, x + r, y);
    return s;
}

/** 圆角矩形平面，UV 归一化到 0..1（ShapeGeometry 默认 UV 是形状坐标）。 */
function roundedPlane(w, h, r) {
    const geo = new THREE.ShapeGeometry(roundedShape(w, h, r), 12);
    const pos = geo.attributes.position;
    const uv = geo.attributes.uv;
    for (let i = 0; i < pos.count; i++) {
        uv.setXY(i, pos.getX(i) / w + 0.5, pos.getY(i) / h + 0.5);
    }
    return geo;
}

/** 圆角棱边的挤出体，厚度方向居中在 z=0。 */
function roundedSlab(w, h, r, depth, edge) {
    const geo = new THREE.ExtrudeGeometry(roundedShape(w - 2 * edge, h - 2 * edge, Math.max(0.2, r - edge)), {
        depth: depth - 2 * edge,
        bevelEnabled: true,
        bevelThickness: edge,
        bevelSize: edge,
        bevelSegments: 8,
        curveSegments: 16,
    });
    geo.translate(0, 0, -(depth - 2 * edge) / 2);
    // ExtrudeGeometry 每个面独立顶点 → 圆角一段段平直着色（出现条纹）。
    // 倒角轮廓与端面、侧壁都是切线连续的，去掉 UV 后合并顶点即可得到平滑法线。
    geo.deleteAttribute('uv');
    geo.deleteAttribute('normal');
    const smooth = mergeVertices(geo, 1e-4);
    smooth.computeVertexNormals();
    return smooth;
}

/** 贴花材质：与白色机身同样的表面属性，透明处露出机身。 */
function decalMaterial(map, base) {
    return new THREE.MeshPhysicalMaterial({
        map,
        transparent: true,
        depthWrite: false,
        roughness: base.roughness,
        clearcoat: base.clearcoat,
        clearcoatRoughness: base.clearcoatRoughness,
        polygonOffset: true,
        polygonOffsetFactor: -2,
    });
}

function shadowed(mesh) {
    mesh.castShadow = true;
    mesh.receiveShadow = true;
    return mesh;
}

/**
 * 创建机身。返回：
 *   root        —— 场景中的根节点（厘米单位，底面贴在 y=0，屏幕朝 +Z）
 *   setScreen(texture, brightness) —— 换屏幕画面（真实 UI 帧）
 *   setKeyPress(k)                 —— AI 键按下程度 0..1
 *   screenCenter / topKey          —— 机身局部（厘米）关键点，供镜头对焦
 */
export function createMosaico() {
    const { width: W, height: H, depth: D } = DIMS;
    const body = new THREE.Group();   // 毫米单位

    // ---------- 白色机身 ----------
    const shell = new THREE.MeshPhysicalMaterial({
        color: 0xf6f6f3, roughness: 0.4, clearcoat: 0.3, clearcoatRoughness: 0.22,
    });
    body.add(shadowed(new THREE.Mesh(roundedSlab(W, H, DIMS.cornerR, D, DIMS.edgeR), shell)));

    // ---------- 正面玻璃 + 屏幕 ----------
    const glassW = W - 2 * DIMS.glassRim;
    const glassH = H - 2 * DIMS.glassRim;
    const glassMat = new THREE.MeshPhysicalMaterial({
        color: 0x020202, roughness: 0.035, clearcoat: 1, clearcoatRoughness: 0.02, ior: 1.52,
    });
    const glass = new THREE.Mesh(roundedSlab(glassW, glassH, DIMS.glassR, 0.6, 0.22), glassMat);
    glass.position.z = D / 2 - 0.15;
    body.add(shadowed(glass));
    const glassFront = glass.position.z + 0.3;

    // 屏幕：真实 UI 帧，自发光；上面再叠一层只有反射的“玻璃”，让高光掠过画面
    const screenMat = new THREE.MeshBasicMaterial({ color: 0xffffff });
    const screen = new THREE.Mesh(roundedPlane(DIMS.screen, DIMS.screen, DIMS.screenR), screenMat);
    screen.position.z = glassFront + 0.01;
    const reflectMat = new THREE.MeshPhysicalMaterial({
        color: 0x000000, roughness: 0.03, transparent: true, blending: THREE.AdditiveBlending, depthWrite: false,
    });
    const reflect = new THREE.Mesh(roundedPlane(DIMS.screen, DIMS.screen, DIMS.screenR), reflectMat);
    reflect.position.z = glassFront + 0.03;
    body.add(screen, reflect);

    // ---------- 侧面：排针孔 + 丝印 ----------
    const flat = D;   // 贴花画布覆盖整个厚度，透明部分不影响圆角
    const sideLayout = (side) => {
        // u：从该侧看过去的右手方向（左侧面朝前，右侧面朝后）
        const front = (mm) => (side === 'left' ? D / 2 + mm : D / 2 - mm);
        const holes = [-3.46, -0.83].map(front);
        const strip = [0.76, 4.0].map(front).sort((a, b) => a - b);
        return { depth: flat, height: H, holesU: holes, stripU: strip, pitch: DIMS.pitch, stripH: 27 };
    };
    for (const side of ['left', 'right']) {
        const mat = decalMaterial(TEX.sideDecal(side, sideLayout(side)), shell);
        const plane = new THREE.Mesh(new THREE.PlaneGeometry(flat, H), mat);
        const sign = side === 'left' ? -1 : 1;
        plane.rotation.y = sign * Math.PI / 2;
        plane.position.x = sign * (W / 2 + 0.01);
        body.add(plane);
    }

    // ---------- 顶面：出音孔 + AI 键 ----------
    const top = new THREE.Mesh(new THREE.PlaneGeometry(W, D),
                               decalMaterial(TEX.topDecal({ width: W, depth: D, slots: TOP.slots }), shell));
    top.rotation.x = -Math.PI / 2;
    top.position.y = H / 2 + 0.01;
    body.add(top);

    const keyRest = H / 2 + TOP.key.proud - 0.7;
    const keyMat = new THREE.MeshPhysicalMaterial({
        color: 0xf2873a, roughness: 0.42, clearcoat: 0.2, bumpMap: TEX.ribBump(5), bumpScale: 0.6,
    });
    const key = shadowed(new THREE.Mesh(new RoundedBoxGeometry(TOP.key.w, 1.4, TOP.key.d, 4, 0.55), keyMat));
    key.position.set(TOP.key.x, keyRest, 0);
    body.add(key);

    // ---------- 底面：开关、USB-C、指示灯 ----------
    const bottom = new THREE.Mesh(new THREE.PlaneGeometry(W, D),
                                  decalMaterial(TEX.bottomDecal({ width: W, depth: D, dots: BOTTOM.dots }), shell));
    bottom.rotation.x = Math.PI / 2;
    bottom.position.y = -H / 2 - 0.01;
    body.add(bottom);

    const switchMat = new THREE.MeshPhysicalMaterial({
        color: 0xc9c9c6, roughness: 0.5, bumpMap: TEX.ribBump(7), bumpScale: 0.5,
    });
    for (const sx of [-1, 1]) {
        const sw = shadowed(new THREE.Mesh(
            new RoundedBoxGeometry(BOTTOM.switchW, 1.0, BOTTOM.switchD, 4, 0.48), switchMat));
        sw.position.set(sx * BOTTOM.switchX, -H / 2, BOTTOM.switchZ);
        body.add(sw);
    }
    const usbRing = new THREE.Mesh(new RoundedBoxGeometry(BOTTOM.usbW, 0.3, BOTTOM.usbD, 4, 0.14),
                                   new THREE.MeshStandardMaterial({ color: 0xb8b8b8, metalness: 1, roughness: 0.3 }));
    usbRing.position.set(0, -H / 2 - 0.02, BOTTOM.switchZ);
    const usbHole = new THREE.Mesh(new RoundedBoxGeometry(BOTTOM.usbW - 0.8, 0.34, BOTTOM.usbD - 0.7, 4, 0.16),
                                   new THREE.MeshStandardMaterial({ color: 0x050505, roughness: 0.8 }));
    usbHole.position.set(0, -H / 2 - 0.04, BOTTOM.switchZ);
    body.add(usbRing, usbHole);

    // ---------- 背面：黑色背板 ----------
    const plateMat = new THREE.MeshPhysicalMaterial({ color: 0x141414, roughness: 0.32, clearcoat: 0.5 });
    const plate = shadowed(new THREE.Mesh(roundedSlab(BACK.width, BACK.height, BACK.r, 0.6, 0.2), plateMat));
    plate.position.z = -D / 2 + 0.15;
    body.add(plate);
    const backFace = plate.position.z - 0.3;
    const print = new THREE.Mesh(roundedPlane(BACK.width - 0.4, BACK.height - 0.4, BACK.r - 0.2),
                                 new THREE.MeshPhysicalMaterial({
                                     map: TEX.backPrint({ width: BACK.width - 0.4, height: BACK.height - 0.4, pads: BACK.pads }),
                                     roughness: 0.32, clearcoat: 0.5,
                                 }));
    print.rotation.y = Math.PI;
    print.position.z = backFace - 0.01;
    body.add(print);

    const gold = new THREE.MeshStandardMaterial({ color: 0xe0ab52, metalness: 1, roughness: 0.28 });
    BACK.pads.labels.forEach((_, i) => {
        const pad = new THREE.Mesh(new THREE.BoxGeometry(BACK.pads.w, BACK.pads.h, 0.08), gold);
        // 背面看过去的右手方向是 -X
        pad.position.set(-(i - (BACK.pads.labels.length - 1) / 2) * BACK.pads.pitch,
                         BACK.height / 2 - BACK.pads.y, backFace - 0.04);
        body.add(pad);
    });
    const screwMat = new THREE.MeshStandardMaterial({ color: 0x1a1a1a, metalness: 0.8, roughness: 0.35 });
    const slotMat = new THREE.MeshStandardMaterial({ color: 0x000000, roughness: 0.9 });
    for (const sx of [-1, 1]) {
        for (const sy of [-1, 1]) {
            const screw = new THREE.Mesh(new THREE.CylinderGeometry(BACK.screws.r, BACK.screws.r, 0.35, 32), screwMat);
            screw.rotation.x = Math.PI / 2;
            screw.position.set(sx * BACK.screws.x, sy * BACK.screws.y, backFace - 0.17);
            body.add(shadowed(screw));
            for (const a of [0, Math.PI / 2]) {
                const slot = new THREE.Mesh(new THREE.BoxGeometry(1.5, 0.28, 0.05), slotMat);
                slot.rotation.z = a + Math.PI / 4;
                slot.position.set(screw.position.x, screw.position.y, backFace - 0.36);
                body.add(slot);
            }
        }
    }

    // ---------- 放进场景：毫米 → 厘米，拨动开关最低点贴地 ----------
    body.scale.setScalar(MM_TO_CM);
    const lowest = H / 2 + 0.5;
    body.position.y = lowest * MM_TO_CM;
    const root = new THREE.Group();
    root.add(body);

    return {
        root,
        setScreen(texture, brightness = 1) {
            screenMat.map = texture;
            screenMat.color.setScalar(brightness);
            screenMat.needsUpdate = true;
        },
        setKeyPress(k) {
            key.position.y = keyRest - TOP.key.travel * k;
        },
        /** 机身局部点（毫米）→ 世界坐标（厘米）。 */
        worldPoint(xMm, yMm, zMm) {
            root.updateMatrixWorld(true);
            return body.localToWorld(new THREE.Vector3(xMm, yMm, zMm));
        },
        screenCenterMm: [0, 0, glassFront],
        keyTopMm: [TOP.key.x, keyRest + 0.7, 0],
        sizeCm: { width: W * MM_TO_CM, height: lowest * 2 * MM_TO_CM, depth: D * MM_TO_CM },
    };
}
