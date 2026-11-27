"""Ground-truth check of the bass notes HPStems writes.

Builds a mix with a known bass line (saw + low-pass, plucked envelope), chords and a real drum loop,
then compares HPStems' notes.json "bass" with the truth: onset within 60 ms and the same MIDI pitch.

Usage:
    python -I bass_truth_test.py make <dir> <drums audio file>
    HPStems.exe --in <dir>/mix.wav --out <dir>/stems
    python -I bass_truth_test.py score <dir>
"""
import json
import sys
from pathlib import Path

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sig

SR = 44100
BPM = 92.0
BEAT = 60.0 / BPM
# A minor: i - VI - III - VII, a two-bar riff per chord with octaves and passing notes (MIDI, beats)
RIFF = {
    45: [(0, 0, 1.0), (1.5, 0, 0.5), (2, 12, 0.5), (3, 0, 0.75), (3.75, -2, 0.25)],
    41: [(0, 0, 1.0), (1.5, 0, 0.5), (2, 7, 0.5), (3, 0, 1.0)],
    48: [(0, 0, 0.75), (1, 0, 0.5), (1.5, 7, 0.5), (2.5, 0, 0.5), (3, 4, 1.0)],
    43: [(0, 0, 1.0), (1.5, 0, 0.5), (2, 2, 0.5), (3, -1, 1.0)],
}
CHORDS = [(45, [57, 60, 64]), (41, [53, 57, 60]), (48, [55, 60, 64]), (43, [55, 59, 62])]


def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def saw_note(m, dur, sr=SR):
    n = int(dur * sr)
    t = np.arange(n) / sr
    f = midi_hz(m)
    x = sum(np.sin(2 * np.pi * f * k * t) / k for k in range(1, 14) if f * k < 6000)
    env = np.minimum(1.0, t / 0.005) * np.exp(-t * 2.2) * np.minimum(1.0, (dur - t) / 0.02).clip(0, 1)
    return x * env


def make(d, drums_file):
    d = Path(d)
    d.mkdir(parents=True, exist_ok=True)
    bars = 16
    total = int(bars * 4 * BEAT * SR) + SR
    bass = np.zeros(total)
    pad = np.zeros(total)
    truth = []
    for bar in range(bars):
        root, chord = CHORDS[(bar // 2) % 4]
        bar_t = bar * 4 * BEAT
        for beat, iv, length in RIFF[root]:
            m = root - 12 + iv  # bass register: A1 = 33 ...
            st = bar_t + beat * BEAT
            dur = length * BEAT * 0.92
            x = saw_note(m, dur)
            i = int(st * SR)
            bass[i:i + len(x)] += x
            truth.append([st, st + dur, m])
        if bar % 2 == 0:
            for m in chord:
                x = saw_note(m, 8 * BEAT * 0.98) * 0.35
                i = int(bar_t * SR)
                pad[i:i + len(x)] += x
    b, a = sig.butter(2, 900 / (SR / 2))
    bass = sig.lfilter(b, a, bass)
    b, a = sig.butter(2, [300 / (SR / 2), 5000 / (SR / 2)], btype="band")
    pad = sig.lfilter(b, a, pad)

    import subprocess, tempfile
    tmp = Path(tempfile.mkdtemp()) / "drums.wav"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", drums_file, "-ac", "1", "-ar", str(SR), str(tmp)], check=True)
    dr = wavfile.read(tmp)[1].astype(np.float64)
    dr /= np.abs(dr).max() + 1e-9
    drums = np.resize(dr, total)

    def norm(x, peak):
        return x / (np.abs(x).max() + 1e-9) * peak

    mix = norm(bass, 0.5) + norm(pad, 0.25) + norm(drums, 0.45)
    mix = np.stack([mix, mix], 1) / max(1.0, np.abs(mix).max() / 0.95)
    wavfile.write(d / "mix.wav", SR, mix.astype(np.float32))
    json.dump(truth, open(d / "truth.json", "w"))
    print("written", d / "mix.wav", len(truth), "bass notes")


def score(d):
    d = Path(d)
    truth = json.load(open(d / "truth.json"))
    notes = json.load(open(d / "stems" / "notes.json"))["stems"]["bass"]
    est = sorted([n for n in notes if n[2] <= 55], key=lambda n: n[0])  # bass register, as the plug-in reads it
    used = set()
    hit = octave = 0
    for t in truth:
        best = None
        for j, e in enumerate(est):
            if j in used or abs(e[0] - t[0]) > 0.06:
                continue
            if e[2] == t[2]:
                best = j
                break
            if (e[2] - t[2]) % 12 == 0 and best is None:
                best = -1 - j
        if best is not None and best >= 0:
            used.add(best)
            hit += 1
        elif best is not None:
            used.add(-1 - best)
            octave += 1
    p = hit / max(1, len(est))
    r = hit / max(1, len(truth))
    f = 2 * p * r / max(1e-9, p + r)
    print(f"truth {len(truth)}  found {len(est)}  exact {hit}  octave-off {octave}")
    print(f"precision {p:.3f}  recall {r:.3f}  F {f:.3f}")


if __name__ == "__main__":
    {"make": lambda: make(sys.argv[2], sys.argv[3]), "score": lambda: score(sys.argv[2])}[sys.argv[1]]()
