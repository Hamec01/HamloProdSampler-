#include "analysis/SourceAnalysis.h"

#include <algorithm>
#include <numeric>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "Analysis/DrumBreakTranscriber.h"
#include "Analysis/SampleHarmonyAnalyzer.h"
#include "Analysis/SampleLabelReader.h"
#include "Analysis/SampleLineTranscriber.h"

namespace hps
{
namespace
{
constexpr double kMaxSourceSeconds   = 20.0 * 60.0;
constexpr double kAnalysisWindowSecs = 64.0; // HPDG: at most 16 bars are ever used

std::vector<float> monoOf(const juce::AudioBuffer<float>& audio, int64_t start, int64_t count)
{
    std::vector<float> mono((size_t) juce::jmax<int64_t>(0, count));
    const int ch = audio.getNumChannels();
    for (int c = 0; c < ch; ++c)
    {
        const float* d = audio.getReadPointer(c);
        for (int64_t i = 0; i < count; ++i)
            mono[(size_t) i] += d[start + i] / (float) ch;
    }
    return mono;
}

// A reader from a fresh format manager (no function-level static: it would be destroyed at DLL unload, and it was
// also shared unsynchronised between the loader thread and the message thread).
std::unique_ptr<juce::AudioFormatReader> createReader(const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader>(fm.createReaderFor(file));
}

void computeFeatures(const juce::AudioBuffer<float>& audio, double sr, double tuningCents,
                     SourceAnalysis& a, const std::function<bool()>& shouldExit)
{
    constexpr int order = 11, N = 1 << order, hop = 256;
    const int64_t total = audio.getNumSamples();
    const auto mono = monoOf(audio, 0, total);
    const int frames = (int) (total / hop) + 1;
    a.featureHop = hop;
    a.specHop = hop * 2;
    a.onsetCurve.assign((size_t) frames, 0.0f);
    a.rms.assign((size_t) frames, 0.0f);
    a.specCols = (frames + 1) / 2;
    a.spectrogram.assign((size_t) a.specCols * (size_t) a.specBins, 0);
    a.chroma.assign((size_t) a.specCols, {});

    juce::dsp::FFT fft(order);
    std::vector<float> window((size_t) N), data((size_t) N * 2), prev((size_t) N / 2, 0.0f);
    for (int i = 0; i < N; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float) i / (float) N);

    const double binHz = sr / N;
    // log-spaced display bands 30 Hz .. min(16 kHz, nyquist)
    std::vector<int> bandLo((size_t) a.specBins), bandHi((size_t) a.specBins);
    const double fLo = 30.0, fHi = std::min(16000.0, sr * 0.5 - binHz);
    for (int b = 0; b < a.specBins; ++b)
    {
        const double f0 = fLo * std::pow(fHi / fLo, (double) b / a.specBins);
        const double f1 = fLo * std::pow(fHi / fLo, (double) (b + 1) / a.specBins);
        bandLo[(size_t) b] = juce::jlimit(1, N / 2 - 1, (int) std::floor(f0 / binHz));
        bandHi[(size_t) b] = juce::jlimit(bandLo[(size_t) b], N / 2 - 1, (int) std::ceil(f1 / binHz));
    }
    const double tuning = juce::jlimit(-50.0, 50.0, tuningCents) / 100.0;
    const int chromaLo = juce::jmax(2, (int) (65.0 / binHz)), chromaHi = juce::jmin(N / 2 - 2, (int) (2100.0 / binHz));
    std::vector<int> binPc((size_t) N / 2, -1);
    for (int bin = chromaLo; bin <= chromaHi; ++bin)
        binPc[(size_t) bin] = (int) ((std::lround(12.0 * std::log2(bin * binHz / 440.0) + 69.0 - tuning) % 12 + 12) % 12);

