// HPSAMPLER automated tests: engine behaviour (pads, overlap, MIDI), sync rendering, chop engine,
// analysis on synthetic material, persistence, bridge snapshot validation.
#include <map>
#include <iostream>

#include <juce_audio_formats/juce_audio_formats.h>

#include "analysis/ChopEngine.h"
#include "analysis/SourceAnalysis.h"
#include "audio/Engine.h"
#include "bridge/BridgeAdapter.h"
#include "core/Serialization.h"
#include "hpbridge/BridgeSession.h"
#include "hpbridge/BridgeValidator.h"
#include "render/PadRenderer.h"

using namespace hps;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; std::cout << "FAIL: " << msg << "  (" #cond ")\n"; } } while (0)

namespace
{
constexpr double kSr = 48000.0;

std::unique_ptr<PadSound> constSound(int frames, float value = 0.5f)
{
    auto s = std::make_unique<PadSound>();
    s->audio.setSize(2, frames);
    for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::fill(s->audio.getWritePointer(c), value, frames);
    s->sampleRate = kSr;
    return s;
}

EngineSnapshot baseSnapshot()
{
    EngineSnapshot s;
    for (int i = 0; i < 4; ++i)
    {
        auto& p = s.pads[(size_t) i];
        p.assigned = true;
        p.mode = PlayMode::OneShot;
        p.src = i < 2 ? 0 : 1;
        p.attackS = 0.0f;
        p.releaseS = 0.005f;
    }
    return s;
}

struct Rig
{
    Engine e;
    juce::AudioBuffer<float> buf { 2, 256 };
    juce::MidiBuffer midi;
    HostTransport host;
    explicit Rig(const EngineSnapshot& s, int soundFrames = 48000)
    {
        e.prepare(kSr, 256);
        e.publishSnapshot(std::make_unique<EngineSnapshot>(s));
        for (int i = 0; i < 4; ++i) e.publishPadSound(i, constSound(soundFrames));
    }
    void block(int n = 256) { buf.setSize(2, n, false, false, true); e.process(buf, midi, host); midi.clear(); }
    int voices(int pad) const { return e.padVoices[(size_t) pad].load(); }
};

void testRapidRetrigger()
{
    auto s = baseSnapshot();
    s.pads[0].repeat = RepeatMode::Layer;
    Rig r(s);
    for (int i = 0; i < 4; ++i) r.e.padDown(0, 1.0f);
    r.block();
    CHECK(r.e.padHits[0].load() == 4, "4 GUI clicks in one block = 4 triggers");
    CHECK(r.voices(0) == 4, "Layer keeps 4 voices");

    Rig m(baseSnapshot());
    m.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, (juce::uint8) 100), 10);
    m.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, (juce::uint8) 100), 20);
    m.block();
    CHECK(m.e.padHits[0].load() == 2, "two MIDI NoteOn in one block both trigger");
    for (int i = 0; i < 4; ++i) m.block();
    CHECK(m.voices(0) == 1, "Retrigger (default) leaves one voice after fade");
}

void testGateOneShot()
{
    auto s = baseSnapshot();
    s.pads[0].mode = PlayMode::Gate;
    Rig r(s);
    r.e.padDown(0, 1.0f); r.block();
    r.e.padDown(1, 1.0f); r.block();
    r.e.padUp(0); r.e.padUp(1);
    for (int i = 0; i < 6; ++i) r.block();
    CHECK(r.voices(0) == 0, "Gate: NoteOff releases");
    CHECK(r.voices(1) == 1, "OneShot: NoteOff does not stop");

    auto t = baseSnapshot();
    t.pads[0].mode = PlayMode::Toggle;
    Rig q(t);
    q.e.padDown(0, 1.0f); q.block(); q.e.padUp(0); q.block();
    CHECK(q.voices(0) == 1, "Toggle: first press starts, release ignored");
    q.e.padDown(0, 1.0f); q.block(); for (int i = 0; i < 6; ++i) q.block();
    CHECK(q.voices(0) == 0, "Toggle: second press stops");
}

void testMidiTokens()
{
    auto s = baseSnapshot();
    s.pads[0].mode = PlayMode::Gate;
    s.pads[0].repeat = RepeatMode::Layer;
    Rig r(s);
    r.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, (juce::uint8) 100), 0);
    r.block();
    r.e.padDown(0, 1.0f); r.block();
    r.e.padUp(0);
    for (int i = 0; i < 6; ++i) r.block();
    CHECK(r.voices(0) == 1, "mouse NoteOff does not release the MIDI-held voice");
    r.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, (juce::uint8) 0), 0); // vel 0 = NoteOff
    for (int i = 0; i < 6; ++i) r.block();
    CHECK(r.voices(0) == 0, "NoteOn velocity 0 acts as NoteOff");
}

void testChokeAndMono()
{
    // the user's case: pad 1 must stop when pad 2 starts -> same choke group
    auto s = baseSnapshot();
    s.pads[0].choke = 1;
    s.pads[1].choke = 1;
    Rig r(s);
    r.e.padDown(0, 1.0f); r.block();
    r.e.padDown(2, 1.0f); r.block();
    r.e.padDown(1, 1.0f);
    for (int i = 0; i < 4; ++i) r.block();
    CHECK(r.voices(0) == 0, "choke: pad 2 cuts pad 1 of the same group");
    CHECK(r.voices(1) == 1, "choke: pad 2 plays");
    CHECK(r.voices(2) == 1, "choke: pad 3 (no group) is untouched");

    auto m = baseSnapshot();
    m.mono = MonoMode::PerSource;
    Rig q(m);
    q.e.padDown(0, 1.0f); q.e.padDown(2, 1.0f); q.block();
    q.e.padDown(1, 1.0f); for (int i = 0; i < 4; ++i) q.block();
    CHECK(q.voices(0) == 0 && q.voices(1) == 1, "mono per source cuts same-source pad");
    CHECK(q.voices(2) == 1, "mono per source keeps other source");

    auto g = baseSnapshot();
    g.mono = MonoMode::All;
    Rig a(g);
    a.e.padDown(0, 1.0f); a.e.padDown(2, 1.0f); a.block();
    a.e.padDown(3, 1.0f); for (int i = 0; i < 4; ++i) a.block();
    CHECK(a.voices(0) + a.voices(2) == 0 && a.voices(3) == 1, "global mono cuts everything");

    Rig def(baseSnapshot());
    def.e.padDown(0, 1.0f); def.e.padDown(1, 1.0f); def.e.padDown(2, 1.0f);
    for (int i = 0; i < 4; ++i) def.block();
    CHECK(def.voices(0) + def.voices(1) + def.voices(2) == 3, "default: pads never cut each other");
}

