import React from 'react';
import {useCurrentFrame} from 'remotion';
import {anim, COLORS, EXPO_IN, FONT, FONT_CN} from './theme';

type CharProps = {
  text: string;
  start: number;          // frame the first character begins
  stagger?: number;       // frames between characters
  length?: number;        // frames each character takes to settle
  exit?: number;          // frame the whole line begins to leave
  style?: React.CSSProperties;
};

/* Per-character reveal: each glyph rises out of a soft blur, like type surfacing in developer. */
export const CharReveal: React.FC<CharProps> = ({text, start, stagger = 2, length = 16, exit, style}) => {
  const frame = useCurrentFrame();
  const leave = exit === undefined ? 0 : anim(frame, [exit, exit + 8], [0, 1], EXPO_IN);
  return (
    <div style={{whiteSpace: 'pre', ...style}}>
      {Array.from(text).map((ch, i) => {
        const t = anim(frame, [start + i * stagger, start + i * stagger + length], [0, 1]);
        return (
          <span
            key={i}
            style={{
              display: 'inline-block',
              opacity: t * (1 - leave),
              filter: `blur(${(1 - t) * 10 + leave * 16}px)`,
              transform: `translateY(${(1 - t) * 0.35}em)`,
            }}
          >
            {ch}
          </span>
        );
      })}
    </div>
  );
};

type CaptionProps = {
  overline: string;       // small tracked Latin caps (amber)
  title: string;          // the Chinese headline
  sub?: string;           // optional second line
  start?: number;
  exit?: number;
  top?: number;
};

/* The scene caption: amber overline, Chinese headline revealed per character, muted sub line. */
export const Caption: React.FC<CaptionProps> = ({overline, title, sub, start = 2, exit, top = 170}) => {
  const frame = useCurrentFrame();
  const line = anim(frame, [start, start + 18], [0, 1]);
  const leave = exit === undefined ? 0 : anim(frame, [exit, exit + 8], [0, 1], EXPO_IN);
  return (
    <div style={{position: 'absolute', top, left: 0, right: 0, textAlign: 'center'}}>
      <div
        style={{
          fontFamily: FONT, fontSize: 28, fontWeight: 500, color: COLORS.amber,
          letterSpacing: `${0.55 - 0.2 * line}em`, opacity: line * (1 - leave), marginBottom: 26,
        }}
      >
        {overline}
      </div>
      <CharReveal
        text={title}
        start={start + 3}
        stagger={2}
        exit={exit}
        style={{fontFamily: FONT_CN, fontSize: 82, fontWeight: 600, color: COLORS.paper, letterSpacing: '0.04em'}}
      />
      {sub && (
        <CharReveal
          text={sub}
          start={start + 10}
          stagger={1}
          exit={exit}
          style={{fontFamily: FONT_CN, fontSize: 40, fontWeight: 400, color: COLORS.muted, marginTop: 22, letterSpacing: '0.06em'}}
        />
      )}
    </div>
  );
};
