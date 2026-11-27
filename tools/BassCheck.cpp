// Checks the bass line HPSAMPLER sends to HPDG, end to end on the real processor (not shipped):
// a mix with a known bass line (tools/stems/bass_truth_test.py make) is split into stems, the BASS stem
// transcribed, one pad plays it in MAIN with key follow, and the context's bass notes are compared with the truth.
// Usage: HPSAMPLER_BassCheck <dir with mix.wav + truth.json>
#include <juce_audio_utils/juce_audio_utils.h>
#include "app/PluginProcessor.h"
#include "app/PluginEditor.h"
#include "bridge/BridgeAdapter.h"
#include "hpbridge/BridgeValidator.h"

using namespace hps;

class BassCheckApp : public juce::JUCEApplicationBase, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "HPSAMPLER_BassCheck"; }
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
        auto args = juce::StringArray::fromTokens(getCommandLineParameters(), true);
        for (auto& a : args) a = a.unquoted();
        if (args.isEmpty()) { std::puts("usage: HPSAMPLER_BassCheck <dir>"); setApplicationReturnValue(2); quit(); return; }
        dir = juce::File(args[0]);
        const auto truthVar = juce::JSON::parse(dir.getChildFile("truth.json"));
        if (auto* arr = truthVar.getArray())
            for (const auto& n : *arr) truth.push_back({ (double) n[0], (double) n[1], (int) n[2] });
        proc.reset(dynamic_cast<HPSamplerProcessor*>(createPluginFilter()));
        proc->prepareToPlay(48000.0, 512);
        proc->loadSource(0, dir.getChildFile("mix.wav"));
        startTimer(100);
    }

