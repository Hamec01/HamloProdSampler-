#include "render/PadRenderer.h"

#include <mutex>

#include <signalsmith-stretch/signalsmith-stretch.h>

namespace hps
{
namespace
{
void hashMix(uint64_t& h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }
uint64_t bitsOf(double d) { uint64_t u; std::memcpy(&u, &d, sizeof u); return u; }

// Band-limited resampling by `ratio` (input samples per output sample): polyphase Kaiser-windowed sinc, no latency.
void resample(juce::AudioBuffer<float>& buf, double ratio)
{
    if (std::abs(ratio - 1.0) < 1.0e-9) return;
    const int inLen = buf.getNumSamples();
    const int outLen = juce::jmax(1, (int) std::floor(inLen / ratio));
    const double cutoff = 0.97 * std::min(1.0, 1.0 / ratio); // of the input Nyquist
    const int half = (int) std::ceil(24.0 * std::max(1.0, ratio)), taps = 2 * half;
    constexpr int phases = 256;
    auto bessel = [](double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 30; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
    constexpr double beta = 8.0;
    const double norm = bessel(beta);
    std::vector<float> table((size_t) ((phases + 1) * taps));
    for (int ph = 0; ph <= phases; ++ph)
    {
        double sum = 0.0;
        std::vector<double> row((size_t) taps);
        for (int t = 0; t < taps; ++t)
        {
            const double x = (double) (t - half + 1) - (double) ph / phases; // distance from the output position
            const double u = x / half;
            const double w = std::abs(u) < 1.0 ? bessel(beta * std::sqrt(1.0 - u * u)) / norm : 0.0;
            const double a = juce::MathConstants<double>::pi * x * cutoff;
            row[(size_t) t] = (std::abs(a) < 1.0e-9 ? 1.0 : std::sin(a) / a) * w;
            sum += row[(size_t) t];
        }
        for (int t = 0; t < taps; ++t) table[(size_t) (ph * taps + t)] = (float) (row[(size_t) t] / sum);
    }
    juce::AudioBuffer<float> out(buf.getNumChannels(), outLen);
    std::vector<float> padded((size_t) (inLen + 2 * taps), 0.0f);
    for (int c = 0; c < buf.getNumChannels(); ++c)
    {
        std::copy_n(buf.getReadPointer(c), inLen, padded.begin() + taps);
        float* o = out.getWritePointer(c);
        for (int i = 0; i < outLen; ++i)
        {
            const double pos = i * ratio;
            const int ip = (int) pos;
            const double fp = (pos - ip) * phases;
            const int ph = juce::jmin(phases - 1, (int) fp);
            const float f = (float) (fp - ph);
            const float* k0 = table.data() + ph * taps;
            const float* k1 = k0 + taps;
            const float* x = padded.data() + taps + ip - half + 1;
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            for (int t = 0; t < taps - 3; t += 4)
            {
                a0 += x[t] * (k0[t] + f * (k1[t] - k0[t]));
                a1 += x[t + 1] * (k0[t + 1] + f * (k1[t + 1] - k0[t + 1]));
                a2 += x[t + 2] * (k0[t + 2] + f * (k1[t + 2] - k0[t + 2]));
                a3 += x[t + 3] * (k0[t + 3] + f * (k1[t + 3] - k0[t + 3]));
            }
            for (int t = taps & ~3; t < taps; ++t) a0 += x[t] * (k0[t] + f * (k1[t] - k0[t]));
            o[i] = a0 + a1 + a2 + a3;
        }
    }
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

bool PadRenderJob::shareable() const
{
    if (audio == nullptr || end <= start) return false;
    const double ratio = sync == SyncMode::Repitch ? 1.0 / speed : timeRatio;
    return (double) (end - start) / sourceRate * ratio > 20.0;
}

PadRenderJob PadRenderer::makeFitJob(const Project& project, int slot, double targetRate)
{
    PadRenderJob job;
    job.pad = kNumPads + slot;
    job.targetRate = targetRate;
    const auto d = project.sourceDataForSlot(slot);
    if (d == nullptr || d->audio == nullptr) return job;
    const auto& st = project.state();
    const auto& src = st.sources[(size_t) slot];
    const double bpm = effectiveSourceBpm(src, d.get()), proj = st.settings.bpm > 20.0 ? st.settings.bpm : 120.0;
    const auto key = effectiveSourceKey(src, d.get());
    job.audio = d->audio;
    job.sourceRate = d->sampleRate;
    job.end = d->numFrames;
    job.fadeInMs = job.fadeOutMs = 0.0f;
    job.sync = st.settings.sync;
    if (job.sync == SyncMode::Repitch && bpm > 20.0) job.speed = juce::jlimit(0.125, 8.0, proj / bpm);
    if (job.sync == SyncMode::Stretch)
    {
        if (bpm > 20.0) job.timeRatio = juce::jlimit(0.125, 8.0, bpm / proj);
        if (key.known() && st.settings.key.known()) job.transpose = (float) keyTransposeSemitones(key, st.settings.key);
    }
    return job;
}

// ---------------------------------------------------------------------------------------------
// Master: one render of a whole source (tempo / key / speed of some pads), made of segments. Several workers render
// different segments, the ones the pads start in come first; a pad is playable as soon as its first seconds exist.

static PadRenderJob wholeOf(PadRenderJob j)
{
    j.start = 0;
    j.end = j.audio->getNumSamples();
    j.fadeInMs = j.fadeOutMs = 0.0f; // the voice fades
    return j;
}

struct PadRenderer::Master
{
    uint64_t key = 0;
    PadRenderJob whole;
    int frames = 0;
    double perFrame = 1.0;                // output frames per source frame
    std::vector<int64_t> bounds;          // source frames, segment k = [bounds[k], bounds[k + 1])
    enum : uint8_t { None, Running, Done };
    std::vector<uint8_t> state;           // guarded by the renderer's lock
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    std::once_flag once;
    juce::CriticalSection mixLock;

    explicit Master(const PadRenderJob& job) : whole(wholeOf(job))
    {
        key = whole.key();
        const double ratio = whole.sync == SyncMode::Repitch ? 1.0 / whole.speed : whole.timeRatio;
        frames = juce::jmax(2, (int) std::llround((double) whole.end * ratio * whole.targetRate / whole.sourceRate));
        perFrame = (double) frames / (double) whole.end;
        // cut where it is quiet: a seam there is not heard
        const auto& in = *whole.audio;
        const int64_t step = (int64_t) (8.0 * whole.sourceRate), win = (int64_t) (0.02 * whole.sourceRate), search = (int64_t) whole.sourceRate;
        bounds.push_back(0);
        for (int64_t b = step; b < whole.end - step / 2; b += step)
        {
            int64_t best = b;
            float bestE = 1.0e9f;
            for (int64_t p = b - search; p < b + search; p += win)
            {
                float e = 0.0f;
                for (int64_t i = p; i < p + win; i += 4) e += std::abs(in.getSample(0, (int) i)) + std::abs(in.getSample(in.getNumChannels() - 1, (int) i));
                if (e < bestE) { bestE = e; best = p + win / 2; }
            }
            bounds.push_back(best);
        }
        bounds.push_back(whole.end);
        state.assign(bounds.size() - 1, None);
    }

    int numSegments() const { return (int) state.size(); }
    int segmentAt(int64_t f) const
    {
        const auto it = std::upper_bound(bounds.begin(), bounds.end(), f);
        return juce::jlimit(0, numSegments() - 1, (int) (it - bounds.begin()) - 1);
    }
    bool complete() const { return std::all_of(state.begin(), state.end(), [](uint8_t s) { return s == Done; }); }

    void allocate()
    {
        std::call_once(once, [this]
        {
            audio = std::make_shared<juce::AudioBuffer<float>>(2, frames);
            audio->clear();
        });
    }

    void renderSegment(int k)
    {
        allocate();
        PadRenderJob job = whole;
        job.reverse = false; // placed backwards below
        const int64_t margin = (int64_t) (0.25 * whole.sourceRate);
        job.start = std::max<int64_t>(0, bounds[(size_t) k] - margin);
        job.end = std::min(whole.end, bounds[(size_t) k + 1] + margin);
        const auto snd = PadRenderer::render(job);
        if (snd == nullptr) return;
        auto& src = snd->audio;
        const bool first = k == 0, last = k == numSegments() - 1;
        const int half = (int) (0.02 * whole.targetRate);
        const int64_t origin = std::llround((double) job.start * perFrame);
        const int64_t o0 = first ? 0 : std::llround((double) bounds[(size_t) k] * perFrame) - half;
        const int64_t o1 = last ? frames : std::llround((double) bounds[(size_t) k + 1] * perFrame) + half;
        const bool same = whole.sync == SyncMode::Repitch || (std::abs(whole.timeRatio - 1.0) < 1.0e-4 && std::abs(whole.transpose) < 1.0e-3f);
        auto shape = [same](float x) { x = juce::jlimit(0.0f, 1.0f, x); return same ? x : std::sin(x * juce::MathConstants<float>::halfPi); };
        const juce::ScopedLock sl(mixLock); // neighbours overlap
        for (int c = 0; c < 2; ++c)
        {
            const float* in = src.getReadPointer(c);
            float* out = audio->getWritePointer(c);
            for (int64_t o = std::max<int64_t>(0, o0); o < std::min<int64_t>(frames, o1); ++o)
            {
                const int64_t i = o - origin;
                if (i < 0 || i >= src.getNumSamples()) continue;
                float g = 1.0f;
                if (!first && o < o0 + 2 * half) g *= shape((float) (o - o0) / (float) (2 * half));
                if (!last && o >= o1 - 2 * half) g *= shape((float) (o1 - o) / (float) (2 * half));
                out[whole.reverse ? frames - 1 - o : o] += in[i] * g;
            }
        }
    }

    std::unique_ptr<PadSound> view(const PadRenderJob& job)
    {
        allocate();
        const int a = juce::jlimit(0, frames - 2, (int) std::llround((double) job.start * perFrame));
        const int z = juce::jlimit(a + 2, frames, (int) std::llround((double) job.end * perFrame));
        const int first = whole.reverse ? frames - z : a, n = z - a;
        auto out = std::make_unique<PadSound>();
        out->master = audio;
        float* ch[2] = { audio->getWritePointer(0, first), audio->getWritePointer(1, first) };
        out->audio.setDataToReferTo(ch, 2, n);
        out->fadeIn = juce::jlimit(0, n / 2, (int) (job.fadeInMs * 0.001 * job.targetRate));
        out->fadeOut = juce::jlimit(0, n / 2, (int) (job.fadeOutMs * 0.001 * job.targetRate));
        out->sampleRate = job.targetRate;
        out->renderKey = job.key();
        return out;
    }
};

struct PadRenderer::Worker : juce::Thread
{
    explicit Worker(PadRenderer& o) : juce::Thread("HPSAMPLER render"), owner(o) {}
    void run() override { while (!threadShouldExit()) if (!owner.workOnce()) wait(250); }
    PadRenderer& owner;
};

PadRenderer::PadRenderer() = default;

PadRenderer::~PadRenderer()
{
    cancelPendingUpdate();
    for (auto* w : workers) w->signalThreadShouldExit();
    for (auto* w : workers) w->notify();
    for (auto* w : workers) w->stopThread(6000);
}

void PadRenderer::submit(PadRenderJob job)
{
    {
        const juce::ScopedLock sl(lock);
        const size_t pad = (size_t) job.pad;
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Item& j) { return j.job.pad == job.pad; }), pending.end());
        Item it;
        it.serial = submitted[pad] = ++serialCounter;
        if (job.shareable())
        {
            // an identical render (same tempo, key, speed...) serves every pad of the source
            const uint64_t key = wholeOf(job).key();
            for (const auto& m : masters) if (m->key == key) it.master = m;
            if (it.master == nullptr) { it.master = std::make_shared<Master>(job); masters.push_back(it.master); }
        }
        wanted[pad] = it.master;
        it.job = std::move(job);
        pending.push_back(std::move(it));
        // renders nobody wants any more
        masters.erase(std::remove_if(masters.begin(), masters.end(), [&](const std::shared_ptr<Master>& m)
        {
            return std::find(wanted.begin(), wanted.end(), m) == wanted.end();
        }), masters.end());
    }
    // Below the DAW: half the physical cores at most, at low priority. Six workers on 8 logical cores shared the
    // physical cores (Hyper-Threading) with the DAW's audio threads, which the thread priority does not prevent.
    if (workers.isEmpty())
        for (int i = juce::jlimit(1, 3, juce::SystemStats::getNumPhysicalCpus() / 2); --i >= 0;)
            workers.add(new Worker(*this))->startThread(juce::Thread::Priority::low);
    for (auto* w : workers) w->notify();
}

void PadRenderer::setFocus(int pad)
{
    const juce::ScopedLock sl(lock);
    focus = pad;
}

bool PadRenderer::isRendering(int pad) const
{
    const juce::ScopedLock sl(lock);
    return delivered[(size_t) pad] < submitted[(size_t) pad];
}

bool PadRenderer::isBusy() const
{
    const juce::ScopedLock sl(lock);
    for (size_t i = 0; i < submitted.size(); ++i) if (delivered[i] < submitted[i]) return true;
    for (const auto& m : masters) if (!m->complete()) return true;
    return false;
}

void PadRenderer::finish(int timeoutMs)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    while (juce::Time::getMillisecondCounter() - t0 < (juce::uint32) timeoutMs)
    {
        deliver();
        if (!isBusy()) break;
        juce::Thread::sleep(5);
    }
}

