// Writes the sampler context HPSAMPLER would send to HPDG for one source, twice: the per-step bass from the
// classical analysis only (as before the stems) and from the BASS stem notes (not shipped; HPDG's
// HPDG_SamplerBassLab measures the generated 808 against them).
// One pad plays the first <bars> bars of the source once in MAIN (tempo and key fixed by the arguments).
// Usage: HPSAMPLER_ContextDump <source.wav> <outDir> <bpm> <sourceKeyRoot 0-11> <projectKeyRoot 0-11> <bars> [truth.json]
// truth.json (tools/stems/bass_truth_test.py): [[startSeconds, endSeconds, midi], ...] -> truth_main.json in MAIN
// ticks and project pitch: [[startTick, lengthTicks, midi], ...].
#include <juce_audio_utils/juce_audio_utils.h>
#include "app/PluginProcessor.h"
#include "bridge/BridgeAdapter.h"
#include "hpbridge/BridgeValidator.h"

using namespace hps;

class ContextDumpApp : public juce::JUCEApplicationBase, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "HPSAMPLER_ContextDump"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void systemRequestedQuit() override { quit(); }
    void unhandledException(const std::exception*, const juce::String&, int) override {}
    void shutdown() override { proc.reset(); }

    void initialise(const juce::String&) override
    {
        args = juce::StringArray::fromTokens(getCommandLineParameters(), true);
        for (auto& a : args) a = a.unquoted();
        if (args.size() < 6) { std::puts("usage: HPSAMPLER_ContextDump <wav> <outDir> <bpm> <srcKey> <projKey> <bars> [truth.json]"); setApplicationReturnValue(2); quit(); return; }
        out = juce::File(args[1]);
        out.createDirectory();
        bpm = args[2].getDoubleValue();
        srcKey = args[3].getIntValue();
        projKey = args[4].getIntValue();
        bars = args[5].getIntValue();
        proc.reset(dynamic_cast<HPSamplerProcessor*>(createPluginFilter()));
        proc->prepareToPlay(48000.0, 512);
        proc->loadSource(0, juce::File(args[0]));
        startTimer(100);
    }

private:
    void fail(const juce::String& why) { std::printf("FAIL: %s\n", why.toRawUTF8()); setApplicationReturnValue(1); stopTimer(); quit(); }

    void timerCallback() override
    {
        ++ticks;
        using S = StemSeparator::State;
        if (stage == 0)
        {
            if (proc->isSourceLoading(0) && ticks < 600) return;
            const auto d = proc->project.sourceDataForSlot(0);
            if (d == nullptr) return fail("source not loaded");
            const int64_t padEnd = juce::jmin<int64_t>(d->numFrames, (int64_t) std::llround(bars * 4 * 60.0 / bpm * d->sampleRate));
            proc->project.edit("setup", [&](ProjectState& s)
            {
                s.settings.followHostTempo = false;
                s.settings.bpm = bpm;
                s.settings.key = { projKey, 0 };
                s.settings.patternBars = bars;
                auto& src = s.sources[0];
                src.bpmOverride = bpm; src.originOverride = 0.0;
                src.keyOverrideRoot = srcKey; src.keyOverrideMode = 0;
            });
            proc->assignToPad(0, 0, 0, padEnd);
            proc->project.edit("pattern", [&](ProjectState& s)
            {
                s.pads[0].s.sync = (int) SyncMode::Stretch;
                s.pads[0].s.playMode = (int) PlayMode::OneShot;
                s.events.push_back({ newId(), 0, 0, -1, (int64_t) bars * kTicksPerBar, 1.0f });
            });
            // classical: the same project, the source without stem notes
            Project classic;
            classic.replaceState(proc->project.state(), true);
            classic.setSourceData(d);
            write("ctx_classic.json", BridgeAdapter::buildSamplerContext(classic, 1, "dump"));
            if (proc->stemsCached(0)) proc->loadCachedStems(0);
            else proc->startStems(0);
            stage = 1; ticks = 0; return;
        }
        if (stage == 1)
        {
            if ((proc->stems.state(0) == S::Separating || proc->stems.state(0) == S::Loading) && ticks < 6000) return;
            const auto d = proc->project.sourceDataForSlot(0);
            if (proc->stems.state(0) != S::Ready || d == nullptr || d->notes == nullptr) return fail("no stem notes: " + proc->stems.error(0));
            write("ctx_stems.json", BridgeAdapter::buildSamplerContext(proc->project, 1, "dump"));
            if (args.size() > 6)
            {
                // truth in MAIN: the pad plays source seconds 1:1 (pattern tempo = source tempo), key follow shifts it
                const int shift = proc->padSyncInfo(0).keyShift;
                const double ticksPerSecond = bpm * kPPQ / 60.0, padSeconds = bars * 4 * 60.0 / bpm;
                juce::Array<juce::var> rows;
                if (auto* arr = juce::JSON::parse(juce::File(args[6])).getArray())
                    for (const auto& n : *arr)
                    {
                        const double s = (double) n[0], e = std::min((double) n[1], padSeconds);
                        if (s >= padSeconds) continue;
                        juce::Array<juce::var> row;
                        row.add((juce::int64) std::llround(s * ticksPerSecond));
                        row.add((juce::int64) std::llround((e - s) * ticksPerSecond));
                        row.add((int) n[2] + shift);
                        rows.add(row);
                    }
                out.getChildFile("truth_main.json").replaceWithText(juce::JSON::toString(rows, true));
                std::printf("truth_main.json: %d notes, key shift %+d\n", rows.size(), shift);
            }
            std::printf("written to %s\n", out.getFullPathName().toRawUTF8());
            stopTimer();
            quit();
        }
    }

    void write(const juce::String& name, const hpbridge::SamplerContextSnapshot& c)
    {
        out.getChildFile(name).replaceWithText(hpbridge::BridgeValidator::toJson(c));
        int confident = 0;
        for (const auto& f : c.features) if (f.bassConfidence >= 0.35f) ++confident;
        std::printf("%s: %d steps, %d with a confident bass, %d bass notes\n", name.toRawUTF8(), (int) c.features.size(), confident, (int) c.bassNotes.size());
    }

    std::unique_ptr<HPSamplerProcessor> proc;
    juce::StringArray args;
    juce::File out;
    double bpm = 120;
    int srcKey = 0, projKey = 0, bars = 4, stage = 0, ticks = 0;
};

START_JUCE_APPLICATION(ContextDumpApp)
