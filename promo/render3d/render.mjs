// 3D 镜头离线渲染驱动。
//
// 起一个本地静态服务器（根目录 promo/，ES module 不能从 file:// 加载），用 headless Chrome
// 打开 scene.html，逐帧调用 window.renderShot(name, frame)，JPEG 写到 public/render3d/<shot>/NNNN.jpg。
//
// 用法：
//   node render3d/render.mjs --shots mock_hero,mock_key [--spp 32] [--w 1080] [--h 1920]
//   node render3d/render.mjs --shots films --from 0 --to 119
//   node render3d/render.mjs --shots all --list 0,45,89 --spp 8 --w 540 --h 960   # 抽帧检查构图
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer-core';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..');
const OUT = path.join(ROOT, 'public', 'render3d');
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const MIME = {
    '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json',
    '.png': 'image/png', '.jpg': 'image/jpeg', '.ttf': 'font/ttf',
};

function parseArgs(argv) {
    const args = { shots: '', spp: 32, w: 1080, h: 1920, from: 0, to: -1 };
    for (let i = 0; i < argv.length; i += 2) {
        const key = argv[i].replace(/^--/, '');
        args[key] = key === 'shots' || key === 'list' ? argv[i + 1] : Number(argv[i + 1]);
    }
    if (!args.shots) throw new Error('--shots is required');
    return args;
}

function serve(root) {
    const server = http.createServer((req, res) => {
        const file = path.join(root, decodeURIComponent(req.url.split('?')[0]));
        if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
            res.writeHead(404).end();
            return;
        }
        res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
        fs.createReadStream(file).pipe(res);
    });
    return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const server = await serve(ROOT);
    const browser = await puppeteer.launch({
        executablePath: CHROME,
        headless: 'new',
        // 让 ANGLE 走 Metal 用真实 GPU，否则会退回很慢的软件渲染
        args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist'],
        protocolTimeout: 900000,
    });
    try {
        const page = await browser.newPage();
        page.on('console', (msg) => console.log('[page]', msg.text()));
        page.on('pageerror', (err) => console.error('[page error]', err.message));
        await page.setViewport({ width: args.w, height: args.h, deviceScaleFactor: 1 });
        const { port } = server.address();
        const off = process.env.OFF ? `&off=${process.env.OFF}` : '';
        await page.goto(`http://127.0.0.1:${port}/render3d/scene.html?w=${args.w}&h=${args.h}&spp=${args.spp}${off}`);
        await page.waitForFunction(() => window.sceneReady === true || window.sceneError, { timeout: 120000 });
        const error = await page.evaluate(() => window.sceneError);
        if (error) throw new Error(error);
        const frameCounts = await page.evaluate(() => window.shotFrames);

        const shots = args.shots === 'all' ? Object.keys(frameCounts) : args.shots.split(',');
        for (const shot of shots) {
            if (!(shot in frameCounts)) throw new Error(`unknown shot ${shot}`);
            const last = args.to >= 0 ? Math.min(args.to, frameCounts[shot] - 1) : frameCounts[shot] - 1;
            // --list 0,45,89 只渲染指定帧（检查构图用）；否则渲染 from..to
            const frames = args.list
                ? args.list.split(',').map(Number).filter((i) => i < frameCounts[shot])
                : Array.from({ length: last - args.from + 1 }, (_, k) => args.from + k);
            const dir = path.join(OUT, shot);
            fs.mkdirSync(dir, { recursive: true });
            for (const i of frames) {
                const started = Date.now();
                const dataUrl = await page.evaluate((s, f) => window.renderShot(s, f), shot, i);
                fs.writeFileSync(path.join(dir, `${String(i).padStart(4, '0')}.jpg`),
                                 Buffer.from(dataUrl.split(',')[1], 'base64'));
                console.log(`${shot} ${i} ${Date.now() - started}ms`);
            }
        }
    } finally {
        await browser.close();
        server.close();
    }
}

main().catch((err) => { console.error(err); process.exit(1); });
