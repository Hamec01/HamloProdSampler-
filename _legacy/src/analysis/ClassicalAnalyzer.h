#pragma once

#include "model/SamplerModel.h"

namespace hps
{
struct FileLabelHints
{
    double       bpm = 0.0;
    juce::String bpmSource;
    int          keyRoot = -1; // 0..11
    int          keyMode = -1; // 0=minor, 1=major
};

struct ChopParams
{
    ChopAlgorithm algorithm       = ChopAlgorithm::FindSpotsHybrid;
    int           maxCandidates   = 8;
    float         sensitivity     = 0.5f;   // 0..1
    double        minSliceSec     = 0.12;
    double        beatsPerSlice   = 1.0;    // for Beat Slicer / Chop BPM
    int           regionCount     = 8;      // for Equal Time / Regions
    int64_t       rangeStartFrame = 0;
    int64_t       rangeEndFrame   = 0;      // 0 = full file
};

// 100% Classical DSP / MIR Analyzer & Chop Engine (ZERO Neural Networks).
// Reuses classical algorithms audited from C:\DRUMENGINE (SampleLabelReader,
// Hann STFT, log-magnitude spectral flux onsets, Albrecht-Shanahan / Krumhansl-Kessler
// chroma key profiles) without any BasicPitch / ML dependencies.
class ClassicalAnalyzer
{
public:
    static FileLabelHints readFilenameAndAcidHints(const juce::File& file,
                                                   const juce::StringPairArray& metadata);

    static AnalysisResult analyze(const juce::AudioBuffer<float>& buffer,
                                  double sampleRate,
                                  const FileLabelHints& hints = {});

    static std::vector<SliceCandidate> generateChopCandidates(const SourceAsset& source,
                                                              const ChopParams& params,
                                                              double fallbackBpm = 120.0);

    static int64_t findNearestZeroCrossing(const juce::AudioBuffer<float>& buffer,
                                           int64_t targetFrame,
                                           int searchRadiusFrames = 256);
};
} // namespace hps