    const float norm = 2.0f / (float) N;
    for (int f = 0; f < frames; ++f)
    {
        if ((f & 255) == 0 && shouldExit && shouldExit()) return;
        std::fill(data.begin(), data.end(), 0.0f);
        const int64_t start = (int64_t) f * hop - N / 2;
        double sumSq = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const int64_t idx = start + i;
            if (idx >= 0 && idx < total)
            {
                const float x = mono[(size_t) idx];
                data[(size_t) i] = x * window[(size_t) i];
                if (i >= N / 2 - hop / 2 && i < N / 2 + hop / 2) sumSq += (double) x * x;
            }
        }
        a.rms[(size_t) f] = (float) std::sqrt(sumSq / hop);
        fft.performFrequencyOnlyForwardTransform(data.data());

        double flux = 0.0;
        for (int bin = 1; bin < N / 2; ++bin)
        {
            const float m = data[(size_t) bin] * norm;
            const float lm = std::log1p(100.0f * m);
            const float d = lm - prev[(size_t) bin];
            const double hz = bin * binHz;
            if (d > 0.0f) flux += d * (hz < 200.0 ? 1.2 : (hz < 5000.0 ? 1.0 : 0.6));
            prev[(size_t) bin] = lm;
        }
        a.onsetCurve[(size_t) f] = (float) flux;

        if ((f & 1) == 0)
        {
            const int col = f / 2;
            auto* out = a.spectrogram.data() + (size_t) col * (size_t) a.specBins;
            for (int b = 0; b < a.specBins; ++b)
            {
                float mx = 0.0f;
                for (int bin = bandLo[(size_t) b]; bin <= bandHi[(size_t) b]; ++bin) mx = std::max(mx, data[(size_t) bin]);
                const float db = 20.0f * std::log10(mx * norm + 1.0e-9f);
                out[b] = (uint8_t) juce::jlimit(0, 255, (int) ((db + 90.0f) / 90.0f * 255.0f));
            }
            auto& chroma = a.chroma[(size_t) col];
            for (int bin = chromaLo; bin <= chromaHi; ++bin)
            {
                const float m = data[(size_t) bin];
                if (m > data[(size_t) bin - 1] && m >= data[(size_t) bin + 1])
                    chroma[(size_t) binPc[(size_t) bin]] += m;
            }
            const float mx = *std::max_element(chroma.begin(), chroma.end());
            if (mx > 1.0e-9f) for (auto& v : chroma) v /= mx;
        }
    }

    // normalise curves (robust: 99th percentile)
    auto normalise = [](std::vector<float>& v)
    {
        if (v.empty()) return;
        auto tmp = v;
        const size_t k = (size_t) ((double) tmp.size() * 0.99);
        std::nth_element(tmp.begin(), tmp.begin() + (long long) std::min(k, tmp.size() - 1), tmp.end());
        const float ref = std::max(1.0e-9f, tmp[std::min(k, tmp.size() - 1)]);
        for (auto& x : v) x = std::min(1.0f, x / ref);
    };
    normalise(a.onsetCurve);
    normalise(a.rms);

    // peak picking: local max, above a moving median + delta, min spacing 30 ms
    const int spacing = juce::jmax(2, (int) (0.030 * sr / hop));
    const int medR = 16;
    std::vector<float> win;
    int64_t lastFrame = -1000000;
    for (int f = 2; f < frames - 2; ++f)
    {
        const float v = a.onsetCurve[(size_t) f];
        if (v < 0.06f) continue;
        bool isMax = true;
        for (int k = -3; k <= 3 && isMax; ++k)
            if (k != 0 && f + k >= 0 && f + k < frames && a.onsetCurve[(size_t) (f + k)] > v) isMax = false;
        if (!isMax) continue;
        win.clear();
        for (int k = -medR; k <= medR; ++k)
            if (f + k >= 0 && f + k < frames) win.push_back(a.onsetCurve[(size_t) (f + k)]);
        std::nth_element(win.begin(), win.begin() + (long long) (win.size() / 2), win.end());
        if (v < win[win.size() / 2] + 0.07f) continue;
        if (f - lastFrame < spacing)
        {
            if (!a.onsetStrength.empty() && v > a.onsetStrength.back())
            {
                a.onsets.back() = (int64_t) f * hop;
                a.onsetStrength.back() = v;
                lastFrame = f;
            }
            continue;
        }
        a.onsets.push_back((int64_t) f * hop);
        a.onsetStrength.push_back(v);
        lastFrame = f;
    }
    for (auto& o : a.onsets)
        o = SourceAnalyzer::refineAttackStart(audio, o, sr);
}
} // namespace