void testSequencerAndRecord()
{
    auto s = baseSnapshot();
    s.bpm = 120.0;
    s.loopTicks = kTicksPerBar;
    s.events = { { 0, 0, 240, 1.0f }, { 1, kPPQ, 240, 1.0f } };
    Rig r(s);
    r.e.setInternalPlaying(true, true);
    // one bar at 120 BPM = 2 s = 96000 samples
    for (int i = 0; i < 96000 / 256 + 2; ++i) r.block();
    CHECK(r.e.padHits[0].load() >= 1 && r.e.padHits[1].load() >= 1, "pattern triggers both events in one loop");

    r.e.setRecording(true);
    r.block();
    r.e.padDown(2, 1.0f); r.block();
    r.e.padDown(2, 1.0f); r.block();
    int ons = 0;
    RecordedHit h;
    while (r.e.popRecordedHit(h)) if (h.type == RecordedHit::On && h.pad == 2) ++ons;
    CHECK(ons == 2, "two quick live hits while recording = two recorded events");
    // sequencer playback is never recorded again
    for (int i = 0; i < 96000 / 256 + 2; ++i) r.block();
    int seqRec = 0;
    while (r.e.popRecordedHit(h)) if (h.pad == 0 || h.pad == 1) ++seqRec;
    CHECK(seqRec == 0, "sequencer playback is not re-recorded (no feedback)");
}

std::unique_ptr<PreviewSource> previewSound()
{
    auto p = std::make_unique<PreviewSource>();
    auto a = std::make_shared<juce::AudioBuffer<float>>(2, 480000);
    for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::fill(a->getWritePointer(c), 0.3f, 480000);
    p->audio = a;
    p->sampleRate = kSr;
    return p;
}

// one thing at a time: pad / audition / MAIN / DAW
void testTransportPriority()
{
    auto s = baseSnapshot();
    s.loopTicks = kTicksPerBar;
    s.events = { { 0, 0, 240, 1.0f } };
    Rig r(s, 480000);
    r.e.publishPreviewSource(0, previewSound());
    auto run = [&](int n = 8) { for (int i = 0; i < n; ++i) r.block(); };
    auto preview = [&] { return r.e.previewFrame.load() >= 0.0; };

    r.e.previewRange(0, 0, 400000, true); run();
    CHECK(preview(), "audition plays");
    r.e.padDown(1, 1.0f); run();
    CHECK(!preview() && r.voices(1) == 1 && r.e.liveVoices.load() == 1, "a pad ends the audition");
    r.e.previewRange(0, 0, 400000, true); run();
    CHECK(preview() && r.voices(1) == 0, "an audition ends the pad");
    r.e.padDown(1, 1.0f); run();
    r.e.stopAll(); run();
    CHECK(!preview() && r.voices(1) == 0 && r.e.liveVoices.load() == 0 && r.buf.getMagnitude(0, 256) < 1.0e-4f, "stop = silence");
    r.e.padDown(1, 1.0f); r.e.stopAll(); run();
    CHECK(r.voices(1) == 0, "pad + stop in one block = silence");
    r.e.padDown(1, 1.0f); run();
    r.e.previewRange(0, 0, 400000, true); r.e.setInternalPlaying(true, true); run();
    CHECK(!preview() && r.voices(1) == 0 && r.voices(0) == 1, "MAIN ends pad and audition");
    r.e.previewRange(0, 0, 400000, true); run();
    CHECK(preview() && !r.e.transportPlaying.load() && r.voices(0) == 0, "an audition ends MAIN");

    // the DAW takes over; MAIN cannot start next to it and does not come back after it
    r.e.setInternalPlaying(true, true); run();
    r.host.valid = true; r.host.playing = true; r.host.bpm = 120.0; r.host.ppq = 1.0;
    auto daw = [&](int n = 8) { for (int i = 0; i < n; ++i) { r.block(); r.host.ppq += 256 / kSr * 2.0; } };
    daw();
    CHECK(r.e.hostDriven.load() && !preview(), "the DAW leads");
    r.e.setInternalPlaying(true, true); daw();
    r.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote + 2, (juce::uint8) 100), 0); daw();
    r.e.stopAll(); daw();
    CHECK(r.voices(2) == 1, "Space never cuts what the DAW plays");
    r.host.playing = false; run();
    CHECK(!r.e.transportPlaying.load() && r.voices(0) + r.voices(2) == 0, "DAW stop = everything it played stops, MAIN does not start by itself");
}

