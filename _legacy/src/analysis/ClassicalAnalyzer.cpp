#include "analysis/ClassicalAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <regex>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

namespace hps
{
namespace
{
// Albrecht & Shanahan (2013) major/minor chroma profiles (proven in DRUMENGINE SampleHarmonyAnalyzer)
constexpr std::array<float, 12> kMajorProfileAS { 0.238f, 0.006f, 0.111f, 0.006f, 0.137f, 0.094f, 0.016f, 0.214f, 0.009f, 0.080f, 0.008f, 0.081f };
constexpr std::array<float, 12> kMinorProfileAS { 0.220f, 0.006f, 0.104f, 0.123f, 0.019f, 0.103f, 0.012f, 0.214f, 0.062f, 0.022f, 0.061f, 0.052f };

float pearsonCorrelation(const std::array<float, 12>& chroma, const std::array<float, 12>& profile, int rotation)
{
    float meanC = 0.0f, meanP = 0.0f;
    for (int i = 0; i < 12; ++i)
    {
        meanC += chroma[static_cast<size_t>(i)];
        meanP += profile[static_cast<size_t>(i)];
    }
    meanC /= 12.0f;
    meanP /= 12.0f;

    float num = 0.0f, varC = 0.0f, varP = 0.0f;
    for (int pc = 0; pc < 12; ++pc)
    {
        const float c = chroma[static_cast<size_t>(pc)] - meanC;
        const float p = profile[static_cast<size_t>((pc - rotation + 12) % 12)] - meanP;
        num  += c * p;
        varC += c * c;
        varP += p * p;
    }
    const float den = std::sqrt(varC * varP);
    return den > 1.0e-9f ? num / den : 0.0f;
}

std::vector<float> makeMono(const juce::AudioBuffer<float>& buf)
{
    const int n = buf.getNumSamples();
    const int ch = buf.getNumChannels();
    std::vector<float> mono(static_cast<size_t>(n), 0.0f);
    if (ch <= 0 || n <= 0) return mono;
    const float scale = 1.0f / static_cast<float>(ch);
    for (int c = 0; c < ch; ++c)
    {
        const float* r = buf.getReadPointer(c);
        for (int i = 0; i < n; ++i)
            mono[static_cast<size_t>(i)] += r[i] * scale;
    }
    return mono;
}
} // namespace

FileLabelHints ClassicalAnalyzer::readFilenameAndAcidHints(const juce::File& file,
                                                           const juce::StringPairArray& metadata)
{
    FileLabelHints hints;
    auto stem = file.getFileNameWithoutExtension();
    // Handle Cyrillic look-alikes ("Сm") as in DRUMENGINE SampleLabelReader
    stem = stem.replaceCharacters(juce::CharPointer_UTF8("\xd0\xa1\xd0\x90\xd0\x92\xd0\x95"), "CABE");
    const std::string s = stem.toStdString();

    static const std::regex explicitAfter(R"((\d{2,3}(?:\.\d+)?)[\s_\-]*bpm)", std::regex::icase);
    static const std::regex explicitBefore(R"(bpm[\s_\-]*(\d{2,3}(?:\.\d+)?))", std::regex::icase);
    for (const auto* pat : { &explicitAfter, &explicitBefore })
    {
        std::smatch m;
        if (std::regex_search(s, m, *pat))
        {
            const double val = std::stod(m[1].str());
            if (val >= 50.0 && val <= 220.0)
            {
                hints.bpm = val;
                hints.bpmSource = "name";
                break;
            }
        }
    }

    const double acidTempo = metadata.getValue(juce::WavAudioFormat::acidTempo, {}).getDoubleValue();
    if (acidTempo >= 50.0 && acidTempo <= 220.0)
    {
        hints.bpm = acidTempo;
        hints.bpmSource = "acid";
    }

    static const std::regex keyRegex(R"((?:^|[^A-Za-z])([A-G])(#|b|sharp|shrp)?[\s_]?(minor|major|min|maj|m)?(?=$|[^A-Za-z]))",
                                     std::regex::icase);
    static constexpr int letterPitch[] { 9, 11, 0, 2, 4, 5, 7 }; // A..G
    for (auto it = std::sregex_iterator(s.begin(), s.end(), keyRegex); it != std::sregex_iterator(); ++it)
    {
        const char letter = (*it)[1].str()[0];
        if (letter < 'A' || letter > 'G') continue;
        int pc = letterPitch[letter - 'A'];
        const auto acc = juce::String((*it)[2].str()).toLowerCase();
        if (acc == "#" || acc.startsWith("s")) ++pc;
        else if (acc == "b") --pc;
        hints.keyRoot = (pc % 12 + 12) % 12;
        const auto suf = juce::String((*it)[3].str()).toLowerCase();
        hints.keyMode = suf.isEmpty() ? -1 : (suf.startsWith("maj") ? 1 : 0);
    }

    return hints;
}

AnalysisResult ClassicalAnalyzer::analyze(const juce::AudioBuffer<float>& buffer,
                                          double sampleRate,
                                          const FileLabelHints& hints)
{
    AnalysisResult res;
    res.state = AnalysisState::Unknown;
    if (buffer.getNumSamples() < 512 || sampleRate < 8000.0)
        return res;

    const auto mono = makeMono(buffer);
    const int fftOrder  = 11; // 2048 samples (~21.5 Hz bins at 44.1 kHz)
    const int frameSize = 1 << fftOrder;
    const int hopSize   = 512;
    const int halfSize  = frameSize / 2;
    res.hopSamples = hopSize;

    juce::dsp::FFT fft(fftOrder);
    juce::dsp::WindowingFunction<float> window(static_cast<size_t>(frameSize),
                                               juce::dsp::WindowingFunction<float>::hann,
                                               true);

    const int numFrames = std::max(1, static_cast<int>((mono.size() + static_cast<size_t>(hopSize) - 1) / static_cast<size_t>(hopSize)));
    const double binHz  = sampleRate / static_cast<double>(frameSize);

    std::vector<float> prevLogMag(static_cast<size_t>(halfSize), 0.0f);
    std::vector<float> fftData(static_cast<size_t>(frameSize * 2), 0.0f);

    std::vector<float> onsetFlux(static_cast<size_t>(numFrames), 0.0f);
    std::vector<int>   onsetBand(static_cast<size_t>(numFrames), 1);
    res.rmsEnvelope.resize(static_cast<size_t>(numFrames), 0.0f);

    // Spectrogram downsampled to at most 512 columns × 64 log-spaced bins
    const int targetSpecCols = std::min(512, numFrames);
    res.specCols = targetSpecCols;
    res.specBins = 64;
    res.spectrogramCols.assign(static_cast<size_t>(res.specCols * res.specBins), 0.0f);

    std::array<double, 12> chromaAccum {};
    double flatnessAccum = 0.0;
    int    voicedFrames  = 0;

    for (int f = 0; f < numFrames; ++f)
    {
        std::fill(fftData.begin(), fftData.end(), 0.0f);
        const size_t start = static_cast<size_t>(f) * static_cast<size_t>(hopSize);
        const int copyN = static_cast<int>(std::min<size_t>(static_cast<size_t>(frameSize),
                                                            start < mono.size() ? mono.size() - start : 0));
        float rmsSum = 0.0f;
        for (int i = 0; i < copyN; ++i)
        {
            const float x = mono[start + static_cast<size_t>(i)];
            fftData[static_cast<size_t>(i)] = x;
            rmsSum += x * x;
        }
        const float rms = std::sqrt(rmsSum / static_cast<float>(std::max(1, copyN)));
        res.rmsEnvelope[static_cast<size_t>(f)] = rms;

        window.multiplyWithWindowingTable(fftData.data(), static_cast<size_t>(frameSize));
        fft.performFrequencyOnlyForwardTransform(fftData.data());

        float fluxTotal = 0.0f, fluxLow = 0.0f, fluxMid = 0.0f, fluxHigh = 0.0f;
        double magSum = 0.0, logMagSum = 0.0;
        int magCount = 0;

        for (int b = 1; b < halfSize; ++b)
        {
            const float mag    = std::max(1.0e-6f, fftData[static_cast<size_t>(b)]);
            const float logMag = std::log1p(mag);
            const float diff   = std::max(0.0f, logMag - prevLogMag[static_cast<size_t>(b)]);
            prevLogMag[static_cast<size_t>(b)] = logMag;

            const double freq = static_cast<double>(b) * binHz;
            fluxTotal += diff;
            if (freq < 200.0)       fluxLow  += diff;
            else if (freq < 2400.0) fluxMid  += diff;
            else                    fluxHigh += diff;

            magSum    += mag;
            logMagSum += std::log(static_cast<double>(mag));
            ++magCount;

            // Accumulate 12-bin chroma in musical range [65 Hz .. 2400 Hz]
            if (freq >= 65.0 && freq <= 2400.0)
            {
                const double midi = 69.0 + 12.0 * std::log2(freq / 440.0);
                const int pc = (static_cast<int>(std::llround(midi)) % 12 + 12) % 12;
                chromaAccum[static_cast<size_t>(pc)] += static_cast<double>(mag) * static_cast<double>(mag);
            }
        }

        onsetFlux[static_cast<size_t>(f)] = fluxTotal;
        onsetBand[static_cast<size_t>(f)] = (fluxLow >= fluxMid && fluxLow >= fluxHigh) ? 0
                                          : (fluxHigh > fluxMid ? 2 : 1);

        if (rms > 0.01f && magCount > 0 && magSum > 0.0)
        {
            const double gm = std::exp(logMagSum / static_cast<double>(magCount));
            const double am = magSum / static_cast<double>(magCount);
            flatnessAccum += (am > 0.0 ? gm / am : 0.0);
            ++voicedFrames;
        }

        // Fill spectrogram column
        const int col = juce::jlimit(0, targetSpecCols - 1, (f * targetSpecCols) / std::max(1, numFrames));
        for (int sb = 0; sb < res.specBins; ++sb)
        {
            const double norm = static_cast<double>(sb) / static_cast<double>(res.specBins - 1);
            const double freq = 40.0 * std::pow((sampleRate * 0.45) / 40.0, norm);
            const int bin = juce::jlimit(1, halfSize - 1, static_cast<int>(freq / binHz));
            const float val = std::min(1.0f, std::log1p(fftData[static_cast<size_t>(bin)]) / 5.5f);
            float& dst = res.spectrogramCols[static_cast<size_t>(col * res.specBins + sb)];
            dst = std::max(dst, val);
        }
    }

    // Normalize onset flux and pick local peaks with adaptive threshold
    const float maxFlux = *std::max_element(onsetFlux.begin(), onsetFlux.end());
    if (maxFlux > 1.0e-6f)
        for (auto& v : onsetFlux) v /= maxFlux;

    const float meanFlux = std::accumulate(onsetFlux.begin(), onsetFlux.end(), 0.0f) / static_cast<float>(onsetFlux.size());
    float varFlux = 0.0f;
    for (float v : onsetFlux) { const float d = v - meanFlux; varFlux += d * d; }
    const float stdFlux = std::sqrt(varFlux / static_cast<float>(onsetFlux.size()));
    const float thresh  = juce::jlimit(0.14f, 0.65f, meanFlux + 0.65f * stdFlux);
    const int minSpacingFrames = std::max(1, static_cast<int>((0.06 * sampleRate) / static_cast<double>(hopSize)));

    int lastPeakFrame = -minSpacingFrames;
    for (int f = 1; f + 1 < numFrames; ++f)
    {
        const float v = onsetFlux[static_cast<size_t>(f)];
        if (v >= thresh && v >= onsetFlux[static_cast<size_t>(f - 1)] && v >= onsetFlux[static_cast<size_t>(f + 1)])
        {
            if (f - lastPeakFrame >= minSpacingFrames)
            {
                res.onsets.push_back({ static_cast<int64_t>(f) * hopSize, v, onsetBand[static_cast<size_t>(f)] });
                lastPeakFrame = f;
            }
        }
    }

    // Autocorrelation BPM estimation (60..190 BPM)
    const double framesPerSec = sampleRate / static_cast<double>(hopSize);
    const int minLag = std::max(2, static_cast<int>(std::floor(60.0 * framesPerSec / 190.0)));
    const int maxLag = std::min(numFrames / 2, static_cast<int>(std::ceil(60.0 * framesPerSec / 60.0)));
    const double durationSec = static_cast<double>(mono.size()) / sampleRate;

    if (durationSec >= 1.5 && maxLag > minLag + 2 && res.onsets.size() >= 3)
    {
        std::vector<std::pair<double, float>> scores;
        double maxAc = 0.0;
        double sumAc = 0.0;
        int    cntAc = 0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            double ac = 0.0;
            const int limit = numFrames - lag;
            for (int t = 0; t < limit; ++t)
                ac += static_cast<double>(onsetFlux[static_cast<size_t>(t)]) * static_cast<double>(onsetFlux[static_cast<size_t>(t + lag)]);
            ac /= static_cast<double>(std::max(1, limit));
            maxAc = std::max(maxAc, ac);
            sumAc += ac;
            ++cntAc;
            const double bpm = 60.0 * framesPerSec / static_cast<double>(lag);
            scores.push_back({ bpm, static_cast<float>(ac) });
        }

        const double meanAc = cntAc > 0 ? sumAc / static_cast<double>(cntAc) : 0.0;
        std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

        for (const auto& [bpmVal, sc] : scores)
        {
            const bool tooClose = std::any_of(res.bpmCandidates.begin(), res.bpmCandidates.end(),
                                              [&](const BpmCandidate& c) { return std::abs(c.bpm - bpmVal) < 4.0; });
            if (!tooClose && maxAc > 1.0e-6)
            {
                const float ratio = static_cast<float>(sc / std::max(1.0e-6, meanAc));
                const float conf  = juce::jlimit(0.0f, 0.98f, (ratio - 1.15f) / 2.2f);
                res.bpmCandidates.push_back({ std::round(bpmVal * 10.0) / 10.0, conf });
                if (res.bpmCandidates.size() >= 3) break;
            }
        }

        if (!res.bpmCandidates.empty() && res.bpmCandidates.front().confidence >= 0.25f)
        {
            res.bpm           = res.bpmCandidates.front().bpm;
            res.bpmConfidence = res.bpmCandidates.front().confidence;
            res.bpmOrigin     = hpbridge::ContextOrigin::Analysis;
        }
    }

