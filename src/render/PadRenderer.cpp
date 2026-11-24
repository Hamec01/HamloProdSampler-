#include "render/PadRenderer.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

namespace hps
{
namespace
{
void hashMix(uint64_t& h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }
uint64_t bitsOf(double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; }

// Band-limited resampling of one channel by `ratio` (input samples per output sample).
void resampleChannel(const float* in, int inLen, float* out, int outLen, double ratio)
{
    juce::WindowedSincInterpolator interp;
    interp.reset();
    // the interpolator may read a few samples past the end: pad with silence
    std::vector<float> padded((size_t) inLen + 64, 0.0f);
    std::copy(in, in + inLen, padded.begin());
    interp.process(ratio, padded.data(), out, outLen, (int) padded.size(), 0);
}

void resample(juce::AudioBuffer<float>& buf, double ratio)
{
    if (std::abs(ratio - 1.0) < 1.0e-9) return;
    const int inLen = buf.getNumSamples();
    const int outLen = juce::jmax(1, (int) std::floor(inLen / ratio));
    juce::AudioBuffer<float> out(buf.getNumChannels(), outLen);
    for (int c = 0; c < buf.getNumChannels(); ++c)
        resampleChannel(buf.getReadPointer(c), inLen, out.getWritePointer(c), outLen, ratio);
    buf = std::move(out);
}

void stretch(juce::AudioBuffer<float>& buf, double sampleRate, double timeRatio, float semitones)
{
    const int inLen = buf.getNumSamples();
    const int outLen = juce::jmax(1, (int) std::llround(inLen * timeRatio));
    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault(2, (float) sampleRate);
    st.setTransposeSemitones(semitones);
    // exact() needs at least seekLength input frames: pad short slices with silence at the end
    const double rate = (double) inLen / outLen;
    const int need = st.outputSeekLength((float) rate) + 16;
    int paddedIn = inLen, paddedOut = outLen;
    if (inLen < need)
    {
        paddedIn = need;
        paddedOut = (int) std::llround(paddedIn / rate);
    }
    juce::AudioBuffer<float> in(2, paddedIn), out(2, paddedOut);
    in.clear();
    for (int c = 0; c < 2; ++c) in.copyFrom(c, 0, buf, std::min(c, buf.getNumChannels() - 1), 0, inLen);
    float* inPtrs[2] = { in.getWritePointer(0), in.getWritePointer(1) };
    float* outPtrs[2] = { out.getWritePointer(0), out.getWritePointer(1) };
    if (!st.exact(inPtrs, paddedIn, outPtrs, paddedOut))
        return; // leave the original (too short even padded)
    juce::AudioBuffer<float> res(2, outLen);
    for (int c = 0; c < 2; ++c) res.copyFrom(c, 0, out, c, 0, std::min(outLen, paddedOut));
    buf = std::move(res);
}
} // namespace

uint64_t PadRenderJob::key() const
{
    uint64_t h = 1469598103934665603ull;
    hashMix(h, (uint64_t) reinterpret_cast<uintptr_t>(audio.get()));
    hashMix(h, bitsOf(sourceRate)); hashMix(h, bitsOf(targetRate));
    hashMix(h, (uint64_t) start); hashMix(h, (uint64_t) end);
    hashMix(h, reverse ? 1u : 0u);
    hashMix(h, bitsOf(fadeInMs)); hashMix(h, bitsOf(fadeOutMs));
    hashMix(h, (uint64_t) sync);
    hashMix(h, bitsOf(timeRatio)); hashMix(h, bitsOf(speed)); hashMix(h, bitsOf(transpose));
    return h;
}

PadRenderJob PadRenderer::makeJob(const Project& project, int pad, double targetRate, PadSyncInfo* info)
{
    PadRenderJob job;
    job.pad = pad;
    job.targetRate = targetRate;
    const auto& st = project.state();
    const auto& p = st.pads[(size_t) pad];
    if (!p.assigned) return job;
    const auto data = project.sourceDataForSlot(p.src);
    if (data == nullptr || data->audio == nullptr) return job;
    const auto& slot = st.sources[(size_t) p.src];

    job.audio = data->audio;
    job.sourceRate = data->sampleRate;
    job.start = juce::jlimit<int64_t>(0, data->numFrames - 1, p.start);
    job.end = juce::jlimit<int64_t>(job.start + 1, data->numFrames, p.end);
    job.reverse = p.s.reverse;
    job.fadeInMs = p.s.fadeInMs;
    job.fadeOutMs = p.s.fadeOutMs;
    job.sync = effectiveSync(p, st.settings);

    const double srcBpm = effectiveSourceBpm(slot, data.get());
    const double projBpm = st.settings.bpm > 20.0 ? st.settings.bpm : 120.0;
    const bool tempoKnown = srcBpm > 20.0;
    const MusicalKey srcKey = effectiveSourceKey(slot, data.get());
    const bool keyFollow = p.s.followKey && job.sync != SyncMode::Off && job.sync != SyncMode::Repitch
                           && srcKey.known() && st.settings.key.known();
    const int keyShift = keyFollow ? keyTransposeSemitones(srcKey, st.settings.key) : 0;
    const float user = p.s.pitch + p.s.fine / 100.0f;
    const double userSpeed = juce::jlimit(0.125, 8.0, (double) p.s.speed);

    switch (job.sync)
    {
        case SyncMode::Off:
            job.timeRatio = 1.0 / userSpeed;
            job.transpose = user;
            break;
        case SyncMode::Repitch:
            job.speed = (tempoKnown ? projBpm / srcBpm : 1.0) * userSpeed * std::pow(2.0, user / 12.0);
            job.speed = juce::jlimit(0.125, 8.0, job.speed);
            break;
        case SyncMode::Stretch:
            job.timeRatio = juce::jlimit(0.125, 8.0, (tempoKnown ? srcBpm / projBpm : 1.0) / userSpeed);
            job.transpose = user + (float) keyShift;
            break;
    }

    if (info != nullptr)
    {
        info->sync = job.sync;
        info->tempoKnown = tempoKnown;
        info->userSpeed = userSpeed;
        info->sourceBpm = srcBpm;
        info->timeRatio = job.sync == SyncMode::Repitch ? 1.0 / job.speed : job.timeRatio;
        info->keyShift = keyShift;
        info->keyFollowActive = keyFollow;
        info->totalTranspose = job.sync == SyncMode::Repitch ? (float) (12.0 * std::log2(job.speed)) : job.transpose;
        info->extreme = info->timeRatio < 0.5 || info->timeRatio > 2.0 || std::abs(info->totalTranspose) > 12.0f;
        info->outputSeconds = (double) (job.end - job.start) / job.sourceRate * info->timeRatio;
    }
    return job;
}

std::unique_ptr<PadSound> PadRenderer::render(const PadRenderJob& job)
{
    if (job.audio == nullptr || job.end <= job.start) return nullptr;
    const auto& src = *job.audio;
    const int len = (int) (job.end - job.start);
    juce::AudioBuffer<float> buf(2, len);
    for (int c = 0; c < 2; ++c)
        buf.copyFrom(c, 0, src, std::min(c, src.getNumChannels() - 1), (int) job.start, len);

    switch (job.sync)
    {
        case SyncMode::Repitch:
            resample(buf, job.speed * job.sourceRate / job.targetRate);
            break;
        case SyncMode::Off:
        case SyncMode::Stretch:
            if (std::abs(job.timeRatio - 1.0) > 1.0e-4 || std::abs(job.transpose) > 1.0e-3f)
                stretch(buf, job.sourceRate, job.timeRatio, job.transpose);
            resample(buf, job.sourceRate / job.targetRate);
            break;
    }

    if (job.reverse)
        for (int c = 0; c < 2; ++c) buf.reverse(c, 0, buf.getNumSamples());

    const int n = buf.getNumSamples();
    const int fi = juce::jlimit(0, n / 2, (int) (job.fadeInMs * 0.001 * job.targetRate));
    const int fo = juce::jlimit(0, n / 2, (int) (job.fadeOutMs * 0.001 * job.targetRate));
    if (fi > 0) for (int c = 0; c < 2; ++c) buf.applyGainRamp(c, 0, fi, 0.0f, 1.0f);
    if (fo > 0) for (int c = 0; c < 2; ++c) buf.applyGainRamp(c, n - fo, fo, 1.0f, 0.0f);

    auto out = std::make_unique<PadSound>();
    out->audio = std::move(buf);
    out->sampleRate = job.targetRate;
    out->renderKey = job.key();
    return out;
}

PadRenderer::PadRenderer() : juce::Thread("HPSAMPLER pad renderer") {}

PadRenderer::~PadRenderer()
{
    *alive = false;
    stopThread(4000);
}

void PadRenderer::submit(PadRenderJob job)
{
    {
        const juce::ScopedLock sl(lock);
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const PadRenderJob& j) { return j.pad == job.pad; }), pending.end());
        pending.push_back(std::move(job));
    }
    if (!isThreadRunning()) startThread();
    notify();
}

void PadRenderer::run()
{
    while (!threadShouldExit())
    {
        PadRenderJob job;
        bool have = false;
        {
            const juce::ScopedLock sl(lock);
            if (!pending.empty()) { job = std::move(pending.front()); pending.erase(pending.begin()); have = true; }
        }
        if (!have) { wait(200); continue; }
        auto sound = render(job);
        auto* raw = sound.release();
        juce::MessageManager::callAsync([this, flag = alive, pad = job.pad, raw]
        {
            std::unique_ptr<PadSound> s(raw);
            if (*flag && onRendered) onRendered(pad, std::move(s));
        });
    }
}
} // namespace hps
