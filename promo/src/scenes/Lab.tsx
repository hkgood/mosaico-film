import React from 'react';
import {AbsoluteFill, useCurrentFrame} from 'remotion';
import {clipIndex, marksUpTo} from '../lib/clip';
import {Phone, ScanView, SharePage} from '../lib/Phone';
import {Plate3D} from '../lib/Plate';
import {clipsOf} from '../lib/timeline';
import {anim, COLORS, FONT} from '../lib/theme';
import {Caption} from '../lib/Type';

/* The redevelop drum starts on the photo's own film and turns one stock per drag (see promo_capture.c). */
const DRUM = ['NIGHT 800T', 'PIXEL 8BIT', 'GOLD 200', 'SOFT 400'];

/* 18-21 s: one night photo, re-developed on three other films by turning the drum. */
export const Redevelop: React.FC = () => {
  const frame = useCurrentFrame();
  const [use] = clipsOf('redevelop');
  const index = clipIndex(use, frame);
  const turns = marksUpTo(use.clip, index, ['sfx']).filter((m) => m.name === 'detent').length;
  const active = Math.min(DRUM.length - 1, turns);
  return (
    <AbsoluteFill>
      <Plate3D shot="redevelop" />
      <Caption overline="REDEVELOP" title="换一卷，再冲一次" sub="同一张底片，不同的胶卷" exit={82} />
      <div style={{position: 'absolute', top: 1500, left: 60, right: 60, display: 'flex', justifyContent: 'center', gap: 18}}>
        {DRUM.map((name, i) => (
          <div
            key={name}
            style={{
              fontFamily: FONT, fontSize: 26, fontWeight: 500, letterSpacing: '0.12em', padding: '14px 22px', borderRadius: 999,
              color: i === active ? COLORS.bench : COLORS.paper, background: i === active ? COLORS.amber : 'rgba(241,232,216,0.08)',
              border: `1px solid ${i === active ? COLORS.amber : 'rgba(241,232,216,0.2)'}`,
            }}
          >
            {name}
          </div>
        ))}
      </div>
    </AbsoluteFill>
  );
};

/* Share scene beats (scene frames): the phone rises, scans the QR, opens the page, taps 保存.
 * PHONE_IN must match shareShot() in render3d/shots.js, where the camera moves the device aside. */
const PHONE_IN = 40;
const SCAN = [44, 62] as const;
const SAVE_AT = 76;
/* A share-clip frame where the QR card is fully drawn, for the phone camera's view. */
const QR_FRAME = 200;
const SHARED = ['shared_0', 'shared_1', 'shared_2'];
const PHONE_W = 420;

/* 21-24 s: pick three photos, SEND TO PHONE, scan the QR and save them on the phone. */
export const Share: React.FC = () => {
  const frame = useCurrentFrame();
  const rise = anim(frame, [PHONE_IN, PHONE_IN + 12], [0, 1]);
  const scanT = anim(frame, [SCAN[0], SCAN[1]], [0, 1], (t) => t);
  const scroll = anim(frame, [SCAN[1] + 4, 90], [0, 150]);
  const saved = anim(frame, [SAVE_AT, SAVE_AT + 2], [0, 1]) * anim(frame, [SAVE_AT + 4, SAVE_AT + 14], [1, 0]);
  return (
    <AbsoluteFill>
      {/* The 3D camera steps the device back to the left as the phone comes in. */}
      <Plate3D shot="share" />
      <Caption overline="SEND TO PHONE" title="扫一下，存进手机" sub="不装 App，网页直接保存" exit={82} />
      {rise > 0 && (
        <Phone
          width={PHONE_W}
          style={{position: 'absolute', left: 1080 - PHONE_W - 70, top: 1000 + (1 - rise) * 900, transform: `rotate(${4 - 2 * rise}deg)`}}
        >
          {(k) =>
            frame < SCAN[1] ? (
              <ScanView scale={k} shareFrame={QR_FRAME} t={scanT} />
            ) : (
              <SharePage scale={k} photos={SHARED} scroll={scroll} savedFlash={saved} />
            )
          }
        </Phone>
      )}
    </AbsoluteFill>
  );
};
