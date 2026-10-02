// 3D 宣传片渲染主程序：搭暗房场景，按镜头表逐帧离线渲染。
//
// 每帧渲染 SPP 次子采样再求平均（与开机动画同一套方法）：
//   - 子采样时间分布在快门开启区间内 → 运动模糊
//   - 机位在光圈圆盘上抖动、始终对准焦点 → 景深
//   - 主光在柔光面上抖动 → 软阴影
//   - 亚像素抖动 → 抗锯齿
// 累积结果是线性 HDR，之后做泛光、色调映射（Neutral：尽量不改 UI 原色），最后加颗粒和暗角。
import * as THREE from 'three';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { TexturePass } from 'three/addons/postprocessing/TexturePass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';
import { FullScreenQuad } from 'three/addons/postprocessing/Pass.js';
import { createMosaico } from './mosaico_model.js';
import { createDarkroom } from './darkroom.js';
import { createScreenCompositor } from './screen.js';
import { buildShots } from './shots.js';
import { loadTimeline } from './timeline.js';
import { halton } from './math.js';

const params = new URLSearchParams(location.search);
const WIDTH = Number(params.get('w') || 1080);
const HEIGHT = Number(params.get('h') || 1920);
const SPP = Number(params.get('spp') || 32);
const SHUTTER_FRAMES = 0.5;    // 快门开启半帧：运动模糊自然又不糊 UI
const SCREEN_NITS = 0.95;      // 屏幕亮度（线性）：白色 UI 接近但不超过泛光阈值

// ---------- 渲染器 ----------
const renderer = new THREE.WebGLRenderer({ antialias: false, preserveDrawingBuffer: true });
renderer.setPixelRatio(1);
renderer.setSize(WIDTH, HEIGHT);
renderer.toneMapping = THREE.NeutralToneMapping;
renderer.toneMappingExposure = 1.0;
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;
document.body.appendChild(renderer.domElement);

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(30, WIDTH / HEIGHT, 0.05, 400);

// ---------- 累积缓冲 ----------
// 两块浮点缓冲“乒乓”累加，全部在着色器里算：ANGLE/Metal 下浮点混合会被静默跳过。
const rtScene = new THREE.WebGLRenderTarget(WIDTH, HEIGHT, { type: THREE.HalfFloatType, samples: 4 });
const accumTargets = [0, 1].map(() => new THREE.WebGLRenderTarget(WIDTH, HEIGHT, { type: THREE.FloatType }));
const accumQuad = new FullScreenQuad(new THREE.ShaderMaterial({
    uniforms: { tAccum: { value: null }, tSrc: { value: null }, uWeight: { value: 1 } },
    vertexShader: 'varying vec2 vUv; void main(){ vUv = uv; gl_Position = vec4(position.xy, 0.0, 1.0); }',
    // 样本限幅到 64：极亮镜面高光会溢出半精度浮点，经泛光扩散成方块
    fragmentShader: 'uniform sampler2D tAccum; uniform sampler2D tSrc; uniform float uWeight; varying vec2 vUv;' +
                    'void main(){ vec3 c = min(texture2D(tSrc, vUv).rgb, vec3(64.0));' +
                    ' gl_FragColor = vec4(texture2D(tAccum, vUv).rgb + c * uWeight, 1.0); }',
    blending: THREE.NoBlending, depthTest: false, depthWrite: false,
}));

// 胶片感收尾：暗角 + 亮度相关的细颗粒（色调映射之后在显示空间里做）。
// 成片里 Remotion 还会整体叠一层颗粒和暗角，所以这里只留很轻的一层。
const FilmShader = {
    uniforms: { tDiffuse: { value: null }, uSeed: { value: 0 }, uGrain: { value: 0.012 }, uVignette: { value: 0.2 },
                uAspect: { value: WIDTH / HEIGHT } },
    vertexShader: 'varying vec2 vUv; void main(){ vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position,1.0); }',
    fragmentShader: `
        uniform sampler2D tDiffuse; uniform float uSeed; uniform float uGrain; uniform float uVignette; uniform float uAspect;
        varying vec2 vUv;
        float hash(vec2 p){ p = fract(p * vec2(443.897, 441.423) + uSeed); p += dot(p, p.yx + 19.19); return fract((p.x + p.y) * p.x); }
        void main(){
            vec4 c = texture2D(tDiffuse, vUv);
            vec2 d = (vUv - 0.5) * vec2(uAspect, 1.0);
            float vig = 1.0 - uVignette * smoothstep(0.3, 0.75, length(d));
            float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));
            float n = (hash(vUv * 1500.0) - 0.5) * uGrain * (1.0 - abs(luma - 0.45) * 1.2);
            gl_FragColor = vec4(c.rgb * vig + n, 1.0);
        }`,
};

const composer = new EffectComposer(renderer);
composer.setPixelRatio(1);
composer.setSize(WIDTH, HEIGHT);
const texturePass = new TexturePass(accumTargets[0].texture);
const bloom = new UnrealBloomPass(new THREE.Vector2(WIDTH, HEIGHT), 0.32, 0.6, 1.0);
const filmPass = new ShaderPass(FilmShader);
composer.addPass(texturePass);
composer.addPass(bloom);
composer.addPass(new OutputPass());
composer.addPass(filmPass);