void testDawNotes()
{
    auto s = baseSnapshot();
    s.loopTicks = kTicksPerBar;
    s.events = { { 0, kPPQ, 240, 1.0f, kPPQ - 100 } }; // recorded 100 ticks early, quantized to beat 2
    auto play = [&](HostPlays hp, bool midi, bool dawRec, int& hitsRec, int& hitsAll)
    {
        s.hostPlays = hp;
        Rig r(s);
        r.host.valid = true; r.host.playing = true; r.host.bpm = 120.0; r.host.recording = dawRec;
        hitsRec = hitsAll = 0;
        for (int i = 0; i < 375; ++i) // 2 s = one bar
        {
            r.host.ppq = i * 256 / kSr * 2.0;
            const double next = (i + 1) * 256 / kSr * 2.0, at = (kPPQ - 100) / (double) kPPQ;
            if (midi && r.host.ppq <= at && at < next) r.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, (juce::uint8) 100), 0);
            r.block();
        }
        RecordedHit h;
        while (r.e.popRecordedHit(h)) { ++hitsAll; if (h.rec) ++hitsRec; }
        return (int) r.e.padHits[0].load();
    };
    int rec = 0, all = 0;
    CHECK(play(HostPlays::Both, true, false, rec, all) == 1, "the same note in MAIN and in the DAW sounds once");
    CHECK(all == 1 && rec == 0, "not recording: the note is only remembered for CAPTURE");
    CHECK(play(HostPlays::Main, true, false, rec, all) == 1, "MAIN only");
    CHECK(play(HostPlays::DawNotes, true, false, rec, all) == 1, "DAW notes only");
    CHECK(play(HostPlays::DawNotes, false, false, rec, all) == 0, "DAW notes only: MAIN is silent");
    play(HostPlays::Both, true, true, rec, all);
    CHECK(rec == 1, "DAW record: the note is recorded");
}

void testWindowClosed()
{
    auto s = baseSnapshot();
    s.loopTicks = kTicksPerBar;
    s.mainNeedsWindow = true;
    s.events = { { 0, 0, 240, 1.0f }, { 0, kPPQ, 240, 1.0f }, { 0, kPPQ * 2, 240, 1.0f }, { 0, kPPQ * 3, 240, 1.0f } };
    Rig r(s, 480000);
    r.host.valid = true; r.host.playing = true; r.host.bpm = 120.0;
    auto beat = [&](bool note = false) // 94 blocks = one beat
    {
        if (note) r.midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, (juce::uint8) 100), 0);
        for (int i = 0; i < 94; ++i) { r.block(); r.host.ppq += 256 / kSr * 2.0; }
    };
    auto hits = [&] { return (int) r.e.padHits[0].load(); };
    beat(); beat();
    CHECK(hits() == 0 && r.voices(0) == 0, "window closed: DAW Play does not start MAIN");
    beat(true);
    CHECK(hits() == 1 && r.voices(0) == 1, "window closed: a DAW note plays the pad, also where MAIN has the same note");
    r.host.playing = false; for (int i = 0; i < 20; ++i) r.block();
    CHECK(r.voices(0) == 0, "DAW stop = silence");
    r.e.windowOpen = true; r.host.playing = true; r.host.ppq = 0.0;
    beat(); beat();
    CHECK(hits() >= 3, "window open: MAIN follows the DAW");
    r.e.windowOpen = false; const int h = hits();
    beat(); beat();
    CHECK(hits() == h && r.voices(0) == 0, "closing the window silences MAIN");
    r.host.playing = false; for (int i = 0; i < 20; ++i) r.block();
    r.e.setInternalPlaying(true, true); for (int i = 0; i < 8; ++i) r.block();
    CHECK(hits() == h + 1, "the plug-in's own PLAY is not affected");
}

void testCountIn()
{
    auto s = baseSnapshot();
    s.loopTicks = kTicksPerBar;
    s.bpm = 120.0;
    s.events = { { 0, 0, 240, 1.0f }, { 0, kTicksPerBar - 480, 240, 1.0f } };
    Rig r(s);
    r.e.setRecording(true);
    r.e.playWithCountIn();
    int seen = 0, clicks = 0, last = 0;
    for (int i = 0; i < 374; ++i) // 2 s minus one block: 1-2-3-4
    {
        if (i == 100) r.e.padDown(1, 1.0f);
        if (i == 368) r.e.padDown(2, 1.0f);
        r.block();
        const int b = r.e.countInBeat.load();
        if (b != last) { ++seen; CHECK(b == last + 1, "count-in beats in order"); last = b; }
        if (r.buf.getMagnitude(0, 256) > 0.2f && i < 90 && i % 94 == 0) ++clicks;
    }
    CHECK(seen == 4 && last == 4, "count-in 1-2-3-4");
    CHECK(clicks > 0, "count-in clicks without METRO");
    CHECK(r.e.padHits[0].load() == 0, "MAIN is silent during the count-in");
    for (int i = 0; i < 4; ++i) r.block();
    CHECK(r.e.countInBeat.load() == 0 && r.e.padHits[0].load() == 1, "MAIN starts on the one");
    int ons = 0; int64_t tick = -1;
    RecordedHit h;
    while (r.e.popRecordedHit(h)) if (h.type == RecordedHit::On) { ++ons; tick = h.tick; }
    CHECK(ons == 1 && tick == 0, "count-in hits are not recorded, one just before the 'one' lands on it");
}

// a hit that was quantized forward must not sound a second time in the same pass
void testNoDoubleAfterRecord()
{
    auto s = baseSnapshot();
    s.loopTicks = kTicksPerBar;
    s.bpm = 120.0;
    Rig r(s);
    r.e.setRecording(true);
    r.e.setInternalPlaying(true, true);
    for (int i = 0; i < 20; ++i) r.block();
    r.e.padDown(0, 1.0f); r.block();
    RecordedHit h;
    CHECK(r.e.popRecordedHit(h) && h.rec, "hit recorded");
    s.events = { { 0, h.tick + 150, 240, 1.0f, h.tick } };
    r.e.publishSnapshot(std::make_unique<EngineSnapshot>(s));
    for (int i = 0; i < 60; ++i) r.block();
    CHECK(r.e.padHits[0].load() == 1, "quantized note is not doubled in the pass it was played");
    for (int i = 0; i < 375; ++i) r.block();
    CHECK(r.e.padHits[0].load() == 2, "... and plays in the next pass");
}