void PadRenderer::deliver()
{
    while (true)
    {
        Ready r;
        {
            const juce::ScopedLock sl(lock);
            if (ready.empty()) return;
            r = std::move(ready.front());
            ready.erase(ready.begin());
            if (r.serial <= delivered[(size_t) r.pad]) continue; // a newer one is already there
            delivered[(size_t) r.pad] = r.serial;
        }
        if (onRendered) onRendered(r.pad, std::move(r.sound));
    }
}

// The segments a job cannot start without. A whole-source audition waits for all of them.
static void neededSegments(const PadRenderJob& j, int numSegs, const std::function<int(int64_t)>& at, int& from, int& to)
{
    if (j.pad >= kNumPads) { from = 0; to = numSegs - 1; return; }
    const int64_t head = (int64_t) (3.0 * j.sourceRate);
    if (j.reverse) { to = at(j.end - 1); from = at(std::max(j.start, j.end - head)); }
    else { from = at(j.start); to = at(std::min(j.end - 1, j.start + head)); }
    if (from > 0) --from; // both sides of the seams
    if (to < numSegs - 1) ++to;
}

bool PadRenderer::workOnce()
{
    Item item;
    std::shared_ptr<Master> master;
    int segment = -1;
    {
        const juce::ScopedLock sl(lock);
        int bestRank = 1000;
        size_t bestIndex = 0;
        for (size_t i = 0; i < pending.size(); ++i)
        {
            const auto& p = pending[i];
            // the pad being played first, then pads, then the whole-source audition
            const int rank = p.job.pad == focus ? 0 : p.job.pad < kNumPads ? 1 : 2;
            if (rank >= bestRank) continue;
            if (p.master == nullptr) { bestRank = rank; bestIndex = i; segment = -1; master = nullptr; continue; }
            int from, to;
            neededSegments(p.job, p.master->numSegments(), [&](int64_t f) { return p.master->segmentAt(f); }, from, to);
            bool all = true;
            int todo = -1;
            for (int k = from; k <= to; ++k)
            {
                all = all && p.master->state[(size_t) k] == Master::Done;
                if (todo < 0 && p.master->state[(size_t) k] == Master::None) todo = k;
            }
            if (all) { bestRank = -1; bestIndex = i; segment = -1; master = nullptr; break; } // ready: hand it out now
            if (todo >= 0) { bestRank = rank; bestIndex = i; segment = todo; master = p.master; }
        }
        if (bestRank == 1000) // nothing urgent: complete the rest
        {
            for (const auto& m : masters)
            {
                for (int k = 0; k < m->numSegments() && segment < 0; ++k) if (m->state[(size_t) k] == Master::None) segment = k;
                if (segment >= 0) { master = m; break; }
            }
            if (segment < 0) return false;
        }
        if (segment >= 0) master->state[(size_t) segment] = Master::Running;
        else
        {
            item = std::move(pending[bestIndex]);
            pending.erase(pending.begin() + (std::ptrdiff_t) bestIndex);
        }
    }
    if (segment >= 0)
    {
        master->renderSegment(segment);
        const juce::ScopedLock sl(lock);
        master->state[(size_t) segment] = Master::Done;
        return true;
    }
    Ready r;
    r.pad = item.job.pad;
    r.serial = item.serial;
    r.sound = item.master != nullptr ? item.master->view(item.job) : render(item.job);
    {
        const juce::ScopedLock sl(lock);
        ready.push_back(std::move(r));
    }
    triggerAsyncUpdate();
    return true;
}
} // namespace hps
