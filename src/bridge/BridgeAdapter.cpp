#include "bridge/BridgeAdapter.h"

namespace hps
{
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
        if (e.muted || e.tick >= loop || !st.pads[(size_t) e.pad].assigned) continue;
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
    c.phrases.push_back({ 0, loop, 0.5f });
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