// ---------- 场景 ----------
let device;
let room;
let screen;
let shots;
let currentShot = null;

async function build() {
    await document.fonts.load('500 24px Jost');
    await document.fonts.load('600 24px Jost');
    await loadTimeline();
    shots = buildShots();
    screen = createScreenCompositor();
    room = await createDarkroom(scene, renderer);
    device = createMosaico();
    scene.add(device.root);
    // 屏幕光贴在玻璃前方一点点，朝前照
    const center = device.worldPoint(...device.screenCenterMm);
    room.screenLight.position.copy(center).add(new THREE.Vector3(0, 0, 0.05));
    room.screenLight.lookAt(center.clone().add(new THREE.Vector3(0, 0, 10)));
}

/** 把相机摆到子采样 s 的位置：光圈圆盘抖动 + 亚像素抖动，始终对准焦平面。 */
function placeCamera(view, s) {
    camera.fov = view.fov;
    camera.position.copy(view.position);
    camera.lookAt(view.target);
    camera.updateMatrixWorld();
    const right = new THREE.Vector3().setFromMatrixColumn(camera.matrixWorld, 0);
    const up = new THREE.Vector3().setFromMatrixColumn(camera.matrixWorld, 1);
    const forward = new THREE.Vector3().subVectors(view.target, view.position).normalize();
    const focusDist = forward.dot(new THREE.Vector3().subVectors(view.focus, view.position));
    const focusPoint = view.position.clone().addScaledVector(forward, focusDist);
    const r = Math.sqrt(halton(s + 1, 2)) * view.aperture;
    const a = halton(s + 1, 3) * Math.PI * 2;
    camera.position.addScaledVector(right, Math.cos(a) * r).addScaledVector(up, Math.sin(a) * r);
    camera.lookAt(focusPoint);
    if (view.roll) camera.rotateZ(view.roll);
    camera.setViewOffset(WIDTH, HEIGHT, halton(s + 1, 5) - 0.5, halton(s + 1, 7) - 0.5, WIDTH, HEIGHT);
    camera.updateProjectionMatrix();
}

/** 主光在柔光面（圆盘）上抖动，多次累积后得到软阴影。 */
function jitterKey(s) {
    const { key } = room;
    const r = Math.sqrt(halton(s + 1, 11)) * key.userData.soft;
    const a = halton(s + 1, 13) * Math.PI * 2;
    key.position.copy(key.userData.base).add(new THREE.Vector3(Math.cos(a) * r, Math.sin(a) * r, 0));
}

/** 渲染镜头 name 的第 frame 帧，返回 JPEG dataURL。 */
async function renderShot(name, frame) {
    const shot = shots[name];
    if (!shot) throw new Error(`unknown shot ${name}`);
    if (currentShot !== name) {
        await room.setPrints(shot.prints());
        room.placeKey(shot.key);
        device.root.visible = shot.device !== false;
        currentShot = name;
    }
    // 屏幕画面按整帧切换（UI 本身是离散帧，不做子采样插值）
    let ui = { keyPress: 0 };
    if (device.root.visible) {
        ui = await screen.compose(shot.scene, frame, { touches: shot.touches !== false });
        device.setScreen(ui.texture, SCREEN_NITS);
        room.screenLight.color.copy(ui.color);
        room.screenLight.intensity = 2.2 * SCREEN_NITS;
    }
    room.screenLight.visible = device.root.visible;

    let [accumRead, accumWrite] = accumTargets;
    renderer.setRenderTarget(accumRead);
    renderer.setClearColor(0x000000, 0);
    renderer.clear();
    for (let s = 0; s < SPP; s++) {
        const sub = frame + ((s + 0.5) / SPP - 0.5) * SHUTTER_FRAMES;
        const st = shot.at(Math.max(0, sub));
        // 镜头可以自己指定按键动作（例如红键微距）；否则跟随采集里的真实按键
        device.setKeyPress(st.keyPress ?? ui.keyPress);
        room.updatePrints(sub);
        accumQuad.material.uniforms.uWeight.value = (st.exposure ?? 1) / SPP;
        jitterKey(s);
        placeCamera(st.view, s);

        renderer.setRenderTarget(rtScene);
        renderer.setClearColor(0x000000, 1);
        renderer.clear();
        renderer.render(scene, camera);

        renderer.setRenderTarget(accumWrite);
        accumQuad.material.uniforms.tAccum.value = accumRead.texture;
        accumQuad.material.uniforms.tSrc.value = rtScene.texture;
        accumQuad.render(renderer);
        [accumRead, accumWrite] = [accumWrite, accumRead];
    }
    renderer.setRenderTarget(null);
    texturePass.map = accumRead.texture;
    filmPass.uniforms.uSeed.value = (frame * 0.618) % 1;
    composer.render();
    return renderer.domElement.toDataURL('image/jpeg', 0.94);
}

build().then(() => {
    window.renderShot = renderShot;
    window.shotFrames = Object.fromEntries(Object.entries(shots).map(([k, s]) => [k, s.frames]));
    window.sceneReady = true;
}).catch((err) => {
    console.error(err);
    window.sceneError = String((err && err.stack) || err);
});
