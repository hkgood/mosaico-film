#!/usr/bin/env python3
"""Synthesizes the Mosaico Film demo soundtrack, locked to promo/src/timeline.json.

A warm 120 BPM electric-piano score with vinyl crackle (the darkroom mood), plus the app's own
sounds placed on the frames where the captured clips raised them. The app sounds are a direct
port of main/film_feedback.c synthesize(): same 16 kHz rate, same LCG noise, same formulas,
resampled to the mix rate. Everything is generated here; no samples.

Usage (needs numpy, scipy and ffmpeg; see promo/package.json "audio"):
    ~/.venvs/film-promo/bin/python projects/mosaico_film/tools/promo/promo_audio.py
"""
from __future__ import annotations

import json
import math
import subprocess
import tempfile
import wave
from pathlib import Path

import numpy as np
from scipy.signal import butter, fftconvolve, lfilter, resample_poly, sosfilt

PROMO = Path(__file__).resolve().parents[2] / "promo"
TIMELINE = json.loads((PROMO / "src/timeline.json").read_text())
MANIFEST = json.loads((PROMO / "public/manifest.json").read_text())
OUTPUT = PROMO / "public/audio/soundtrack.wav"

SR = 48000
FPS = TIMELINE["fps"]
BEAT = 60.0 / TIMELINE["bpm"]
BAR = 4 * BEAT
LENGTH = TIMELINE["durationInFrames"] / FPS
N = int(LENGTH * SR)
RNG = np.random.default_rng(1977)

# Fmaj9 - Em7 - Dm9 - Cmaj7, one chord per bar: a slow, nostalgic descent (MIDI notes, root first).
CHORDS = [
    (41, 57, 60, 64, 67, 69),
    (40, 55, 59, 62, 64, 67),
    (38, 53, 57, 60, 62, 64),
    (36, 52, 55, 59, 60, 64),
]


def sec(frame: float) -> float:
    return frame / FPS


def scene_at(name: str, frame: float = 0) -> float:
    return sec(TIMELINE["scenes"][name]["from"] + frame)


def hz(note: float) -> float:
    return 440.0 * 2 ** ((note - 69) / 12)


def t_axis(duration: float) -> np.ndarray:
    return np.arange(int(duration * SR)) / SR


def lowpass(x: np.ndarray, cutoff: float) -> np.ndarray:
    return sosfilt(butter(2, cutoff, fs=SR, output="sos"), x)


def highpass(x: np.ndarray, cutoff: float) -> np.ndarray:
    return sosfilt(butter(2, cutoff, btype="high", fs=SR, output="sos"), x)


def bandpass(x: np.ndarray, lo: float, hi: float) -> np.ndarray:
    return sosfilt(butter(2, [lo, hi], btype="band", fs=SR, output="sos"), x)


class Bus:
    """A stereo buffer; `send` feeds the shared reverb."""

    def __init__(self):
        self.dry = np.zeros((2, N))
        self.send = np.zeros((2, N))

    def add(self, start: float, mono: np.ndarray, gain: float = 1.0, pan: float = 0.0, reverb: float = 0.0):
        s0 = int(start * SR)
        if s0 >= N or s0 + len(mono) <= 0:
            return
        a, b = max(0, -s0), min(len(mono), N - s0)
        seg = mono[a:b] * gain
        left, right = math.sqrt((1 - pan) / 2), math.sqrt((1 + pan) / 2)
        for ch, k in ((0, left), (1, right)):
            self.dry[ch, s0 + a:s0 + b] += seg * k
            if reverb:
                self.send[ch, s0 + a:s0 + b] += seg * k * reverb


# ---- the app's sounds (port of main/film_feedback.c) -------------------------------------

APP_RATE = 16000
APP_SECONDS = {"shutter": 0.16, "detent": 0.025, "click": 0.05, "lever": 0.20, "shake": 0.30, "error": 0.30,
               "boot": 0.70}
APP_KIND = {"shutter": 0, "detent": 1, "click": 2, "lever": 3, "shake": 4, "error": 5, "boot": 6}


