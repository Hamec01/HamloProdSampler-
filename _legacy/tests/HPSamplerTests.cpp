#include <cmath>
#include <iostream>
#include <vector>

#include "analysis/ClassicalAnalyzer.h"
#include "audio/VoiceEngine.h"
#include "hpbridge/BridgeValidator.h"
#include "hpbridge/FileSnapshotTransport.h"
#include "model/SamplerModel.h"
#include "persistence/StateSerializer.h"

using namespace hps;

namespace
{
int gFailures = 0;

void expect(bool cond, const char* msg)
{
    if (!cond)
    {
        std::cerr << "[FAIL] " << msg << "\n";
        ++gFailures;
    }
    else
    {
        std::cout << "[PASS] " << msg << "\n";
    }
}

SourceAsset makeSineSource(int slot, double freqHz, double sr, double seconds, const juce::String& name)
{
    const int n = static_cast<int>(sr * seconds);
    auto buf = std::make_shared<juce::AudioBuffer<float>>(2, n);
    for (int i = 0; i < n; ++i)
    {
        const float s = 0.6f * std::sin(2.0f * juce::MathConstants<float>::pi * static_cast<float>(freqHz * i / sr));
        buf->setSample(0, i, s);
        buf->setSample(1, i, s);
    }
    SourceAsset a;
    a.id          = makeUuid();
    a.visibleSlot = slot;
    a.name        = name;
    a.color       = SourceColor::defaultForSlot(slot);
    a.sampleRate  = sr;
    a.numFrames   = n;
    a.audio       = buf;
    return a;
}

SourceAsset makePercussiveClicksSource(int slot, double bpm, double sr, double seconds)
{
    const int n = static_cast<int>(sr * seconds);
    auto buf = std::make_shared<juce::AudioBuffer<float>>(2, n);
    buf->clear();
    const int beatSamples = static_cast<int>((60.0 / bpm) * sr);
    juce::Random rng(42);
    for (int pos = 0; pos < n; pos += beatSamples)
    {
        for (int k = 0; k < 400 && pos + k < n; ++k)
        {
            const float env = std::exp(-static_cast<float>(k) / 80.0f);
            const float noise = (rng.nextFloat() * 2.0f - 1.0f) * env * 0.9f;
            buf->setSample(0, pos + k, noise);
            buf->setSample(1, pos + k, noise);
        }
    }
    SourceAsset a;
    a.id          = makeUuid();
    a.visibleSlot = slot;
    a.name        = "Breakbeat";
    a.color       = SourceColor::defaultForSlot(slot);
    a.sampleRate  = sr;
    a.numFrames   = n;
    a.audio       = buf;
    return a;
}

void testRapidTriggersAndModes()
{
    SamplerProject proj;
    proj.setSourceInSlot(0, makeSineSource(0, 440.0, 44100.0, 2.0, "Piano"));
    auto sl = proj.createSlice(proj.getSourceBySlot(0)->id, 0, 44100, "Chord 1");
    proj.assignSliceToPad(sl.id, 0);
    proj.assignSliceToPad(sl.id, 1);

    VoiceEngine eng;
    eng.prepare(44100.0, 128);
    eng.publishSnapshot(VoiceEngine::buildSnapshot(proj));

    // 1. Rapid 4 MIDI NoteOn hits in ONE block (no debounce, no lost triggers!)
    juce::AudioBuffer<float> out(2, 128);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, 0.9f), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, 0.9f), 32);
    midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, 0.9f), 64);
    midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, 0.9f), 96);
    eng.processBlock(out, midi);
    expect(eng.getTotalNoteOnCount() == 4, "4 rapid MIDI NoteOn events in one block all triggered");
    expect(out.getMagnitude(0, 128) > 0.05f, "Audio output produced on rapid retrigger");

    // 2. ONE SHOT ignores NoteOff
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, kBaseMidiNote), 0);
    eng.processBlock(out, midi);
    expect(eng.getActiveVoiceCount() >= 1, "ONE_SHOT continues playing after NoteOff");

    // 3. GATE releases on matching NoteOff, and mouse NoteOff does NOT kill MIDI voice!
    proj.getPad(0).assignment->settings.playbackModeOverride = PlaybackMode::Gate;
    proj.getPad(0).assignment->settings.releaseMs = 5.0f;
    proj.getPad(0).assignment->settings.overlapOverride = OverlapPolicy::PolyLayer;
    eng.reset();
    eng.publishSnapshot(VoiceEngine::buildSnapshot(proj));

    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, 0.9f), 0);
    eng.processBlock(out, midi);
    expect(eng.getActiveVoiceCount() == 1, "MIDI GATE voice started");

    // Mouse NoteOff with different origin/token must NOT release MIDI voice
    PadTriggerMessage mouseOff;
    mouseOff.isNoteOn     = false;
    mouseOff.flatPadIndex = 0;
    mouseOff.origin       = TriggerOrigin::GuiMouse;
    mouseOff.token        = 99;
    eng.pushGuiTrigger(mouseOff);
    midi.clear();
    for (int i = 0; i < 6; ++i) eng.processBlock(out, midi);
    expect(eng.getActiveVoiceCount() == 1, "GUI Mouse NoteOff does not kill MIDI-held Gate voice");

    // MIDI NoteOn velocity=0 acts as NoteOff and releases the Gate voice
    midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote, static_cast<uint8_t>(0)), 0);
    for (int i = 0; i < 6; ++i) { eng.processBlock(out, midi); midi.clear(); }
    expect(eng.getActiveVoiceCount() == 0, "MIDI NoteOn velocity=0 acts as NoteOff and releases Gate voice");

    // 4. Choke Group affects ONLY its group, not other pads
    proj.getPad(0).assignment->settings.playbackModeOverride = PlaybackMode::OneShot;
    proj.getPad(0).assignment->settings.overlapOverride      = OverlapPolicy::ChokeGroup;
    proj.getPad(0).assignment->settings.chokeGroup           = 1;
    proj.getPad(1).assignment->settings.playbackModeOverride = PlaybackMode::OneShot;
    proj.getPad(1).assignment->settings.overlapOverride      = OverlapPolicy::ChokeGroup;
    proj.getPad(1).assignment->settings.chokeGroup           = 2;
    eng.reset();
    eng.publishSnapshot(VoiceEngine::buildSnapshot(proj));

    eng.pushGuiTrigger({ true, 0, 0.9f, 0, TriggerOrigin::GuiMouse, 1 });
    eng.pushGuiTrigger({ true, 1, 0.9f, 0, TriggerOrigin::GuiMouse, 2 });
    eng.processBlock(out, midi);
    expect(eng.getActiveVoiceCount() == 2, "Different choke groups play simultaneously");
}

