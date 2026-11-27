#include "bridge/BridgeAdapter.h"
#include "render/PadRenderer.h"
#include "stems/Stems.h"

namespace hps
{
namespace
{
// This composes cached measurements on a musical grid. It never synthesises MAIN audio.
void projectFeatures(const Project& project, hpbridge::SamplerContextSnapshot& context)
{
    const auto& st = project.state();
    const double ticksPerSecond = context.transport.bpm * kPPQ / 60.0;
    const double loopSeconds = context.patternBars * kTicksPerBar / ticksPerSecond;
    const double stepSeconds = (kPPQ / 4) / ticksPerSecond;
    constexpr int substeps = 8;
    struct PadInfo
    {
        std::shared_ptr<const SourceData> data;
        PadRenderJob render;
        PadSyncInfo sync;
        double seconds = 0;
        float gain = 0;
        std::array<float, 3> attenuation {};
    };
    std::array<PadInfo, kNumPads> pads;
    for (int i = 0; i < kNumPads; ++i)
    {
        if (!st.pads[(size_t) i].assigned) continue;
        auto& p = pads[(size_t) i];
        p.data = project.sourceDataForSlot(st.pads[(size_t) i].src);
        p.render = PadRenderer::makeJob(project, i, 48000, &p.sync);
        p.seconds = p.sync.outputSeconds;
        const auto& pad = st.pads[(size_t) i];
        const auto& src = st.sources[(size_t) pad.src];
        const float panL = std::min(1.0f, 1 - pad.s.pan) * std::min(1.0f, 1 - src.pan);
        const float panR = std::min(1.0f, 1 + pad.s.pan) * std::min(1.0f, 1 + src.pan);
        p.gain = (float) dbToGain(pad.s.gainDb + src.gainDb + st.settings.masterDb)
            * std::sqrt((panL * panL + panR * panR) * 0.5f);
        const float hp = std::max(pad.s.hpHz, src.lowCutHz), lp = std::min(pad.s.lpHz, src.highCutHz);
        const std::array<float, 3> centers { 80, 550, 6000 };
        for (size_t b = 0; b < 3; ++b)
            p.attenuation[b] = (float) (1.0 / std::sqrt((1.0 + std::pow(hp / centers[b], 4))
                * (1.0 + std::pow(centers[b] / std::max(10.0f, lp), 4))));
    }
    struct Trigger { double seconds; const PatternEvent* event; };
    std::vector<Trigger> triggers;
    for (const auto& e : st.events)
    {
        if (e.muted || e.pad < 0 || e.pad >= kNumPads || e.tick < 0
            || e.tick >= context.patternBars * kTicksPerBar || pads[(size_t) e.pad].seconds <= 0) continue;
        double tick = (double) e.tick;
        if (std::abs(e.tick % (kPPQ / 2) - kPPQ / 4) <= 8) tick += st.settings.swing * (kPPQ / 6.0);
        for (int cycle : { -1, 0 }) triggers.push_back({ tick / ticksPerSecond + cycle * loopSeconds, &e });
    }
    std::stable_sort(triggers.begin(), triggers.end(), [](auto a, auto b) { return a.seconds < b.seconds; });
    struct Voice
    {
        const PatternEvent* event = nullptr;
        double start = 0, release = 0, cut = 1.0e20;
    };
    std::array<Voice, 48> voices {};
    size_t trigger = 0;
    // the bass line MAIN plays (stems only): one note per (voice, source note); open = the one sounding now
    struct BassId { const PatternEvent* event = nullptr; double voiceStart = 0; const SourceNote* note = nullptr; int midi = -1; };
    BassId open;
    int64_t openStart = 0;
    float openVelocity = 0, openConfidence = 0;
    const double ticksPerSubstep = (kPPQ / 4.0) / substeps;
    auto closeBass = [&](int64_t endTick)
    {
        if (open.note != nullptr && endTick > openStart)
            context.bassNotes.push_back({ openStart, endTick - openStart, open.midi, openVelocity, openConfidence });
        open = {};
    };
    context.featureVersion = 1;
    context.features.resize((size_t) context.patternBars * 16);
    std::vector<float> lowOccupancy(context.features.size());
    for (int sample = -(int) context.features.size() * substeps;
         sample < (int) context.features.size() * substeps; ++sample)
    {
        const double now = (sample + 0.5) * stepSeconds / substeps;
        while (trigger < triggers.size() && triggers[trigger].seconds <= now)
        {
            const auto t = triggers[trigger++];
            const auto& p = st.pads[(size_t) t.event->pad];
            const auto mode = effectivePlayMode(p, st.settings);
            bool toggleOff = false;
            for (auto& v : voices)
            {
                if (v.event == nullptr) continue;
                const auto& other = st.pads[(size_t) v.event->pad];
                const bool otherLoops = other.s.loop && effectivePlayMode(other, st.settings) != PlayMode::OneShot;
                if ((!otherLoops && t.seconds >= v.start + pads[(size_t) v.event->pad].seconds)
                    || t.seconds >= v.cut + 0.005 || t.seconds >= v.release + other.s.releaseMs * 0.001)
                { v.event = nullptr; continue; }
                if (mode == PlayMode::Toggle && v.event->pad == t.event->pad && v.cut > t.seconds
                    && v.release > t.seconds)
                {
                    v.release = t.seconds;
                    toggleOff = true;
                }
                else if (st.settings.mono == MonoMode::All
                    || (st.settings.mono == MonoMode::PerSource && p.src == other.src)
                    || (p.s.chokeGroup > 0 && p.s.chokeGroup == other.s.chokeGroup && t.event->pad != v.event->pad)
                    || (t.event->pad == v.event->pad && p.s.repeat == RepeatMode::Retrigger))
                    v.cut = std::min(v.cut, t.seconds);
            }
            if (toggleOff) continue;
            auto slot = std::find_if(voices.begin(), voices.end(), [](const Voice& v) { return v.event == nullptr; });
            if (slot == voices.end())
                slot = std::min_element(voices.begin(), voices.end(), [](const Voice& a, const Voice& b) { return a.start < b.start; });
            *slot = { t.event, t.seconds, mode == PlayMode::Gate ? t.seconds + t.event->length / ticksPerSecond : 1.0e20, 1.0e20 };
        }
        hpbridge::ContextStep mixed;
        float totalPower = 0, knownPower = 0, bassWeight = 0, bassTotal = 0;
        bool bassFromStem = false;
        float stemConfidence = 0, stemVelocity = 0;
        BassId chosen;
        for (auto& v : voices)
        {
            if (v.event == nullptr) continue;
            const auto& p = st.pads[(size_t) v.event->pad];
            const auto& info = pads[(size_t) v.event->pad];
            const double age = now - v.start;
            const bool loop = p.s.loop && effectivePlayMode(p, st.settings) != PlayMode::OneShot;
            if ((!loop && age >= info.seconds) || now >= v.cut + 0.005 || now >= v.release + p.s.releaseMs * 0.001)
            { v.event = nullptr; continue; }
            if (sample < 0 || !info.data) continue;
            const auto& a = info.data->analysis;
            const double pos = loop ? std::fmod(age, info.seconds) : age;
            double frame = pos / info.seconds * (info.render.end - info.render.start);
            frame = info.render.reverse ? info.render.end - 1 - frame : info.render.start + frame;
            const size_t f = (size_t) std::max(0.0, frame / std::max(1, a.featureHop));
            if (!a.ready || f >= a.absoluteRms.size() || f >= a.bandRms.size()) continue;
            float envelope = (float) std::min(1.0, age / std::max(0.0005, p.s.attackMs * 0.001));
            if (p.s.decayMs > 0 && age > p.s.attackMs * 0.001)
                envelope *= (float) std::max((double) p.s.sustain, 1.0 - (1.0 - p.s.sustain)
                    * (age - p.s.attackMs * 0.001) / (p.s.decayMs * 0.001));
            if (now > v.release) envelope *= (float) std::max(0.0, 1.0 - (now - v.release) / std::max(0.001, p.s.releaseMs * 0.001));
            if (now > v.cut) envelope *= (float) std::max(0.0, 1.0 - (now - v.cut) / 0.005);
            envelope *= (float) std::min(1.0, pos / std::max(0.000001, p.s.fadeInMs * 0.001));
            envelope *= (float) std::min(1.0, (info.seconds - pos) / std::max(0.000001, p.s.fadeOutMs * 0.001));
            const float gain = info.gain * v.event->velocity * envelope;
            const float power = a.absoluteRms[f] * a.absoluteRms[f] * gain * gain;
            totalPower += power;
            knownPower += power * 0.7f; // approximate mix, not a calibrated audio measurement
            mixed.energy += power;
            float bands[3] {};
            for (size_t b = 0; b < 3; ++b)
                bands[b] = a.bandRms[f][b] * gain * info.attenuation[b];
            mixed.low += bands[0] * bands[0]; mixed.mid += bands[1] * bands[1]; mixed.high += bands[2] * bands[2];
            mixed.onset = std::max(mixed.onset, a.onsetCurve[f] * std::min(1.0f, gain));
            const size_t col = (size_t) std::max(0.0, frame / std::max(1, a.specHop));
            const float pitch = info.sync.totalTranspose;
            if (col < a.chroma.size() && !a.atonal)
                for (int pc = 0; pc < 12; ++pc)
                {
                    const double shifted = pc + pitch;
                    const int lo = (int) std::floor(shifted);
                    const float frac = (float) (shifted - lo);
                    const float weight = a.chroma[col][(size_t) pc] * power * a.keyConfidence;
                    mixed.chroma[(size_t) ((lo % 12 + 12) % 12)] += weight * (1 - frac);
                    mixed.chroma[(size_t) (((lo + 1) % 12 + 12) % 12)] += weight * frac;
                }
            const double seconds = frame / info.data->sampleRate;
            const auto& srcSlot = st.sources[(size_t) p.src];
            const auto* line = info.data->notes != nullptr && !info.data->notes->bassLine.empty() ? &info.data->notes->bassLine : nullptr;
            const auto bass = std::lower_bound(a.bass.begin(), a.bass.end(), seconds,
                [](const SourceAnalysis::Bass& b, double s) { return b.end <= s; });
            if (line != nullptr) // the BASS stem knows: the exact note, or no bass at all from this voice
            {
                const auto it = std::upper_bound(line->begin(), line->end(), seconds,
                                                 [](double s, const SourceNote& n) { return s < n.start; });
                if (bassStemAudible(srcSlot) && it != line->begin() && seconds < std::prev(it)->end)
                {
                    const auto& n = *std::prev(it);
                    const int note = n.midi + (int) std::lround(pitch);
                    const float stemGain = srcSlot.stemsOn ? (float) dbToGain(srcSlot.stemDb[kStemBass]) : 1.0f;
                    const float level = gain * stemGain * info.attenuation[0] * (0.35f + 0.65f * n.amp);
                    const float weight = level * level;
                    bassTotal += weight;
                    if (weight > bassWeight && note >= 0 && note <= 127)
                    {
                        mixed.bassMidi = note; bassWeight = weight; bassFromStem = true;
                        stemConfidence = 0.6f + 0.35f * n.amp;
                        stemVelocity = juce::jlimit(0.05f, 1.0f, v.event->velocity * (0.35f + 0.65f * n.amp));
                        chosen = { v.event, v.start, &n, note };
                    }
                }
            }
            else if (!a.atonal && a.drumLikelihood < 0.6f && bass != a.bass.end() && seconds >= bass->start && bass->midi >= 0)
            {
                const int note = bass->midi + (int) std::lround(pitch);
                const float weight = bands[0] * bands[0] * bass->confidence;
                bassTotal += bands[0] * bands[0];
                if (weight > bassWeight && note >= 0 && note <= 127)
                { mixed.bassMidi = note; bassWeight = weight; bassFromStem = false; chosen = {}; }
            }
        }
        if (sample < 0) continue;
        {
            const int64_t tick = (int64_t) std::llround(sample * ticksPerSubstep);
            const bool same = chosen.note != nullptr && chosen.note == open.note && chosen.event == open.event
                              && chosen.voiceStart == open.voiceStart && chosen.midi == open.midi;
            if (!same)
            {
                closeBass(tick);
                if (chosen.note != nullptr) { open = chosen; openStart = tick; openVelocity = stemVelocity; openConfidence = stemConfidence; }
            }
        }
        auto& out = context.features[(size_t) sample / substeps];
        out.energy += mixed.energy / substeps;
        out.low += mixed.low / substeps; out.mid += mixed.mid / substeps; out.high += mixed.high / substeps;
        out.onset = std::max(out.onset, mixed.onset);
        out.confidence += (totalPower > 1.0e-12f ? knownPower / totalPower : 0) / substeps;
        for (size_t pc = 0; pc < 12; ++pc) out.chroma[pc] += mixed.chroma[pc];
        const float bassConfidence = bassTotal > 1.0e-12f ? bassWeight / bassTotal * (bassFromStem ? stemConfidence : 0.7f) : 0;
        if (bassConfidence > out.bassConfidence)
        { out.bassMidi = mixed.bassMidi; out.bassConfidence = bassConfidence; }
        lowOccupancy[(size_t) sample / substeps] += mixed.low > 1.0e-7f ? 1.0f / substeps : 0;
    }
    closeBass((int64_t) context.features.size() * (kPPQ / 4));
    // notes shorter than a 1/64 are switching noise between voices, not played notes
    context.bassNotes.erase(std::remove_if(context.bassNotes.begin(), context.bassNotes.end(),
        [](const hpbridge::BassNoteDto& n) { return n.lengthTicks < kPPQ / 16; }), context.bassNotes.end());
    float peak = 1.0e-9f;
    for (const auto& f : context.features) peak = std::max(peak, std::sqrt(f.energy));
    for (auto& f : context.features)
    {
        f.energy = std::min(1.0f, std::sqrt(f.energy) / peak);
        f.low = std::min(1.0f, std::sqrt(f.low) / peak);
        f.mid = std::min(1.0f, std::sqrt(f.mid) / peak);
        f.high = std::min(1.0f, std::sqrt(f.high) / peak);
        float chromaSum = 0; for (float x : f.chroma) chromaSum += x;
        if (chromaSum > 0) for (auto& x : f.chroma) x /= chromaSum;
    }
    context.lowEnd.occupancy = std::move(lowOccupancy);
    context.phrases.clear();
    for (int bar = 0; bar < context.patternBars; ++bar)
    {
        float energy = 0;
        for (int s = 0; s < 16; ++s) energy += context.features[(size_t) bar * 16 + s].energy / 16;
        context.phrases.push_back({ (int64_t) bar * kTicksPerBar, (int64_t) (bar + 1) * kTicksPerBar, energy });
    }
}
}

hpbridge::SamplerContextSnapshot BridgeAdapter::buildSamplerContext(const Project& project, int64_t revision,
                                                                    const juce::String& instanceId)
{
    const auto& st = project.state();
    hpbridge::SamplerContextSnapshot c;
    c.revision = std::max<int64_t>(1, revision);
    c.projectId = instanceId;
    c.samplerInstanceId = instanceId;
    c.scope = hpbridge::ContextScope::MainMix;
    c.timebase.ticksPerQuarter = kPPQ;
    c.transport.bpm = juce::jlimit(20.0, 300.0, st.settings.bpm);
    c.transport.hostSync = st.settings.followHostTempo;
    c.transport.loopBars = st.settings.patternBars;
    if (st.settings.key.known())
    {
        c.key.tonicPitchClass = st.settings.key.root;
        c.key.mode = st.settings.key.mode == 1 ? "major" : "minor";
        c.key.confidence = 1.0f;
        c.key.origin = hpbridge::ContextOrigin::UserOverride; // the project target key is a user decision
    }
    for (int i = 0; i < kNumSources; ++i)
    {
        const auto& s = st.sources[(size_t) i];
        if (s.empty()) continue;
        const auto data = project.sourceData(s.id);
        hpbridge::SourceSummary sum;
        sum.sourceId = s.id;
        sum.name = s.name;
        const double bpm = effectiveSourceBpm(s, data.get());
        if (bpm > 20.0) sum.originalBpm = bpm;
        c.sources.push_back(sum);
    }
    c.patternBars = st.settings.patternBars;
    const int64_t loop = (int64_t) st.settings.patternBars * kTicksPerBar;
    for (const auto& e : st.events)
    {
        if (e.muted || e.pad < 0 || e.pad >= kNumPads || e.tick < 0 || e.tick >= loop || !st.pads[(size_t) e.pad].assigned) continue;
        const auto& p = st.pads[(size_t) e.pad];
        hpbridge::PatternEventDto d;
        d.tick = e.tick;
        d.durationTicks = e.length;
        d.padId = padLabel(e.pad);
        d.sliceId = p.sliceId;
        d.sourceId = st.sources[(size_t) p.src].id;
        d.velocity = juce::jlimit(0.0f, 1.0f, e.velocity);
        if (std::abs(p.s.pitch) > 0.01f) d.pitchSemitones = juce::roundToInt(p.s.pitch);
        c.patternEvents.push_back(d);
        // accents: every pattern hit, strength = velocity (method: arranged trigger velocity)
        hpbridge::AccentDto a;
        a.tick = e.tick;
        a.strength = d.velocity;
        a.band = "mid";
        c.accents.push_back(a);
    }
    for (int64_t t = 0; t < loop; t += kPPQ) c.beatGrid.beatTicks.push_back(t);
    for (int64_t t = 0; t < loop; t += kTicksPerBar) c.beatGrid.downbeatTicks.push_back(t);
    c.groove.subdivision = "1/16";
    c.groove.swingAmount = st.settings.swing;
    c.groove.microOffsetTicks.assign((size_t) c.patternBars * 16, 0);
    std::vector<float> strongest(c.groove.microOffsetTicks.size());
    for (const auto& e : c.patternEvents)
    {
        const int64_t nearest = (int64_t) std::llround((double) e.tick / (kPPQ / 4));
        const size_t cell = (size_t) nearest % strongest.size();
        if (e.velocity < strongest[cell]) continue;
        strongest[cell] = e.velocity;
        const double swing = std::abs(e.tick % (kPPQ / 2) - kPPQ / 4) <= 8 ? st.settings.swing * (kPPQ / 6.0) : 0;
        c.groove.microOffsetTicks[cell] = (int32_t) std::llround(e.tick + swing - nearest * (kPPQ / 4));
    }
    projectFeatures(project, c);
    for (auto& accent : c.accents)
    {
        const auto& f = c.features[(size_t) accent.tick / (kPPQ / 4)];
        accent.strength *= f.onset;
        accent.band = f.low > f.mid && f.low > f.high ? "low" : f.high > f.mid ? "high" : "mid";
    }
    return c;
}

GuideOverlay BridgeAdapter::fromDrumGuide(const hpbridge::DrumGuideSnapshot& g, const juce::String& origin)
{
    GuideOverlay o;
    o.present = true;
    o.origin = origin;
    o.revision = g.revision;
    o.bpm = g.transport.bpm;
    o.loopBars = g.transport.loopBars;
    const double scale = g.timebase.ticksPerQuarter > 0 ? (double) kPPQ / g.timebase.ticksPerQuarter : 1.0;
    for (const auto& e : g.drumEvents)
        o.hits.push_back({ (int64_t) std::llround((double) (e.tick - g.timebase.originTick) * scale), e.laneClass, e.velocity });
    return o;
}
} // namespace hps