WaveformPeaks SourceAnalyzer::computePeaks(const juce::AudioBuffer<float>& audio, int framesPerPeak)
{
    WaveformPeaks p;
    p.framesPerPeak = framesPerPeak;
    const int64_t n = audio.getNumSamples();
    const int64_t count = (n + framesPerPeak - 1) / framesPerPeak;
    p.mins.assign((size_t) count, 0.0f);
    p.maxs.assign((size_t) count, 0.0f);
    const int ch = audio.getNumChannels();
    for (int64_t i = 0; i < count; ++i)
    {
        float mn = 1.0f, mx = -1.0f;
        const int64_t s = i * framesPerPeak, e = std::min(n, s + framesPerPeak);
        for (int c = 0; c < ch; ++c)
        {
            const float* d = audio.getReadPointer(c);
            for (int64_t k = s; k < e; ++k) { mn = std::min(mn, d[k]); mx = std::max(mx, d[k]); }
        }
        p.mins[(size_t) i] = mn > mx ? 0.0f : mn;
        p.maxs[(size_t) i] = mn > mx ? 0.0f : mx;
    }
    return p;
}

int64_t SourceAnalyzer::nearestZeroCrossing(const juce::AudioBuffer<float>& audio, int64_t frame, int64_t radius)
{
    const int64_t n = audio.getNumSamples();
    if (n < 2) return frame;
    frame = juce::jlimit<int64_t>(0, n - 1, frame);
    auto mono = [&](int64_t i)
    {
        float s = 0.0f;
        for (int c = 0; c < audio.getNumChannels(); ++c) s += audio.getSample(c, (int) i);
        return s;
    };
    for (int64_t d = 0; d <= radius; ++d)
    {
        for (int sign : { -1, 1 })
        {
            const int64_t i = frame + sign * d;
            if (i <= 0 || i >= n) continue;
            const float a = mono(i - 1), b = mono(i);
            if ((a <= 0.0f && b >= 0.0f) || (a >= 0.0f && b <= 0.0f)) return i;
        }
    }
    return frame;
}

int64_t SourceAnalyzer::refineAttackStart(const juce::AudioBuffer<float>& audio, int64_t approx, double sr)
{
    const int64_t n = audio.getNumSamples();
    const int64_t s = juce::jlimit<int64_t>(0, n, approx - (int64_t) (0.025 * sr));
    const int64_t e = juce::jlimit<int64_t>(0, n, approx + (int64_t) (0.030 * sr));
    if (e - s < 8) return approx;
    auto absAt = [&](int64_t i)
    {
        float m = 0.0f;
        for (int c = 0; c < audio.getNumChannels(); ++c) m = std::max(m, std::abs(audio.getSample(c, (int) i)));
        return m;
    };
    int64_t peakPos = s;
    float peak = 0.0f;
    for (int64_t i = s; i < e; ++i)
    {
        const float v = absAt(i);
        if (v > peak) { peak = v; peakPos = i; }
    }
    if (peak < 1.0e-4f) return approx;
    // walk back from the peak while a 32-frame max envelope stays above 12 % of the peak
    const int win = 32;
    int64_t pos = peakPos;
    while (pos - win > s)
    {
        float m = 0.0f;
        for (int64_t k = pos - win; k < pos; ++k) m = std::max(m, absAt(k));
        if (m < 0.12f * peak) break;
        pos -= win;
    }
    return juce::jlimit<int64_t>(0, n - 1, pos);
}

