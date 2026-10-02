import React from 'react';
import {AbsoluteFill, useCurrentFrame} from 'remotion';
import {clipIndex, Flash, marksUpTo, printOf} from '../lib/clip';
import {Plate3D} from '../lib/Plate';
import {BEAT, clipsOf} from '../lib/timeline';
import {COLORS, FONT, FONT_STAMP} from '../lib/theme';
import {Caption} from '../lib/Type';

const FILM_PRINTS = ['films_0', 'films_1', 'films_2', 'films_3', 'films_4', 'films_5', 'films_6', 'films_7'];

/* 5-9 s: swiping through films in the M6 viewfinder while the same street, printed on each film,
 * lands on the bench (in the 3D plate). Each swipe's detent sound in the capture is the cue for the
 * next print, so the bench and the name below always show the film named on the device. */
export const Films: React.FC = () => {
  const frame = useCurrentFrame();
  const [use] = clipsOf('films');
  const current = Math.min(FILM_PRINTS.length - 1, marksUpTo(use.clip, clipIndex(use, frame), ['sfx']).filter((m) => m.name === 'detent').length);
  return (
    <AbsoluteFill>
      <Plate3D shot="films" />
      <Caption overline="8 FILM STOCKS" title="8 款经典胶卷" sub="实时取景，左右一划就换卷" top={110} exit={112} />
      <div
        style={{
          position: 'absolute', top: 1745, left: 0, right: 0, textAlign: 'center', fontFamily: FONT, fontSize: 40,
          fontWeight: 500, letterSpacing: '0.24em', color: COLORS.paper,
        }}
      >
        <span style={{fontFamily: FONT_STAMP, fontSize: 26, color: COLORS.stamp, marginRight: 28, letterSpacing: '0.1em'}}>
          {`${current + 1}`.padStart(2, '0')}/08
        </span>
        {printOf(FILM_PRINTS[current]).filmName}
      </div>
    </AbsoluteFill>
  );
};

/* 9-12 s: the camera pushes onto the red key, which goes down just before the cut on the beat;
 * then the shutter curtain, the proof and DEVELOPED on the screen. */
export const Shutter: React.FC = () => {
  const [shot] = clipsOf('shutter');
  return (
    <AbsoluteFill>
      <Plate3D shot="shutter" />
      <Flash at={shot.at} length={7} strength={0.6} />
      <Caption overline="THE RED KEY" title="按下红键，咔嚓" sub="真正的快门手感" exit={82} />
    </AbsoluteFill>
  );
};

/* 12-15 s: pull the body picker down from the top edge; the leather closes and the SX-70 arrives
 * while the camera crosses in front of the device. */
export const Bodies: React.FC = () => {
  const frame = useCurrentFrame();
  /* The body swaps on the lever sound, which the timeline puts on a beat. */
  const swapped = frame >= 3 * BEAT;
  const label = (text: string, sub: string, active: boolean) => (
    <div style={{textAlign: 'center', opacity: active ? 1 : 0.35}}>
      <div style={{fontFamily: FONT, fontSize: 56, fontWeight: 500, letterSpacing: '0.12em', color: active ? COLORS.amber : COLORS.paper}}>
        {text}
      </div>
      <div style={{fontFamily: FONT, fontSize: 22, letterSpacing: '0.3em', color: COLORS.muted, marginTop: 10}}>{sub}</div>
    </div>
  );
  return (
    <AbsoluteFill>
      <Plate3D shot="bodies" />
      <Caption overline="TWO BODIES" title="两台机身" sub="旁轴 M6 · 宝丽来 SX-70" exit={82} />
      <div style={{position: 'absolute', top: 1500, left: 0, right: 0, display: 'flex', justifyContent: 'center', gap: 160}}>
        {label('M6', 'RANGEFINDER', !swapped)}
        {label('SX-70', 'INSTANT', swapped)}
      </div>
    </AbsoluteFill>
  );
};

/* 15-18 s: the instant print slides out and develops under its milky layer; low along the bench. */
export const Develop: React.FC = () => (
  <AbsoluteFill>
    <Plate3D shot="develop" />
    <Caption overline="INSTANT FILM" title="看着它，慢慢显影" sub="和真的拍立得一样" exit={82} />
  </AbsoluteFill>
);
