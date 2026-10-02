// Renders selected frames at reduced scale into out/sheet/ for review.
// Usage: node scripts/contact-sheet.mjs 20 60 130 ...   (SCALE=0.4 by default)
import path from 'node:path';
import {bundle} from '@remotion/bundler';
import {renderStill, selectComposition} from '@remotion/renderer';

const CHROME = process.env.REMOTION_CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const frames = process.argv.slice(2).map(Number);
if (!frames.length) {
  console.error('usage: node scripts/contact-sheet.mjs <frame> [frame...]');
  process.exit(1);
}

const serveUrl = await bundle({entryPoint: path.resolve('src/index.ts')});
const inputProps = {audio: false};
const composition = await selectComposition({serveUrl, id: 'FilmPromo', inputProps, browserExecutable: CHROME});
for (const frame of frames) {
  const output = path.resolve(`out/sheet/${String(frame).padStart(4, '0')}.jpg`);
  await renderStill({composition, serveUrl, frame, output, inputProps, imageFormat: 'jpeg', jpegQuality: 85,
    scale: Number(process.env.SCALE ?? 0.4), browserExecutable: CHROME, overwrite: true});
  console.log(`frame ${frame} -> ${output}`);
}
