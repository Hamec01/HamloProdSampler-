#include "fx/FxLite.h"

#include <cmath>

namespace hps
{
void FilterEqLite::prepare(double sr, int maxBlockSize, int numChannels)
{
    sampleRate = sr > 8000.0 ? sr : 44100.0;
    juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(std::max(1, maxBlockSize)), static_cast<juce::uint32>(std::max(1, numChannels)) };
    filter.prepare(spec);
    filter.reset();
    setParameters(enabled, filterType, cutoffHz, resonance, mix);
}

void FilterEqLite::setParameters(bool isEnabled, int type, float cutoff, float res, float wet) noexcept
{
    enabled    = isEnabled;
    filterType = juce::jlimit(0, 2, type);
    cutoffHz   = juce::jlimit(20.0f, static_cast<float>(sampleRate * 0.45), cutoff);
    resonance  = juce::jlimit(0.1f, 8.0f, res);
    mix        = juce::jlimit(0.0f, 1.0f, wet);

    filter.setCutoffFrequency(cutoffHz);
    filter.setResonance(resonance);
    if (filterType == 0)      filter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    else if (filterType == 1) filter.setType(juce::dsp::StateVariableTPTFilterType::highpass);
    else                      filter.setType(juce::dsp::StateVariableTPTFilterType::bandpass);
}

void FilterEqLite::process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept
{
    if (!enabled || mix <= 1.0e-4f || numSamples <= 0)
        return;

    const int channels = std::min(2, buffer.getNumChannels());
    for (int ch = 0; ch < channels; ++ch)
    {
        float* ptr = buffer.getWritePointer(ch, startSample);
        for (int i = 0; i < numSamples; ++i)
        {
            const float dry = ptr[i];
            const float wet = filter.processSample(ch, dry);
            ptr[i] = dry + mix * (wet - dry);
        }
    }
}

void FilterEqLite::reset() noexcept
{
    filter.reset();
}

// ============================================================================

float WarmTubeCompLite::tubeCurve(float x) noexcept
{
    return x >= 0.0f ? std::tanh(x) : std::tanh(1.4f * x) / 1.4f;
}

void WarmTubeCompLite::prepare(double sr, int, int)
{
    rate        = sr > 8000.0 ? sr : 44100.0;
    dcCoef      = std::exp(-2.0f * juce::MathConstants<float>::pi * 10.0f / static_cast<float>(rate));
    lpCoef      = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 12000.0f / static_cast<float>(rate));
    evenCoef    = 1.0f - std::exp(-2.0f * juce::MathConstants<float>::pi * 20.0f / static_cast<float>(rate));
    msCoef      = 1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(rate)));
    attackCoef  = 1.0f - std::exp(-1.0f / (0.015f * static_cast<float>(rate)));
    releaseCoef = 1.0f - std::exp(-1.0f / (0.300f * static_cast<float>(rate)));
    reset();
}

void WarmTubeCompLite::setParameters(bool isEnabled, float driveDb, float mix, bool limOn) noexcept
{
    enabled   = isEnabled;
    driveGain = juce::Decibels::decibelsToGain(juce::jlimit(-6.0f, 18.0f, driveDb));
    wetMix    = juce::jlimit(0.0f, 1.0f, mix);
    limiterOn = limOn;
}

void WarmTubeCompLite::reset() noexcept
{
    dcIn.fill(0.0f);
    dcOut.fill(0.0f);
    lowpass.fill(0.0f);
    evenMean.fill(0.0f);
    meanSquare  = 0.0f;
    reductionDb = 0.0f;
}

void WarmTubeCompLite::process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples) noexcept
{
    const int channels = std::min(2, buffer.getNumChannels());
    if (channels <= 0 || numSamples <= 0) return;

    constexpr float bias = 0.1f;
    const float fBias    = tubeCurve(bias);
    const float fPrime   = 1.0f - std::tanh(bias) * std::tanh(bias);
    const float normDen  = std::max(1.0e-5f, driveGain * fPrime);
    const float makeup   = juce::Decibels::decibelsToGain(2.4f);

    for (int i = 0; i < numSamples; ++i)
    {
        float s0 = buffer.getSample(0, startSample + i);
        float s1 = channels > 1 ? buffer.getSample(1, startSample + i) : s0;

        if (enabled && wetMix > 1.0e-4f)
        {
            float dry[2] = { s0, s1 };
            float wet[2] = { s0, s1 };

            // 1. HamloMag tube saturation + DC blocker + 12 kHz ease
            for (int c = 0; c < 2; ++c)
            {
                const float sat = (tubeCurve(driveGain * wet[c] + bias) - fBias) / normDen;
                const float dc  = sat - dcIn[static_cast<size_t>(c)] + dcCoef * dcOut[static_cast<size_t>(c)];
                dcIn[static_cast<size_t>(c)]  = sat;
                dcOut[static_cast<size_t>(c)] = dc;
                lowpass[static_cast<size_t>(c)] += lpCoef * (dc - lowpass[static_cast<size_t>(c)]);
                wet[c] = lowpass[static_cast<size_t>(c)];
            }

            // 2. HamloComp vari-mu tube compressor
            const float sq = 0.5f * (wet[0] * wet[0] + wet[1] * wet[1]);
            meanSquare += msCoef * (sq - meanSquare);
            const float rmsDb = juce::Decibels::gainToDecibels(std::sqrt(std::max(1.0e-12f, meanSquare)));
            const float over  = std::max(0.0f, rmsDb - (-24.0f));
            const float ratio = 1.0f + 1.5f * std::min(1.0f, over / 18.0f);
            const float targetRed = over > 0.0f ? over * (1.0f - 1.0f / ratio) : 0.0f;
            const float coef = targetRed > reductionDb ? attackCoef : releaseCoef;
            reductionDb += coef * (targetRed - reductionDb);

            const float compGain = juce::Decibels::decibelsToGain(-reductionDb) * makeup;
            const float colorAmt = std::min(1.0f, reductionDb / 12.0f);

            for (int c = 0; c < 2; ++c)
            {
                float y = wet[c] * compGain;
                const float e = (y * y) / (1.0f + std::abs(y));
                evenMean[static_cast<size_t>(c)] += evenCoef * (e - evenMean[static_cast<size_t>(c)]);
                y += colorAmt * (0.04f * (e - evenMean[static_cast<size_t>(c)]) + 0.02f * (std::tanh(y) - y));
                wet[c] = dry[c] + wetMix * (y - dry[c]);
            }

            s0 = wet[0];
            s1 = wet[1];
        }

        if (limiterOn)
        {
            s0 = std::tanh(s0 * 0.95f) / 0.95f;
            s1 = std::tanh(s1 * 0.95f) / 0.95f;
        }

        if (!std::isfinite(s0)) s0 = 0.0f;
        if (!std::isfinite(s1)) s1 = 0.0f;

        buffer.setSample(0, startSample + i, s0);
        if (channels > 1) buffer.setSample(1, startSample + i, s1);
    }
}
} // namespace hps