void testFourSourcesChopAndSafety()
{
    SamplerProject proj;
    auto piano = makeSineSource(0, 220.0, 44100.0, 3.0, "Piano_120BPM_Am");
    piano.analysis = ClassicalAnalyzer::analyze(*piano.audio, 44100.0, ClassicalAnalyzer::readFilenameAndAcidHints(juce::File("Piano_120BPM_Am.wav"), {}));
    proj.setSourceInSlot(0, piano);
    proj.setProjectAnchorFromSource(0);
    expect(std::abs(proj.state().projectBpm - 120.0) < 0.1, "SRC 1 accepted as Project Anchor (120 BPM)");

    // Load percussive atonal breakbeat into SRC 2 -> key must remain Unknown/atonal, anchor stays 120 BPM
    auto brk = makePercussiveClicksSource(1, 100.0, 44100.0, 3.0);
    brk.analysis = ClassicalAnalyzer::analyze(*brk.audio, 44100.0);
    proj.setSourceInSlot(1, brk);
    expect(brk.analysis.isAtonalOrPercussive, "Percussive noise burst detected as atonal/percussive (no fake key)");
    expect(!brk.analysis.key.tonicPitchClass.has_value(), "Atonal breakbeat reports Unknown key");
    expect(std::abs(proj.state().projectBpm - 120.0) < 0.1, "Loading SRC 2 does not overwrite SRC 1 Project Anchor");

    // ChopEngine candidates + Rescan safety (must NOT overwrite accepted slices or assigned pads!)
    ChopParams cp;
    cp.algorithm   = ChopAlgorithm::ChopRegions;
    cp.regionCount = 4;
    auto cands = ClassicalAnalyzer::generateChopCandidates(*proj.getSourceBySlot(0), cp, 120.0);
    expect(cands.size() == 4, "Chop Regions generated 4 candidates");
    cands[0].pinned = true;
    proj.replaceCandidatesForSlot(0, cands);
    auto accepted = proj.acceptCandidate(0, cands[0].id);
    proj.assignSliceToPad(accepted.id, 0);

    // Rescan with different algorithm
    cp.algorithm   = ChopAlgorithm::SlicerEqualTime;
    cp.regionCount = 8;
    proj.replaceCandidatesForSlot(0, ClassicalAnalyzer::generateChopCandidates(*proj.getSourceBySlot(0), cp, 120.0));
    expect(proj.getPad(0).assignment.has_value() && proj.getPad(0).assignment->sliceId == accepted.id,
           "Rescanning candidates never overwrites assigned pad or accepted slice");

    // 4-lane ArrangementView projection & SourceColor inheritance
    auto brkSlice = proj.createSlice(proj.getSourceBySlot(1)->id, 0, 22050, "Break 1");
    proj.assignSliceToPad(brkSlice.id, 1);
    expect(proj.getPadColor(0).badge == "S1" && proj.getPadColor(1).badge == "S2",
           "Pads automatically inherit S1/S2 color badges from their source");

    PatternEvent e1 { makeUuid(), proj.getPad(0).padId, 0, 0, 480, 0.9f, 0, std::nullopt };
    PatternEvent e2 { makeUuid(), proj.getPad(1).padId, 960, 960, 480, 0.9f, 0, std::nullopt };
    proj.addPatternEvent(e1);
    proj.addPatternEvent(e2);
    expect(proj.getEventSourceLane(proj.state().pattern.events[0]) == 0
        && proj.getEventSourceLane(proj.state().pattern.events[1]) == 1,
           "MAIN Pattern projects events onto their true SRC 1..4 lanes");

    // Non-destructive Record Quantize preserves rawStartTick
    proj.mutableState().recordQuantizeOn = true;
    PatternEvent rawEv { makeUuid(), proj.getPad(0).padId, 255, 255, 240, 0.8f, 0, std::nullopt };
    proj.applyRecordQuantizeToEvent(rawEv);
    expect(rawEv.startTick == 240 && rawEv.rawStartTick == 255, "Record Quantize snaps startTick while preserving rawStartTick");

    // State roundtrip + missing file detection
    const auto json = StateSerializer::serializeToJson(proj);
    SamplerProject restored;
    const auto loadRes = StateSerializer::deserializeFromJson(json, restored, false);
    expect(loadRes.ok && restored.state().pattern.events.size() == 2 && restored.getPad(0).assignment.has_value(),
           "Project state serializes and restores pads, slices, and 4-lane pattern");
}

