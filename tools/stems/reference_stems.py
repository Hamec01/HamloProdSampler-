"""Reference stems from Python demucs, to check HPStems.exe against.

Usage:  python -I reference_stems.py <audio file> <out dir> [seconds=30]
Writes <out dir>/input.wav (44.1 kHz stereo, first N seconds) and ref_<stem>.wav, using the same
settings HPStems uses: htdemucs, split, overlap 0.25, no random shifts.
"""
import sys
from pathlib import Path

import torch
import torchaudio
from demucs.apply import apply_model
from demucs.audio import AudioFile, save_audio
from demucs.pretrained import get_model


def main():
    src, out = sys.argv[1], Path(sys.argv[2])
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
    out.mkdir(parents=True, exist_ok=True)
    bag = get_model("htdemucs")
    sr = bag.samplerate
    wav = AudioFile(src).read(streams=0, samplerate=sr, channels=2)
    wav = wav[:, : int(seconds * sr)].contiguous()
    save_audio(wav, str(out / "input.wav"), sr, as_float=True)

    ref = wav.mean(0)
    mean, std = ref.mean(), ref.std()
    x = (wav - mean) / std
    with torch.no_grad():
        stems = apply_model(bag, x[None], shifts=0, split=True, overlap=0.25, progress=False)[0]
    stems = stems * std + mean
    for name, s in zip(bag.sources, stems):
        save_audio(s, str(out / f"ref_{name}.wav"), sr, as_float=True)
    print("ok", wav.shape, flush=True)


if __name__ == "__main__":
    main()
