#pragma once

#include "core/Project.h"

namespace hps
{
// Decodes an audio file and runs the classical (non-neural) analysis:
//   - waveform peaks, spectrogram, onset curve + peak-picked onsets, RMS, chroma frames
//   - tempo / beat-1 origin / bars / swing (HPDG DrumBreakTranscriber, 64 s windows)
//   - key + confidence (HPDG SampleHarmonyAnalyzer chroma path, Basic Pitch removed)
//   - BPM / key labels from the file name or WAV acid chunk (HPDG SampleLabelReader)
// Runs on a worker thread; never on the audio thread.
class SourceAnalyzer
{
public:
    // Step 1 (fast): decode + peaks. Returns nullptr data with error text on failure.
    static std::shared_ptr<SourceData> decode(const juce::File& file, const juce::String& id, juce::String& error);

    // Step 2 (slower): fills data.analysis. `shouldExit` lets the job be cancelled.
    static void analyse(SourceData& data, const juce::File& file, const std::function<bool()>& shouldExit);

    // Waveform helpers
    static WaveformPeaks computePeaks(const juce::AudioBuffer<float>& audio, int framesPerPeak);
    static int64_t nearestZeroCrossing(const juce::AudioBuffer<float>& audio, int64_t frame, int64_t searchRadius);
    // Refines an onset to the start of its attack (looking back from the peak).
    static int64_t refineAttackStart(const juce::AudioBuffer<float>& audio, int64_t approxFrame, double sampleRate);
};

// Runs decode + analysis jobs in the background and hands results back on the message thread.
class SourceLoader : private juce::Thread
{
public:
    SourceLoader();
    ~SourceLoader() override;

    // Results delivered on the message thread. `analysed` false = decoded only (waveform ready).
    std::function<void(int slot, std::shared_ptr<const SourceData> data, bool analysed)> onResult;
    std::function<void(int slot, juce::String error)> onError;

    void load(int slot, const juce::File& file, const juce::String& id);
    bool isBusy() const;

private:
    void run() override;
    struct Job { int slot; juce::File file; juce::String id; };
    mutable juce::CriticalSection lock;
    std::vector<Job> jobs;
    std::atomic<bool> busy { false };
    std::shared_ptr<bool> alive = std::make_shared<bool>(true); // message-thread guard for callAsync
};
} // namespace hps
