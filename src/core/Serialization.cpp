#include "core/Serialization.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace hps
{
namespace
{
using Obj = juce::DynamicObject;

juce::var padSettingsToVar(const PadSettings& s)
{
    auto* o = new Obj();
    o->setProperty("playMode", s.playMode);
    o->setProperty("repeat", (int) s.repeat);
    o->setProperty("choke", s.chokeGroup);
    o->setProperty("loop", s.loop);
    o->setProperty("reverse", s.reverse);
    o->setProperty("sync", s.sync);
    o->setProperty("followKey", s.followKey);
    o->setProperty("speed", s.speed);
    o->setProperty("pitch", s.pitch);
    o->setProperty("fine", s.fine);
    o->setProperty("gainDb", s.gainDb);
    o->setProperty("pan", s.pan);
    o->setProperty("attackMs", s.attackMs);
    o->setProperty("decayMs", s.decayMs);
    o->setProperty("sustain", s.sustain);
    o->setProperty("releaseMs", s.releaseMs);
    o->setProperty("fadeInMs", s.fadeInMs);
    o->setProperty("fadeOutMs", s.fadeOutMs);
    o->setProperty("hpHz", s.hpHz);
    o->setProperty("lpHz", s.lpHz);
    return juce::var(o);
}

template <class T> T get(const juce::var& v, const char* name, T def)
{
    const auto& p = v[name];
    if (p.isVoid()) return def;
    if constexpr (std::is_same_v<T, bool>) return (bool) p;
    else if constexpr (std::is_same_v<T, int>) return (int) p;
    else if constexpr (std::is_same_v<T, float>) return (float) (double) p;
    else if constexpr (std::is_same_v<T, double>) return (double) p;
    else if constexpr (std::is_same_v<T, int64_t>) return (int64_t) (juce::int64) p;
    else return T(p.toString());
}

PadSettings padSettingsFromVar(const juce::var& v)
{
    PadSettings s;
    s.playMode = juce::jlimit(-1, 2, get(v, "playMode", s.playMode));
    s.repeat = (RepeatMode) juce::jlimit(0, 1, get(v, "repeat", (int) s.repeat));
    s.chokeGroup = juce::jlimit(0, kMaxChokeGroups, get(v, "choke", s.chokeGroup));
    s.loop = get(v, "loop", s.loop);
    s.reverse = get(v, "reverse", s.reverse);
    s.sync = juce::jlimit(-1, 2, get(v, "sync", s.sync));
    s.followKey = get(v, "followKey", s.followKey);
    s.speed = juce::jlimit(0.125f, 8.0f, get(v, "speed", s.speed));
    s.pitch = get(v, "pitch", s.pitch);
    s.fine = get(v, "fine", s.fine);
    s.gainDb = get(v, "gainDb", s.gainDb);
    s.pan = get(v, "pan", s.pan);
    s.attackMs = get(v, "attackMs", s.attackMs);
    s.decayMs = get(v, "decayMs", s.decayMs);
    s.sustain = get(v, "sustain", s.sustain);
    s.releaseMs = get(v, "releaseMs", s.releaseMs);
    s.fadeInMs = get(v, "fadeInMs", s.fadeInMs);
    s.fadeOutMs = get(v, "fadeOutMs", s.fadeOutMs);
    s.hpHz = get(v, "hpHz", s.hpHz);
    s.lpHz = get(v, "lpHz", s.lpHz);
    return s;
}
} // namespace

juce::String Serialization::toJson(const ProjectState& s)
{
    auto* root = new Obj();
    root->setProperty("schema", kSchema);

    juce::Array<juce::var> sources;
    for (const auto& src : s.sources)
    {
        auto* o = new Obj();
        o->setProperty("id", src.id);
        o->setProperty("name", src.name);
        o->setProperty("path", src.filePath);
        o->setProperty("colour", src.colour.toString());
        o->setProperty("bpmOverride", src.bpmOverride);
        o->setProperty("originOverride", src.originOverride);
        o->setProperty("keyOverrideRoot", src.keyOverrideRoot);
        o->setProperty("keyOverrideMode", src.keyOverrideMode);
        o->setProperty("gainDb", src.gainDb);
        o->setProperty("pan", src.pan);
        o->setProperty("filter", src.filter);
        sources.add(juce::var(o));
    }
    root->setProperty("sources", sources);

    juce::Array<juce::var> slices;
    for (const auto& sl : s.slices)
    {
        auto* o = new Obj();
        o->setProperty("id", sl.id);
        o->setProperty("src", sl.src);
        o->setProperty("start", (juce::int64) sl.start);
        o->setProperty("end", (juce::int64) sl.end);
        o->setProperty("name", sl.name);
        slices.add(juce::var(o));
    }
    root->setProperty("slices", slices);

    juce::Array<juce::var> pads;
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = s.pads[(size_t) i];
        if (!p.assigned) continue;
        auto* o = new Obj();
        o->setProperty("index", i);
        o->setProperty("sliceId", p.sliceId);
        o->setProperty("src", p.src);
        o->setProperty("start", (juce::int64) p.start);
        o->setProperty("end", (juce::int64) p.end);
        o->setProperty("name", p.name);
        o->setProperty("settings", padSettingsToVar(p.s));
        pads.add(juce::var(o));
    }
    root->setProperty("pads", pads);

    juce::Array<juce::var> events;
    for (const auto& e : s.events)
    {
        auto* o = new Obj();
        o->setProperty("id", e.id);
        o->setProperty("pad", e.pad);
        o->setProperty("tick", (juce::int64) e.tick);
        o->setProperty("rawTick", (juce::int64) e.rawTick);
        o->setProperty("length", (juce::int64) e.length);
        o->setProperty("velocity", e.velocity);
        if (e.muted) o->setProperty("muted", true);
        events.add(juce::var(o));
    }
    root->setProperty("events", events);

    const auto& st = s.settings;
    auto* so = new Obj();
    so->setProperty("bpm", st.bpm);
    so->setProperty("keyRoot", st.key.root);
    so->setProperty("keyMode", st.key.mode);
    so->setProperty("anchorSrc", st.anchorSrc);
    so->setProperty("followHostTempo", st.followHostTempo);
    so->setProperty("grid", (int) st.grid);
    so->setProperty("snap", st.snap);
    so->setProperty("sync", (int) st.sync);
    so->setProperty("defaultMode", (int) st.defaultMode);
    so->setProperty("mono", (int) st.mono);
    so->setProperty("patternBars", st.patternBars);
    so->setProperty("swing", st.swing);
    so->setProperty("recQuantize", st.recQuantize);
    so->setProperty("recGrid", (int) st.recGrid);
    so->setProperty("metronome", st.metronome);
    so->setProperty("masterDb", st.masterDb);
    so->setProperty("countIn", st.countIn);
    so->setProperty("hostPlays", (int) st.hostPlays);
    so->setProperty("hostCapture", (int) st.hostCapture);
    root->setProperty("settings", juce::var(so));

    return juce::JSON::toString(juce::var(root), false);
}