std::shared_ptr<juce::AudioBuffer<float>> sine(double hz, double seconds, double sr)
{
    auto b = std::make_shared<juce::AudioBuffer<float>>(2, (int) (seconds * sr));
    for (int i = 0; i < b->getNumSamples(); ++i)
    {
        const float x = 0.5f * (float) std::sin(juce::MathConstants<double>::twoPi * hz * i / sr);
        b->setSample(0, i, x); b->setSample(1, i, x);
    }
    return b;
}

double estimateHz(const juce::AudioBuffer<float>& b, double sr)
{
    int crossings = 0;
    const float* d = b.getReadPointer(0);
    const int s = b.getNumSamples() / 4, e = b.getNumSamples() * 3 / 4;
    for (int i = s + 1; i < e; ++i) if (d[i - 1] < 0.0f && d[i] >= 0.0f) ++crossings;
    return crossings / ((e - s) / sr);
}

void testRendererSync()
{
    PadRenderJob j;
    j.audio = sine(440.0, 2.0, 44100.0);
    j.sourceRate = 44100.0;
    j.targetRate = 48000.0;
    j.start = 0; j.end = 88200;
    j.fadeInMs = 0; j.fadeOutMs = 0;

    j.sync = SyncMode::Stretch; j.timeRatio = 90.0 / 120.0; j.transpose = 0.0f; // 90 BPM source -> 120 BPM project
    auto a = PadRenderer::render(j);
    CHECK(a != nullptr && std::abs(a->audio.getNumSamples() - 2.0 * 0.75 * 48000.0) < 50, "stretch: duration follows tempo");
    CHECK(std::abs(estimateHz(a->audio, 48000.0) - 440.0) < 8.0, "stretch: pitch unchanged");

    j.timeRatio = 1.0; j.transpose = 3.0f;
    auto b = PadRenderer::render(j);
    CHECK(std::abs(b->audio.getNumSamples() - 96000) < 50, "pitch shift keeps duration");
    CHECK(std::abs(estimateHz(b->audio, 48000.0) - 440.0 * std::pow(2.0, 3.0 / 12.0)) < 10.0, "pitch shift +3 st");

    j.sync = SyncMode::Repitch; j.speed = 2.0;
    auto c = PadRenderer::render(j);
    CHECK(std::abs(c->audio.getNumSamples() - 48000) < 50, "repitch x2: half duration");
    CHECK(std::abs(estimateHz(c->audio, 48000.0) - 880.0) < 10.0, "repitch x2: octave up");
    {
        // long sounds: one segmented render shared by several pads
        const int n = 44100 * 50;
        auto buf = std::make_shared<juce::AudioBuffer<float>>(2, n);
        for (int i = 0; i < n; ++i) for (int ch = 0; ch < 2; ++ch) buf->setSample(ch, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 300.0 * i / 44100.0));
        std::map<int, std::unique_ptr<PadSound>> got;
        PadRenderer r;
        r.onRendered = [&](int pad, std::unique_ptr<PadSound> s) { got[pad] = std::move(s); };
        PadRenderJob j; j.audio = buf; j.sourceRate = 44100.0; j.targetRate = 48000.0; j.end = n; j.sync = SyncMode::Off;
        j.pad = 0; r.submit(j);
        j.pad = 1; j.start = 44100 * 5; r.submit(j);
        j.pad = 2; j.end = 44100 * 40; j.reverse = true; r.submit(j);
        r.finish(60000);
        CHECK(!r.isBusy() && got.size() == 3 && got[0]->master == got[1]->master, "long pads share one render");
        double err = 0.0;
        for (int i = 2000; i < 48000 * 49; i += 7) err = std::max(err, std::abs(0.5 * std::sin(2.0 * juce::MathConstants<double>::pi * 300.0 * i / 48000.0) - (double) got[0]->audio.getSample(0, i)));
        CHECK(err < 0.003, "segmented render has no seams (as recorded), error " + juce::String(err, 5));
        CHECK(std::abs(got[1]->audio.getNumSamples() - 48000 * 45) < 3 && got[1]->audio.getSample(0, 1000) == got[0]->audio.getSample(0, 48000 * 5 + 1000), "a pad starts at its place");
        CHECK(std::abs(got[2]->audio.getNumSamples() - 48000 * 35) < 3 && std::abs(got[2]->audio.getSample(0, 5000) - got[0]->audio.getSample(0, 48000 * 40 - 1 - 5000)) < 0.02f, "reversed long pad");
        j = {}; j.audio = buf; j.sourceRate = 44100.0; j.targetRate = 48000.0; j.end = n; j.sync = SyncMode::Stretch; j.timeRatio = 1.2;
        for (float st : { 1.0f, 2.0f, 3.0f }) { j.transpose = st; j.pad = 0; r.submit(j); } // fast changes: only the last one counts
        r.finish(60000);
        const auto& a = got[0]->audio;
        float lo = 1.0f;
        for (int w = 48000; w + 9600 < a.getNumSamples() - 48000; w += 2400) lo = std::min(lo, a.getRMSLevel(0, w, 4800));
        CHECK(std::abs(estimateHz(a, 48000.0) - 300.0 * std::pow(2.0, 3.0 / 12.0)) < 12.0 && std::abs(a.getNumSamples() - 48000 * 60) < 3, "stretched long pad: pitch and length");
        CHECK(lo > 0.2f, "no hole at the seams, lowest RMS " + juce::String(lo, 3));
    }
    {
        auto buf = std::make_shared<juce::AudioBuffer<float>>(2, 44100);
        for (int i = 0; i < 44100; ++i) for (int ch = 0; ch < 2; ++ch) buf->setSample(ch, i, (float) std::sin(2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 44100.0));
        PadRenderJob rj; rj.audio = buf; rj.sourceRate = 44100.0; rj.targetRate = 48000.0; rj.start = 0; rj.end = 44100; rj.sync = SyncMode::Off;
        const auto rs = PadRenderer::render(rj);
        double err = 0.0;
        for (int i = 1000; i < 46000; ++i) err = std::max(err, std::abs((double) rs->audio.getSample(0, i) - std::sin(2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 48000.0)));
        CHECK(err < 0.002, "44.1 -> 48 kHz: exact and without delay, error " + juce::String(err, 5));
    }

    j.sync = SyncMode::Stretch; j.end = 600; j.timeRatio = 1.25; j.transpose = 2.0f; // very short slice
    auto d = PadRenderer::render(j);
    CHECK(d != nullptr && d->audio.getNumSamples() > 0, "short slice still renders");
}

