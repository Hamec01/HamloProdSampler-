// Renders the real HPSAMPLER editor to PNG files for visual review (no audio device needed).
// Usage: HPSAMPLER_Shot <outDir> <src1.wav> [src2.wav]
#include <juce_audio_utils/juce_audio_utils.h>
#include "app/PluginProcessor.h"
#include "app/PluginEditor.h"

using namespace hps;

static void shot(juce::Component& c, const juce::File& f)
{
    auto img = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat().writeImageToStream(img, os);
}

class ShotApp : public juce::JUCEApplicationBase, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "HPSAMPLER_Shot"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void systemRequestedQuit() override { quit(); }
    void unhandledException(const std::exception*, const juce::String&, int) override {}
    void shutdown() override { ed.reset(); proc.reset(); }

    void initialise(const juce::String&) override
    {
        args = juce::StringArray::fromTokens(getCommandLineParameters(), true);
        args.trim();
        for (auto& a : args) a = a.unquoted();
        if (args.size() < 2) { quit(); return; }
        out = juce::File(args[0]);
        out.createDirectory();
        proc.reset(dynamic_cast<HPSamplerProcessor*>(createPluginFilter()));
        proc->prepareToPlay(48000.0, 512);
        for (int i = 1; i < args.size() && i < 5; ++i) proc->loadSource(i - 1, juce::File(args[i]));
        startTimer(100);
    }

private:
    void timerCallback() override
    {
        ++ticks;
        if (stage == 0)
        {
            bool busy = false;
            for (int i = 0; i < args.size() - 1 && i < 4; ++i) busy = busy || proc->isSourceLoading(i);
            if (busy && ticks < 200) return;
            setup();
            stage = 1; ticks = 0;
            return;
        }
        if (stage == 1 && ticks > 15)
        {
            proc->ui.view = 0;
            ed.reset(proc->createEditor()); ed->setSize(1440, 880);
            stage = 2; ticks = 0; return;
        }
        if (stage == 2 && ticks > 3)
        {
            shot(*ed, out.getChildFile("src1.png"));
            proc->ui.view = -1; proc->ui.inspectorOpen = true;
            ed.reset(proc->createEditor()); ed->setSize(1440, 880);
            stage = 3; ticks = 0; return;
        }
        if (stage == 3 && ticks > 3)
        {
            shot(*ed, out.getChildFile("main.png"));
            proc->project.edit("speed", [](ProjectState& s) { s.pads[0].s.speed = 2.0f; });
            proc->ui.view = 0; proc->ui.editingPad = 0; proc->ui.selectedPad = 0;
            proc->ui.src[0].viewStart = 0; proc->ui.src[0].viewLen = 0;
            ed.reset(proc->createEditor()); ed->setSize(1440, 880);
            stage = 4; ticks = 0; return;
        }
        if (stage == 4 && ticks > 3)
        {
            shot(*ed, out.getChildFile("editpad.png"));
            stopTimer();
            quit();
        }
    }

    void setup()
    {
        auto d = proc->project.sourceDataForSlot(0);
        if (d == nullptr) return;
        const int64_t beat = (int64_t) (d->sampleRate * 60.0 / std::max(60.0, d->analysis.bpm));
        auto& v = proc->ui.src[0];
        v.selStart = beat * 4; v.selEnd = beat * 6;
        proc->assignToPad(0, 0, 0, beat);
        proc->assignToPad(1, 0, beat * 2, beat * 3);
        if (proc->project.sourceDataForSlot(1)) proc->assignToPad(2, 1, beat, beat * 2);
        proc->project.edit("cut", [](ProjectState& s) { s.pads[0].s.chokeGroup = 1; s.pads[1].s.chokeGroup = 1; s.pads[2].s.playMode = 0; });
        v.candidates = ChopEngine::findSpots(*d, beat * 8, beat * 16, {}, proc->gridForSource(0));
        proc->project.edit("pattern", [](ProjectState& s)
        {
            for (int i = 0; i < 8; ++i) s.events.push_back({ newId(), i % 2, (int64_t) i * 960, -1, 480, 0.9f });
            s.events.push_back({ newId(), 2, 1200, 1180, 720, 0.7f });
            s.events.push_back({ newId(), 2, 1440, -1, 720, 0.7f });
        });
        proc->ui.selectedPad = 0;
    }

    juce::StringArray args;
    juce::File out;
    std::unique_ptr<HPSamplerProcessor> proc;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    int stage = 0, ticks = 0;
};

START_JUCE_APPLICATION(ShotApp)
