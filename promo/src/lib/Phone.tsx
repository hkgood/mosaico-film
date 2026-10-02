import React from 'react';
import {Img} from 'remotion';
import {frameUrl, printUrl, UI_SIZE} from './clip';
import {anim, FONT} from './theme';

/* A generic modern phone, sized by its outer width; content is laid out in 390 CSS-px "phone points". */
const POINTS = 390;
const ASPECT = 2.06;
const FRAME = 0.035;

type PhoneProps = {width: number; children: (scale: number) => React.ReactNode; style?: React.CSSProperties};

export const Phone: React.FC<PhoneProps> = ({width, children, style}) => {
  const height = width * ASPECT;
  const frame = width * FRAME;
  const screenW = width - 2 * frame;
  return (
    <div
      style={{
        position: 'relative', width, height, borderRadius: width * 0.16, background: 'linear-gradient(150deg, #3a3835, #121110)',
        boxShadow: `0 ${width * 0.06}px ${width * 0.16}px rgba(0,0,0,0.7), inset 0 0 0 2px rgba(255,255,255,0.12)`,
        ...style,
      }}
    >
      <div
        style={{
          position: 'absolute', left: frame, top: frame, width: screenW, height: height - 2 * frame,
          borderRadius: width * 0.13, overflow: 'hidden', background: '#000',
        }}
      >
        {children(screenW / POINTS)}
        {/* Dynamic-island style camera cutout */}
        <div
          style={{
            position: 'absolute', top: screenW * 0.03, left: '50%', width: screenW * 0.3, height: screenW * 0.085,
            transform: 'translateX(-50%)', borderRadius: 999, background: '#000',
          }}
        />
      </div>
    </div>
  );
};

/* Where the QR code sits on the captured share screen (480 x 480 UI pixels). */
const QR_CENTER = {x: 128, y: 165};

type ScanProps = {scale: number; shareFrame: number; t: number};

/* The phone camera pointed at the device: the real share screen fills the view, a bracket locks onto the QR,
 * then the address pill drops in. `t` runs 0..1 over the scan. */
export const ScanView: React.FC<ScanProps> = ({scale, shareFrame, t}) => {
  const view = POINTS * scale;
  const zoom = (1.5 + 0.15 * t) * scale;            // UI pixels -> screen pixels, creeping in while it locks
  const lock = anim(t, [0.15, 0.55], [0, 1]);
  const bracket = (1.3 - 0.3 * lock) * 165 * zoom;  // the QR modules span about 150 UI pixels
  const pill = anim(t, [0.55, 0.8], [0, 1]);
  return (
    <div style={{position: 'absolute', inset: 0, background: '#000', overflow: 'hidden'}}>
      <Img
        src={frameUrl('share', shareFrame)}
        style={{
          position: 'absolute', width: UI_SIZE * zoom, height: UI_SIZE * zoom,
          left: view / 2 - QR_CENTER.x * zoom, top: 330 * scale - QR_CENTER.y * zoom,
          filter: 'blur(0.6px) saturate(0.9)', transform: `rotate(${-3 + 2 * lock}deg)`,
        }}
      />
      <div
        style={{
          position: 'absolute', left: view / 2 - bracket / 2, top: 330 * scale - bracket / 2, width: bracket, height: bracket,
          border: `${3 * scale}px solid #ffd60a`, borderRadius: 18 * scale, opacity: 0.4 + 0.6 * lock,
        }}
      />
      <div
        style={{
          position: 'absolute', left: '50%', top: 560 * scale, transform: `translate(-50%, ${(1 - pill) * 20 * scale}px)`,
          opacity: pill, padding: `${12 * scale}px ${22 * scale}px`, borderRadius: 999, background: 'rgba(255,214,10,0.95)',
          color: '#111', fontFamily: FONT, fontWeight: 600, fontSize: 17 * scale, whiteSpace: 'nowrap',
        }}
      >
        http://192.168.1.23
      </div>
    </div>
  );
};

type PageProps = {scale: number; photos: string[]; scroll: number; savedFlash: number};

/* The album page the device serves (main/film_share.c), rebuilt in the same colours, type and layout. */
export const SharePage: React.FC<PageProps> = ({scale, photos, scroll, savedFlash}) => {
  const s = (v: number) => v * scale;
  return (
    <div style={{position: 'absolute', inset: 0, background: '#0e0e0d', color: '#efe7d6', overflow: 'hidden'}}>
      <div style={{transform: `translateY(${-s(scroll)}px)`, paddingTop: s(54)}}>
        <div style={{padding: `${s(26)}px ${s(18)}px ${s(6)}px`}}>
          <div style={{font: `600 ${s(21)}px Georgia, serif`, letterSpacing: '.12em'}}>MOSAICO FILM</div>
          <div style={{color: '#9c958a', margin: `${s(8)}px 0 0`, lineHeight: 1.5, fontSize: s(15), fontFamily: '"PingFang SC"'}}>
            {photos.length} 张照片 · 点「保存」或长按图片存到手机
          </div>
        </div>
        <div style={{display: 'grid', gap: s(16), padding: s(14)}}>
          {photos.map((name, i) => (
            <div
              key={name}
              style={{background: '#1b1a18', borderRadius: s(8), overflow: 'hidden', boxShadow: `0 ${s(6)}px ${s(20)}px #0008`}}
            >
              <Img src={printUrl(name)} style={{width: '100%', display: 'block'}} />
              <div
                style={{
                  textAlign: 'center', padding: s(12), color: '#ff9a3c', fontWeight: 600, letterSpacing: '.06em',
                  fontSize: s(15), fontFamily: '"PingFang SC"',
                  background: i === 0 ? `rgba(255,154,60,${0.25 * savedFlash})` : undefined,
                }}
              >
                保存
              </div>
            </div>
          ))}
        </div>
      </div>
      {/* Safari-style address bar */}
      <div
        style={{
          position: 'absolute', left: s(14), right: s(14), bottom: s(26), height: s(46), borderRadius: s(16),
          background: 'rgba(40,38,35,0.92)', color: '#efe7d6', fontFamily: FONT, fontSize: s(15),
          display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}
      >
        192.168.1.23
      </div>
    </div>
  );
};