void testPadSpeed()
{
    Project pr;
    auto d = std::make_shared<SourceData>();
    d->id = "s"; d->sampleRate = 48000.0; d->numFrames = 96000;
    d->audio = sine(440.0, 2.0, 48000.0);
    pr.edit("x", [](ProjectState& s)
    {
        s.sources[0].id = "s";
        s.pads[0].assigned = true; s.pads[0].src = 0; s.pads[0].start = 0; s.pads[0].end = 96000;
        s.pads[0].s.speed = 2.0f; s.pads[0].s.fadeInMs = 0; s.pads[0].s.fadeOutMs = 0;
    });
    pr.setSourceData(d);
    PadSyncInfo info;
    auto job = PadRenderer::makeJob(pr, 0, 48000.0, &info);
    auto snd = PadRenderer::render(job);
    CHECK(snd && std::abs(snd->audio.getNumSamples() - 48000) < 50, "speed x2 (STRETCH): half the length");
    CHECK(std::abs(estimateHz(snd->audio, 48000.0) - 440.0) < 8.0, "speed x2 (STRETCH): pitch kept");
    pr.edit("x", [](ProjectState& s) { s.pads[0].s.sync = (int) SyncMode::Repitch; });
    snd = PadRenderer::render(PadRenderer::makeJob(pr, 0, 48000.0));
    CHECK(snd && std::abs(estimateHz(snd->audio, 48000.0) - 880.0) < 10.0, "speed x2 (REPITCH): octave up");
    pr.edit("x", [](ProjectState& s) { s.pads[0].s.sync = (int) SyncMode::Off; s.pads[0].s.speed = 0.5f; });
    snd = PadRenderer::render(PadRenderer::makeJob(pr, 0, 48000.0));
    CHECK(snd && std::abs(snd->audio.getNumSamples() - 192000) < 80, "speed x0.5 (OFF): double length");
    ProjectState back; juce::String err;
    Serialization::fromJson(Serialization::toJson(pr.state()), back, err);
    CHECK(std::abs(back.pads[0].s.speed - 0.5f) < 1e-4f, "speed is saved");
}

void testLiveBridge()
{
    using namespace hpbridge;
    const auto now0 = juce::Time::currentTimeMillis();
    auto sampler = std::make_unique<BridgeSession>(PeerType::Sampler, "test sampler");
    auto hpdg = std::make_unique<BridgeSession>(PeerType::Hpdg, "test hpdg");
    auto tickAll = [&](juce::int64 t) { for (int i = 0; i < 3; ++i) { sampler->tick(t); hpdg->tick(t); } };

    bool seen = false;
    for (const auto& p : sampler->discover()) seen = seen || p.id == hpdg->getInstanceId();
    CHECK(seen, "HPSAMPLER discovers the running HPDG");

    // Connect started on the HPDG side ("Connect to HPSAMPLER")
    hpdg->connectTo(sampler->getInstanceId());
    tickAll(now0);
    CHECK(hpdg->getState() == BridgeConnectionState::Connected && sampler->getState() == BridgeConnectionState::Connected,
          "Connect from HPDG -> both sides Connected (one handshake)");

    // both directions on the same connection
    juce::String gotOnHpdg, gotOnSampler;
    hpdg->onIncoming = [&](const juce::String& schema, const juce::String& json, std::int64_t) { gotOnHpdg = schema + json; return true; };
    sampler->onIncoming = [&](const juce::String& schema, const juce::String& json, std::int64_t) { gotOnSampler = schema + json; return true; };
    const auto rev = sampler->publish(kSamplerContextSchemaV1, "{\"a\":1}");
    hpdg->publish(kDrumGuideSchemaV1, "{\"g\":2}");
    tickAll(now0 + 100);
    CHECK(gotOnHpdg == juce::String(kSamplerContextSchemaV1) + "{\"a\":1}", "sampler context reaches HPDG");
    CHECK(gotOnSampler == juce::String(kDrumGuideSchemaV1) + "{\"g\":2}", "drum guide reaches HPSAMPLER");
    CHECK(sampler->getPartnerAckRevision() == rev, "HPDG acknowledges the context revision");

    // a second sampler cannot steal the HPDG
    auto other = std::make_unique<BridgeSession>(PeerType::Sampler, "other sampler");
    other->connectTo(hpdg->getInstanceId());
    for (int i = 0; i < 3; ++i) { other->tick(now0 + 200); hpdg->tick(now0 + 200); sampler->tick(now0 + 200); }
    CHECK(other->getState() == BridgeConnectionState::Disconnected, "busy HPDG refuses a second sampler");
    CHECK(hpdg->getState() == BridgeConnectionState::Connected && sampler->getState() == BridgeConnectionState::Connected,
          "existing pair untouched");

    // silent partner -> Stale (not Connected)
    hpdg->tick(now0 + 200 + kBridgeAliveMs + 1000);
    CHECK(hpdg->getState() == BridgeConnectionState::Stale, "no heartbeat -> Stale");

    // disconnect from one side shows on the other
    sampler->tick(juce::Time::currentTimeMillis());
    hpdg->tick(juce::Time::currentTimeMillis());
    sampler->disconnect();
    hpdg->tick(juce::Time::currentTimeMillis());
    CHECK(hpdg->getState() == BridgeConnectionState::Disconnected, "partner Disconnect -> Disconnected");

    // connect from HPSAMPLER, then the HPDG instance closes
    sampler->connectTo(hpdg->getInstanceId());
    tickAll(juce::Time::currentTimeMillis());
    CHECK(sampler->getState() == BridgeConnectionState::Connected, "Connect from HPSAMPLER works too");
    const auto file = bridgeSlotFile(PeerType::Hpdg, hpdg->getInstanceId());
    hpdg.reset();
    CHECK(!file.existsAsFile(), "closed instance removes its slot");
    sampler->tick(juce::Time::currentTimeMillis());
    sampler->tick(juce::Time::currentTimeMillis() + kBridgeGiveUpMs + 1000);
    CHECK(sampler->getState() != BridgeConnectionState::Connected, "closed partner is never shown Connected");
}

