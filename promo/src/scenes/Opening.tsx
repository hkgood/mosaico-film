import React from 'react';
import {AbsoluteFill, Img, useCurrentFrame} from 'remotion';
import {printUrl} from '../lib/clip';
import {Bench} from '../lib/Darkroom';
import {Plate3D} from '../lib/Plate';
import {anim, COLORS, FONT, FONT_CN, FONT_STAMP} from '../lib/theme';
import {Caption, CharReveal} from '../lib/Type';

/* 0-3 s: the bench light comes up while the camera circles Mosaico from behind; the real boot animation plays on it. */
export const Boot: React.FC = () => (
  <AbsoluteFill>
    <Plate3D shot="boot" />
    <Caption overline="ESP-MOSAICO" title="一块掌心大的屏幕" start={34} exit={80} top={200} />
  </AbsoluteFill>
);

const STRIP_FILMS = ['films_0', 'films_1', 'films_2', 'films_3', 'films_4', 'films_5', 'films_6', 'films_7'];
const STRIP_H = 420;
const FRAME_H = 310;
const FRAME_W = (FRAME_H * 4) / 3;
const FRAME_GAP = 28;
const HOLE = {w: 26, h: 18, pitch: 48};

/* A strip of negatives-turned-positives: the same street through all eight films, sliding under the safelight. */
const FilmStrip: React.FC<{offset: number}> = ({offset}) => {
  const pitch = FRAME_W + FRAME_GAP;
  const frames = [...STRIP_FILMS, ...STRIP_FILMS];
  const holes = Array.from({length: Math.ceil((frames.length * pitch) / HOLE.pitch)});
  const band = (top: number) => (
    <div style={{position: 'absolute', left: 0, top, height: HOLE.h, width: frames.length * pitch}}>
      {holes.map((_, i) => (
        <div
          key={i}
          style={{
            position: 'absolute', left: i * HOLE.pitch + 11, width: HOLE.w, height: HOLE.h, borderRadius: 4,
            background: COLORS.bench,
          }}
        />
      ))}
    </div>
  );
  return (
    <div style={{position: 'absolute', left: 0, right: 0, height: STRIP_H, overflow: 'hidden'}}>
      <div
        style={{
          position: 'absolute', top: 0, height: STRIP_H, left: -offset, width: frames.length * pitch,
          background: 'linear-gradient(180deg, #2a1d14 0%, #1b130d 50%, #2a1d14 100%)',
        }}
      >
        {band(22)}
        {band(STRIP_H - 22 - HOLE.h)}
        {frames.map((name, i) => (
          <React.Fragment key={i}>
            <Img
              src={printUrl(name)}
              style={{position: 'absolute', left: i * pitch + FRAME_GAP / 2, top: (STRIP_H - FRAME_H) / 2, width: FRAME_W, height: FRAME_H}}
            />
            <div
              style={{
                position: 'absolute', left: i * pitch + FRAME_GAP / 2 + 6, top: STRIP_H - 22 - HOLE.h - 26,
                fontFamily: FONT_STAMP, fontSize: 14, color: 'rgba(255,154,60,0.75)', letterSpacing: '0.1em',
              }}
            >
              {`${(i % 8) + 1}`.padStart(2, '0')}
            </div>
          </React.Fragment>
        ))}
      </div>
    </div>
  );
};

/* 3-5 s: title over a strip of real prints under a red safelight. */
export const Title: React.FC = () => {
  const frame = useCurrentFrame();
  const glow = anim(frame, [0, 20], [0, 1]);
  const stripIn = anim(frame, [0, 14], [0, 1]);
  return (
    <AbsoluteFill>
      <Bench glow="200,55,31" strength={0.3 * glow} glowY="52%" />
      <div style={{position: 'absolute', top: 560, left: 0, right: 0, textAlign: 'center'}}>
        <CharReveal
          text="MOSAICO FILM"
          start={2}
          stagger={2}
          style={{fontFamily: FONT, fontSize: 112, fontWeight: 300, letterSpacing: '0.16em', color: COLORS.paper}}
        />
      </div>
      <div style={{position: 'absolute', top: 800, left: 0, right: 0, opacity: stripIn, transform: `scaleY(${0.6 + 0.4 * stripIn})`}}>
        <FilmStrip offset={200 + frame * 7} />
      </div>
      <div style={{position: 'absolute', top: 1310, left: 0, right: 0, textAlign: 'center'}}>
        <CharReveal
          text="胶片相机"
          start={14}
          stagger={3}
          style={{fontFamily: FONT_CN, fontSize: 64, fontWeight: 500, letterSpacing: '0.5em', color: COLORS.paper, paddingLeft: '0.5em'}}
        />
      </div>
    </AbsoluteFill>
  );
};