    // Apply filename/acid BPM hint if audio BPM was absent or low confidence
    if (hints.bpm >= 50.0 && hints.bpm <= 220.0 && (!res.bpm.has_value() || res.bpmConfidence < 0.45f))
    {
        res.bpm           = hints.bpm;
        res.bpmConfidence = 0.85f;
        res.bpmOrigin     = hpbridge::ContextOrigin::Analysis;
    }

    // Atonal / percussive check via spectral flatness
    res.averageFlatness = voicedFrames > 0 ? static_cast<float>(flatnessAccum / static_cast<double>(voicedFrames)) : 1.0f;
    res.isAtonalOrPercussive = (res.averageFlatness > 0.42f);

    double maxChroma = 0.0;
    for (double v : chromaAccum) maxChroma = std::max(maxChroma, v);
    if (maxChroma > 1.0e-9)
        for (int i = 0; i < 12; ++i)
            res.chroma[static_cast<size_t>(i)] = static_cast<float>(chromaAccum[static_cast<size_t>(i)] / maxChroma);

    // Key estimation (skip silently guessing key on pure noise/drums!)
    if (!res.isAtonalOrPercussive && maxChroma > 1.0e-6)
    {
        float bestCorr   = -2.0f;
        float secondCorr = -2.0f;
        int   bestRoot   = 0;
        bool  bestMajor  = false;

        for (int root = 0; root < 12; ++root)
        {
            const float cMaj = pearsonCorrelation(res.chroma, kMajorProfileAS, root);
            const float cMin = pearsonCorrelation(res.chroma, kMinorProfileAS, root);

            auto consider = [&](float c, bool isMaj) {
                if (c > bestCorr) { secondCorr = bestCorr; bestCorr = c; bestRoot = root; bestMajor = isMaj; }
                else if (c > secondCorr) { secondCorr = c; }
            };
            consider(cMaj, true);
            consider(cMin, false);
        }

        const float margin = std::max(0.0f, bestCorr - secondCorr);
        const float conf   = juce::jlimit(0.0f, 0.98f, 0.5f * std::max(0.0f, bestCorr) + 1.5f * margin);

        if (bestCorr >= 0.30f && conf >= 0.25f)
        {
            res.key.tonicPitchClass = bestRoot;
            res.key.mode            = bestMajor ? "major" : "minor";
            res.key.confidence      = conf;
            res.key.origin          = hpbridge::ContextOrigin::Analysis;
        }
    }