void testSharedBridgeFilesInSync()
{
    // HP Bridge files are shared with HPDG (like HamloLinkProtocol): both copies must stay identical.
    const juce::File hpdg("C:/DRUMENGINE/Source/Services/HPBridge");
    const juce::File mine = juce::File(__FILE__).getParentDirectory().getParentDirectory().getChildFile("HPBridgeCore");
    if (!hpdg.isDirectory()) { std::cout << "  (HPDG checkout not found - shared-file check skipped)\n"; return; }
    for (auto [a, b] : { std::pair { "include/hpbridge/BridgeTypes.h", "hpbridge/BridgeTypes.h" },
                         std::pair { "include/hpbridge/BridgeValidator.h", "hpbridge/BridgeValidator.h" },
                         std::pair { "include/hpbridge/BridgeSession.h", "hpbridge/BridgeSession.h" },
                         std::pair { "src/BridgeValidator.cpp", "BridgeValidator.cpp" },
                         std::pair { "src/BridgeSession.cpp", "BridgeSession.cpp" } })
        CHECK(mine.getChildFile(a).loadFileAsString() == hpdg.getChildFile(b).loadFileAsString(),
              (juce::String("shared bridge file in sync with HPDG: ") + b).toStdString());
}

void testKeyTranspose()
{
    CHECK(keyTransposeSemitones({ 0, 1 }, { 9, 0 }) == 0, "C major -> A minor = 0 (relative)");
    CHECK(keyTransposeSemitones({ 2, 0 }, { 9, 0 }) == -5, "D minor -> A minor = -5");
    CHECK(keyTransposeSemitones({ 9, 0 }, { 2, 0 }) == 5, "A minor -> D minor = +5");
    CHECK(keyTransposeSemitones({ -1, 0 }, { 2, 0 }) == 0, "unknown key never transposes");
}

std::shared_ptr<SourceData> clickTrack(double bpm, double seconds, double sr)
{
    auto d = std::make_shared<SourceData>();
    d->id = "t"; d->sampleRate = sr;
    auto b = std::make_shared<juce::AudioBuffer<float>>(2, (int) (seconds * sr));
    b->clear();
    const double beat = 60.0 / bpm * sr;
    juce::Random rnd(1);
    for (int k = 0; k * beat < b->getNumSamples(); ++k)
    {
        const int s = (int) (k * beat);
        const float amp = (k % 4 == 0) ? 0.9f : ((k % 2) ? 0.7f : 0.5f);
        for (int i = 0; i < 4000 && s + i < b->getNumSamples(); ++i)
        {
            const float env = std::exp(-(float) i / 600.0f);
            const float x = amp * env * ((k % 2) ? (rnd.nextFloat() * 2 - 1) : (float) std::sin(i * 0.02));
            b->setSample(0, s + i, x); b->setSample(1, s + i, x);
        }
    }
    d->numFrames = b->getNumSamples();
    d->peaks = SourceAnalyzer::computePeaks(*b, 64);
    d->audio = b;
    return d;
}