class AppClip:
    """One feedback clip at the device rate; methods mirror the C helpers one to one."""

    def __init__(self, name: str):
        self.x = np.zeros(int(APP_SECONDS[name] * APP_RATE))
        self.seed = (0xF11A + APP_KIND[name]) & 0xFFFFFFFF

    def _mix(self, first: int, values: np.ndarray):
        end = min(len(self.x), first + len(values))
        if first < end:
            self.x[first:end] += values[:end - first]

    def _noise(self, n: int) -> np.ndarray:
        out = np.empty(n)
        for i in range(n):
            self.seed = (self.seed * 1664525 + 1013904223) & 0xFFFFFFFF
            out[i] = (self.seed >> 8) / 8388608.0 - 1.0
        return out

    def ring(self, start, dur, freq, amp, decay):
        t = np.arange(int(dur * APP_RATE)) / APP_RATE
        self._mix(int(start * APP_RATE), amp * np.minimum(1, t * 2000) * np.exp(-decay * t) * np.sin(2 * np.pi * freq * t))

    def hit(self, start, dur, amp, smooth, decay):
        n = int(dur * APP_RATE)
        lp = lfilter([smooth], [1, -(1 - smooth)], self._noise(n))
        self._mix(int(start * APP_RATE), amp * np.exp(-decay * np.arange(n) / APP_RATE) * lp)

    def glide(self, start, dur, f0, f1, amp):
        n = int(dur * APP_RATE)
        t = np.arange(n) / n
        phase = np.cumsum(2 * np.pi * f0 * (f1 / f0) ** t / APP_RATE)
        self._mix(int(start * APP_RATE), amp * np.sin(np.pi * t) * np.sin(phase))

    def whoosh(self, start, dur, amp):
        n = int(dur * APP_RATE)
        t = np.arange(n) / n
        white = self._noise(n)
        smooth = 0.03 + 0.12 * np.sin(np.pi * t)
        lp, acc = np.empty(n), 0.0
        for i in range(n):
            acc += (white[i] - acc) * smooth[i]
            lp[i] = acc
        self._mix(int(start * APP_RATE), amp * np.sin(np.pi * t) * lp)

    def shutter(self, start, weight):
        self.hit(start, 0.03, 0.55 * weight, 0.75, 160)
        self.glide(start, 0.035, 180, 90, 0.35 * weight)
        self.ring(start + 0.002, 0.05, 2350, 0.10 * weight, 90)
        self.hit(start + 0.065, 0.03, 0.45 * weight, 0.6, 180)
        self.ring(start + 0.066, 0.06, 1650, 0.10 * weight, 70)


