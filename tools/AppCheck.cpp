// Drives the real processor + editor (DAW transport, keys, mouse tools in MAIN) and checks the result. Not shipped.
// HPSAMPLER_AppCheck <outDir for screenshots> <source.wav>
#include <iostream>
#include <juce_audio_utils/juce_audio_utils.h>
#include <set>
#include "app/PluginEditor.h"
#include "app/PluginProcessor.h"
using namespace hps;

static int failures = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; std::cout << "FAIL: " << msg << "\n"; } } while (0)
static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

struct Head : juce::AudioPlayHead
{
    bool playing = false, recording = false;
    double ppq = 0.0, bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying(playing); p.setIsRecording(recording); p.setPpqPosition(ppq); p.setBpm(bpm);
        return p;
    }
};

template <class T> T* findChild(juce::Component& c)
{
    if (auto* t = dynamic_cast<T*>(&c)) return t;
    for (auto* ch : c.getChildren()) if (auto* t = findChild<T>(*ch)) return t;
    return nullptr;
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3) return 2;
    const juce::File out{ juce::String(argv[1]) };
    out.createDirectory();
    std::unique_ptr<HPSamplerProcessor> proc(dynamic_cast<HPSamplerProcessor*>(createPluginFilter()));
    Head head;
    proc->setPlayHead(&head);
    proc->prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> b(2, 256);
    juce::MidiBuffer midi;
    auto blocks = [&](int n) { for (int i = 0; i < n; ++i) { proc->processBlock(b, midi); midi.clear(); if (head.playing) head.ppq += 256 / 48000.0 * head.bpm / 60.0; } };
    proc->loadSource(0, juce::File(juce::String(argv[2])));
    for (int i = 0; i < 400 && proc->isSourceLoading(0); ++i) pump(50);
    for (int p = 0; p < 4; ++p) proc->assignToPad(p, 0, 48000 * p, 48000 * (p + 1));
    proc->project.mutate([](ProjectState& s) { s.settings.recQuantize = 1.0f; s.settings.patternBars = 2; });
    blocks(1); pump(1500); blocks(1);
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
    ed->setSize(1440, 880);
    auto* lanes = findChild<LaneCanvas>(*ed);
    auto* view = findChild<PatternView>(*ed);
    CHECK(lanes && view, "editor");
    auto shot = [&](const char* n)
    {
        pump(150);
        auto f = out.getChildFile(n); f.deleteFile();
        juce::FileOutputStream os(f);
        juce::PNGImageFormat().writeImageToStream(ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f), os);
    };
    auto events = [&]() -> const std::vector<PatternEvent>& { return proc->project.state().events; };
    auto space = [&] { const bool r = ed->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)); blocks(6); return r; };
    auto key = [&](char c) { ed->keyPressed(juce::KeyPress(c, juce::ModifierKeys(), (juce::juce_wchar) c)); };

    // ---- 1. Space = one switch (the reported case: Space, pad, Space)
    proc->ui.view = 0; ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    lanes = findChild<LaneCanvas>(*ed); view = findChild<PatternView>(*ed);
    space();
    CHECK(proc->engine.previewFrame.load() >= 0, "Space in SRC plays the source");
    proc->triggerPad(1, 0.9f, 0); blocks(6);
    CHECK(proc->engine.previewFrame.load() < 0 && proc->engine.padVoices[1].load() == 1, "pad ends the source");
    space();
    CHECK(proc->engine.padVoices[1].load() == 0 && proc->engine.previewFrame.load() < 0, "Space, pad, Space = silence");
    space(); space();
    CHECK(proc->engine.previewFrame.load() < 0, "Space twice = silence");
    proc->ui.view = -1; ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    lanes = findChild<LaneCanvas>(*ed); view = findChild<PatternView>(*ed);

    // ---- 2. DAW record -> MAIN, loop passes do not duplicate, piano roll does not double
    auto passWithNotes = [&](bool rec)
    {
        head.ppq = 0.0; head.playing = true; head.recording = rec;
        for (int i = 0; i < 12; ++i)
        {
            midi.addEvent(juce::MidiMessage::noteOn(1, 36 + i % 4, (juce::uint8) 100), 30); // 30 samples late
            blocks(40); // 0.2133 s = 0.4267 beat
            midi.addEvent(juce::MidiMessage::noteOff(1, 36 + i % 4), 0);
            head.ppq = (i + 1) * 0.5; // next 1/8
            pump(40);
        }
    };
    passWithNotes(true);
    head.playing = head.recording = false; blocks(8); pump(120);
    CHECK(events().size() == 12, "DAW record writes 12 notes, got " + std::to_string(events().size()));
    CHECK(!proc->project.canUndo() || proc->project.undoLabel() == "Record from DAW", "one undo step");
    bool onGrid = true;
    for (const auto& e : events()) onGrid = onGrid && e.tick % 240 == 0;
    CHECK(onGrid, "REC Q applied");
    passWithNotes(true);
    head.playing = head.recording = false; blocks(8); pump(120);
    CHECK(events().size() == 12, "a second pass of the same notes adds nothing, got " + std::to_string(events().size()));
    proc->project.edit("clear", [](ProjectState& s) { s.events.clear(); });
    passWithNotes(false);
    head.playing = false; blocks(8); pump(120);
    CHECK(events().empty() && proc->captureCount() == 12, "no DAW record: CAPTURE offers 12, got " + std::to_string(proc->captureCount()));
    proc->captureTake();
    CHECK(events().size() == 12 && proc->captureCount() == 0, "CAPTURE writes them");
    shot("main_lanes.png");

    // ---- 3. REC with count-in
    proc->setRecordArmed(true); blocks(120); pump(60);
    CHECK(proc->engine.countInBeat.load() > 0, "count-in runs");
    shot("countin.png");
    space();
    CHECK(!proc->isRecordArmed() && !proc->engine.transportPlaying.load(), "Space stops recording");

    // ---- 4. tools
    proc->project.edit("clear", [](ProjectState& s) { s.events.clear(); });
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    using Mods = juce::ModifierKeys;
    auto ev = [&](juce::Point<float> p, juce::Point<float> d, Mods m)
    {
        return juce::MouseEvent(src, p, m, 1.0f, 0, 0, 0, 0, lanes, lanes, juce::Time::getCurrentTime(), d, juce::Time::getCurrentTime(), 1, p != d);
    };
    auto drag = [&](juce::Point<float> a, juce::Point<float> z, int mods = Mods::leftButtonModifier)
    {
        lanes->mouseDown(ev(a, a, Mods(mods)));
        for (int i = 1; i <= 10; ++i) lanes->mouseDrag(ev(a + (z - a) * (i / 10.0f), a, Mods(mods)));
        lanes->mouseUp(ev(z, a, Mods(mods)));
    };
    auto at = [&](int row, double tick) { const auto r = lanes->rows()[(size_t) row]; return juce::Point<float>((float) lanes->tickToX(tick), (float) (r.y + r.h / 2)); };
    proc->ui.padRows = true; view->refresh();
    CHECK(lanes->rows().size() == 4, "one row per pad");
    key('b'); CHECK(proc->ui.tool == UiState::Tool::Paint, "B = paint");
    view->lastLength = 240;
    drag(at(0, 20), at(0, 8 * 240 - 20));
    CHECK(events().size() == 8, "paint: 8 notes of 1/16, got " + std::to_string(events().size()));
    key('n');
    drag(at(1, 20), at(1, 4 * 240 - 20));
    CHECK(events().size() == 12, "step paint on");
    drag(at(1, 20), at(1, 4 * 240 - 20));
    CHECK(events().size() == 8, "step paint on a note = off");
    key('t'); drag(at(0, 100), at(0, 100));
    int muted = 0; for (const auto& e : events()) muted += e.muted;
    CHECK(muted == 1, "mute tool");
    key('d'); drag(at(0, 100), at(0, 3 * 240 - 100));
    CHECK(events().size() == 5, "delete tool sweeps 3, got " + std::to_string(events().size()));
    proc->project.edit("one", [](ProjectState& s) { s.events.clear(); PatternEvent e; e.id = newId(); e.pad = 2; e.tick = 0; e.length = 960; s.events.push_back(e); });
    key('c'); drag(at(2, 500), at(2, 500));
    CHECK(events().size() == 2 && events()[0].length == 480, "slice tool");
    proc->project.undo();
    CHECK(events().size() == 1 && events()[0].length == 960, "slice undo");
    key('p'); drag(at(2, 300), at(0, 300 + 960));
    CHECK(events()[0].pad == 0 && events()[0].tick == 960, "draw: drag moves in time and to another pad");
    drag(at(0, 1200), at(0, 1200), Mods::rightButtonModifier);
    CHECK(events().empty(), "right click deletes");
    drag(at(3, 1930), at(3, 1930));
    CHECK(events().size() == 1 && events()[0].pad == 3 && events()[0].tick == 1920, "draw places the row's pad");
    const auto va = lanes->velocityArea();
    const juce::Point<float> v((float) lanes->tickToX(1920), (float) va.getCentreY());
    drag(v, v);
    CHECK(std::abs(events()[0].velocity - 0.5f) < 0.15f, "velocity strip");
    key('z'); drag(at(0, 960), at(0, 1920));
    CHECK(std::abs(lanes->visibleTicks() - 960) < 30 && std::abs(proc->ui.mainStart - 960) < 30, "zoom tool");
    const double t0 = lanes->xToTick(700);
    lanes->zoomTime(1.5, 700);
    CHECK(std::abs(lanes->xToTick(700) - t0) < 2.0, "zoom keeps the point under the mouse");
    key('f'); CHECK(lanes->visibleTicks() == (double) view->loopTicks(), "F = fit");
    lanes->zoomRows(6.0, lanes->notesArea().getCentreY());
    CHECK(lanes->contentHeight() > lanes->notesArea().getHeight(), "row zoom");
    key('f'); key('p');
    for (int p = 0; p < 4; ++p) for (int i = p; i < 32; i += 3 + p) view->lanePad(0), proc->project.edit("x", [&](ProjectState& s) { PatternEvent e; e.id = newId(); e.pad = p; e.tick = i * 240; e.velocity = 0.4f + 0.15f * (float) p; e.muted = i == 9; s.events.push_back(e); });
    lanes->showTicks(0, 3840);
    shot("main_padrows.png");
    ed->setSize(1100, 700);
    shot("main_small.png");

    // ---- 5. SRC: length modes, one-click map, tools, tap tempo, fitted audition
    for (int i = 0; i < 4; ++i) proc->clearPad(i);
    proc->ui.view = 0; ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    auto* canvas = findChild<WaveCanvas>(*ed);
    auto* sv = findChild<SourceView>(*ed);
    auto button = [&](const juce::String& start) -> juce::TextButton*
    {
        for (auto* c : sv->getChildren()) if (auto* t = dynamic_cast<juce::TextButton*>(c)) if (t->getButtonText().startsWith(start)) return t;
        return nullptr;
    };
    auto combo = [&](const juce::String& item) -> juce::ComboBox*
    {
        for (auto* c : sv->getChildren()) if (auto* t = dynamic_cast<juce::ComboBox*>(c)) for (int i = 0; i < t->getNumItems(); ++i) if (t->getItemText(i) == item) return t;
        return nullptr;
    };
    auto pick = [&](const juce::String& item) { auto* c = combo(item); CHECK(c != nullptr, "combo item " + item.toStdString()); for (int i = 0; c && i < c->getNumItems(); ++i) if (c->getItemText(i) == item) c->setSelectedItemIndex(i, juce::sendNotificationSync); };
    auto& sview = proc->ui.src[0];
    const auto sd = proc->project.sourceDataForSlot(0);
    sview.selStart = sview.selEnd = -1; sv->refresh();
    button("FIND")->onClick();
    CHECK(!sview.candidates.empty(), "find spots");
    const int64_t foundEnd = sview.candidates[0].end;
    pick("LEN: TO END");
    CHECK(sview.candidates[0].end == sd->numFrames, "found spots play to the end");
    shot("src_toend.png");
    const int n = (int) sview.candidates.size();
    button("-> PADS")->onClick();
    int mapped = 0; std::set<int> groups;
    for (const auto& p : proc->project.state().pads) if (p.assigned) { ++mapped; groups.insert(p.s.chokeGroup); }
    CHECK(mapped == n && groups.size() == 1 && *groups.begin() > 0, "one click maps all, they cut each other");
    for (int i = 0; i < 200 && proc->padSoundSeconds(n - 1) <= 0.0; ++i) pump(50);
    CHECK(proc->padSoundSeconds(0) > proc->padSoundSeconds(n - 1) && proc->padSoundSeconds(n - 1) > 0.0, "through pads are rendered");
    proc->project.undo();
    CHECK(proc->project.firstEmptyPad(0) == 0, "map is one undo step");
    auto cev = [&](juce::Point<float> p) { return juce::MouseEvent(src, p, Mods(Mods::leftButtonModifier), 1.0f, 0, 0, 0, 0, canvas, canvas, juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1, false); };
    auto click = [&](float x) { const juce::Point<float> p(x, 120.0f); canvas->mouseDown(cev(p)); canvas->mouseUp(cev(p)); };
    click(300.0f);
    CHECK(sview.selStart > 0 && sview.selEnd == sd->numFrames, "TO END: click = from here to the end");
    const auto cue = sview.selStart;
    click(700.0f);
    CHECK(sview.selStart > cue && sview.selEnd == sd->numFrames, "TO END: the next click moves the start");
    pick("LEN: FOUND");
    sview.selStart = sview.selEnd = -1; sv->refresh();
    button("FIND")->onClick();
    CHECK(sview.candidates[0].end == foundEnd, "FOUND gives the found length");
    const size_t total = sview.candidates.size();
    sview.selectedCandidate = 0; sview.selStart = sview.candidates[0].start; sview.selEnd = sview.candidates[0].end;
    button("FIND")->onClick();
    CHECK(sview.candidates.size() == total, "a clicked candidate does not become the search range");
    const double tapBpm = 80.0, srcSr = sd->sampleRate;
    juce::ignoreUnused(srcSr);
    sv->keyPressed(juce::KeyPress('t', {}, 't'));
    CHECK(proc->project.state().sources[0].bpmOverride == 0.0, "T does nothing until TAP is on");
    proc->engine.stopPreview(); blocks(40);
    button("TAP")->onClick();
    blocks(1);
    const double spb = 60.0 / tapBpm;
    for (int i = 0; i < 6; ++i) { blocks((int) (spb * 48000 / 256)); sv->keyPressed(juce::KeyPress('t', {}, 't')); }
    CHECK(proc->project.state().sources[0].bpmOverride == 0.0, "tempo is set when tapping ends");
    pump(2400);
    const double got = proc->project.state().sources[0].bpmOverride;
    CHECK(std::abs(got / tapBpm - 1.0) < 0.025, "tap tempo: " + std::to_string(got));
    CHECK(!proc->gridForSource(0).tentative && proc->project.state().sources[0].originOverride >= 0.0, "tapped grid is the source grid");
    pick("BEAT SLICER");
    button("SLICE")->onClick();
    CHECK(sview.candidates.size() == 8, "beat slicer: 8, got " + std::to_string(sview.candidates.size()));
    const auto len1 = sview.candidates[0].end - sview.candidates[0].start;
    pick("2 beats");
    CHECK(std::abs((double) (sview.candidates[0].end - sview.candidates[0].start) / (double) len1 - 2.0) < 0.01, "beat slicer follows its setting live");
    shot("src_beat.png");
    pick("EQUAL REGIONS");
    button("CHOP")->onClick();
    const auto gr = proc->gridForSource(0);
    const double off = ((double) sview.candidates[1].start - gr.originFrame()) / gr.framesPerBeat() * 4.0;
    CHECK(std::abs(off - std::round(off)) < 0.01, "equal regions lie on the grid");
    shot("src_equal.png");
    pick("GRID CHOP");
    button("SLICE")->onClick();
    CHECK(sview.candidates.size() > 8, "grid chop cuts the whole file");
    proc->project.edit("bpm", [](ProjectState& s) { s.settings.followHostTempo = false; s.settings.bpm = 100.0; s.settings.sync = SyncMode::Stretch; });
    auto* hear = button("HEAR");
    hear->setToggleState(true, juce::dontSendNotification); hear->onClick();
    for (int i = 0; i < 400 && !proc->fitReady(0); ++i) pump(50);
    CHECK(proc->fitReady(0), "source rendered at the project tempo");
    proc->engine.previewRange(0, 0, sd->numFrames, false); blocks(100);
    const double expect = 100 * 256 / 48000.0 * sd->sampleRate * 100.0 / got;
    CHECK(std::abs(proc->engine.previewFrame.load() / expect - 1.0) < 0.01, "fitted audition runs at the project tempo");
    proc->engine.stopPreview(); blocks(10);
    shot("src_fit.png");
    proc->assignToPad(0, 0, 1000, 20000);
    proc->togglePadToEnd(0); CHECK(proc->padPlaysToEnd(0), "pad to end");
    proc->togglePadToEnd(0); CHECK(proc->project.state().pads[0].end == 20000, "and back");
    proc->ui.inspectorOpen = true; ed->setSize(1100, 700);
    shot("src_small.png");
    // ---- 6. bank edit: last change wins
    proc->assignToPad(1, 0, 30000, 60000); proc->assignToPad(16, 0, 30000, 60000);
    auto spd = [&](int p) { return proc->project.state().pads[(size_t) p].s.speed; };
    proc->project.edit("one", [](ProjectState& s) { s.pads[0].s.speed = 0.5f; s.pads[0].s.pitch = 3; });
    proc->ui.editBank = 0; proc->ui.selectedPad = 0; proc->ui.inspectorOpen = true;
    ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    auto* insp = findChild<PadInspector>(*ed);
    shot("bank_mixed.png");
    std::function<juce::TextButton*(juce::Component&, const juce::String&)> deep = [&](juce::Component& c, const juce::String& t) -> juce::TextButton*
    {
        for (auto* ch : c.getChildren())
        {
            if (auto* tb = dynamic_cast<juce::TextButton*>(ch)) if (tb->getButtonText() == t) return tb;
            if (auto* r = deep(*ch, t)) return r;
        }
        return nullptr;
    };
    deep(*insp, "2x")->onClick();
    CHECK(spd(0) == 1.0f && spd(1) == 1.0f, "2x of the shown pad (0.5) goes to both");
    deep(*insp, "2x")->onClick();
    CHECK(spd(0) == 2.0f && spd(1) == 2.0f && spd(16) == 1.0f, "bank edit overrides earlier pad edits, other banks stay");
    CHECK(proc->project.state().pads[0].s.pitch == 3.0f, "parameters that were not moved stay");
    deep(*insp, "ONE PAD")->onClick();
    proc->ui.selectedPad = 1; insp->refresh();
    deep(*insp, "1x")->onClick();
    CHECK(spd(0) == 2.0f && spd(1) == 1.0f, "a pad edited after the bank keeps its own value");
    deep(*insp, "BANK A")->onClick();
    CHECK(proc->ui.editBank == 0, "back to the bank");
    deep(*insp, "RESET PITCH")->onClick();
    CHECK(spd(0) == 1.0f && proc->project.state().pads[0].s.pitch == 0.0f, "reset goes to the bank");
    proc->project.undo();
    CHECK(spd(0) == 2.0f && spd(1) == 1.0f, "one undo step for the bank");
    proc->ui.editBank = -1;
    // ---- 7. quick cut groups
    proc->assignToPad(2, 0, 60000, 90000); proc->assignToPad(3, 0, 60000, 90000);
    proc->ui.bank = 0; proc->ui.selectedPad = 0; proc->ui.inspectorOpen = false;
    ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    auto* pg = findChild<PadGrid>(*ed);
    auto cg = [&](int p) { return proc->project.state().pads[(size_t) p].s.chokeGroup; };
    deep(*pg, "BANK")->onClick();
    CHECK(cg(0) == 1 && cg(3) == 1 && cg(16) == 0, "BANK: the whole bank in one click");
    deep(*pg, "BANK")->onClick();
    CHECK(cg(0) == 0 && cg(3) == 0, "BANK again = off");
    deep(*pg, "PICK")->onClick();
    deep(*pg, "2")->onClick();
    CHECK(proc->ui.cutPaint == 2 && cg(0) == 0, "in PICK the numbers choose the group");
    std::vector<PadButton*> pbs;
    for (auto* c : pg->getChildren()) if (auto* q = dynamic_cast<PadButton*>(c)) pbs.push_back(q);
    auto pev = [&](PadButton* on, juce::Point<float> p) { return juce::MouseEvent(src, p, Mods(Mods::leftButtonModifier), 1.0f, 0, 0, 0, 0, on, on, juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 1, true); };
    auto sweep = [&](int a, int z)
    {
        pbs[(size_t) a]->mouseDown(pev(pbs[(size_t) a], { 20, 30 }));
        for (int i = a; i <= z; ++i) pbs[(size_t) a]->mouseDrag(pev(pbs[(size_t) a], (pbs[(size_t) i]->getBounds().getCentre() - pbs[(size_t) a]->getPosition()).toFloat()));
        pbs[(size_t) a]->mouseUp(pev(pbs[(size_t) a], { 20, 30 }));
    };
    sweep(1, 3);
    CHECK(cg(0) == 0 && cg(1) == 2 && cg(2) == 2 && cg(3) == 2, "sweep puts pads into the group");
    blocks(2);
    CHECK(proc->engine.padVoices[1].load() == 0, "PICK makes no sound");
    shot("cut_pick.png");
    sweep(3, 3);
    CHECK(cg(3) == 0 && cg(2) == 2, "again = out");
    proc->project.undo(); proc->project.undo();
    CHECK(cg(1) == 0 && cg(2) == 0 && cg(3) == 0, "a sweep is one undo step");
    ed->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    CHECK(proc->ui.padMode == UiState::PadMode::Perform, "Esc ends PICK");
    proc->setCutGroup(HPSamplerProcessor::CutScope::Source, 3, false);
    CHECK(cg(0) == 3 && cg(16) == 3, "whole source, across banks");
    ed->setSize(1100, 700); shot("cut_small.png");
    for (int p : { 1, 2, 3, 16 }) proc->clearPad(p);
    // ---- 8. PLAY follows the tab, slicing while listening
    proc->ui.view = 0; proc->ui.chopLength = LengthMode::Found; proc->ui.snap = SnapMode::Off; proc->ui.padMode = UiState::PadMode::Perform;
    sview.selStart = sview.selEnd = -1; sview.viewStart = 0;
    ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    pg = findChild<PadGrid>(*ed);
    auto* cs = findChild<ControlStrip>(*ed);
    deep(*cs, "PLAY")->onClick(); blocks(4);
    CHECK(proc->engine.previewFrame.load() >= 0.0 && !proc->engine.transportPlaying.load(), "PLAY in a SRC tab plays the sample");
    blocks(100); proc->triggerPad(4, 0.9f, 0); blocks(100); pump(80);
    shot("live_marks.png");
    midi.addEvent(juce::MidiMessage::noteOn(1, 36 + 5, (juce::uint8) 100), 10); blocks(100); pump(80);
    proc->triggerPad(6, 0.9f, 0); blocks(4); pump(80);
    {
        const auto& pp = proc->project.state().pads;
        CHECK(pp[4].assigned && pp[5].assigned && pp[6].assigned, "pads (mouse and MIDI) cut while the sample plays");
        CHECK(pp[4].end == pp[5].start && pp[5].end == pp[6].start && pp[6].end == sd->numFrames, "each cut ends at the next one");
        CHECK(pp[4].start < pp[5].start && proc->engine.previewFrame.load() >= 0.0, "the sample plays on");
    }
    proc->project.undo();
    CHECK(!proc->project.state().pads[6].assigned && proc->project.state().pads[5].end == sd->numFrames, "a cut is one undo step");
    deep(*cs, "PLAY")->onClick(); blocks(20);
    CHECK(proc->engine.previewFrame.load() < 0.0, "PLAY again stops it");
    proc->clearPad(4); proc->clearPad(5);
    proc->project.edit("ev", [](ProjectState& s) { s.settings.patternBars = 2; s.events.clear(); PatternEvent e; e.id = newId(); e.pad = 0; s.events.push_back(e); });
    juce::String err;
    const auto wavF = proc->writeDragFile(false, err), midF = proc->writeDragFile(true, err);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(wavF));
    CHECK(rd && std::abs((double) rd->lengthInSamples - 2 * 240.0 / 100.0 * 48000.0) < 2.0, "drag WAV is exactly the loop");
    juce::MidiFile mf;
    { juce::FileInputStream mis(midF); CHECK(mis.openedOk() && mf.readFrom(mis) && mf.getNumTracks() > 0, "drag MIDI"); }
    midF.deleteFile();
    rd.reset();
    wavF.deleteFile();
    proc->ui.inspectorOpen = false; proc->ui.view = -1; ed->setSize(1440, 880); ed.reset(); ed.reset(proc->createEditor()); ed->setSize(1440, 880);
    shot("main_drag.png");
    ed.reset();
    std::cout << (checks - failures) << "/" << checks << " app checks passed\n";
    return failures ? 1 : 0;
}
