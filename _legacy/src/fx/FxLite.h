#pragma once

#include <array>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

namespace hps
{
struct IFxProcessor
{
    virtual ~IFxProcessor() = default;
    virtual void   prepare(double sampleRate, int maxBlockSize, int numChannels) = 0;
    virtual void   process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept = 0;
    virtual void   reset() noexcept = 0;
    virtual int    getLatencySamples() const noexcept { return 0; }
    virtual double getTailSeconds() const noexcept { return 0.0; }
};

// Lightweight zero-alloc per-pad SVF filter (LP / HP / BP) with wet/dry mix.
class FilterEqLite final : public IFxProcessor
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void setParameters(bool enabled, int type, float cutoffHz, float resonance, float mix) noexcept;
    void process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept override;
    void reset() noexcept override;

private:
    juce::dsp::StateVariableTPTFilter<float> filter;
    double sampleRate = 44100.0;
    bool   enabled    = false;
    int    filterType = 0;
    float  cutoffHz   = 12000.0f;
    float  resonance  = 0.707f;
    float  mix        = 1.0f;
};

// Lightweight zero-latency Master Tube Saturation + Vari-Mu Compressor Lite,
// adapted from the HamloMag / HamloComp equations audited in C:\DRUMENGINE\Source\Audio\LofiColor.h.
class WarmTubeCompLite final : public IFxProcessor
{
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) override;
    void setParameters(bool enabled, float driveDb, float mix, bool limiterOn) noexcept;
    void process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept override;
    void reset() noexcept override;

    float getLastGainReductionDb() const noexcept { return reductionDb; }

private:
    static float tubeCurve(float x) noexcept;

    double               rate        = 44100.0;
    bool                 enabled     = false;
    bool                 limiterOn   = true;
    float                driveGain   = 1.2589f; // +2 dB
    float                wetMix      = 0.5f;

    std::array<float, 2> dcIn {};
    std::array<float, 2> dcOut {};
    std::array<float, 2> lowpass {};
    std::array<float, 2> evenMean {};

    float                dcCoef      = 0.9985f;
    float                lpCoef      = 0.5f;
    float                evenCoef    = 0.002f;
    float                msCoef      = 0.002f;
    float                attackCoef  = 0.001f;
    float                releaseCoef = 0.0001f;
    float                meanSquare  = 0.0f;
    float                reductionDb = 0.0f;
};
} // namespace hps