    // Apply filename key hint if audio didn't strongly contradict it
    if (hints.keyRoot >= 0 && !res.isAtonalOrPercussive)
    {
        const bool isMaj = (hints.keyMode == 1);
        const float corr = pearsonCorrelation(res.chroma, isMaj ? kMajorProfileAS : kMinorProfileAS, hints.keyRoot);
        if (corr > -0.1f && (!res.key.tonicPitchClass.has_value() || res.key.confidence < 0.65f))
        {
            res.key.tonicPitchClass = hints.keyRoot;
            res.key.mode            = isMaj ? "major" : "minor";
            res.key.confidence      = std::max(0.70f, res.key.confidence);
            res.key.origin          = hpbridge::ContextOrigin::Analysis;
        }
    }

    res.state = AnalysisState::Ready;
    return res;
}

int64_t ClassicalAnalyzer::findNearestZeroCrossing(const juce::AudioBuffer<float>& buffer,
                                                   int64_t targetFrame,
                                                   int searchRadiusFrames)
{
    const int64_t n = buffer.getNumSamples();
    if (n < 2 || buffer.getNumChannels() <= 0) return juce::jlimit<int64_t>(0, std::max<int64_t>(0, n - 1), targetFrame);

    targetFrame = juce::jlimit<int64_t>(1, n - 1, targetFrame);
    const float* ch0 = buffer.getReadPointer(0);

    int64_t bestFrame = targetFrame;
    int     bestDist  = searchRadiusFrames + 1;

    const int64_t lo = std::max<int64_t>(1, targetFrame - searchRadiusFrames);
    const int64_t hi = std::min<int64_t>(n - 1, targetFrame + searchRadiusFrames);
    for (int64_t i = lo; i <= hi; ++i)
    {
        if ((ch0[i - 1] <= 0.0f && ch0[i] >= 0.0f) || (ch0[i - 1] >= 0.0f && ch0[i] <= 0.0f))
        {
            const int d = static_cast<int>(std::abs(i - targetFrame));
            if (d < bestDist)
            {
                bestDist  = d;
                bestFrame = i;
            }
        }
    }
    return bestFrame;
}

