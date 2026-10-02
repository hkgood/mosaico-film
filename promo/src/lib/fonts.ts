import {continueRender, delayRender, staticFile} from 'remotion';

/* Loads the app's faces before the first frame renders; Chinese uses the system PingFang SC. */
const FACES: [family: string, file: string, weight: string][] = [
  ['Jost', 'fonts/Jost.ttf', '100 900'],
  ['DSEG7', 'fonts/DSEG7Classic-Regular.ttf', '400'],
];

const handle = delayRender('Loading fonts');
Promise.all(
  FACES.map(async ([family, file, weight]) => {
    const face = new FontFace(family, `url(${staticFile(file)})`, {weight});
    document.fonts.add(await face.load());
  }),
)
  .then(() => continueRender(handle))
  .catch((err) => {
    console.error('font load failed', err);
    continueRender(handle);
  });
