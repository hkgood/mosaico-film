import React from 'react';
import {Composition} from 'remotion';
import {Promo, PromoProps} from './Promo';
import {DURATION, FPS, HEIGHT, WIDTH} from './lib/timeline';

export const Root: React.FC = () => (
  <Composition
    id="FilmPromo"
    component={Promo}
    durationInFrames={DURATION}
    fps={FPS}
    width={WIDTH}
    height={HEIGHT}
    defaultProps={{audio: true} satisfies PromoProps}
  />
);
