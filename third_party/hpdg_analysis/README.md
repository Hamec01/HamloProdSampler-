# hpdg_analysis — classical (non-neural) analysis ported from HPDG (C:\DRUMENGINE\Source)

Copied verbatim from our own HPDG plugin, namespace `bbg`, except:
- `SampleHarmonyAnalyzer`: the Basic Pitch (neural network) note-key branch is removed;
  the chroma (Albrecht-Shanahan profiles) + bass + opening-note key decision is unchanged.

Used by HPSAMPLER: DrumBreakTranscriber (tempo, beat-1 origin, bars, swing),
SampleLabelReader (BPM/key from file name / WAV acid chunk), SampleLineTranscriber
(tuning cents), SampleHarmonyAnalyzer (key + confidence).
Do not edit HPDG from here; re-sync deliberately if HPDG improves these files.