std::shared_ptr<SourceData> SourceAnalyzer::decode(const juce::File& file, const juce::String& id, juce::String& error)
{
    auto reader = createReader(file);
    if (reader == nullptr) { error = "Unsupported or unreadable file: " + file.getFileName(); return nullptr; }
    if (reader->lengthInSamples <= 16) { error = "File is empty: " + file.getFileName(); return nullptr; }
    if ((double) reader->lengthInSamples / reader->sampleRate > kMaxSourceSeconds)
    {
        error = "File is longer than 20 minutes: " + file.getFileName();
        return nullptr;
    }
    auto data = std::make_shared<SourceData>();
    data->id = id;
    data->sampleRate = reader->sampleRate;
    data->numFrames = reader->lengthInSamples;
    const int len = (int) reader->lengthInSamples;
    auto buf = std::make_shared<juce::AudioBuffer<float>>(2, len);
    buf->clear();
    const int ch = (int) reader->numChannels;
    if (ch >= 2)
    {
        reader->read(buf.get(), 0, len, 0, true, true);
    }
    else
    {
        reader->read(buf.get(), 0, len, 0, true, false);
        buf->copyFrom(1, 0, *buf, 0, 0, len);
    }
    // sanitize NaN/inf
    for (int c = 0; c < 2; ++c)
    {
        auto* d = buf->getWritePointer(c);
        for (int i = 0; i < len; ++i) if (!std::isfinite(d[i])) d[i] = 0.0f;
    }
    data->peaks = computePeaks(*buf, 64);
    data->audio = std::move(buf);
    return data;
}

void SourceAnalyzer::analyse(SourceData& data, const juce::File& file, const std::function<bool()>& shouldExit)
{
    auto& a = data.analysis;
    const auto& audio = *data.audio;
    const double sr = data.sampleRate;
    const int64_t total = audio.getNumSamples();

    // labels (file name / acid chunk)
    bbg::SampleLabels labels;
    if (auto reader = createReader(file))
        labels = bbg::SampleLabelReader::read(file, reader->metadataValues);
    else
        labels = bbg::SampleLabelReader::fromName(file.getFileName());
    if (labels.hasBpm() || labels.hasKey()) a.labelInfo = "file says: " + labels.describe().fromFirstOccurrenceOf("labels: ", false, false);

    const int64_t windowLen = std::min<int64_t>(total, (int64_t) (kAnalysisWindowSecs * sr));
    const auto mono = monoOf(audio, 0, windowLen);
    const bool audioCut = windowLen < total;

    bbg::DrumBreakOptions opts;
    opts.labelBpm = labels.bpm;
    opts.audioCut = audioCut;
    bbg::DrumBreakTranscriber transcriber;
    auto tempo = transcriber.analyze(mono, sr, opts);
    if (shouldExit && shouldExit()) return;

    if (audioCut && labels.bpm <= 20.0)
    {
        const int64_t secondStart = std::min<int64_t>(total - windowLen, (int64_t) (total * bbg::DrumBreakTranscriber::kSongSecondWindowStart));
        if (secondStart > 0)
        {
            const auto second = transcriber.analyze(monoOf(audio, secondStart, windowLen), sr, opts);
            if (bbg::DrumBreakTranscriber::preferSecondWindowTempo(tempo, second))
            {
                auto forced = opts;
                forced.forcedBpm = second.bpm;
                tempo = transcriber.analyze(mono, sr, forced); // origin on the window that plays
                tempo.tempoConfidence = second.tempoConfidence;
            }
        }
    }
    if (shouldExit && shouldExit()) return;

    if (tempo.valid && tempo.bpm > 20.0)
    {
        a.bpm = tempo.bpm;
        a.bpmConfidence = tempo.tempoConfidence;
        a.originSeconds = std::max(0.0, tempo.originSeconds);
        a.bars = tempo.bars;
        a.exactLoop = tempo.exactLoop;
        a.swingPercent = tempo.swingPercent;
    }
    a.drumLikelihood = tempo.valid ? tempo.drumLoopConfidence : 0.0f;

    a.tuningCents = bbg::SampleLineTranscriber::estimateTuningCents(mono, sr);
    if (shouldExit && shouldExit()) return;

    bbg::SampleHarmonyAnalyzer harmonyAnalyzer;
    const double beat = a.bpm > 20.0 ? 60.0 / a.bpm : 0.5;
    const auto harmony = harmonyAnalyzer.analyze(mono, sr, beat, a.originSeconds, a.tuningCents, labels.keyRoot, labels.keyMode);
    if (harmony.valid)
    {
        a.key = { harmony.keyRoot, harmony.scaleMode };
        a.keyConfidence = harmony.keyConfidence;
    }
    // Percussion / noise: never silently transpose. A drum loop with no stated key, or a key the
    // chroma cannot support, is Unknown.
    a.atonal = !harmony.valid
        || (!labels.hasKey() && a.drumLikelihood >= 0.6f && a.keyConfidence < 0.5f)
        || a.keyConfidence < 0.10f;

    if (shouldExit && shouldExit()) return;
    computeFeatures(audio, sr, a.tuningCents, a, shouldExit);

    juce::String s;
    s << (a.bpm > 20.0 ? juce::String(a.bpm, 1) + " BPM (" + juce::String(juce::roundToInt(a.bpmConfidence * 100)) + "%)" : juce::String("BPM ?"));
    s << " | " << (a.atonal ? juce::String("no key") : a.key.label() + " (" + juce::String(juce::roundToInt(a.keyConfidence * 100)) + "%)");
    if (a.labelInfo.isNotEmpty()) s << " | " << a.labelInfo;
    a.summary = s;
    a.ready = true;
}

