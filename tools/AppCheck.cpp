// Drives the real processor + editor (DAW transport, keys, mouse tools in MAIN) and checks the result. Not shipped.
// HPSAMPLER_AppCheck <outDir for screenshots> <source.wav>
#include <iostream>
#include <juce_audio_utils/juce_audio_utils.h>
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
    ed.reset();
    std::cout << (checks - failures) << "/" << checks << " app checks passed\n";
    return failures ? 1 : 0;
}
