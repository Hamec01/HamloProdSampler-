# HPSAMPLER / HPDG bridge handoff — 2026-11-26

## Delivered

Live HPSAMPLER context now uses the optional features.version=1 capability in hp.sample-context.v1.
Source analysis retains absolute RMS, three spectral bands and measured bass segments.
The worker projects these cached measurements through MAIN events, pitch / speed / reverse,
envelopes and cuts onto a sixteenth grid with chroma, bass confidence and low-end occupancy.
Newer edits supersede an in-flight context before publication.

The automatic full-MAIN render, WAV writing and HPDG re-analysis route have been removed.
HPDG selects the numeric source from Sample Analysis → HPSAMPLER and updates its existing
Guide inputs on new revisions, including with the editor closed. Generate remains explicit.
Manual audio-file analysis continues through the existing file workflow.
Clear, an empty MAIN, manual-file selection and legacy-peer transitions have regression checks.

The production HPDG manifest helper now uses JUCE_VST3_CAN_REPLACE_VST2=0, matching its wrapper.
A metadata-based host initially failed to instantiate HPDG despite successful SDK instantiation.
After the helper flag correction, both VST3s instantiate and show their editors.

## Verification

- HPSAMPLER_Tests — 165/165 passed.
- Numeric projection — 16 bars / 256 notes in 0.7048 ms, 81742-byte payload on the final run.
  Earlier runs were 0.85–1.09 ms. These are synthetic-fixture timings.
- HPDG_BridgeContextTests — 1000 contexts, 0 failures.
- The real HPDG processor received 50 revisions in 27.2781 ms in the final idle run.
- Current HPDG tests also exercise Generate / Mutate / lane RG across all genres and six Trap
  substyles, editor lifetime and peer deletion. All completed without exceptions.
- Shared BridgeTypes.h and BridgeValidator.cpp copies match byte-for-byte.
- Release VST3s built and loaded by HPSAMPLER_HostCheck, including editor creation and teardown.
- Final bundles copied to E:/HamloProd VST/_vst3 and binary / manifest SHA256 compared with build outputs.
  Both copied bundles then passed HostCheck again.
- Guide strip rendered and visually reviewed at ../_build/hpdg/bridge-context-final.png.

Commands from the repository directories

```powershell
cmake --build build-diagnostic --config Release --target HPSAMPLER_Tests HPSAMPLER_VST3 --parallel 3
& 'build-diagnostic/Release/HPSAMPLER_Tests.exe'
cmake --build '../_build/hpdg' --config Release --target HPDG_BridgeContextTests HPDG_VST3 --parallel 3
& '../_build/hpdg/Release/HPDG_BridgeContextTests.exe' '<fresh output PNG path>'
& 'build/Release/HPSAMPLER_HostCheck.exe' 'E:/HamloProd VST/_vst3/HPSAMPLER.vst3'
& 'build/Release/HPSAMPLER_HostCheck.exe' 'E:/HamloProd VST/_vst3/HPDG.vst3'
```

An initial HPSAMPLER renderer check timed out while the CPU was occupied by HPDG LTO.
The same test passed with its original threshold when builds finished. One overlapping build
hit LNK1104 on the shared library and passed on a sequential retry. No thresholds were loosened.
The attempted direct multi-target MSBuild solution command returned MSB4057. CMake builds succeeded.

## Limits and next acceptance work

This is a source-feature projection, with a conservative influence score.
It approximates band filtering and summed energy. It does not measure mix phase cancellation,
nonlinear clipping, exact stretch-induced spectra or transients shorter than the sampling grid.
Bass outside the source analyzer's measured window stays unknown. Chroma does not assert chord roots.
See BRIDGE_PROTOCOL.md for the payload and semantics.

Actual DAW underruns and driver / buffer behaviour have not been measured in the owner's musical
session. Equivalent musical quality against the former WAV analysis has not been established by
blind listening or a fixed-seed audio corpus. The removed workload is verified, while complete
elimination of every crackle remains unverified.

Next acceptance work is a paired FL Studio session at the owner's normal buffer, plus matched
fixed-seed A/B listening for Guide output. Use Astra at xhigh for interpreting musical regressions,
and medium for known builds / packaging. No model or effort switch was performed.

## Working state and outputs

Verification inputs were based on HPSAMPLER b473913 and HPDG 7bb9f65 with the working changes described here.
Other work and commits appeared in both checkouts during this task. The builds contain the current
checkout state, including the owner's existing work. A concurrent HPDG change supplies the
getSamplePlaybackTimelineLocked fix and additional generation / peer-deletion regression checks.
These changes were preserved and the combined result was verified.

Bridge work is committed with this handoff. Changes cover the protocol / adapter, source feature retention,
processor and minimal Guide UI integration, tests, build flags and protocol documentation.
HPDG also has unrelated quality-lab changes which must be preserved.
The diagnostic build directory HPSAMPLER/build-diagnostic remains local.
No active build or test jobs remain.

Installed workspace bundles
- E:/HamloProd VST/_vst3/HPSAMPLER.vst3
- E:/HamloProd VST/_vst3/HPDG.vst3

Previous bundles
- E:/HamloProd VST/_backup/bridge-context-20261126-41e2d81e

The legacy HPSAMPLER/build cache still references E:/hamloprodsampler.
The new CMake default locates JUCE in the sibling HPDG/JUCE directory.