void testAnalysisAndChop()
{
    auto d = clickTrack(100.0, 12.0, 44100.0);
    SourceAnalyzer::analyse(*d, juce::File(), nullptr);
    CHECK(d->analysis.ready, "analysis completes");
    std::cout << "  click track 100 BPM -> " << d->analysis.summary << " origin " << d->analysis.originSeconds << "\n";
    CHECK(std::abs(d->analysis.bpm - 100.0) < 1.5 || std::abs(d->analysis.bpm - 200.0) < 3.0 || std::abs(d->analysis.bpm - 50.0) < 1.0,
          "tempo of a 100 BPM click track (or octave)");
    CHECK(d->analysis.onsets.size() >= 15, "onsets found on clicks");
    CHECK(d->analysis.atonal, "drum clicks have no key (no false transposition)");

    SourceGrid g { 100.0, 0.0, 44100.0 };
    SpotParams p; p.focus = SpotFocus::Attack; p.maxSpots = 8;
    const auto spots = ChopEngine::findSpots(*d, 0, d->numFrames, p, g);
    CHECK(!spots.empty() && (int) spots.size() <= 8, "find spots returns <= max candidates");
    bool nearOnset = true;
    for (const auto& c : spots) if (std::fmod((double) c.start + 200.0, 26460.0) > 600.0) nearOnset = false;
    CHECK(nearOnset, "attack spots start at the clicks");

    const auto sl = ChopEngine::beatSlicer(*d, 0, 8, 1.0, g);
    CHECK(sl.size() == 8 && std::abs((double) (sl[1].start - sl[0].start) - 26460.0) < 2.0, "beat slicer: 1 beat each");
    const auto rg = ChopEngine::equalRegions(1000, 9000, 4);
    CHECK(rg.size() == 4 && rg[0].start == 1000 && rg[3].end == 9000, "equal regions cover the selection");
    {
        Engine e; e.prepare(48000.0, 512);
        auto snap = std::make_unique<EngineSnapshot>();
        snap->pads[0].assigned = true;
        e.publishSnapshot(std::move(snap));
        auto snd = std::make_unique<PadSound>();
        snd->audio.setSize(2, 1000);
        for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::fill(snd->audio.getWritePointer(c), 0.5f, 1000);
        snd->fadeIn = 400; snd->fadeOut = 200;
        e.publishPadSound(0, std::move(snd));
        e.padDown(0, 1.0f, 0);
        juce::AudioBuffer<float> b1(2, 512), b2(2, 512); juce::MidiBuffer mb; HostTransport h;
        b1.clear(); b2.clear();
        e.process(b1, mb, h); e.process(b2, mb, h);
        const float mid = b1.getSample(0, 450);
        CHECK(mid > 0.1f && std::abs(b1.getSample(0, 200) / mid - 0.5f) < 0.02f && std::abs(b2.getSample(0, 900 - 512) / mid - 0.5f) < 0.02f,
              "shared render: the voice fades in and out");
    }
    double beats = 0;
    const auto eg = ChopEngine::equalRegionsOnGrid(26460 * 4 - 700, 26460 * 12 + 900, 4, d->numFrames, g, &beats);
    bool onBeat = !eg.empty();
    for (const auto& c : eg) onBeat = onBeat && c.start % 26460 == 0;
    CHECK(onBeat && std::abs(beats - 2.0) < 1e-9, "equal regions on grid: musical length, starts on beats");
    std::vector<ChopCandidate> lc { { 100, 200, 1.0f, "a" }, { 500, 900, 1.0f, "b" } };
    ChopEngine::applyLength(lc, LengthMode::ToEnd, 5000);
    CHECK(lc[0].end == 5000 && lc[1].end == 5000, "length: to the end");
    ChopEngine::applyLength(lc, LengthMode::ToNext, 5000);
    CHECK(lc[0].end == 500 && lc[1].end == 900, "length: to the next chop");
    lc[0].manualEnd = true; lc[0].foundEnd = 150;
    ChopEngine::applyLength(lc, LengthMode::ToEnd, 5000);
    CHECK(lc[0].end == 150, "a dragged end is kept");
    ChopEngine::applyLength(lc, LengthMode::Found, 5000);
    CHECK(lc[1].end == 900, "length: found length comes back");
    CHECK(std::abs(60.0 / ChopEngine::tapPeriod({ 0.0, 0.76, 1.49, 2.26, 3.75, 4.50 }) - 80.0) < 1.0, "tap tempo: jitter and a missed beat");
    CHECK(ChopEngine::tapPeriod({ 0.0, 0.5 }) == 0.0, "tap tempo needs more taps");
    const auto snapped = ChopEngine::snap(*d, 26460 + 500, SnapMode::Grid, g, GridDiv::Quarter, 5000);
    CHECK(std::abs((double) snapped - 26460.0) < 2.0, "grid snap to beat");
}

