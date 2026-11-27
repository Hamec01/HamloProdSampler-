// HPStems.exe — stem separation helper for HPSAMPLER (HTDemucs v4 through ONNX Runtime).
//
// Runs as its own process so a long, memory-hungry separation can never take the DAW down.
//
//   HPStems.exe --in <input.wav> --out <dir> [--model <htdemucs.onnx>] [--threads N]
//
// Reads any WAV (HPSAMPLER writes the decoded source, so frames line up exactly with the source),
// writes <dir>/drums.wav bass.wav other.wav vocals.wav (32-bit float, the input's rate and length)
// and <dir>/stems.json last (its presence means "complete").
// stdout, one line each:  HPSTEMS PROGRESS <0..1> | HPSTEMS DONE | HPSTEMS ERROR <text>
//
// The graph is HTDemucs without its STFT (see tools/stems/export_htdemucs_onnx.py); the STFT,
// iSTFT, segmenting and overlap-add here reproduce demucs.apply.apply_model(split, overlap 0.25, no shifts).

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <onnxruntime_cxx_api.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>
#endif
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr int kRate = 44100;
constexpr int kSegment = 343980;            // int(7.8 * 44100)
constexpr int kNfft = 4096, kHop = 1024, kBins = 2048;
constexpr int kFrames = 336;                // ceil(kSegment / kHop)
constexpr int kSources = 4;
const char* const kSourceNames[kSources] = { "drums", "bass", "other", "vocals" };

void say(const juce::String& s)
{
    std::fputs(("HPSTEMS " + s + "\n").toRawUTF8(), stdout);
    std::fflush(stdout);
}