private:
    struct Truth { double start, end; int midi; };
    static constexpr double kBpm = 92.0;

    void check(bool ok, const juce::String& what)
    {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
        std::fflush(stdout);
        if (!ok) ++failures;
    }
    void finish()
    {
        stopTimer();
        std::printf("%d failure(s)\n", failures);
        setApplicationReturnValue(failures == 0 ? 0 : 1);
        quit();
    }

    // truth notes inside the pad (first 2 bars), in MAIN ticks, shifted by `semis`, vs the context
    void compare(const hpbridge::SamplerContextSnapshot& c, int semis, const juce::String& label)
    {
        const double ticksPerSecond = kBpm * kPPQ / 60.0;
        const double padSeconds = 8 * 60.0 / kBpm;
        int expected = 0, hit = 0;
        std::vector<bool> used(c.bassNotes.size(), false);
        for (const auto& t : truth)
        {
            if (t.start >= padSeconds - 0.05) continue;
            ++expected;
            const double tick = t.start * ticksPerSecond;
            for (size_t i = 0; i < c.bassNotes.size(); ++i)
                if (!used[i] && std::abs((double) c.bassNotes[i].startTick - tick) <= 0.06 * ticksPerSecond
                    && c.bassNotes[i].midi == t.midi + semis)
                { used[i] = true; ++hit; break; }
        }
        const int found = (int) c.bassNotes.size();
        check(expected > 0 && hit >= expected * 0.9 && hit >= found * 0.75,
              label + ": " + juce::String(hit) + " of " + juce::String(expected) + " truth notes at +" + juce::String(semis)
              + " st, " + juce::String(found) + " sent");
        int stepsOk = 0, stepsKnown = 0;
        for (const auto& t : truth)
        {
            if (t.start >= padSeconds - 0.05) continue;
            const size_t step = (size_t) std::floor((t.start + 0.05) * ticksPerSecond / (kPPQ / 4));
            if (step >= c.features.size()) continue;
            ++stepsKnown;
            if (c.features[step].bassMidi == t.midi + semis && c.features[step].bassConfidence >= 0.35f) ++stepsOk;
        }
        check(stepsOk >= stepsKnown * 0.9, label + ": per-step bassMidi right on " + juce::String(stepsOk) + " / " + juce::String(stepsKnown) + " note steps (conf >= 0.35)");
    }

    void timerCallback() override
    {
        ++ticks;
        using S = StemSeparator::State;
        if (stage == 0)
        {
            if (proc->isSourceLoading(0) && ticks < 600) return;
            auto d = proc->project.sourceDataForSlot(0);
            check(d != nullptr, "source loaded");
            if (d == nullptr) return finish();
            const int64_t padEnd = (int64_t) std::llround(8 * 60.0 / kBpm * d->sampleRate);
            proc->project.edit("setup", [&](ProjectState& s)
            {
                s.settings.followHostTempo = false;
                s.settings.bpm = kBpm;
                s.settings.key = { 11, 0 };       // B minor
                s.settings.patternBars = 2;
                auto& src = s.sources[0];
                src.bpmOverride = kBpm; src.originOverride = 0.0;
                src.keyOverrideRoot = 9; src.keyOverrideMode = 0; // A minor
            });
            proc->assignToPad(0, 0, 0, padEnd);
            proc->project.edit("pattern", [](ProjectState& s)
            {
                s.pads[0].s.sync = (int) SyncMode::Stretch;
                s.pads[0].s.playMode = (int) PlayMode::OneShot;
                s.events.push_back({ newId(), 0, 0, -1, 2 * kTicksPerBar, 1.0f });
            });
            StemSeparator::cacheDirFor(proc->project.state().sources[0], *d).deleteRecursively();
            proc->startStems(0);
            stage = 1; ticks = 0; return;
        }
        if (stage == 1)
        {
            if ((proc->stems.state(0) == S::Separating || proc->stems.state(0) == S::Loading) && ticks < 3000) return;
            check(proc->stems.state(0) == S::Ready, "stems + notes ready " + proc->stems.error(0));
            const auto d = proc->project.sourceDataForSlot(0);
            check(d != nullptr && d->notes != nullptr && !d->notes->bassLine.empty(),
                  "bass line attached to the source (" + juce::String(d && d->notes ? (int) d->notes->bassLine.size() : 0) + " notes)");
            if (d == nullptr || d->notes == nullptr) return finish();
            const auto sync = proc->padSyncInfo(0);
            check(sync.keyShift == 2, "key follow A min -> B min = +" + juce::String(sync.keyShift));

            compare(BridgeAdapter::buildSamplerContext(proc->project, 1, "check"), 2, "key follow");

            proc->project.edit("pitch", [](ProjectState& s) { s.pads[0].s.pitch = 5.0f; });
            compare(BridgeAdapter::buildSamplerContext(proc->project, 1, "check"), 7, "key follow + pad pitch 5");

            proc->project.edit("mute bass", [](ProjectState& s) { s.sources[0].stemMute[kStemBass] = true; });
            const auto muted = BridgeAdapter::buildSamplerContext(proc->project, 1, "check");
            int strong = 0;
            for (const auto& f : muted.features) if (f.bassConfidence >= 0.35f) ++strong;
            check(muted.bassNotes.empty() && strong == 0, "BASS stem muted -> no bass for HPDG (" + juce::String((int) muted.bassNotes.size())
                  + " notes, " + juce::String(strong) + " confident steps)");

            proc->project.edit("unmute", [](ProjectState& s) { s.sources[0].stemMute[kStemBass] = false; });
            const auto json = hpbridge::BridgeValidator::toJson(BridgeAdapter::buildSamplerContext(proc->project, 1, "check"));
            hpbridge::SamplerContextSnapshot back;
            const auto r = hpbridge::BridgeValidator::fromJson(json, back);
            check(r.ok && !back.bassNotes.empty(), "protocol round trip with bassNotes (" + r.error + ")");
            proc->project.edit("pitch back", [](ProjectState& s) { s.pads[0].s.pitch = 0.0f; });
            proc->ui.view = -1; proc->ui.mainTab = 0;
            ed.reset(proc->createEditor()); ed->setSize(1440, 880);
            stage = 2; ticks = 0; return;
        }
        if (stage == 2 && ticks > 5)
        {
            auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.0f);
            const auto f = dir.getChildFile("main_bass.png");
            f.deleteFile();
            juce::FileOutputStream os(f);
            juce::PNGImageFormat().writeImageToStream(img, os);
            ed.reset();
            return finish();
        }
    }

    std::unique_ptr<HPSamplerProcessor> proc;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    juce::File dir;
    std::vector<Truth> truth;
    int stage = 0, ticks = 0, failures = 0;
};

START_JUCE_APPLICATION(BassCheckApp)
