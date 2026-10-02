import React from 'react';
import {staticFile, useCurrentFrame} from 'remotion';
import manifest from '../../public/manifest.json';
import type {ClipUse} from './timeline';
import {anim} from './theme';

/* Every clip frame is a 480 x 480 capture of the real app (tools/promo/promo_capture.c). */
export const UI_SIZE = 480;

/* What happened on a captured frame: a touch sample, the red key, or a sound cue. */
export type Mark = {frame: number; type: string; name: string; x: number; y: number};
type ClipInfo = {frames: number; marks: Mark[]};
/* A print developed by the real darkroom pipeline during the capture. */
export type PrintInfo = {name: string; id: number; film: number; filmName: string; instant: boolean; width: number; height: number};

const CLIPS = manifest.clips as Record<string, ClipInfo>;
export const PRINTS = manifest.prints as PrintInfo[];

export const clipInfo = (clip: string) => {
  const info = CLIPS[clip];
  if (!info) throw new Error(`clip ${clip} is missing; run tools/promo/export_assets.py`);
  return info;
};

export const printOf = (name: string) => {
  const p = PRINTS.find((x) => x.name === name);
  if (!p) throw new Error(`print ${name} is missing`);
  return p;
};

export const printUrl = (name: string) => staticFile(`prints/${name}.jpg`);
export const frameUrl = (clip: string, index: number) => staticFile(`clips/${clip}/${String(index).padStart(4, '0')}.jpg`);

/* Clip frame shown at scene frame `frame` for a placement (held on its first / last frame outside it). */
export const clipIndex = (use: ClipUse, frame: number) => {
  const raw = Math.floor(use.src + Math.max(0, frame - use.at) * use.rate);
  return Math.min(clipInfo(use.clip).frames - 1, Math.max(0, raw));
};

/* Marks of `types` that happened at or before clip frame `index`, newest last. */
export const marksUpTo = (clip: string, index: number, types: string[]) =>
  clipInfo(clip).marks.filter((m) => m.frame <= index && types.includes(m.type));

/* Brief white bloom, e.g. on a shutter release. */
export const Flash: React.FC<{at: number; length?: number; strength?: number}> = ({at, length = 8, strength = 0.85}) => {
  const frame = useCurrentFrame();
  const o = anim(frame, [at, at + 1], [0, strength]) * anim(frame, [at + 1, at + length], [1, 0]);
  return o > 0 ? <div style={{position: 'absolute', inset: 0, background: '#fff8ee', opacity: o}} /> : null;
};
