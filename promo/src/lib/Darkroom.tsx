import React from 'react';
import {AbsoluteFill, staticFile, useCurrentFrame} from 'remotion';
import {COLORS, hash} from './theme';

type BenchProps = {glow?: string; glowY?: string; strength?: number};

/* The darkroom bench: near-black with a warm pool of light where the subject sits. */
export const Bench: React.FC<BenchProps> = ({glow = '255,170,100', glowY = '46%', strength = 0.13}) => (
  <AbsoluteFill
    style={{
      background: `radial-gradient(ellipse 75% 45% at 50% ${glowY}, rgba(${glow},${strength}) 0%, rgba(${glow},0) 70%), ${COLORS.bench}`,
    }}
  />
);

/* Film grain over the whole picture: one tile, jumped to a new offset every frame so it crawls like stock. */
export const Grain: React.FC<{opacity?: number}> = ({opacity = 0.11}) => {
  const frame = useCurrentFrame();
  const x = Math.floor(hash(frame) * 512);
  const y = Math.floor(hash(frame + 1000) * 512);
  return (
    <AbsoluteFill
      style={{
        backgroundImage: `url(${staticFile('fx/grain.png')})`,
        backgroundPosition: `${x}px ${y}px`,
        mixBlendMode: 'overlay',
        opacity,
        pointerEvents: 'none',
      }}
    />
  );
};

/* Edge falloff, as from an enlarger lens. */
export const Vignette: React.FC = () => (
  <AbsoluteFill
    style={{background: 'radial-gradient(ellipse 85% 70% at 50% 50%, rgba(0,0,0,0) 55%, rgba(0,0,0,0.55) 100%)', pointerEvents: 'none'}}
  />
);