void testHPBridgeSchemasAndValidation()
{
    SamplerProject proj;
    proj.setSourceInSlot(0, makeSineSource(0, 220.0, 44100.0, 1.0, "Piano"));
    auto sl = proj.createSlice(proj.getSourceBySlot(0)->id, 0, 22050, "Slice 1");
    proj.assignSliceToPad(sl.id, 0);
    proj.addPatternEvent({ makeUuid(), proj.getPad(0).padId, 0, 0, 480, 0.9f, 0, 2 });

    // 1. SamplerContextSnapshot roundtrip
    const auto ctx = proj.buildSamplerContextSnapshot(hpbridge::ContextScope::MainMix, 0);
    expect(hpbridge::BridgeValidator::validate(ctx).ok, "SamplerContextSnapshot validates cleanly");
    const auto ctxJson = hpbridge::BridgeValidator::toJson(ctx);
    hpbridge::SamplerContextSnapshot parsedCtx;
    expect(hpbridge::BridgeValidator::fromJson(ctxJson, parsedCtx).ok && parsedCtx.patternEvents.size() == 1,
           "hp.sample-context.v1 JSON roundtrip succeeds");

    // 2. DrumGuideSnapshot (2 bars, 4/4, 120 BPM, kick/snare + swing)
    hpbridge::DrumGuideSnapshot guide;
    guide.hpdgInstanceId = "hpdg-test-1";
    guide.patternId      = "pat-1";
    guide.transport.bpm  = 120.0;
    guide.transport.loopBars = 2;
    guide.groove.swingAmount = 0.12f;
    guide.drumEvents.push_back({ 0, hpbridge::DrumLaneClass::Kick, 0.95f, 1.0f });
    guide.drumEvents.push_back({ 960, hpbridge::DrumLaneClass::Snare, 0.88f, 0.9f });
    guide.phrases.push_back({ 0, 7680, 0.6f });
    expect(hpbridge::BridgeValidator::validate(guide).ok, "HPDGDrumGuide (hp.drum-guide.v1) validates cleanly");

    const auto guideJson = hpbridge::BridgeValidator::toJson(guide);
    hpbridge::DrumGuideSnapshot parsedGuide;
    expect(hpbridge::BridgeValidator::fromJson(guideJson, parsedGuide).ok && parsedGuide.drumEvents.size() == 2,
           "hp.drum-guide.v1 JSON roundtrip succeeds");

    // 3. Schema rejection tests
    auto badRev = ctx;
    badRev.revision = 0;
    expect(!hpbridge::BridgeValidator::validate(badRev).ok, "Validator rejects revision <= 0");

    auto badSchema = guide;
    badSchema.schema = "hp.unknown.v99";
    expect(!hpbridge::BridgeValidator::validate(badSchema).ok, "Validator rejects incompatible schema");

    auto badTick = guide;
    badTick.drumEvents.push_back({ 999999, hpbridge::DrumLaneClass::Kick, 0.9f, 0.9f });
    expect(!hpbridge::BridgeValidator::validate(badTick).ok, "Validator rejects out-of-range drumEvent tick");
}
} // namespace

int main()
{
    testRapidTriggersAndModes();
    testFourSourcesChopAndSafety();
    testHPBridgeSchemasAndValidation();

    if (gFailures == 0)
    {
        std::cout << "\nALL HPSAMPLER TESTS PASSED!\n";
        return 0;
    }
    std::cerr << "\n" << gFailures << " TEST(S) FAILED!\n";
    return 1;
}
