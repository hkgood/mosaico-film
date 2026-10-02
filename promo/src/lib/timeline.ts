import timeline from '../timeline.json';

export type SceneId = keyof typeof timeline.scenes;

export const FPS = timeline.fps;
export const WIDTH = timeline.width;
export const HEIGHT = timeline.height;
export const DURATION = timeline.durationInFrames;
/* Frames per musical beat (15 at 120 BPM); cuts and hits land on multiples of this. */
export const BEAT = (60 / timeline.bpm) * timeline.fps;

export const scene = (id: SceneId) => timeline.scenes[id];

/* A clip placement: from scene frame `at`, show clip frame `src + (frame - at) * rate` for `duration` frames. */
export type ClipUse = {clip: string; at: number; src: number; rate: number; duration: number};

/* The clip placements of one scene, as listed in timeline.json (shared with the audio mix). */
export const clipsOf = (id: SceneId): ClipUse[] =>
  timeline.clips.filter((c) => c.scene === id).map(({scene: _s, ...rest}) => rest);

/* The placement active at a scene frame (the last one that has started). */
export const clipAt = (id: SceneId, frame: number): ClipUse => {
  const uses = clipsOf(id);
  if (uses.length === 0) throw new Error(`scene ${id} has no clip`);
  return uses.filter((u) => u.at <= frame).pop() ?? uses[0];
};
