# HPSAMPLER — architecture (rewrite v0.2)

The Gemini P0 code is kept untouched in `_legacy/` for reference only; nothing builds from it.

## Layers

```
core/      Types (PPQ 960, enums, key transpose), Project (undoable ProjectState + SourceData store),
           Serialization (hpsampler.project.v2 JSON, MIDI export)
analysis/  SourceAnalysis  decode + peaks + onsets/RMS/chroma/spectrogram + tempo/key (HPDG ports)
           ChopEngine      Find Spots (Hybrid/Attack/Tonal/Phrase), Beat Slicer, Regions, Threshold,
                           Grid chop, snapping (grid / transient / zero-cross)
render/    PadRenderer     slice -> Signalsmith Stretch (tempo+key, independent) or band-limited
                           repitch -> reverse -> fades, at the engine sample rate, worker thread
audio/     Engine          voices, Gate/OneShot/Toggle, Retrigger/Layer, cut groups, MONO per source/all,
                           960-PPQ sequencer (host or internal clock), live record, audition, metronome
bridge/    BridgeAdapter   MAIN context -> hp.sample-context.v1, hp.drum-guide.v1 -> guide overlay
app/       Processor (glue, timers, recording, export) / Editor
ui/        SourceView, PadGrid, ControlStrip, PatternView (MAIN), PadInspector, LookAndFeel
third_party/  signalsmith-stretch + signalsmith-linear (MIT), hpdg_analysis (our HPDG code, BasicPitch removed)
```

## Data model

* `ProjectState` (plain values, copied for Undo): 4 `SourceSlot`s (file, user BPM/beat-1/key overrides),
  `Slice` library, 64 `Pad`s (each with its own copy of the range: editing one pad never moves another),
  `PatternEvent`s (pad, tick, rawTick = recorded timing, length, velocity), `ProjectSettings`.
* `SourceData` (immutable, shared, not undoable): decoded audio, peaks, `SourceAnalysis`.
* MAIN lanes are a projection: an event's lane is always `pads[event.pad].src`.

## Sync rules (why a pad sounds the way it does)

| SYNC | tempo | key |
|---|---|---|
| STRETCH (default) | time x (srcBPM / projectBPM), pitch unchanged | + key-follow shift (relative-key aware, -6..+5 st) |
| REPITCH | speed x (projectBPM / srcBPM), pitch moves with it | — |
| OFF | as recorded | user pitch only (time kept) |

Unknown source BPM (confidence < 0.35, HPDG threshold) -> no stretch, never a fake tempo.
Unknown / atonal key (drum loops, low confidence) -> never transposed.

## Realtime

The audio thread never allocates, locks, decodes or parses. Pad sounds, previews and the engine
snapshot are published as raw pointers (`Published<T>`); replaced objects are freed on the message
thread after 1.5 s and only when no voice uses them. GUI -> audio: lock-free FIFO; audio -> GUI
(recorded hits): lock-free FIFO.

## Tests

`HPSAMPLER_Tests` (49 checks): rapid retrigger, Gate/OneShot/Toggle, MIDI vs mouse tokens, vel-0 NoteOff,
cut groups, mono modes, sequencer + recording without feedback, stretch/pitch/repitch accuracy,
key transpose, analysis + chop on synthetic audio, persistence, undo, bridge validation.
`HPSAMPLER_Tests file.wav ...` prints the analysis of real files. `HPSAMPLER_Shot` renders the editor to PNG.
