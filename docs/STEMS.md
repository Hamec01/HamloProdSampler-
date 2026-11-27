# Stems (optional): DRUMS / BASS / VOX / INST

The SRC header has a stem strip between **USE AS PROJECT ANCHOR** and **WAVE**: a round A/B button
and four cells (DRUMS, BASS, VOX, INST). It rebalances a source the way iZotope Music Rebalance
or Serato Stems do. Pads, the fitted preview and the waveform all use the rebalanced audio.

| Action on a cell | Result |
| --- | --- |
| drag up / down (Shift = fine) | level, -60 dB (off) .. +12 dB; the line in the cell shows it, 0 dB is the tick |
| click | mute / unmute |
| double-click | 0 dB |
| right-click | solo, "without this stem", reset all, A/B, split again, delete stems |
| round button | A/B: the original vs. the stem mix (the mix is kept) |

Levels are part of the project (saved with the DAW project, undo / redo). The stems themselves are not:
they live in a disk cache and come back on their own when a project with a stem mix is opened.

## How it works

* The split is **optional and heavy**, so it never starts by itself. The first click shows a warning with
  the estimated time, RAM and disk use. The user can cancel at any time by clicking the progress bar.
* `HPStems.exe` does the work **in its own process** at below-normal priority, using cores - 1.
  ONNX Runtime and the model are never loaded into the DAW.
* The plug-in writes the decoded source as `input.wav`, so the stems line up with the source frame for frame.
  HPStems resamples to 44.1 kHz for the model and back to the source rate.
* Model: HTDemucs v4 (`htdemucs`, MIT, Meta), 4 sources: drums, bass, other (= INST), vocals.
  The ONNX graph excludes the STFT. HPStems does the STFT / iSTFT, 7.8 s segments, 25 % overlap
  and the triangle cross-fade the same way as `demucs.apply.apply_model` (no random shifts).
  Checked against Python demucs on a 30 s song: 77-92 dB SNR per stem.
* Speed (CPU, 8 cores): about 0.6 s per second of audio. A 4-minute song takes about 2.5 minutes.
* Cache: `Documents\HamloProd\HPSAMPLER\Stems\<name>_<hash>\{drums,bass,vocals,other}.wav + stems.json`.
  The cache key is the file path, size, modification time, frame count and rate.
  `stems.json` is written last; without it, a cache folder counts as incomplete.

## Notes for HPDG (bass line)

* After the split, HPStems transcribes the BASS, VOX and INST stems with Basic Pitch (Spotify, Apache-2.0,
  the dependency-free C++ port from HPDG in `third_party/basic_pitch`) into `notes.json` in the cache.
  This takes about 2 s for 30 s of audio. Older caches get it on load (`HPStems --notes-only --out <dir>`).
* The plug-in builds one bass line from the BASS notes (`bassLineOf`): MIDI <= 55, octave/twelfth ghosts dropped,
  monophonic (a new attack ends the sounding note). It travels with the source data (`SourceData::notes`).
* Notes are recognised **once per source**, never on MAIN. MAIN is what HPDG gets, and it is computed:
  the bridge's MAIN model (slice, stretch / repitch, reverse, gate, cut groups, loop) says which source moment
  sounds at each 1/128. The bass note there + the pad's transpose (key follow + pitch + fine) gives the note
  in the project key. Example: source A min, project B min, so +2 st: A1 in the source is sent as B1.
* Context for HPDG (hp.sample-context.v1, backward compatible):
  - per-step `bassMidi` / `bassConfidence` come from the stem notes where a source has them
    (confidence 0.6-0.95), otherwise from the classical estimate as before. HPDG uses these today.
  - new optional `bassNotes`: `[startTick, lengthTicks, midi, velocity, confidence]`, the exact MAIN bass line.
    Older peers ignore it.
  - A muted / off BASS stem sends no bass from that source.
* MAIN shows the line as the strip "BASS -> HPDG" under the drum guide.
* Checked: `tools/stems/bass_truth_test.py` (a mix with a known bass line: 71/72 notes exact after split +
  transcription) and `HPSAMPLER_BassCheck <dir>` (end to end: 10/10 notes at +2 st key follow and +7 st with pad pitch,
  per-step bassMidi right, muted BASS = no bass, protocol round trip).

## Installing the engine

The engine ships **inside the plug-in**. The build copies `HPStems.exe`, `onnxruntime.dll`,
`onnxruntime_providers_shared.dll` and `htdemucs.onnx` into `HPSAMPLER.vst3/Contents/x86_64-win/HPStems/`
(and into `HPStems/` next to the standalone exe). The HUB installs, rolls back and removes it together
with HPSAMPLER, so no extra step is needed. The bundle is about 114 MB.

The plug-in looks for `HPStems.exe` together with `htdemucs.onnx` in these places, in order:
`%HPSTEMS_DIR%`, `HPStems\` next to the plug-in binary, `Contents\Resources\HPStems\`,
`Documents\HamloProd\HPSAMPLER\HPStems\`. Without it, the strip says where the files go.
CMake: `-DHPSAMPLER_BUNDLE_STEMS=OFF` builds the plug-in without the engine.

## Building

* `third_party/onnxruntime` holds Microsoft.ML.OnnxRuntime 1.31.1 (nuget.org, win-x64 CPU, MIT):
  headers, import libraries and DLLs. CMake builds the `HPStems` target when it is present.
* `third_party/htdemucs/htdemucs.onnx` (86 MB) is in the repository, so the HUB needs no Python.
  To recreate it (needs torch, demucs 4.x and onnx):
  `python -I tools/stems/export_htdemucs_onnx.py full.onnx --check`, then
  `python -I tools/stems/shrink_fp16.py full.onnx third_party/htdemucs/htdemucs.onnx`.
  The second step stores the weights as fp16. This is lossless, because Meta publishes the checkpoint in fp16:
  the stems are bit-identical to the 174 MB fp32 file.
* Check against Python: `python -I tools/stems/reference_stems.py <song> <dir> 30`, then
  `HPStems.exe --in <dir>/input.wav --out <dir>/cpp` and compare.
  End to end on the plug-in: `HPSAMPLER_StemsCheck <outDir> <source.wav>`.

## Later

* GPU (DirectML / CUDA execution provider): 5-10x faster. The process boundary stays the same.
* 6-stem model (htdemucs_6s: + guitar, piano) with the same export.