// --------------------------------------------------------------------------------------------
// Offline windowed-sinc resampler: zero phase, so output frame m sits exactly at input time m / ratio.
std::vector<float> resample(const float* in, int64_t n, double fromRate, double toRate, int64_t outLen)
{
    std::vector<float> out((size_t) outLen, 0.0f);
    const double step = fromRate / toRate;              // input frames per output frame
    const double cutoff = std::min(1.0, toRate / fromRate) * 0.97;
    constexpr int half = 32;                            // taps each side (at the cutoff's scale)
    const double reach = half / cutoff;
    // kernel table over |x| in [0, reach], linear interpolation between entries
    constexpr int perUnit = 1024;
    const int tableLen = (int) std::ceil(reach * perUnit) + 2;
    std::vector<float> table((size_t) tableLen);
    const double beta = 8.6, i0b = std::cyl_bessel_i(0.0, beta);
    for (int i = 0; i < tableLen; ++i)
    {
        const double x = (double) i / perUnit, r = std::min(1.0, x / reach);
        const double win = std::cyl_bessel_i(0.0, beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        const double arg = juce::MathConstants<double>::pi * x * cutoff;
        table[(size_t) i] = (float) (cutoff * (x < 1e-9 ? 1.0 : std::sin(arg) / arg) * win);
    }
    for (int64_t m = 0; m < outLen; ++m)
    {
        const double t = (double) m * step;
        const int64_t a = (int64_t) std::ceil(t - reach), b = (int64_t) std::floor(t + reach);
        double acc = 0.0;
        for (int64_t k = std::max<int64_t>(0, a); k <= std::min<int64_t>(n - 1, b); ++k)
        {
            const double p = std::abs((double) k - t) * perUnit;
            const int i = (int) p;
            const float fr = (float) (p - i);
            acc += in[k] * (table[(size_t) i] + fr * (table[(size_t) i + 1] - table[(size_t) i]));
        }
        out[(size_t) m] = (float) acc;
    }
    return out;
}

// --------------------------------------------------------------------------------------------
// Demucs spectrogram (HTDemucs._spec / _ispec with torch.stft / istft, normalized, periodic Hann).
struct Spectral
{
    juce::dsp::FFT fft { 12 };
    std::vector<float> window = std::vector<float>(kNfft), work = std::vector<float>(kNfft * 2);
    std::vector<float> env;   // iSTFT window-square envelope over the kept output range

    Spectral()
    {
        for (int i = 0; i < kNfft; ++i)
            window[(size_t) i] = 0.5f - 0.5f * (float) std::cos(2.0 * juce::MathConstants<double>::pi * i / kNfft);
        // istft frames t = 0..kFrames+3 start at t*hop in the OLA buffer; output n reads OLA[n + nfft/2];
        // _ispec keeps n in [pad, pad + kSegment) with pad = 1536.
        constexpr int pad = kHop / 2 * 3;
        env.assign(kSegment, 0.0f);
        for (int n = 0; n < kSegment; ++n)
        {
            const int m = n + pad + kNfft / 2;
            double e = 0.0;
            for (int t = std::max(0, (m - kNfft) / kHop); t <= std::min(kFrames + 3, m / kHop); ++t)
            {
                const int i = m - t * kHop;
                if (i >= 0 && i < kNfft) e += (double) window[(size_t) i] * window[(size_t) i];
            }
            env[(size_t) n] = (float) e;
        }
    }

    // x: one channel, kSegment samples. Writes re / im planes [kBins][kFrames].
    void forward(const float* x, float* re, float* im)
    {
        constexpr int pad = kHop / 2 * 3;
        const float scale = 1.0f / std::sqrt((float) kNfft);
        auto at = [&](int i) -> float // reflect-padded signal (pad left, pad + 64 right)
        {
            int j = i - pad;
            if (j < 0) j = -j;
            if (j >= kSegment) j = 2 * (kSegment - 1) - j;
            return x[j];
        };
        for (int k = 0; k < kFrames; ++k)
        {
            for (int i = 0; i < kNfft; ++i) work[(size_t) i] = at(k * kHop + i) * window[(size_t) i];
            std::fill(work.begin() + kNfft, work.end(), 0.0f);
            fft.performRealOnlyForwardTransform(work.data(), true);
            for (int f = 0; f < kBins; ++f)
            {
                re[(size_t) f * kFrames + (size_t) k] = work[(size_t) (2 * f)] * scale;
                im[(size_t) f * kFrames + (size_t) k] = work[(size_t) (2 * f + 1)] * scale;
            }
        }
    }

    // re / im planes [kBins][kFrames] -> adds kSegment samples to y.
    void inverse(const float* re, const float* im, float* y)
    {
        constexpr int pad = kHop / 2 * 3;
        const float scale = std::sqrt((float) kNfft);
        for (int k = 0; k < kFrames; ++k)
        {
            std::fill(work.begin(), work.end(), 0.0f);
            for (int f = 0; f < kBins; ++f)
            {
                work[(size_t) (2 * f)] = re[(size_t) f * kFrames + (size_t) k] * scale;
                work[(size_t) (2 * f + 1)] = f == 0 ? 0.0f : im[(size_t) f * kFrames + (size_t) k] * scale;
            }
            fft.performRealOnlyInverseTransform(work.data());
            // frame k starts at output sample k*hop - pad
            const int start = k * kHop - pad;
            for (int i = 0; i < kNfft; ++i)
            {
                const int n = start + i;
                if (n < 0 || n >= kSegment) continue;
                y[n] += work[(size_t) i] * window[(size_t) i] / env[(size_t) n];
            }
        }
    }
};

struct Args { juce::File in, out, model; int threads = 0; };

bool parse(const juce::StringArray& argv, Args& a)
{
    for (int i = 1; i + 1 < argv.size(); i += 2)
    {
        const juce::String k = argv[i], v = argv[i + 1];
        if (k == "--in") a.in = juce::File(v);
        else if (k == "--out") a.out = juce::File(v);
        else if (k == "--model") a.model = juce::File(v);
        else if (k == "--threads") a.threads = v.getIntValue();
        else return false;
    }
    if (a.model == juce::File())
        a.model = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("htdemucs.onnx");
    return a.in != juce::File() && a.out != juce::File();
}

int run(const Args& args)
{
    if (!args.in.existsAsFile()) { say("ERROR input not found: " + args.in.getFullPathName()); return 2; }
    if (!args.model.existsAsFile()) { say("ERROR model not found: " + args.model.getFullPathName()); return 2; }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(args.in));
    if (reader == nullptr || reader->lengthInSamples <= 0) { say("ERROR cannot read " + args.in.getFileName()); return 2; }
    const double srcRate = reader->sampleRate;
    const int64_t srcLen = reader->lengthInSamples;
    if (srcLen > (int64_t) srcRate * 60 * 30) { say("ERROR longer than 30 minutes"); return 2; }
    juce::AudioBuffer<float> src(2, (int) srcLen);
    reader->read(&src, 0, (int) srcLen, 0, true, true);
    if (reader->numChannels == 1) src.copyFrom(1, 0, src, 0, 0, (int) srcLen);
    reader.reset();
    say("PROGRESS 0.01");

    // to 44.1 kHz
    const bool resampled = std::abs(srcRate - kRate) > 0.5;
    const int64_t n = resampled ? (int64_t) std::llround((double) srcLen * kRate / srcRate) : srcLen;
    std::vector<float> mix[2];
    for (int c = 0; c < 2; ++c)
        mix[c] = resampled ? resample(src.getReadPointer(c), srcLen, srcRate, kRate, n)
                           : std::vector<float>(src.getReadPointer(c), src.getReadPointer(c) + n);
    src.setSize(0, 0);

    // normalise by the mono reference (separate.py)
    double sum = 0.0, sq = 0.0;
    for (int64_t i = 0; i < n; ++i) { const double r = 0.5 * ((double) mix[0][(size_t) i] + mix[1][(size_t) i]); sum += r; sq += r * r; }
    const double mean = sum / (double) n;
    const double stdev = std::sqrt(std::max(1e-12, (sq - (double) n * mean * mean) / (double) std::max<int64_t>(1, n - 1)));
    for (auto& ch : mix) for (auto& v : ch) v = (float) ((v - mean) / stdev);

    // model
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "HPStems");
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(args.threads > 0 ? args.threads : juce::jmax(1, juce::SystemStats::getNumPhysicalCpus()));
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session(env, args.model.getFullPathName().toWideCharPointer(), so);
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const char* inNames[] = { "mix", "spec" };
    const char* outNames[] = { "freq", "time" };
    const int64_t mixShape[] = { 1, 2, kSegment }, specShape[] = { 1, 4, kBins, kFrames };
    say("PROGRESS 0.03");

    // segments (apply_model split): triangle weights, 25 % overlap
    const int stride = (int) (0.75 * kSegment);
    std::vector<float> weight(kSegment);
    for (int i = 0; i < kSegment; ++i)
        weight[(size_t) i] = (float) (i < kSegment / 2 ? i + 1 : kSegment - i) / (float) (kSegment / 2);
    std::vector<float> out((size_t) kSources * 2 * (size_t) n, 0.0f), sumWeight((size_t) n, 0.0f);
    std::vector<float> segIn((size_t) 2 * kSegment), specIn((size_t) 4 * kBins * kFrames), y(kSegment);
    Spectral sp;
    const int64_t numSegments = (n + stride - 1) / stride;

    for (int64_t s = 0; s < numSegments; ++s)
    {
        const int64_t offset = s * stride;
        const int64_t len = std::min<int64_t>(kSegment, n - offset);
        const int64_t delta = kSegment - len;
        const int64_t start = offset - delta / 2;  // TensorChunk.padded: centred, real context where it exists
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kSegment; ++i)
            {
                const int64_t j = start + i;
                segIn[(size_t) c * kSegment + (size_t) i] = (j >= 0 && j < n) ? mix[c][(size_t) j] : 0.0f;
            }
        for (int c = 0; c < 2; ++c)
            sp.forward(&segIn[(size_t) c * kSegment], &specIn[(size_t) (2 * c) * kBins * kFrames],
                       &specIn[(size_t) (2 * c + 1) * kBins * kFrames]);

        Ort::Value ins[] = {
            Ort::Value::CreateTensor<float>(mem, segIn.data(), segIn.size(), mixShape, 3),
            Ort::Value::CreateTensor<float>(mem, specIn.data(), specIn.size(), specShape, 4) };
        auto outs = session.Run(Ort::RunOptions { nullptr }, inNames, ins, 2, outNames, 2);
        const float* freq = outs[0].GetTensorData<float>(); // [1][S][4][F][T]
        const float* time = outs[1].GetTensorData<float>(); // [1][S][2][L]

        const int64_t trim = delta / 2;                       // center_trim to len
        for (int src = 0; src < kSources; ++src)
            for (int c = 0; c < 2; ++c)
            {
                std::copy_n(time + ((size_t) (src * 2 + c)) * kSegment, kSegment, y.begin());
                const float* re = freq + ((size_t) (src * 4 + 2 * c)) * kBins * kFrames;
                sp.inverse(re, re + (size_t) kBins * kFrames, y.data());
                float* dst = &out[((size_t) (src * 2 + c)) * (size_t) n];
                for (int64_t i = 0; i < len; ++i)
                    dst[offset + i] += weight[(size_t) i] * y[(size_t) (trim + i)];
            }
        for (int64_t i = 0; i < len; ++i) sumWeight[(size_t) (offset + i)] += weight[(size_t) i];
        say("PROGRESS " + juce::String(0.05 + 0.85 * (double) (s + 1) / (double) numSegments, 3));
    }

    // write
    if (!args.out.createDirectory()) { say("ERROR cannot create " + args.out.getFullPathName()); return 3; }
    juce::WavAudioFormat wav;
    juce::Array<juce::File> written;
    for (int src = 0; src < kSources; ++src)
    {
        juce::AudioBuffer<float> b(2, (int) srcLen);
        for (int c = 0; c < 2; ++c)
        {
            float* d = &out[((size_t) (src * 2 + c)) * (size_t) n];
            for (int64_t i = 0; i < n; ++i) d[i] = (float) (d[i] / sumWeight[(size_t) i] * stdev + mean);
            if (resampled)
            {
                const auto back = resample(d, n, kRate, srcRate, srcLen);
                b.copyFrom(c, 0, back.data(), (int) srcLen);
            }
            else b.copyFrom(c, 0, d, (int) srcLen);
        }
        const auto tmp = args.out.getChildFile(juce::String(kSourceNames[src]) + ".wav.part");
        tmp.deleteFile();
        std::unique_ptr<juce::OutputStream> os(tmp.createOutputStream().release());
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), srcRate, 2, 32, {}, 0));
        if (w == nullptr) { say("ERROR cannot write " + tmp.getFullPathName()); return 3; }
        os.release();
        w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
        w.reset();
        const auto fin = args.out.getChildFile(juce::String(kSourceNames[src]) + ".wav");
        fin.deleteFile();
        if (!tmp.moveFileTo(fin)) { say("ERROR cannot write " + fin.getFullPathName()); return 3; }
        written.add(fin);
        say("PROGRESS " + juce::String(0.9 + 0.025 * (src + 1), 3));
    }

    auto* o = new juce::DynamicObject();
    o->setProperty("format", "hp.stems.v1");
    o->setProperty("model", "htdemucs");
    o->setProperty("sampleRate", srcRate);
    o->setProperty("frames", (juce::int64) srcLen);
    juce::Array<juce::var> names;
    for (auto* s : kSourceNames) names.add(juce::String(s));
    o->setProperty("stems", names);
    args.out.getChildFile("stems.json").replaceWithText(juce::JSON::toString(juce::var(o)));
    say("DONE");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    juce::StringArray argList;
   #if JUCE_WINDOWS
    // wide command line: paths with non-ASCII letters (Documents of a Cyrillic user name) survive
    int wargc = 0;
    if (auto** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        for (int i = 0; i < wargc; ++i) argList.add(juce::String(wargv[i]));
        LocalFree(wargv);
    }
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); // the DAW's audio keeps the CPU it needs
    juce::ignoreUnused(argc, argv);
   #else
    for (int i = 0; i < argc; ++i) argList.add(juce::String::fromUTF8(argv[i]));
   #endif
    Args args;
    if (!parse(argList, args))
    {
        say("ERROR usage: HPStems --in <input.wav> --out <dir> [--model <htdemucs.onnx>] [--threads N]");
        return 1;
    }
    try { return run(args); }
    catch (const Ort::Exception& e) { say(juce::String("ERROR onnx: ") + e.what()); }
    catch (const std::exception& e) { say(juce::String("ERROR ") + e.what()); }
    catch (...) { say("ERROR unknown failure"); }
    return 4;
}