def app_sound(name: str) -> np.ndarray:
    c = AppClip(name)
    if name == "shutter":
        c.shutter(0, 1.0)
    elif name == "detent":
        c.hit(0, 0.012, 0.30, 0.9, 400)
        c.ring(0, 0.02, 3200, 0.06, 220)
    elif name == "click":
        c.hit(0, 0.02, 0.35, 0.7, 260)
        c.glide(0, 0.03, 1300, 900, 0.15)
    elif name == "lever":
        c.hit(0, 0.015, 0.22, 0.85, 300)
        c.hit(0.045, 0.015, 0.24, 0.85, 300)
        c.hit(0.09, 0.04, 0.45, 0.55, 120)
        c.ring(0.091, 0.08, 1900, 0.08, 60)
    elif name == "shake":
        c.whoosh(0, 0.30, 0.45)
    elif name == "error":
        c.glide(0, 0.12, 240, 200, 0.30)
        c.glide(0.15, 0.14, 200, 160, 0.30)
    elif name == "boot":
        c.shutter(0, 1.25)
        c.glide(0, 0.12, 110, 55, 0.35)
        c.glide(0.24, 0.40, 300, 360, 0.10)
        c.whoosh(0.24, 0.40, 0.15)
    return resample_poly(np.clip(c.x, -1, 1), SR // APP_RATE, 1)


def app_cues() -> list[tuple[float, str]]:
    """The app's recorded sounds, mapped through each clip placement to film time."""
    cues = []
    for use in TIMELINE["clips"]:
        scene = TIMELINE["scenes"][use["scene"]]
        for m in MANIFEST["clips"][use["clip"]]["marks"]:
            if m["type"] != "sfx":
                continue
            local = (m["frame"] - use["src"]) / use["rate"]
            if 0 <= local < use["duration"]:
                cues.append((sec(scene["from"] + use["at"] + local), m["name"]))
    return sorted(cues)


# ---- instruments -------------------------------------------------------------------------

def env(n: int, attack: float, release: float) -> np.ndarray:
    t = np.arange(n) / SR
    return np.minimum(1, t / max(1e-4, attack)) * np.clip((n / SR - t) / max(1e-4, release), 0, 1)


def epiano(note: float, duration: float = 1.6, velocity: float = 1.0) -> np.ndarray:
    """A Rhodes-like tine: FM body whose brightness decays, plus a short metallic bark."""
    t = t_axis(duration)
    f = hz(note)
    index = (0.4 + 1.6 * velocity) * np.exp(-t * 3.0)
    body = np.sin(2 * np.pi * f * t + index * np.sin(2 * np.pi * f * t))
    tine = 0.18 * velocity * np.sin(2 * np.pi * f * 7.02 * t) * np.exp(-t * 28)
    trem = 1 + 0.06 * np.sin(2 * np.pi * 4.8 * t)
    return (body + tine) * trem * np.exp(-t * 1.6) * env(len(t), 0.003, 0.25)


def pad(chord, duration: float, cutoff: float = 1600) -> np.ndarray:
    t = t_axis(duration)
    out = np.zeros_like(t)
    for note in chord[1:]:
        for detune in (-0.06, 0.06):
            f = hz(note + detune)
            phase = RNG.uniform(0, 2 * np.pi)
            out += sum(np.sin(2 * np.pi * f * k * t + phase) / k for k in range(1, 6))
    return lowpass(out, cutoff) * env(len(t), 0.6, 0.8) / (2 * len(chord))


def bass(note: float, duration: float) -> np.ndarray:
    t = t_axis(duration)
    f = hz(note)
    tone = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(4 * np.pi * f * t) * np.exp(-t * 6)
    return tone * env(len(t), 0.006, 0.06)


def kick(power: float = 1.0) -> np.ndarray:
    t = t_axis(0.4)
    freq = 48 + 95 * np.exp(-t * 30)
    body = np.sin(np.cumsum(2 * np.pi * freq / SR)) * np.exp(-t * 8)
    return power * np.tanh(1.4 * body)


def rim() -> np.ndarray:
    """A dry rim-shot/snap, softer than a clap: suits the camera clicks around it."""
    t = t_axis(0.12)
    tone = np.sin(2 * np.pi * 1750 * t) * np.exp(-t * 90)
    noise = bandpass(RNG.uniform(-1, 1, len(t)), 1500, 6000) * np.exp(-t * 50)
    return 0.5 * tone + 0.7 * noise


def hat(open_: bool = False) -> np.ndarray:
    t = t_axis(0.22 if open_ else 0.05)
    return highpass(RNG.uniform(-1, 1, len(t)), 7500) * np.exp(-t * (16 if open_ else 80))


def bell(note: float, duration: float = 2.4, decay: float = 2.2) -> np.ndarray:
    t = t_axis(duration)
    f = hz(note)
    partials = ((1, 1.0), (2.76, 0.4), (5.4, 0.2), (8.93, 0.1))
    return sum(a * np.sin(2 * np.pi * f * r * t) * np.exp(-t * decay * r ** 0.5) for r, a in partials) * np.minimum(1, t * 1500)


def impact(size: float = 1.0) -> np.ndarray:
    t = t_axis(2.2)
    freq = 30 + 55 * np.exp(-t * 4)
    sub = np.sin(np.cumsum(2 * np.pi * freq / SR)) * np.exp(-t * 2.0)
    air = lowpass(RNG.uniform(-1, 1, len(t)), 4000) * np.exp(-t * 3.0) * 0.25
    return size * np.tanh(1.2 * (sub + air))


def swell(duration: float) -> np.ndarray:
    t = t_axis(duration)
    return lowpass(RNG.uniform(-1, 1, len(t)), 3500) * (t / duration) ** 3


def beep(note: float) -> np.ndarray:
    """The date-back beep: a short soft square-ish tone, for the numbers lit like the date stamp."""
    t = t_axis(0.16)
    f = hz(note)
    tone = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(6 * np.pi * f * t)
    return tone * np.exp(-t * 22) * np.minimum(1, t * 600)


def vinyl(duration: float) -> np.ndarray:
    """Surface hiss and sparse crackle: the darkroom's record player."""
    n = int(duration * SR)
    hiss = bandpass(RNG.normal(0, 1, n), 900, 7000) * 0.06
    pops = np.zeros(n)
    for at in RNG.integers(0, n - 200, int(duration * 9)):
        pops[at:at + 60] += RNG.uniform(0.2, 1.0) * np.exp(-np.arange(60) / 8) * RNG.choice((-1, 1))
    return hiss + highpass(pops, 1500) * 0.5


# ---- score -------------------------------------------------------------------------------

def chord_at(time: float):
    return CHORDS[int(time // BAR) % len(CHORDS)]


def write_pads(bus: Bus, start: float, end: float, gain: float, cutoff: float = 1600):
    bar = math.floor(start / BAR) * BAR
    while bar < end - 1e-6:
        a = max(bar, start)
        length = min(bar + BAR, end) - a
        if length > 0.05:
            bus.add(a, pad(chord_at(bar), length + 0.6, cutoff), gain, 0, reverb=0.5)
        bar += BAR


def write_keys(bus: Bus, start: float, end: float, gain: float):
    """Electric piano comping: the chord on beat 1, then pushed off-beat stabs."""
    beat = start
    while beat < end - 1e-6:
        in_bar = int(round((beat % BAR) / BEAT)) % 4
        chord = chord_at(beat)
        if in_bar == 0:
            for k, note in enumerate(chord[1:]):
                bus.add(beat + k * 0.012, epiano(note, 1.8, 0.8), gain / 4, -0.3 + 0.15 * k, reverb=0.45)
        elif in_bar == 2:
            for note in chord[2:5]:
                bus.add(beat + BEAT / 2, epiano(note + 12, 0.6, 0.5), gain / 6, 0.3, reverb=0.45)
        beat += BEAT


def write_groove(bus: Bus, start: float, end: float, level: float = 1.0, hats: bool = True):
    beat = start
    while beat < end - 1e-6:
        in_bar = int(round((beat % BAR) / BEAT)) % 4
        if in_bar in (0, 2):
            bus.add(beat, kick(), 0.8 * level)
        if in_bar == 2 or (in_bar == 3 and RNG.uniform() < 0.3):
            bus.add(beat + (BEAT / 2 if in_bar == 3 else 0), kick(0.6), 0.5 * level)
        if in_bar in (1, 3):
            bus.add(beat, rim(), 0.32 * level, 0.15, reverb=0.3)
        if hats:
            for q in range(2):
                bus.add(beat + q * BEAT / 2 + (0.012 if q else 0), hat(), 0.10 * (1.0 if q else 0.6) * level, 0.35)
        root = chord_at(beat)[0]
        bus.add(beat + 0.02, bass(root, BEAT * 0.9), 0.32 * level)
        beat += BEAT


def compose() -> np.ndarray:
    bus = Bus()
    title, films = scene_at("title"), scene_at("films")
    key_press = scene_at("shutter", 30)
    share, numbers, finale = scene_at("share"), scene_at("numbers"), scene_at("finale")

    bus.add(0, vinyl(LENGTH), 0.55)

    # Boot: lights up on a lone low pad; the boot sound (app cue) lands inside the animation.
    write_pads(bus, 0, title, 0.45, cutoff=700)
    bus.add(title - 0.9, swell(0.9), 0.22)

    # Title: the downbeat, a bell over the first electric-piano chord.
    bus.add(title, impact(0.6), 0.6, reverb=0.5)
    bus.add(title, bell(81, 2.6, 1.6), 0.13, 0.1, reverb=0.8)
    write_keys(bus, title, films, 0.7)
    write_pads(bus, title, films, 0.35)

    # Camera features: the groove, dropping out for one beat before the red key so the shutter speaks alone.
    write_pads(bus, films, share, 0.3)
    write_keys(bus, films, share, 0.6)
    write_groove(bus, films, key_press - BEAT)
    write_groove(bus, key_press, share)
    bus.add(key_press, impact(0.5), 0.45, reverb=0.4)
    bus.add(scene_at("shutter", 27), app_sound("click"), 0.5)     # the red key bottoming out in the macro shot
    for name in ("films", "bodies", "develop", "redevelop"):
        bus.add(scene_at(name), hat(open_=True), 0.16, -0.3, reverb=0.3)

    # Share: lighter, hats only on the off-beats.
    write_pads(bus, share, numbers, 0.3)
    write_keys(bus, share, numbers, 0.55)
    write_groove(bus, share, numbers, level=0.7, hats=False)

    # Numbers: a date-back beep and a chord stab on every stat.
    write_pads(bus, numbers, finale, 0.35, cutoff=2400)
    write_groove(bus, numbers, finale - BEAT, level=0.85)
    for k in range(3):
        at = scene_at("numbers", 8 + 15 * k)
        bus.add(at, beep(88 + (0, 3, 7)[k]), 0.18, (-0.3, 0.3, 0)[k], reverb=0.3)
        for note in chord_at(at)[2:]:
            bus.add(at, epiano(note + 12, 0.8, 1.0), 0.06, 0, reverb=0.5)

    # Finale: the progression resolves on F, a bell melody, and one last shutter as the light goes out.
    write_pads(bus, finale, LENGTH, 0.4, cutoff=1200)
    for k, note in enumerate((53, 57, 60, 64, 67, 72)):
        bus.add(finale + k * 0.03, epiano(note, 3.0, 0.7), 0.12, -0.4 + 0.16 * k, reverb=0.7)
    for k, note in enumerate((76, 79, 81, 84)):
        bus.add(finale + 0.5 + k * BEAT, bell(note, 2.2, 2.0), 0.08, 0.2 * (-1) ** k, reverb=0.8)
    bus.add(scene_at("finale", 78), app_sound("shutter"), 0.7, 0, reverb=0.3)

    # The app itself, on the frames where the capture heard it.
    for at, name in app_cues():
        gain = {"detent": 0.55, "click": 0.6}.get(name, 0.9)
        bus.add(at, app_sound(name), gain, 0, reverb=0.15)

    return mixdown(bus)


def mixdown(bus: Bus) -> np.ndarray:
    t = t_axis(2.4)
    impulse = np.stack([lowpass(RNG.uniform(-1, 1, len(t)), 6500) * np.exp(-t * 2.6) for _ in range(2)])
    impulse /= np.sqrt(np.sum(impulse ** 2, axis=1, keepdims=True))
    wet = np.stack([fftconvolve(bus.send[ch], impulse[ch])[:N] for ch in range(2)])
    mix = highpass(bus.dry + 0.55 * wet, 30)
    mix *= np.clip((LENGTH - np.arange(N) / SR) / 0.6, 0, 1)
    peak = np.max(np.abs(mix))
    return np.tanh(1.15 * mix / max(1e-6, peak)) * 0.9


def main() -> int:
    mix = compose()
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="film-audio-") as tmp:
        raw = Path(tmp) / "raw.wav"
        with wave.open(str(raw), "wb") as out:
            out.setnchannels(2)
            out.setsampwidth(2)
            out.setframerate(SR)
            out.writeframes((np.clip(mix.T, -1, 1) * 32767).astype(np.int16).tobytes())
        # Phones play short videos around -14 LUFS.
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(raw), "-af", "loudnorm=I=-14:TP=-1.2:LRA=9",
                        "-ar", str(SR), "-c:a", "pcm_s16le", str(OUTPUT)], check=True)
    cues = app_cues()
    print(f"wrote {OUTPUT} ({LENGTH:.1f} s); {len(cues)} app cues:")
    print("  " + ", ".join(f"{at:.2f}s {name}" for at, name in cues))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