void testPersistenceAndBridge()
{
    Project pr;
    pr.edit("setup", [](ProjectState& s)
    {
        s.sources[0].id = "src-a"; s.sources[0].name = "Piano"; s.sources[0].filePath = "C:/x/piano.wav"; s.sources[0].bpmOverride = 92.0;
        s.pads[5].assigned = true; s.pads[5].src = 0; s.pads[5].start = 100; s.pads[5].end = 5000;
        s.pads[5].s.chokeGroup = 3; s.pads[5].s.playMode = (int) PlayMode::Gate; s.pads[5].s.pitch = -2.0f;
        s.events.push_back({ "e1", 5, 480, 470, 240, 0.8f });
        s.settings.bpm = 88.0; s.settings.key = { 2, 0 }; s.settings.mono = MonoMode::PerSource;
    });
    const auto json = Serialization::toJson(pr.state());
    ProjectState back;
    juce::String err;
    CHECK(Serialization::fromJson(json, back, err), "project JSON parses");
    CHECK(back.pads[5].assigned && back.pads[5].s.chokeGroup == 3 && back.pads[5].s.playMode == (int) PlayMode::Gate, "pad settings restore");
    CHECK(back.events.size() == 1 && back.events[0].rawTick == 470, "recorded raw timing restores");
    CHECK(back.settings.key.root == 2 && back.settings.mono == MonoMode::PerSource && back.sources[0].bpmOverride == 92.0, "settings restore");

    pr.undo();
    CHECK(!pr.state().pads[5].assigned, "undo reverts the edit");
    pr.redo();
    CHECK(pr.state().pads[5].assigned, "redo re-applies");

    const auto ctx = BridgeAdapter::buildSamplerContext(pr, 3, "inst-1");
    CHECK(hpbridge::BridgeValidator::validate(ctx).ok, "sampler context validates");
    hpbridge::SamplerContextSnapshot parsed;
    CHECK(hpbridge::BridgeValidator::fromJson(hpbridge::BridgeValidator::toJson(ctx), parsed).ok, "sampler context JSON roundtrip");

    const auto midi = Serialization::patternToMidi(pr.state());
    CHECK(midi.getNumTracks() == 1, "MIDI export has a track");
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String(argv[1]) == "--e2e-hpdg") // live check against a running HPDG (separate process)
    {
        using namespace hpbridge;
        BridgeSession s(PeerType::Sampler, "HPSAMPLER e2e test");
        std::vector<BridgePeer> peers;
        for (int i = 0; i < 50 && peers.empty(); ++i) { peers = s.discover(); juce::Thread::sleep(100); }
        if (peers.empty()) { std::cout << "no HPDG found\n"; return 2; }
        std::cout << "found " << peers.front().label() << " bpm " << peers.front().bpm << "\n";
        juce::String guideJson;
        s.onIncoming = [&](const juce::String& schema, const juce::String& json, std::int64_t rev)
        {
            DrumGuideSnapshot g;
            const auto r = BridgeValidator::fromJson(json, g);
            std::cout << "received " << schema << " rev " << rev << ": " << (r.ok ? "valid" : ("INVALID " + r.error).toStdString())
                      << ", " << g.drumEvents.size() << " drum events, " << g.transport.loopBars << " bars, " << g.transport.bpm << " BPM\n";
            guideJson = json;
            return r.ok;
        };
        // HPDG pairs by itself with the only free HPSAMPLER: wait for it instead of connecting from here
        for (int i = 0; i < 100 && s.getState() != BridgeConnectionState::Connected; ++i)
        {
            s.tick(juce::Time::currentTimeMillis());
            if (i % 10 == 0)
            {
                std::cout << "  t=" << i / 10 << "s state=" << s.getStatusText();
                for (const auto& p : s.discover()) std::cout << " | peer " << p.label() << (p.busy ? " busy" : " free") << (p.alive ? "" : " dead");
                int samplers = 0;
                for (const auto& f : bridgeDirectory().findChildFiles(juce::File::findFiles, false, "hpsampler-*.slot")) { juce::ignoreUnused(f); ++samplers; }
                std::cout << " | sampler slots " << samplers << "\n";
            }
            juce::Thread::sleep(100);
        }
        std::cout << (s.getState() == BridgeConnectionState::Connected ? "HPDG connected by itself (auto-connect)\n"
                                                                       : "HPDG did not auto-connect, connecting from the sampler\n");
        if (s.getState() != BridgeConnectionState::Connected) s.connectTo(peers.front().id);
        SamplerContextSnapshot ctx;
        {   // a real rendered loop next to the slots, as HPSAMPLER writes it
            const auto dir = bridgeDirectory().getChildFile("audio").getChildFile("e2e");
            dir.createDirectory();
            const auto wav = dir.getChildFile("HPSAMPLER main 92 BPM A min r1.wav");
            juce::AudioBuffer<float> b(2, (int) (48000 * 60.0 / 92.0 * 8));
            b.clear();
            for (int beat = 0; beat < 8; ++beat)
                for (int i = 0; i < 3000; ++i)
                {
                    const float x = 0.8f * std::exp(-i / 500.0f) * (float) std::sin(i * 0.05);
                    b.setSample(0, (int) (beat * 48000 * 60.0 / 92.0) + i, x);
                    b.setSample(1, (int) (beat * 48000 * 60.0 / 92.0) + i, x);
                }
            wav.deleteFile();
            if (auto os = std::unique_ptr<juce::FileOutputStream>(wav.createOutputStream()))
            {
                juce::WavAudioFormat fmt;
                if (auto w = std::unique_ptr<juce::AudioFormatWriter>(fmt.createWriterFor(os.get(), 48000.0, 2, 24, {}, 0)))
                {
                    os.release();
                    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
                }
            }
            ctx.audioReference = wav.getFullPathName();
        }
        ctx.projectId = ctx.samplerInstanceId = "e2e";
        ctx.transport.bpm = 92.0;
        ctx.key.tonicPitchClass = 9; ctx.key.mode = "minor"; ctx.key.confidence = 1.0f;
        ctx.patternBars = 2;
        ctx.patternEvents.push_back({ 0, 480, "A01", "s1", "src1", 0.9f, {} });
        std::int64_t sent = 0;
        for (int i = 0; i < 100; ++i)
        {
            s.tick(juce::Time::currentTimeMillis());
            if (s.getState() == BridgeConnectionState::Connected && sent == 0)
            {
                std::cout << "Connected to " << s.getPartner().label() << "\n";
                sent = s.publish(kSamplerContextSchemaV1, BridgeValidator::toJson(ctx));
            }
            if (sent > 0 && s.getPartnerAckRevision() == sent && guideJson.isNotEmpty()) break;
            juce::Thread::sleep(100);
        }
        std::cout << "state: " << s.getStatusText() << "; HPDG acknowledged context rev " << s.getPartnerAckRevision() << " of " << sent << "\n";
        // HPDG follows the sampler tempo by default: its published BPM should become 92 (or its Tempo-Mode octave)
        for (int i = 0; i < 40 && std::abs(s.getPartner().bpm - 92.0f) > 0.5f && std::abs(s.getPartner().bpm - 184.0f) > 0.5f; ++i)
        {
            s.tick(juce::Time::currentTimeMillis());
            juce::Thread::sleep(100);
        }
        std::cout << "HPDG tempo after the context: " << s.getPartner().bpm << " BPM\n";
        const bool ok = s.getState() == BridgeConnectionState::Connected && s.getPartnerAckRevision() == sent && guideJson.isNotEmpty();
        s.disconnect();
        return ok ? 0 : 1;
    }
    if (argc > 1) // manual check: HPSAMPLER_Tests.exe file.wav ... prints the analysis
    {
        for (int i = 1; i < argc; ++i)
        {
            const juce::File f(juce::String::fromUTF8(argv[i]));
            juce::String err;
            auto d = SourceAnalyzer::decode(f, "x", err);
            if (!d) { std::cout << f.getFileName() << ": " << err << "\n"; continue; }
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            SourceAnalyzer::analyse(*d, f, nullptr);
            std::cout << f.getFileName() << ": " << d->analysis.summary << "  origin " << d->analysis.originSeconds
                      << " s, onsets " << d->analysis.onsets.size() << ", drum " << d->analysis.drumLikelihood
                      << "  (" << (int) (juce::Time::getMillisecondCounterHiRes() - t0) << " ms)\n";
            SpotParams p;
            const auto spots = ChopEngine::findSpots(*d, 0, d->numFrames, p, { d->analysis.bpm, d->analysis.originSeconds, d->sampleRate });
            std::cout << "   find spots (hybrid): " << spots.size() << "\n";
        }
        return 0;
    }
    testRapidRetrigger();
    testGateOneShot();
    testMidiTokens();
    testChokeAndMono();
    testSequencerAndRecord();
    testTransportPriority();
    testDawNotes();
    testWindowClosed();
    testCountIn();
    testNoDoubleAfterRecord();
    testRendererSync();
    testKeyTranspose();
    testPadSpeed();
    testAnalysisAndChop();
    testPersistenceAndBridge();
    testLiveBridge();
    testSharedBridgeFilesInSync();
    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
