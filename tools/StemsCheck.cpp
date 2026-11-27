// End-to-end check of the stem feature on the real processor (not shipped):
// load -> split with HPStems.exe -> mute DRUMS -> the source audio is bass + vox + inst -> undo -> original ->
// save state -> a new instance restores the mix from the cache. Writes PNGs of the SRC header.
// Usage: HPSAMPLER_StemsCheck <outDir> <source.wav>
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

class StemsCheckApp : public juce::JUCEApplicationBase, private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "HPSAMPLER_StemsCheck"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted(const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void systemRequestedQuit() override { quit(); }
    void unhandledException(const std::exception*, const juce::String&, int) override {}
    void shutdown() override { ed.reset(); proc.reset(); proc2.reset(); }

    void initialise(const juce::String&) override
    {
        auto args = juce::StringArray::fromTokens(getCommandLineParameters(), true);
        args.trim();
        for (auto& a : args) a = a.unquoted();
        if (args.size() < 2) { std::puts("usage: HPSAMPLER_StemsCheck <outDir> <source.wav>"); setApplicationReturnValue(2); quit(); return; }
        out = juce::File(args[0]);
        out.createDirectory();
        file = juce::File(args[1]);
        proc.reset(dynamic_cast<HPSamplerProcessor*>(createPluginFilter()));
        proc->prepareToPlay(48000.0, 512);
        proc->loadSource(0, file);
        startTimer(100);
    }

private:
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
    void editorShot(HPSamplerProcessor& p, const juce::String& name)
    {
        p.ui.view = 0;
        ed.reset(p.createEditor());
        ed->setSize(1440, 880);
        pendingShot = name;
    }

    void timerCallback() override
    {
        ++ticks;
        if (pendingShot.isNotEmpty() && ticks % 3 == 0)
        {
            shot(*ed, out.getChildFile(pendingShot + ".png"));
            ed.reset();
            pendingShot = {};
            return;
        }
        using S = StemSeparator::State;
        switch (stage)
        {
            case 0: // source analysed
                if (proc->isSourceLoading(0) && ticks < 600) return;
                original = proc->project.sourceDataForSlot(0);
                check(original != nullptr, "source loaded");
                if (original == nullptr) return finish();
                check(proc->stemEngineInstalled(), "stem engine found: " + StemSeparator::helperExe().getFullPathName());
                StemSeparator::cacheDirFor(proc->project.state().sources[0], *original).deleteRecursively(); // a real split
                check(!proc->stemsCached(0), "no cache before the split");
                editorShot(*proc, "1_offer");
                proc->assignToPad(0, 0, 0, (int64_t) original->sampleRate); // a pad that must follow the stem mix
                stage = 1; ticks = 0; return;
            case 1:
                if (pendingShot.isNotEmpty()) return;
                t0 = juce::Time::getMillisecondCounterHiRes();
                proc->startStems(0);
                stage = 2; ticks = 0; return;
            case 2: // splitting
                if (proc->stems.state(0) == S::Separating && ticks == 20) editorShot(*proc, "2_working");
                if ((proc->stems.state(0) == S::Separating || proc->stems.state(0) == S::Loading) && ticks < 3000) return;
                check(proc->stems.state(0) == S::Ready, "stems ready (" + proc->stems.error(0) + ") in "
                      + juce::String((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) + " s");
                if (proc->stems.state(0) != S::Ready) return finish();
                check(proc->stemsCached(0), "cache written");
                check(proc->project.sourceDataForSlot(0) == original, "unity mix = the original is heard");
                padBefore = proc->padSoundSeconds(0);
                proc->project.edit("Stem mute", [](ProjectState& s) { s.sources[0].stemMute[kStemDrums] = true; s.sources[0].stemDb[kStemVox] = 3.0f; });
                stage = 3; ticks = 0; return;
            case 3: // mix applied
            {
                const auto d = proc->project.sourceDataForSlot(0);
                if (d == original && ticks < 100) return;
                check(d != original, "mix published");
                if (d == original) return finish();
                const auto set = proc->stems.stems(0);
                double err = 0.0, ref = 0.0, resid = 0.0;
                const float gv = (float) dbToGain(3.0f);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < (int) d->numFrames; ++i)
                    {
                        const float want = set->stems[kStemBass]->getSample(c, i) + gv * set->stems[kStemVox]->getSample(c, i)
                                         + set->stems[kStemInst]->getSample(c, i);
                        err = juce::jmax(err, (double) std::abs(d->audio->getSample(c, i) - want));
                        ref += (double) original->audio->getSample(c, i) * original->audio->getSample(c, i);
                        double sum = 0.0;
                        for (int k = 0; k < kNumStems; ++k) sum += set->stems[(size_t) k]->getSample(c, i);
                        resid += (sum - original->audio->getSample(c, i)) * (sum - original->audio->getSample(c, i));
                    }
                check(err < 1e-5, "mixed audio = bass + vox(+3 dB) + inst (max err " + juce::String(err, 8) + ")");
                check(10.0 * std::log10(ref / juce::jmax(1e-12, resid)) > 15.0,
                      "sum of the 4 stems ~ original (" + juce::String(10.0 * std::log10(ref / juce::jmax(1e-12, resid)), 1) + " dB)");
                check(d->peaks.maxs.size() == original->peaks.maxs.size(), "waveform peaks follow the mix");
                editorShot(*proc, "3_ready_drums_muted");
                stage = 4; ticks = 0; return;
            }
            case 4: // pad re-rendered from the mix, then undo
                if (pendingShot.isNotEmpty() || (proc->isPadRendering(0) && ticks < 100)) return;
                check(!proc->isPadRendering(0) && proc->padSoundSeconds(0) > 0.0, "pad re-rendered from the mix");
                proc->project.undo();
                stage = 5; ticks = 0; return;
            case 5:
                if (proc->project.sourceDataForSlot(0) != original && ticks < 50) return;
                check(proc->project.sourceDataForSlot(0) == original, "undo -> the original again");
                proc->project.redo();
                stage = 6; ticks = 0; return;
            case 6:
            {
                if (proc->project.sourceDataForSlot(0) == original && ticks < 100) return;
                check(proc->project.sourceDataForSlot(0) != original, "redo -> the mix again");
                juce::MemoryBlock mb;
                proc->getStateInformation(mb);
                proc2.reset(dynamic_cast<HPSamplerProcessor*>(createPluginFilter()));
                proc2->prepareToPlay(48000.0, 512);
                proc2->setStateInformation(mb.getData(), (int) mb.getSize());
                stage = 7; ticks = 0; return;
            }
            case 7: // restored instance gets the mix back from the cache, no split
            {
                const auto d = proc2->project.sourceDataForSlot(0);
                const bool mixed = d != nullptr && proc2->stems.state(0) == S::Ready && d->audio.get() != nullptr
                                   && std::abs(d->audio->getSample(0, 1000) - proc->project.sourceDataForSlot(0)->audio->getSample(0, 1000)) < 1e-6f
                                   && proc2->project.state().sources[0].stemMute[kStemDrums];
                if (!mixed && ticks < 600) return;
                check(mixed, "restored project: stem mix back from the cache");
                editorShot(*proc2, "4_restored");
                stage = 8; ticks = 0; return;
            }
            case 8:
                if (pendingShot.isNotEmpty()) return;
                finish();
                return;
            default: return;
        }
    }

    std::unique_ptr<HPSamplerProcessor> proc, proc2;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    std::shared_ptr<const SourceData> original;
    juce::File out, file;
    juce::String pendingShot;
    double t0 = 0.0, padBefore = 0.0;
    int stage = 0, ticks = 0, failures = 0;
};

START_JUCE_APPLICATION(StemsCheckApp)
