import React from 'react';
import {AbsoluteFill, Img, staticFile, useCurrentFrame} from 'remotion';
import {scene, SceneId} from './timeline';

/* A scene's 3D background plate: the device on the darkroom bench with its real UI on the screen,
 * the prints and the camera move, rendered one JPEG per scene frame by render3d/ (npm run render3d). */
export const Plate3D: React.FC<{shot: SceneId}> = ({shot}) => {
  const frame = useCurrentFrame();
  const index = Math.min(scene(shot).duration - 1, Math.max(0, frame));
  return (
    <AbsoluteFill>
      <Img src={staticFile(`render3d/${shot}/${String(index).padStart(4, '0')}.jpg`)} style={{width: '100%', height: '100%'}} />
    </AbsoluteFill>
  );
};
