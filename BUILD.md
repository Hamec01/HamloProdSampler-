# Windows build

Install CMake 3.22+ and Visual Studio with the Desktop development with C++ tools.

Double-click `build.bat` to build Release VST3 and Standalone. The window stays open and
prints the output paths. From a terminal:

```
build.bat
build.bat debug
build.bat nopause
```

JUCE 8 is detected from the existing CMake cache, this project's `JUCE/` folder, a neighboring
`HPDG808/JUCE` or `DRUMENGINE/JUCE`, or the HPDG checkout in your Documents folder.
An explicit `HPSAMPLER_JUCE_DIR` environment variable takes priority:

```
set "HPSAMPLER_JUCE_DIR=D:\JUCE"
build.bat
```

Release output:

- `build/HPSAMPLER_artefacts/Release/VST3/HPSAMPLER.vst3` (copy the entire folder when installing).
- `build/HPSAMPLER_artefacts/Release/Standalone/HPSAMPLER.exe`.

The script builds both formats and checks that the resulting files exist. It uses the generator
of an existing `build/` folder, selects x64 for a fresh folder, and returns a nonzero exit code on
failure. `nopause` is intended for terminals and automation. Installation into the system VST3
folder is a separate step.

## Stems engine (optional)

`build.bat` also builds `build/Release/HPStems.exe` (stem separation helper, ONNX Runtime from
`third_party/onnxruntime`). It needs the model `htdemucs.onnx` next to it; see `docs/STEMS.md` for
exporting the model and where the plug-in looks for the engine (e.g. `Documents\HamloProd\HPSAMPLER\HPStems`).
