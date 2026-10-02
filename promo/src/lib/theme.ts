import {Easing, interpolate} from 'remotion';

/* The app's own Latin face (Jost), Chinese in PingFang, and the camera's 7-segment date stamp. */
export const FONT = '"Jost", "Avenir Next", "PingFang SC", sans-serif';
export const FONT_CN = '"PingFang SC", "Jost", sans-serif';
export const FONT_STAMP = '"DSEG7", monospace';

/* Darkroom palette: near-black bench, warm paper, safelight red, and the app's amber. */
export const COLORS = {
  bench: '#0c0a09',
  paper: '#f1e8d8',
  muted: '#9c958a',
  amber: '#ff9a3c',
  stamp: '#ff8a2a',
  safelight: '#c8371f',
  key: '#ff5a1f',
};

export const EXPO_OUT = Easing.bezier(0.16, 1, 0.3, 1);
export const EXPO_IN = Easing.bezier(0.7, 0, 0.84, 0);
export const IN_OUT = Easing.bezier(0.65, 0, 0.35, 1);

/* Clamped interpolation with an easing; the workhorse for every animation. */
export const anim = (
  frame: number,
  input: [number, number],
  output: [number, number],
  easing: (t: number) => number = EXPO_OUT,
) =>
  interpolate(frame, input, output, {
    easing,
    extrapolateLeft: 'clamp',
    extrapolateRight: 'clamp',
  });

/* Deterministic pseudo-random in [0, 1) for per-frame jitter (grain offset, print tilt). */
export const hash = (n: number) => {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
  return x - Math.floor(x);
};
