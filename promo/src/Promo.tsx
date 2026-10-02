import React from 'react';
import {AbsoluteFill, Html5Audio, Sequence, staticFile} from 'remotion';
import './lib/fonts';
import {Grain, Vignette} from './lib/Darkroom';
import {scene, SceneId} from './lib/timeline';
import {Bodies, Develop, Films, Shutter} from './scenes/Camera';
import {Finale, Numbers} from './scenes/Closing';
import {Redevelop, Share} from './scenes/Lab';
import {Boot, Title} from './scenes/Opening';

export type PromoProps = {audio: boolean};

const SCENES: [SceneId, React.FC][] = [
  ['boot', Boot],
  ['title', Title],
  ['films', Films],
  ['shutter', Shutter],
  ['bodies', Bodies],
  ['develop', Develop],
  ['redevelop', Redevelop],
  ['share', Share],
  ['numbers', Numbers],
  ['finale', Finale],
];

/* The whole film: scenes laid out by timeline.json, the darkroom grain and lens falloff over everything,
 * and the beat-synced soundtrack that tools/promo/promo_audio.py renders from the same timeline. */
export const Promo: React.FC<PromoProps> = ({audio}) => (
  <AbsoluteFill style={{background: '#000'}}>
    {SCENES.map(([id, Scene]) => {
      const {from, duration} = scene(id);
      return (
        <Sequence key={id} name={id} from={from} durationInFrames={duration}>
          <Scene />
        </Sequence>
      );
    })}
    <Vignette />
    <Grain />
    {audio && <Html5Audio src={staticFile('audio/soundtrack.wav')} />}
  </AbsoluteFill>
);