std::vector<SliceCandidate> ClassicalAnalyzer::generateChopCandidates(const SourceAsset& source,
                                                                      const ChopParams& params,
                                                                      double fallbackBpm)
{
    std::vector<SliceCandidate> out;
    if (source.audio == nullptr || source.numFrames <= 0 || source.sampleRate <= 0.0)
        return out;

    const int64_t startF = juce::jlimit<int64_t>(0, source.numFrames - 1, params.rangeStartFrame);
    const int64_t endF   = params.rangeEndFrame > startF
                         ? juce::jlimit<int64_t>(startF + 1, source.numFrames, params.rangeEndFrame)
                         : source.numFrames;
    const int64_t span   = endF - startF;
    const int64_t minLen = std::max<int64_t>(256, static_cast<int64_t>(params.minSliceSec * source.sampleRate));

    auto makeCandidate = [&](int64_t s, int64_t e, const juce::String& origin, float score) {
        s = juce::jlimit<int64_t>(startF, endF - 1, s);
        e = juce::jlimit<int64_t>(s + 1, endF, e);
        SliceCandidate c;
        c.id                = makeUuid();
        c.sourceId          = source.id;
        c.startFrame        = findNearestZeroCrossing(*source.audio, s, 128);
        c.endFrameExclusive = std::max(c.startFrame + 1, findNearestZeroCrossing(*source.audio, e, 128));
        c.algorithm         = params.algorithm;
        c.originLabel       = origin;
        c.score             = juce::jlimit(0.0f, 1.0f, score);
        c.sourceRevision    = source.revision;
        out.push_back(std::move(c));
    };

    // Mode C & D: Equal Time Regions / Chop Regions
    if (params.algorithm == ChopAlgorithm::SlicerEqualTime || params.algorithm == ChopAlgorithm::ChopRegions)
    {
        const int count = juce::jlimit(1, 64, params.regionCount);
        const double step = static_cast<double>(span) / static_cast<double>(count);
        for (int i = 0; i < count; ++i)
        {
            const int64_t s = startF + static_cast<int64_t>(std::llround(i * step));
            const int64_t e = (i + 1 == count) ? endF : startF + static_cast<int64_t>(std::llround((i + 1) * step));
            makeCandidate(s, e, "Region " + juce::String(i + 1), 1.0f);
        }
        return out;
    }

    // Mode C & D: Beat Slicer / Chop BPM
    if (params.algorithm == ChopAlgorithm::SlicerBeat || params.algorithm == ChopAlgorithm::ChopBpm)
    {
        const double bpm = source.analysis.bpm.value_or(fallbackBpm);
        if (bpm < 20.0 || bpm > 300.0) return out; // Refuse to invent fake beat grid
        const int64_t sliceFrames = std::max<int64_t>(256, static_cast<int64_t>((60.0 / bpm) * params.beatsPerSlice * source.sampleRate));
        const int count = juce::jlimit(1, 64, params.maxCandidates);
        for (int i = 0; i < count; ++i)
        {
            const int64_t s = startF + i * sliceFrames;
            if (s >= endF - 256) break;
            const int64_t e = std::min(endF, s + sliceFrames);
            makeCandidate(s, e, "Beat " + juce::String(i + 1), source.analysis.bpm.has_value() ? 0.9f : 0.6f);
        }
        return out;
    }

    // Mode D: Threshold / Mode B: Find Spots (Attack / Tonal / Phrase / Hybrid)
    const float sensThresh = juce::jlimit(0.08f, 0.85f, 0.85f - 0.72f * params.sensitivity);
    std::vector<std::pair<int64_t, float>> scoredStarts;

    if (params.algorithm == ChopAlgorithm::FindSpotsPhrase)
    {
        // Phrase boundaries via RMS valley -> rise transitions
        const auto& env = source.analysis.rmsEnvelope;
        const int hop = std::max(1, source.analysis.hopSamples);
        for (size_t i = 2; i + 2 < env.size(); ++i)
        {
            const int64_t frame = static_cast<int64_t>(i) * hop;
            if (frame < startF || frame >= endF - minLen) continue;
            const float rise = env[i + 1] - env[i - 1];
            if (rise > sensThresh * 0.35f && env[i - 1] < 0.25f)
                scoredStarts.push_back({ frame, juce::jlimit(0.0f, 1.0f, rise * 2.5f) });
        }
    }
    else
    {
        for (const auto& o : source.analysis.onsets)
        {
            if (o.frame < startF || o.frame >= endF - minLen) continue;
            float score = o.strength;
            if (params.algorithm == ChopAlgorithm::FindSpotsTonal && o.band == 1) score *= 1.25f;
            if (params.algorithm == ChopAlgorithm::FindSpotsAttack && o.band != 1) score *= 1.15f;
            if (score >= sensThresh)
                scoredStarts.push_back({ o.frame, std::min(1.0f, score) });
        }
    }

    // Sort by score descending, enforce minSliceSec spacing, take top-K, then sort chronologically
    std::sort(scoredStarts.begin(), scoredStarts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::pair<int64_t, float>> picked;
    const int k = juce::jlimit(1, 32, params.maxCandidates);
    for (const auto& cand : scoredStarts)
    {
        const bool tooClose = std::any_of(picked.begin(), picked.end(),
                                          [&](const auto& p) { return std::abs(p.first - cand.first) < minLen; });
        if (!tooClose)
        {
            picked.push_back(cand);
            if (static_cast<int>(picked.size()) >= k) break;
        }
    }

    std::sort(picked.begin(), picked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    juce::String originTag = "Spot";
    switch (params.algorithm)
    {
        case ChopAlgorithm::FindSpotsAttack: originTag = "Attack"; break;
        case ChopAlgorithm::FindSpotsTonal:  originTag = "Tonal"; break;
        case ChopAlgorithm::FindSpotsPhrase: originTag = "Phrase"; break;
        case ChopAlgorithm::FindSpotsHybrid: originTag = "Hybrid"; break;
        case ChopAlgorithm::ChopThreshold:   originTag = "Thresh"; break;
        default: break;
    }

    for (size_t i = 0; i < picked.size(); ++i)
    {
        const int64_t s = picked[i].first;
        const int64_t e = (i + 1 < picked.size()) ? picked[i + 1].first : std::min(endF, s + std::max(minLen * 4, static_cast<int64_t>(source.sampleRate * 2.0)));
        makeCandidate(s, e, originTag + " " + juce::String(i + 1), picked[i].second);
    }

    return out;
}
} // namespace hps