bool Serialization::fromJson(const juce::String& json, ProjectState& out, juce::String& error)
{
    const auto v = juce::JSON::parse(json);
    if (!v.isObject()) { error = "Project state is not valid JSON"; return false; }
    if (v["schema"].toString() != kSchema) { error = "Unknown project schema: " + v["schema"].toString(); return false; }

    ProjectState s;
    if (auto* arr = v["sources"].getArray())
        for (int i = 0; i < juce::jmin(kNumSources, arr->size()); ++i)
        {
            const auto& o = arr->getReference(i);
            auto& src = s.sources[(size_t) i];
            src.id = o["id"].toString();
            src.name = o["name"].toString();
            src.filePath = o["path"].toString();
            src.colour = o["colour"].toString().isNotEmpty() ? juce::Colour::fromString(o["colour"].toString()) : sourceColour(i);
            src.bpmOverride = get(o, "bpmOverride", 0.0);
            src.originOverride = get(o, "originOverride", -1.0);
            src.keyOverrideRoot = juce::jlimit(-2, 11, get(o, "keyOverrideRoot", -2));
            src.keyOverrideMode = juce::jlimit(0, 1, get(o, "keyOverrideMode", 0));
            src.gainDb = juce::jlimit(-60.0f, 12.0f, get(o, "gainDb", 0.0f));
            src.pan = juce::jlimit(-1.0f, 1.0f, get(o, "pan", 0.0f));
            src.filter = juce::jlimit(-1.0f, 1.0f, get(o, "filter", 0.0f));
        }
    if (auto* arr = v["slices"].getArray())
        for (const auto& o : *arr)
        {
            Slice sl;
            sl.id = o["id"].toString();
            sl.src = juce::jlimit(0, kNumSources - 1, get(o, "src", 0));
            sl.start = get(o, "start", (int64_t) 0);
            sl.end = get(o, "end", (int64_t) 0);
            sl.name = o["name"].toString();
            if (sl.end > sl.start) s.slices.push_back(sl);
        }
    if (auto* arr = v["pads"].getArray())
        for (const auto& o : *arr)
        {
            const int i = get(o, "index", -1);
            if (i < 0 || i >= kNumPads) continue;
            auto& p = s.pads[(size_t) i];
            p.assigned = true;
            p.sliceId = o["sliceId"].toString();
            p.src = juce::jlimit(0, kNumSources - 1, get(o, "src", 0));
            p.start = get(o, "start", (int64_t) 0);
            p.end = get(o, "end", (int64_t) 0);
            p.name = o["name"].toString();
            p.s = padSettingsFromVar(o["settings"]);
            if (p.end <= p.start) p.assigned = false;
        }
    if (auto* arr = v["events"].getArray())
        for (const auto& o : *arr)
        {
            PatternEvent e;
            e.id = o["id"].toString();
            e.pad = juce::jlimit(0, kNumPads - 1, get(o, "pad", 0));
            e.tick = juce::jmax<int64_t>(0, get(o, "tick", (int64_t) 0));
            e.rawTick = get(o, "rawTick", (int64_t) -1);
            e.length = juce::jmax<int64_t>(1, get(o, "length", (int64_t) (kPPQ / 4)));
            e.velocity = juce::jlimit(0.0f, 1.0f, get(o, "velocity", 0.9f));
            e.muted = get(o, "muted", false);
            if (e.id.isEmpty()) e.id = newId();
            s.events.push_back(e);
        }
    const auto so = v["settings"];
    auto& st = s.settings;
    st.bpm = juce::jlimit(20.0, 400.0, get(so, "bpm", st.bpm));
    st.key.root = juce::jlimit(-1, 11, get(so, "keyRoot", -1));
    st.key.mode = juce::jlimit(0, 1, get(so, "keyMode", 0));
    st.anchorSrc = juce::jlimit(-1, kNumSources - 1, get(so, "anchorSrc", -1));
    st.followHostTempo = get(so, "followHostTempo", st.followHostTempo);
    st.grid = (GridDiv) juce::jlimit(0, (int) GridDiv::Off, get(so, "grid", (int) st.grid));
    st.snap = get(so, "snap", st.snap);
    st.sync = (SyncMode) juce::jlimit(0, 2, get(so, "sync", (int) st.sync));
    st.defaultMode = (PlayMode) juce::jlimit(0, 2, get(so, "defaultMode", (int) st.defaultMode));
    st.mono = (MonoMode) juce::jlimit(0, 2, get(so, "mono", (int) st.mono));
    st.patternBars = juce::jlimit(1, 16, get(so, "patternBars", st.patternBars));
    st.swing = juce::jlimit(0.0f, 1.0f, get(so, "swing", st.swing));
    st.recQuantize = juce::jlimit(0.0f, 1.0f, get(so, "recQuantize", st.recQuantize));
    st.recGrid = (GridDiv) juce::jlimit(0, (int) GridDiv::Off, get(so, "recGrid", (int) st.recGrid));
    st.metronome = get(so, "metronome", st.metronome);
    st.masterDb = juce::jlimit(-60.0f, 12.0f, get(so, "masterDb", st.masterDb));
    st.countIn = get(so, "countIn", st.countIn);
    st.hostPlays = (HostPlays) juce::jlimit(0, 2, get(so, "hostPlays", (int) st.hostPlays));
    st.hostCapture = (HostCapture) juce::jlimit(0, 2, get(so, "hostCapture", (int) st.hostCapture));
    out = s;
    return true;
}

juce::MidiFile Serialization::patternToMidi(const ProjectState& s)
{
    juce::MidiMessageSequence seq;
    seq.addEvent(juce::MidiMessage::tempoMetaEvent((int) std::llround(60000000.0 / s.settings.bpm)), 0);
    seq.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0);
    for (const auto& e : s.events)
    {
        const int note = kBaseMidiNote + e.pad;
        if (note > 127 || e.muted || e.tick >= (int64_t) s.settings.patternBars * kTicksPerBar) continue;
        const auto& p = s.pads[(size_t) e.pad];
        const bool gate = p.assigned && effectivePlayMode(p, s.settings) == PlayMode::Gate;
        const int64_t len = gate ? e.length : std::min<int64_t>(e.length, kPPQ / 8); // OneShot: trigger only
        seq.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) juce::jlimit(1, 127, juce::roundToInt(e.velocity * 127.0f))), (double) e.tick);
        seq.addEvent(juce::MidiMessage::noteOff(1, note), (double) (e.tick + std::max<int64_t>(1, len)));
    }
    seq.updateMatchedPairs();
    juce::MidiFile f;
    f.setTicksPerQuarterNote(kPPQ);
    f.addTrack(seq);
    return f;
}
} // namespace hps
