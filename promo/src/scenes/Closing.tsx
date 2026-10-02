import React from 'react';
import {AbsoluteFill, useCurrentFrame} from 'remotion';
import {Plate3D} from '../lib/Plate';
import {BEAT} from '../lib/timeline';
import {anim, COLORS, EXPO_IN, FONT, FONT_CN, FONT_STAMP} from '../lib/theme';
import {Caption, CharReveal} from '../lib/Type';

const STATS: [number: string, cn: string, en: string][] = [
  ['8', '款胶卷', 'FILM STOCKS'],
  ['2', '台机身', 'CAMERA BODIES'],
  ['36', '张一卷', 'FRAMES PER ROLL'],
];

/* 24-27 s: the numbers, lit like the camera's orange date stamp, one per beat, over the bench
 * strewn with every print from the capture (seen from above in the 3D plate, dimmed so the numbers read). */
export const Numbers: React.FC = () => {
  const frame = useCurrentFrame();
  return (
    <AbsoluteFill>
      <Plate3D shot="numbers" />
      <AbsoluteFill style={{background: 'linear-gradient(180deg, rgba(12,10,9,0.92) 0%, rgba(12,10,9,0.62) 40%, rgba(12,10,9,0.92) 100%)'}} />
      <Caption overline="MOSAICO FILM" title="不同的胶卷，不同的光" top={200} exit={82} />
      {STATS.map(([num, cn, en], i) => {
        const at = 8 + i * BEAT;
        const t = anim(frame, [at, at + 10], [0, 1]);
        const leave = anim(frame, [82, 90], [0, 1], EXPO_IN);
        return (
          <div
            key={num}
            style={{
              position: 'absolute', top: 640 + i * 360, left: 0, right: 0, display: 'flex', alignItems: 'center',
              justifyContent: 'center', gap: 44, opacity: t * (1 - leave), transform: `translateY(${(1 - t) * 40}px)`,
            }}
          >
            <div
              style={{
                fontFamily: FONT_STAMP, fontSize: 190, color: COLORS.stamp, width: 360, textAlign: 'right',
                textShadow: '0 0 24px rgba(255,138,42,0.65), 0 0 60px rgba(255,90,20,0.35)',
              }}
            >
              {num}
            </div>
            <div style={{width: 380}}>
              <div style={{fontFamily: FONT_CN, fontSize: 64, fontWeight: 600, color: COLORS.paper}}>{cn}</div>
              <div style={{fontFamily: FONT, fontSize: 24, letterSpacing: '0.3em', color: COLORS.muted, marginTop: 8}}>{en}</div>
            </div>
          </div>
        );
      })}
    </AbsoluteFill>
  );
};

/* 27-30 s: the camera rises to the 3/4 hero view, the viewfinder on a sunset; the name settles, then the light goes out. */
export const Finale: React.FC = () => {
  const frame = useCurrentFrame();
  const lightsOut = anim(frame, [78, 90], [0, 1], EXPO_IN);
  return (
    <AbsoluteFill>
      <Plate3D shot="finale" />
      <div style={{position: 'absolute', top: 180, left: 0, right: 0, textAlign: 'center'}}>
        <CharReveal
          text="MOSAICO FILM"
          start={8}
          stagger={2}
          style={{fontFamily: FONT, fontSize: 100, fontWeight: 300, letterSpacing: '0.16em', color: COLORS.paper}}
        />
        <div
          style={{
            fontFamily: FONT, fontSize: 28, fontWeight: 500, color: COLORS.amber, marginTop: 26,
            letterSpacing: `${0.6 - 0.2 * anim(frame, [24, 50], [0, 1])}em`, opacity: anim(frame, [24, 40], [0, 1]),
          }}
        >
          ESP-MOSAICO
        </div>
      </div>
      <AbsoluteFill style={{background: '#000', opacity: lightsOut}} />
    </AbsoluteFill>
  );
};