// --------------------------------------------------------------------------------------------

SourceLoader::SourceLoader() : juce::Thread("HPSAMPLER source loader") {}

SourceLoader::~SourceLoader() { *alive = false; stopThread(4000); }

void SourceLoader::load(int slot, const juce::File& file, const juce::String& id)
{
    {
        const juce::ScopedLock sl(lock);
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [slot](const Job& j) { return j.slot == slot; }), jobs.end());
        jobs.push_back({ slot, file, id });
    }
    busy = true;
    if (!isThreadRunning()) startThread(juce::Thread::Priority::low); // decoding / analysis never competes with the DAW
    notify();
}

bool SourceLoader::isBusy() const { return busy.load(); }

void SourceLoader::run()
{
    while (!threadShouldExit())
    {
        Job job;
        {
            const juce::ScopedLock sl(lock);
            if (jobs.empty()) { busy = false; }
            else { job = jobs.front(); jobs.erase(jobs.begin()); }
        }
        if (job.id.isEmpty()) { wait(500); continue; }

        juce::String error;
        auto data = SourceAnalyzer::decode(job.file, job.id, error);
        if (data == nullptr)
        {
            juce::MessageManager::callAsync([this, flag = alive, slot = job.slot, error] { if (*flag && onError) onError(slot, error); });
            continue;
        }
        std::shared_ptr<const SourceData> decoded = std::make_shared<SourceData>(*data);
        juce::MessageManager::callAsync([this, flag = alive, slot = job.slot, decoded] { if (*flag && onResult) onResult(slot, decoded, false); });

        const auto id = job.id;
        const int slot = job.slot;
        SourceAnalyzer::analyse(*data, job.file, [this, slot, id]
        {
            if (threadShouldExit()) return true;
            const juce::ScopedLock sl(lock);
            for (const auto& j : jobs) if (j.slot == slot) return true; // superseded
            return false;
        });
        if (!data->analysis.ready) continue;
        std::shared_ptr<const SourceData> analysed = std::move(data);
        juce::MessageManager::callAsync([this, flag = alive, slot, analysed] { if (*flag && onResult) onResult(slot, analysed, true); });
    }
}
} // namespace hps
