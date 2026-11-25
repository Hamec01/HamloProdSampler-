#include "app/PluginProcessor.h"

#include "app/PluginEditor.h"
#include "core/Serialization.h"
#include "hpbridge/BridgeValidator.h"
#include "hpbridge/FileSnapshotTransport.h"
#include "hpbridge/HamloLinkReader.h"

namespace hps
{
HPSamplerProcessor::HPSamplerProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    project.onChanged = [this] { onProjectChanged(); };

    loader.onResult = [this](int slot, std::shared_ptr<const SourceData> data, bool analysed)
    {
        if (project.state().sources[(size_t) slot].id != data->id) return; // replaced meanwhile
        project.setSourceData(data);
        loading[(size_t) slot] = !analysed;
        if (!analysed) { setStatus("SRC " + juce::String(slot + 1) + ": waveform ready, analysing tempo / key..."); return; }

        const auto& a = data->analysis;
        auto& st = project.state();
        juce::String msg = "SRC " + juce::String(slot + 1) + ": " + a.summary;
        // First tonal source becomes the musical anchor when the project has none yet (explicit,
        // visible, and changeable on the BPM / KEY displays).
        if (st.settings.anchorSrc < 0 && !restoredSlot[(size_t) slot])
        {
            const bool hostTempo = st.settings.followHostTempo && engine.hostBpm.load() > 20.0;
            const double bpm = effectiveSourceBpm(st.sources[(size_t) slot], data.get());
            const auto key = effectiveSourceKey(st.sources[(size_t) slot], data.get());
            if ((bpm > 20.0 && !hostTempo) || (key.known() && !st.settings.key.known()))
            {
                mutateNoUndo([&](ProjectState& s)
                {
                    s.settings.anchorSrc = slot;
                    if (bpm > 20.0 && !hostTempo) s.settings.bpm = std::round(bpm * 10.0) / 10.0;
                    if (key.known() && !s.settings.key.known()) s.settings.key = key;
                });
                msg << "  ->  project anchor: " << (bpm > 20.0 && !hostTempo ? juce::String(bpm, 1) + " BPM " : juce::String())
                    << (key.known() ? key.label() : juce::String()) << " (change on BPM / KEY)";
            }
        }
        setStatus(msg);
    };
    loader.onError = [this](int slot, juce::String error)
    {
        loading[(size_t) slot] = false;
        // a file that cannot be decoded never leaves a half-loaded slot behind
        if (project.sourceDataForSlot(slot) == nullptr && (int) project.padsUsingSource(slot).size() == 0)
            project.mutate([slot](ProjectState& s) { const auto c = s.sources[(size_t) slot].colour; s.sources[(size_t) slot] = SourceSlot {}; s.sources[(size_t) slot].colour = c; });
        setStatus("SRC " + juce::String(slot + 1) + ": " + error);
        listeners.call([](Listener& l) { l.projectChanged(); });
    };
    renderer.onRendered = [this](int pad, std::unique_ptr<PadSound> s)
    {
        if (pad >= kNumPads) // a whole source at the project tempo
        {
            const size_t slot = (size_t) (pad - kNumPads);
            const auto d = project.sourceDataForSlot((int) slot);
            if (s == nullptr || d == nullptr || s->renderKey != fitSubmitted[slot]) return; // outdated
            auto p = std::make_unique<PreviewSource>();
            p->sampleRate = s->sampleRate;
            p->ratio = (double) s->audio.getNumSamples() / (double) d->numFrames;
            if (s->master != nullptr) p->audio = s->master; // shared with the pads
            else p->audio = std::make_shared<juce::AudioBuffer<float>>(std::move(s->audio));
            engine.publishPreviewSource((int) slot, std::move(p));
            fitPublished[slot] = fitSubmitted[slot];
            listeners.call([](Listener& l) { l.projectChanged(); });
            return;
        }
        contextDirty = true; // MAIN sounds different now
        lastProjectChangeMs = juce::Time::getMillisecondCounter();
        soundSeconds[(size_t) pad] = s ? s->audio.getNumSamples() / s->sampleRate : 0.0;
        engine.publishPadSound(pad, std::move(s));
        listeners.call([](Listener& l) { l.statusChanged(); });
    };
    session = std::make_unique<hpbridge::BridgeSession>(hpbridge::PeerType::Sampler, "HPSAMPLER");
    session->onStateChanged = [this]
    {
        bridgeStatus = "HP Bridge: " + session->getStatusText();
        if (session->getState() == hpbridge::BridgeConnectionState::Connected) contextDirty = true; // send on connect
        listeners.call([](Listener& l) { l.statusChanged(); });
    };
    session->onIncoming = [this](const juce::String& schema, const juce::String& json, std::int64_t revision)
    {
        if (schema != hpbridge::kDrumGuideSchemaV1) return false;
        hpbridge::DrumGuideSnapshot g;
        const auto r = hpbridge::BridgeValidator::fromJson(json, g);
        if (!r.ok) { setStatus("HPDG guide rejected: " + r.error); return false; }
        if (!followGuide) return true; // acknowledged, but the frozen guide stays
        linkedSlot = juce::File();
        guide = BridgeAdapter::fromDrumGuide(g, "HPDG live");
        bridgeStatus = "HP Bridge: " + session->getStatusText() + "  |  guide rev " + juce::String(revision);
        listeners.call([](Listener& l) { l.projectChanged(); });
        return true;
    };
    bridgeStatus = "HP Bridge: " + session->getStatusText();
    syncEngine();
    startTimerHz(30);
}

HPSamplerProcessor::~HPSamplerProcessor()
{
    stopTimer();
    session.reset(); // closes the slot: a connected HPDG shows Disconnected
}

bool HPSamplerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void HPSamplerProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    engine.prepare(sampleRate, samplesPerBlock);
    if (std::abs(sampleRate - engineRate) > 0.5)
    {
        engineRate = sampleRate;
        juce::MessageManager::callAsync([this] { submittedKeys.fill(0); syncEngine(); });
    }
}

void HPSamplerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    HostTransport host;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            host.valid = true;
            host.playing = pos->getIsPlaying();
            host.ppq = pos->getPpqPosition().orFallback(0.0);
            host.bpm = pos->getBpm().orFallback(0.0);
            host.recording = pos->getIsRecording();
            if (!pos->getPpqPosition().hasValue()) host.playing = false;
        }
    }
    host.offline = isNonRealtime();
    engine.process(buffer, midi, host);
    midi.clear();
}

juce::AudioProcessorEditor* HPSamplerProcessor::createEditor() { return new HPSamplerEditor(*this); }

// --------------------------------------------------------------------------------------------

void HPSamplerProcessor::setStatus(const juce::String& s)
{
    status = s;
    listeners.call([](Listener& l) { l.statusChanged(); });
}

void HPSamplerProcessor::mutateNoUndo(const std::function<void(ProjectState&)>& fn) { project.mutate(fn); }

void HPSamplerProcessor::onProjectChanged()
{
    contextDirty = true;
    lastProjectChangeMs = juce::Time::getMillisecondCounter();
    syncEngine();
    listeners.call([](Listener& l) { l.projectChanged(); });
}

std::unique_ptr<EngineSnapshot> HPSamplerProcessor::buildSnapshot() const
{
    const auto& st = project.state();
    auto s = std::make_unique<EngineSnapshot>();
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = st.pads[(size_t) i];
        auto& e = s->pads[(size_t) i];
        e.assigned = p.assigned;
        if (!p.assigned) continue;
        e.mode = effectivePlayMode(p, st.settings);
        e.repeat = p.s.repeat;
        e.choke = p.s.chokeGroup;
        e.loop = p.s.loop;
        e.src = p.src;
        const float g = (float) dbToGain(p.s.gainDb);
        e.gainL = g * juce::jmin(1.0f, 1.0f - p.s.pan);
        e.gainR = g * juce::jmin(1.0f, 1.0f + p.s.pan);
        e.attackS = p.s.attackMs * 0.001f;
        e.decayS = p.s.decayMs * 0.001f;
        e.sustain = juce::jlimit(0.0f, 1.0f, p.s.sustain);
        e.releaseS = juce::jmax(0.001f, p.s.releaseMs * 0.001f);
        e.hpHz = p.s.hpHz;
        e.lpHz = p.s.lpHz;
    }
    for (int i = 0; i < kNumSources; ++i)
    {
        const auto& src = st.sources[(size_t) i];
        auto& m = s->sources[(size_t) i];
        const float g = (float) dbToGain(src.gainDb);
        m.gainL = g * juce::jmin(1.0f, 1.0f - src.pan);
        m.gainR = g * juce::jmin(1.0f, 1.0f + src.pan);
        filterKnobToHz(src.filter, m.hpHz, m.lpHz);
    }
    s->mono = st.settings.mono;
    s->loopTicks = (int64_t) st.settings.patternBars * kTicksPerBar;
    for (const auto& ev : st.events)
        if (!ev.muted && ev.tick < s->loopTicks && st.pads[(size_t) ev.pad].assigned)
            s->events.push_back({ ev.pad, ev.tick, ev.length, ev.velocity, ev.rawTick });
    std::sort(s->events.begin(), s->events.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    s->bpm = st.settings.bpm;
    s->swing = st.settings.swing;
    s->metronome = st.settings.metronome;
    s->masterGain = (float) dbToGain(st.settings.masterDb);
    s->mainNeedsWindow = st.settings.mainNeedsWindow && wrapperType != wrapperType_Standalone;
    s->hostPlays = st.settings.hostPlays;
    s->hostCapture = st.settings.hostCapture;
    return s;
}

void HPSamplerProcessor::syncEngine()
{
    engine.publishSnapshot(buildSnapshot());
    for (int i = 0; i < kNumSources; ++i)
    {
        const auto d = project.sourceDataForSlot(i);
        const void* ptr = d ? d->audio.get() : nullptr;
        auto fit = ui.hearFitted && d ? PadRenderer::makeFitJob(project, i, engineRate) : PadRenderJob {};
        if (fit.audio != nullptr && std::abs(fit.timeRatio - 1.0) < 1e-4 && std::abs(fit.speed - 1.0) < 1e-4 && std::abs(fit.transpose) < 1e-3f)
            fit.audio = nullptr; // nothing to fit
        const uint64_t fitKey = fit.audio ? fit.key() : 0;
        const bool backToOriginal = fitKey == 0 && fitPublished[(size_t) i] != 0;
        if (fitKey == 0) fitSubmitted[(size_t) i] = fitPublished[(size_t) i] = 0;
        else if (fitKey != fitSubmitted[(size_t) i])
        {
            fitSubmitted[(size_t) i] = fitKey;
            renderer.submit(std::move(fit));
        }
        if (ptr != publishedPreview[(size_t) i] || backToOriginal)
        {
            publishedPreview[(size_t) i] = ptr;
            fitPublished[(size_t) i] = 0;
            auto p = std::make_unique<PreviewSource>();
            if (d) { p->audio = d->audio; p->sampleRate = d->sampleRate; }
            engine.publishPreviewSource(i, std::move(p));
        }
    }
    for (int i = 0; i < kNumPads; ++i)
    {
        auto job = PadRenderer::makeJob(project, i, engineRate);
        const uint64_t key = job.audio ? job.key() : 0;
        if (key != submittedKeys[(size_t) i])
        {
            submittedKeys[(size_t) i] = key;
            renderer.submit(std::move(job));
        }
    }
}

void HPSamplerProcessor::timerCallback()
{
    handleRecordedHits();
    engine.collectGarbage();
    const bool daw = engine.hostDriven.load();
    if (dawWasPlaying && !daw) // the DAW stopped: the pass is over
    {
        if (recordArmed) setRecordArmed(false);
        else if (dawGesture) project.endGesture();
        dawGesture = false;
        if (dawRecorded > 0)
            setStatus(juce::String(dawRecorded) + " note(s) recorded from the DAW into MAIN. The same notes in the DAW's piano roll will not sound twice.");
        else if (const int n = captureCount(); n > 0)
            setStatus(juce::String(n) + " note(s) played with the DAW are not in MAIN - CAPTURE writes them there");
        dawRecorded = 0;
        for (auto& t : take) t.open = false;
    }
    if (daw != dawWasPlaying) { dawWasPlaying = daw; listeners.call([](Listener& l) { l.statusChanged(); }); }

    const auto& st = project.state();
    const double hb = engine.hostBpm.load();
    if (st.settings.followHostTempo && hb > 20.0 && std::abs(hb - st.settings.bpm) > 0.005)
        mutateNoUndo([hb](ProjectState& s) { s.settings.bpm = hb; });

    if (session != nullptr)
    {
        session->setBpm((float) st.settings.bpm);
        // debounce: send the MAIN context 400 ms after the last edit, only while connected (no regeneration in HPDG)
        if (autoSyncContext && contextDirty && session->getState() == hpbridge::BridgeConnectionState::Connected
            && juce::Time::getMillisecondCounter() - lastProjectChangeMs > 400 && !renderer.isBusy())
            sendContextNow();
        if ((guidePoll % 30) == 0) updateBridgeName();
    }

    if (linkedSlot != juce::File() && ++guidePoll >= 30)
    {
        guidePoll = 0;
        hpbridge::DrumGuideSnapshot g;
        const auto r = hpbridge::HamloLinkReader::readDrumGuideFromSlot(linkedSlot, g);
        if (r.ok)
        {
            if (g.revision != guide.revision || !guide.present)
            {
                guide = BridgeAdapter::fromDrumGuide(g, "HPDG live (read-only)");
                bridgeStatus = "HPDG guide linked (read-only) rev " + juce::String(g.revision);
                listeners.call([](Listener& l) { l.projectChanged(); });
            }
        }
        else if (bridgeStatus.startsWith("HPDG guide linked"))
        {
            bridgeStatus = "HPDG guide stale: " + r.error;
            listeners.call([](Listener& l) { l.statusChanged(); });
        }
    }
}

// --------------------------------------------------------------------------------------------
// sources

SourceGrid HPSamplerProcessor::gridForSource(int slot) const
{
    SourceGrid g;
    const auto d = project.sourceDataForSlot(slot);
    if (d == nullptr) return g;
    const auto& s = project.state().sources[(size_t) slot];
    g.bpm = effectiveSourceBpm(s, d.get());
    if (g.bpm <= 20.0 && d->analysis.ready) // a guess is better than no grid: shown as such, never used to stretch
    {
        g.tentative = true;
        g.bpm = d->analysis.bpm > 20.0 ? d->analysis.bpm : project.state().settings.bpm;
    }
    g.origin = effectiveSourceOrigin(s, d.get());
    g.sampleRate = d->sampleRate;
    return g;
}

void HPSamplerProcessor::loadSource(int slot, const juce::File& file)
{
    if (slot < 0 || slot >= kNumSources) return;
    const auto id = newId();
    project.edit("Load SRC " + juce::String(slot + 1), [&](ProjectState& s)
    {
        auto& src = s.sources[(size_t) slot];
        const auto colour = src.colour;
        src = SourceSlot {};
        src.id = id;
        src.name = file.getFileNameWithoutExtension();
        src.filePath = file.getFullPathName();
        src.colour = colour.isTransparent() ? sourceColour(slot) : colour;
        if (s.settings.anchorSrc == slot) s.settings.anchorSrc = -1;
    });
    ui.src[(size_t) slot] = UiState::SrcView {};
    loading[(size_t) slot] = true;
    restoredSlot[(size_t) slot] = false;
    setStatus("SRC " + juce::String(slot + 1) + ": loading " + file.getFileName() + "...");
    loader.load(slot, file, id);
}

void HPSamplerProcessor::clearSource(int slot)
{
    project.edit("Clear SRC " + juce::String(slot + 1), [&](ProjectState& s)
    {
        for (int i = 0; i < kNumPads; ++i)
            if (s.pads[(size_t) i].assigned && s.pads[(size_t) i].src == slot) s.pads[(size_t) i] = Pad {};
        s.events.erase(std::remove_if(s.events.begin(), s.events.end(), [&](const PatternEvent& e) { return !s.pads[(size_t) e.pad].assigned; }), s.events.end());
        s.slices.erase(std::remove_if(s.slices.begin(), s.slices.end(), [&](const Slice& x) { return x.src == slot; }), s.slices.end());
        const auto colour = s.sources[(size_t) slot].colour;
        s.sources[(size_t) slot] = SourceSlot {};
        s.sources[(size_t) slot].colour = colour;
        if (s.settings.anchorSrc == slot) s.settings.anchorSrc = -1;
    });
    ui.src[(size_t) slot] = UiState::SrcView {};
}

// --------------------------------------------------------------------------------------------
// pads

void HPSamplerProcessor::assignToPad(int pad, int slot, int64_t start, int64_t end, const juce::String& name)
{
    if (pad < 0 || pad >= kNumPads || end <= start) return;
    project.edit("Assign " + padLabel(pad), [&](ProjectState& s)
    {
        Slice sl { newId(), slot, start, end, {} };
        s.slices.push_back(sl);
        auto& p = s.pads[(size_t) pad];
        const bool keepSettings = p.assigned;
        const auto settings = p.s;
        p = Pad {};
        p.assigned = true;
        p.sliceId = sl.id;
        p.src = slot;
        p.start = start;
        p.end = end;
        p.name = name.isNotEmpty() ? name : s.sources[(size_t) slot].name.substring(0, 10);
        if (keepSettings) p.s = settings; // re-assigning keeps the pad's character
        else p.s.chokeGroup = throughCutGroup(s, slot, end);
    });
    ui.selectedPad = pad;
    setStatus(padLabel(pad) + " <- SRC " + juce::String(slot + 1) + " ("
              + juce::String((double) (end - start) / juce::jmax(1.0, project.sourceDataForSlot(slot) ? project.sourceDataForSlot(slot)->sampleRate : 44100.0), 2) + " s)");
}

int HPSamplerProcessor::assignToNextEmptyPad(int slot, int64_t start, int64_t end)
{
    int pad = project.firstEmptyPad(ui.bank * kPadsPerBank);
    if (pad < 0) pad = project.firstEmptyPad(0);
    if (pad < 0) { setStatus("All 64 pads are used - drop onto a pad to replace it"); return -1; }
    assignToPad(pad, slot, start, end);
    ui.bank = pad / kPadsPerBank;
    return pad;
}

int HPSamplerProcessor::mapCandidatesToEmptyPads(int slot, const std::vector<ChopCandidate>& c, int fromPad, bool replace)
{
    int count = 0;
    project.edit("Map chops to pads", [&](ProjectState& s)
    {
        int next = fromPad;
        for (const auto& cand : c)
        {
            while (!replace && next < kNumPads && s.pads[(size_t) next].assigned) ++next;
            if (next >= kNumPads) break;
            Slice sl { newId(), slot, cand.start, cand.end, cand.origin };
            s.slices.push_back(sl);
            auto& p = s.pads[(size_t) next];
            p = Pad {};
            p.s.chokeGroup = throughCutGroup(s, slot, cand.end);
            p.assigned = true;
            p.sliceId = sl.id;
            p.src = slot;
            p.start = cand.start;
            p.end = cand.end;
            p.name = cand.origin + " " + juce::String(count + 1);
            ++count;
            ++next;
        }
    });
    return count;
}

int HPSamplerProcessor::throughCutGroup(const ProjectState& s, int slot, int64_t end) const
{
    const auto d = project.sourceDataForSlot(slot);
    if (d == nullptr || !ui.cutThrough || ui.chopLength != LengthMode::ToEnd || end < d->numFrames) return 0;
    std::array<bool, kMaxChokeGroups + 1> used {};
    for (const auto& p : s.pads)
    {
        if (!p.assigned || p.s.chokeGroup <= 0) continue;
        if (p.src == slot && p.end >= d->numFrames) return p.s.chokeGroup; // join the pads of this source
        used[(size_t) juce::jlimit(0, kMaxChokeGroups, p.s.chokeGroup)] = true;
    }
    for (int g = 1; g <= kMaxChokeGroups; ++g) if (!used[(size_t) g]) return g;
    return kMaxChokeGroups;
}

std::vector<int> HPSamplerProcessor::editTargets() const
{
    std::vector<int> t;
    const auto& pads = project.state().pads;
    if (ui.editBank < 0) { if (pads[(size_t) ui.selectedPad].assigned) t.push_back(ui.selectedPad); }
    else
        for (int i = ui.editBank * kPadsPerBank; i < (ui.editBank + 1) * kPadsPerBank; ++i)
            if (pads[(size_t) i].assigned) t.push_back(i);
    return t;
}

int HPSamplerProcessor::editRefPad() const
{
    if (ui.editBank < 0) return ui.selectedPad;
    const auto t = editTargets();
    if (t.empty()) return ui.editBank * kPadsPerBank;
    return std::find(t.begin(), t.end(), ui.selectedPad) != t.end() ? ui.selectedPad : t.front();
}

juce::String HPSamplerProcessor::editScopeName() const
{
    return ui.editBank < 0 ? padLabel(ui.selectedPad) : "BANK " + juce::String::charToString(juce::juce_wchar('A' + ui.editBank));
}

void HPSamplerProcessor::editPads(const juce::String& label, const std::function<void(PadSettings&, int)>& fn)
{
    const auto t = editTargets();
    if (t.empty()) return;
    project.edit(label + " " + editScopeName(), [&](ProjectState& s) { for (int i : t) fn(s.pads[(size_t) i].s, i); });
}

std::vector<int> HPSamplerProcessor::cutScopePads(CutScope scope) const
{
    std::vector<int> t;
    const auto& pads = project.state().pads;
    const int src = pads[(size_t) ui.selectedPad].src;
    for (int i = 0; i < kNumPads; ++i)
        if (pads[(size_t) i].assigned
            && (scope == CutScope::All || (scope == CutScope::Bank ? i / kPadsPerBank == ui.bank : pads[(size_t) i].src == src)))
            t.push_back(i);
    return t;
}

int HPSamplerProcessor::setCutGroup(CutScope scope, int group, bool toggle)
{
    const auto t = cutScopePads(scope);
    if (t.empty()) return 0;
    const auto& pads = project.state().pads;
    if (toggle && std::all_of(t.begin(), t.end(), [&](int i) { return pads[(size_t) i].s.chokeGroup == group; })) group = 0;
    project.edit("Cut group", [&](ProjectState& s) { for (int i : t) s.pads[(size_t) i].s.chokeGroup = group; });
    setStatus(juce::String((int) t.size()) + (group > 0 ? " pads are in cut group " + juce::String(group) + ": they stop each other" : juce::String(" pads do not cut each other"))
              + ".  Ctrl+Z = undo");
    return group;
}

bool HPSamplerProcessor::padPlaysToEnd(int pad) const
{
    const auto& p = project.state().pads[(size_t) pad];
    const auto d = p.assigned ? project.sourceDataForSlot(p.src) : nullptr;
    return d != nullptr && p.end >= d->numFrames;
}

void HPSamplerProcessor::togglePadToEnd(int pad)
{
    const auto& p = project.state().pads[(size_t) pad];
    const auto d = p.assigned ? project.sourceDataForSlot(p.src) : nullptr;
    if (d == nullptr) return;
    int64_t end = d->numFrames;
    if (p.end >= d->numFrames) // back to the length it was chopped with
    {
        for (const auto& sl : project.state().slices) if (sl.id == p.sliceId) end = sl.end;
        if (end <= p.start || end >= d->numFrames) { setStatus(padLabel(pad) + " already plays to the end (drag its right edge to shorten it)"); return; }
    }
    project.edit(padLabel(pad) + " length", [&](ProjectState& s) { s.pads[(size_t) pad].end = end; });
    setStatus(padLabel(pad) + (end >= d->numFrames ? " plays to the end of the sample" : " is back to its chopped length"));
}

void HPSamplerProcessor::clearPad(int pad)
{
    project.edit("Clear " + padLabel(pad), [&](ProjectState& s)
    {
        s.pads[(size_t) pad] = Pad {};
        s.events.erase(std::remove_if(s.events.begin(), s.events.end(), [pad](const PatternEvent& e) { return e.pad == pad; }), s.events.end());
    });
}

void HPSamplerProcessor::pastePad(int pad)
{
    if (!hasClipboard || !clipboard.assigned) return;
    project.edit("Paste " + padLabel(pad), [&](ProjectState& s) { s.pads[(size_t) pad] = clipboard; });
}

void HPSamplerProcessor::triggerPad(int pad, float velocity, int pointer)
{
    renderer.setFocus(pad);
    engine.padDown(pad, velocity, pointer);
    if (ui.selectedPad != pad)
    {
        ui.selectedPad = pad;
        listeners.call([](Listener& l) { l.statusChanged(); });
    }
}

void HPSamplerProcessor::releasePad(int pad, int pointer) { engine.padUp(pad, pointer); }

PadSyncInfo HPSamplerProcessor::padSyncInfo(int pad) const
{
    PadSyncInfo info;
    PadRenderer::makeJob(project, pad, engineRate, &info);
    return info;
}

double HPSamplerProcessor::padSoundSeconds(int pad) const { return soundSeconds[(size_t) pad]; }

// --------------------------------------------------------------------------------------------
// transport / recording

void HPSamplerProcessor::togglePlay()
{
    if (engine.hostDriven.load()) { setStatus("The DAW is playing and leads MAIN - start / stop it in the DAW"); return; }
    if (engine.transportPlaying.load()) stop();
    else playMain();
}

void HPSamplerProcessor::playMain(int64_t fromTick)
{
    if (engine.hostDriven.load()) { setStatus("The DAW is playing and leads MAIN - start / stop it in the DAW"); return; }
    engine.playFrom(fromTick);
}

void HPSamplerProcessor::stop()
{
    engine.stopAll();
    if (recordArmed) setRecordArmed(false);
}

bool HPSamplerProcessor::stopEverything()
{
    const bool main = engine.transportPlaying.load() && !engine.hostDriven.load();
    const bool any = main || engine.previewFrame.load() >= 0.0 || engine.liveVoices.load() > 0;
    if (any)
    {
        engine.stopAll();
        if (main && recordArmed) setRecordArmed(false);
    }
    return any;
}

HPSamplerProcessor::NowPlaying HPSamplerProcessor::nowPlaying() const
{
    NowPlaying n;
    using Who = NowPlaying::Who;
    const bool daw = engine.hostDriven.load(), main = engine.transportPlaying.load() && !daw;
    const bool src = engine.previewFrame.load() >= 0.0, pad = engine.liveVoices.load() > 0;
    const double t = engine.playheadTick.load();
    const juce::String pos = juce::String((int) (t / kTicksPerBar) + 1) + "." + juce::String((int) std::fmod(t / kPPQ, 4.0) + 1);
    const juce::String hand = src ? "SRC " + juce::String(engine.previewSlot.load() + 1)
                            : pad && engine.livePad.load() >= 0 ? padLabel(engine.livePad.load()) : juce::String();
    if (daw)
    {
        n.who = Who::Daw;
        n.title = (engine.hostRecording.load() ? "DAW REC " : "DAW ") + pos;
        const auto hp = project.state().settings.hostPlays;
        n.detail = hand.isNotEmpty() ? "+ " + hand : hp == HostPlays::Main ? "MAIN only" : hp == HostPlays::DawNotes ? "DAW notes only" : "MAIN + DAW notes";
    }
    else if (main && engine.countInBeat.load() > 0) { n.who = Who::Main; n.title = "COUNT " + juce::String(engine.countInBeat.load()); n.detail = "recording starts after 4"; }
    else if (main) { n.who = Who::Main; n.title = "MAIN " + pos; n.detail = hand.isNotEmpty() ? "+ " + hand : juce::String("plug-in only"); }
    else if (src) { n.who = Who::Source; n.title = hand; n.detail = "source audition"; }
    else if (pad) { n.who = Who::Pad; n.title = "PAD " + hand; n.detail = "plug-in only"; }
    else { n.title = "STOP"; n.detail = "Space = play"; }
    return n;
}

void HPSamplerProcessor::setRecordArmed(bool armed)
{
    if (armed == recordArmed) return;
    recordArmed = armed;
    engine.setRecording(armed);
    if (armed)
    {
        project.beginGesture("Record pass");
        pendingRec.clear();
        dawGesture = false;
        if (!engine.transportPlaying.load())
        {
            if (project.state().settings.countIn) engine.playWithCountIn();
            else engine.playFrom(0);
        }
        setStatus("REC: play pads / MIDI - hits are written to the 4 source lanes (overdub)");
    }
    else
    {
        handleRecordedHits();
        project.endGesture();
        pendingRec.clear();
        setStatus("Recording stopped");
    }
}

PatternEvent HPSamplerProcessor::eventFromHit(int pad, int64_t rawTick, float velocity) const
{
    const auto& st = project.state();
    const int64_t loop = (int64_t) st.settings.patternBars * kTicksPerBar;
    PatternEvent e;
    e.id = newId();
    e.pad = pad;
    e.rawTick = rawTick;
    e.tick = rawTick;
    const int g = gridTicks(st.settings.recGrid);
    if (g > 0 && st.settings.recQuantize > 0.0f)
    {
        const double q = std::round((double) rawTick / g) * g;
        const int64_t t = (int64_t) std::llround(rawTick + (q - rawTick) * st.settings.recQuantize);
        e.tick = ((t % loop) + loop) % loop;
    }
    e.velocity = velocity;
    e.length = effectivePlayMode(st.pads[(size_t) pad], st.settings) == PlayMode::Gate ? kPPQ / 8 : kPPQ / 4;
    return e;
}

bool HPSamplerProcessor::hasSameHit(int pad, int64_t tick) const
{
    const auto& st = project.state();
    const int64_t loop = (int64_t) st.settings.patternBars * kTicksPerBar;
    for (const auto& e : st.events)
    {
        if (e.pad != pad) continue;
        for (const int64_t t : { e.tick, e.rawTick })
        {
            if (t < 0) continue;
            const int64_t d = std::abs(t - tick);
            if (std::min(d, loop - d) <= kSameHitTicks) return true;
        }
    }
    return false;
}

void HPSamplerProcessor::handleRecordedHits()
{
    RecordedHit h;
    const int64_t loop = (int64_t) project.state().settings.patternBars * kTicksPerBar;
    while (engine.popRecordedHit(h))
    {
        // a pass longer than the pattern started under another length
        h.tick %= loop;
        const bool midi = (h.token & Engine::kMidiToken) != 0;
        if (h.type == RecordedHit::On)
        {
            if (h.pass != takePass) { take.clear(); takePass = h.pass; } // a new pass replaces the old take
            bool known = false;
            for (const auto& t : take)
            {
                const int64_t d = std::abs(t.tick - h.tick);
                known = known || (t.pad == h.pad && std::min(d, loop - d) <= kSameHitTicks);
            }
            if (!known && take.size() < 4096) take.push_back({ h.pad, h.tick, 0, h.velocity, h.token, true });

            const bool armed = recordArmed && h.rec;
            const bool fromDaw = h.rec && !recordArmed && h.host;
            if (!armed && !fromDaw) continue;
            if (h.host && midi && hasSameHit(h.pad, h.tick)) continue; // the DAW repeats a note MAIN already has
            if (fromDaw && !dawGesture) { project.beginGesture("Record from DAW"); dawGesture = true; pendingRec.clear(); }
            const auto e = eventFromHit(h.pad, h.tick, h.velocity);
            project.edit("Record", [&](ProjectState& s) { s.events.push_back(e); });
            pendingRec.push_back({ h.token, h.pad, e.id, h.tick });
            if (fromDaw) ++dawRecorded;
        }
        else
        {
            for (auto& t : take)
                if (t.open && t.token == h.token && t.pad == h.pad) { t.length = ((h.tick - t.tick) % loop + loop) % loop; t.open = false; }
            for (auto it = pendingRec.begin(); it != pendingRec.end(); ++it)
            {
                if (it->token != h.token || it->pad != h.pad) continue;
                const int64_t len = juce::jmax<int64_t>(30, ((h.tick - it->onTick) % loop + loop) % loop);
                const auto id = it->eventId;
                const auto& p = project.state().pads[(size_t) h.pad];
                if (effectivePlayMode(p, project.state().settings) == PlayMode::Gate)
                    project.edit("Record", [&](ProjectState& s)
                    {
                        for (auto& e : s.events) if (e.id == id) e.length = len;
                    });
                pendingRec.erase(it);
                break;
            }
        }
    }
}

int HPSamplerProcessor::captureCount() const
{
    const int64_t loop = (int64_t) project.state().settings.patternBars * kTicksPerBar;
    int n = 0;
    for (const auto& t : take)
        if (project.state().pads[(size_t) t.pad].assigned && !hasSameHit(t.pad, t.tick % loop)) ++n;
    return n;
}

void HPSamplerProcessor::captureTake()
{
    handleRecordedHits();
    const int64_t loop = (int64_t) project.state().settings.patternBars * kTicksPerBar;
    std::vector<PatternEvent> add;
    for (const auto& t : take)
    {
        if (!project.state().pads[(size_t) t.pad].assigned || hasSameHit(t.pad, t.tick % loop)) continue;
        auto e = eventFromHit(t.pad, t.tick % loop, t.velocity);
        if (t.length > 0 && effectivePlayMode(project.state().pads[(size_t) t.pad], project.state().settings) == PlayMode::Gate)
            e.length = juce::jmax<int64_t>(30, t.length);
        add.push_back(e);
    }
    if (add.empty()) { setStatus("Nothing to capture: play pads while the DAW (or MAIN) runs, then press CAPTURE"); return; }
    project.edit("Capture", [&](ProjectState& s) { s.events.insert(s.events.end(), add.begin(), add.end()); });
    ui.selectedEvents.clear();
    for (const auto& e : add) ui.selectedEvents.push_back(e.id);
    setStatus(juce::String((int) add.size()) + " note(s) captured into MAIN (REC Q applied, Undo removes them)");
    listeners.call([](Listener& l) { l.projectChanged(); });
}

void HPSamplerProcessor::requantizeRecorded()
{
    const auto& st = project.state();
    const int g = gridTicks(st.settings.recGrid);
    const int64_t loop = (int64_t) st.settings.patternBars * kTicksPerBar;
    project.edit("Quantize", [&](ProjectState& s)
    {
        for (auto& e : s.events)
        {
            const int64_t raw = e.rawTick >= 0 ? e.rawTick : e.tick;
            if (g <= 0 || s.settings.recQuantize <= 0.0f) { e.tick = raw; continue; }
            const double q = std::round((double) raw / g) * g;
            e.tick = ((int64_t) std::llround(raw + (q - raw) * s.settings.recQuantize) % loop + loop) % loop;
            if (e.rawTick < 0) e.rawTick = raw;
        }
    });
}

void HPSamplerProcessor::useSourceAsAnchor(int slot)
{
    const auto d = project.sourceDataForSlot(slot);
    if (d == nullptr) return;
    const auto& s = project.state().sources[(size_t) slot];
    const double bpm = effectiveSourceBpm(s, d.get());
    const auto key = effectiveSourceKey(s, d.get());
    project.edit("Use SRC " + juce::String(slot + 1) + " as anchor", [&](ProjectState& st)
    {
        st.settings.anchorSrc = slot;
        if (bpm > 20.0)
        {
            st.settings.bpm = std::round(bpm * 10.0) / 10.0;
            if (engine.hostBpm.load() > 20.0 && std::abs(engine.hostBpm.load() - bpm) > 0.05)
                st.settings.followHostTempo = false; // the anchor wins; the conflict is shown on BPM
        }
        if (key.known()) st.settings.key = key;
    });
    juce::String msg = "Project anchor: SRC " + juce::String(slot + 1) + " ";
    msg << (bpm > 20.0 ? juce::String(bpm, 1) + " BPM" : juce::String("(no reliable BPM)")) << ", "
        << (key.known() ? key.label() : juce::String("no key"));
    if (engine.hostBpm.load() > 20.0 && bpm > 20.0 && std::abs(engine.hostBpm.load() - bpm) > 0.05)
        msg << "  |  DAW is at " << juce::String(engine.hostBpm.load(), 1) << " BPM - set the DAW tempo to match";
    setStatus(msg);
}

// --------------------------------------------------------------------------------------------
// export / result

bool HPSamplerProcessor::exportWav(const juce::File& f, juce::String& error, double tailSeconds)
{
    renderer.finish(30000);
    std::array<const PadSound*, kNumPads> sounds {};
    for (int i = 0; i < kNumPads; ++i) sounds[(size_t) i] = engine.padSound(i);
    juce::AudioBuffer<float> out;
    auto snap = buildSnapshot();
    snap->metronome = false; // never in a file
    Engine::renderOffline(*snap, sounds, engineRate, out, tailSeconds);
    f.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream(f.createOutputStream());
    if (stream == nullptr) { error = "Cannot write " + f.getFullPathName(); return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream.get(), engineRate, 2, 24, {}, 0));
    if (w == nullptr) { error = "WAV writer failed"; return false; }
    stream.release();
    w->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
    return true;
}

bool HPSamplerProcessor::exportMidi(const juce::File& f, juce::String& error)
{
    auto midi = Serialization::patternToMidi(project.state());
    f.deleteFile();
    juce::FileOutputStream os(f);
    if (!os.openedOk()) { error = "Cannot write " + f.getFullPathName(); return false; }
    midi.writeTo(os);
    return true;
}

juce::File HPSamplerProcessor::writeDragFile(bool midi, juce::String& error)
{
    const auto& st = project.state();
    if (st.events.empty()) { error = "MAIN is empty: nothing to drag"; return {}; }
    const auto dir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HamloProd/HPSAMPLER/Drag");
    if (!dir.createDirectory()) { error = "Cannot create " + dir.getFullPathName(); return {}; }
    juce::String name = "HPSAMPLER " + juce::String(st.settings.bpm, std::abs(st.settings.bpm - std::round(st.settings.bpm)) < 0.05 ? 0 : 1) + " BPM";
    if (st.settings.key.known()) name << " " << st.settings.key.label();
    name << " " << juce::Time::getCurrentTime().formatted("%y%m%d-%H%M%S");
    const auto f = dir.getNonexistentChildFile(juce::File::createLegalFileName(name), midi ? ".mid" : ".wav", false);
    const bool ok = midi ? exportMidi(f, error) : exportWav(f, error, 0.0); // exactly the loop: it repeats cleanly in the playlist
    return ok ? f : juce::File();
}

void HPSamplerProcessor::renderResult()
{
    if (project.state().events.empty()) { resultAudio.reset(); resultRevision = project.revision(); return; }
    renderer.finish(30000);
    std::array<const PadSound*, kNumPads> sounds {};
    for (int i = 0; i < kNumPads; ++i) sounds[(size_t) i] = engine.padSound(i);
    auto out = std::make_shared<juce::AudioBuffer<float>>();
    Engine::renderOffline(*buildSnapshot(), sounds, engineRate, *out, 0.0);
    resultAudio = out;
    resultRevision = project.revision();
}

// --------------------------------------------------------------------------------------------
// bridge

bool HPSamplerProcessor::exportSamplerContext(juce::File& written, juce::String& error)
{
    hpbridge::FileSnapshotTransport t;
    const auto snap = BridgeAdapter::buildSamplerContext(project, ++contextRevision, instanceId);
    written = t.getExchangeDirectory().getChildFile("hp.sample-context.v1.json");
    const auto r = t.publishSamplerContext(snap, written);
    if (!r.ok) { error = r.error; return false; }
    bridgeStatus = "Context snapshot rev " + juce::String(contextRevision) + " exported (file mode)";
    return true;
}

bool HPSamplerProcessor::importDrumGuide(const juce::File& f, juce::String& error)
{
    hpbridge::FileSnapshotTransport t;
    hpbridge::DrumGuideSnapshot g;
    const auto r = t.loadDrumGuide(f, g);
    if (!r.ok) { error = r.error; return false; }
    linkedSlot = juce::File();
    guide = BridgeAdapter::fromDrumGuide(g, "file " + f.getFileName());
    bridgeStatus = "Drum guide imported (file) rev " + juce::String(g.revision);
    listeners.call([](Listener& l) { l.projectChanged(); });
    return true;
}

std::vector<hpbridge::BridgePeerInfo> HPSamplerProcessor::discoverHpdg()
{
    return hpbridge::HamloLinkReader::discoverLiveHpdgInstances();
}

bool HPSamplerProcessor::linkHpdgGuide(const hpbridge::BridgePeerInfo& peer, juce::String& error)
{
    hpbridge::DrumGuideSnapshot g;
    const auto r = hpbridge::HamloLinkReader::readDrumGuideFromSlot(peer.backingFile, g);
    if (!r.ok) { error = r.error; return false; }
    linkedSlot = peer.backingFile;
    guide = BridgeAdapter::fromDrumGuide(g, "HPDG live (read-only)");
    bridgeStatus = "HPDG guide linked (read-only) rev " + juce::String(g.revision);
    listeners.call([](Listener& l) { l.projectChanged(); });
    return true;
}

void HPSamplerProcessor::sendContextNow()
{
    if (session == nullptr) return;
    auto snap = BridgeAdapter::buildSamplerContext(project, session->getPublishedRevision() + 1, instanceId);
    const auto wav = writeMainAudioReference();
    if (wav.existsAsFile()) snap.audioReference = wav.getFullPathName();
    const auto check = hpbridge::BridgeValidator::validate(snap);
    if (!check.ok) { setStatus("Context not sent: " + check.error); contextDirty = false; return; }
    const auto rev = session->publish(hpbridge::kSamplerContextSchemaV1, hpbridge::BridgeValidator::toJson(snap));
    contextDirty = false;
    bridgeStatus = "HP Bridge: " + session->getStatusText() + "  |  context rev " + juce::String(rev)
                   + (session->getPartnerAckRevision() > 0 ? "  (HPDG has rev " + juce::String(session->getPartnerAckRevision()) + ")" : juce::String());
    listeners.call([](Listener& l) { l.statusChanged(); });
}

juce::File HPSamplerProcessor::writeMainAudioReference()
{
    const auto& st = project.state();
    const auto dir = hpbridge::bridgeDirectory().getChildFile("audio")
                         .getChildFile(juce::String::toHexString((juce::int64) session->getInstanceId()).paddedLeft('0', 16));
    dir.createDirectory();
    if (st.events.empty()) return {};

    std::array<const PadSound*, kNumPads> sounds {};
    for (int i = 0; i < kNumPads; ++i) sounds[(size_t) i] = engine.padSound(i);
    juce::AudioBuffer<float> out;
    Engine::renderOffline(*buildSnapshot(), sounds, engineRate, out, 0.0); // exactly one loop: HPDG reads N bars

    // "HPSAMPLER main 83 BPM B min.wav": the tempo / key labels HPDG trusts (still checked against the audio)
    juce::String name = "HPSAMPLER main " + (std::abs(st.settings.bpm - std::round(st.settings.bpm)) < 0.05
                                                 ? juce::String(juce::roundToInt(st.settings.bpm)) : juce::String(st.settings.bpm, 1)) + " BPM";
    if (st.settings.key.known())
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        name << " " << names[st.settings.key.root] << (st.settings.key.mode == 1 ? " maj" : " min");
    }
    // write next to the old files, then remove the old ones (HPDG may still be reading the previous revision)
    const auto file = dir.getChildFile(name + " r" + juce::String(session->getPublishedRevision() + 1) + ".wav");
    {
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr) return {};
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream.get(), engineRate, 2, 24, {}, 0));
        if (w == nullptr) return {};
        stream.release();
        w->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
    }
    auto olds = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
    std::sort(olds.begin(), olds.end(), [](const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    for (int i = 0; i < olds.size(); ++i)
        if (olds[i] != file && i >= 2) olds.getReference(i).deleteFile(); // keep the newest two
    return file;
}

void HPSamplerProcessor::updateBridgeName()
{
    // "HPSAMPLER - Piano" helps picking the right instance in HPDG when several are open
    juce::String n = "HPSAMPLER";
    for (const auto& s : project.state().sources)
        if (!s.empty()) { n << " - " << s.name.substring(0, 24); break; }
    if (n != bridgeName) { bridgeName = n; session->setDisplayName(n); }
}

void HPSamplerProcessor::unlinkGuide()
{
    linkedSlot = juce::File();
    guide = GuideOverlay {};
    bridgeStatus = {};
    listeners.call([](Listener& l) { l.projectChanged(); });
}

// --------------------------------------------------------------------------------------------
// state

void HPSamplerProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const auto json = Serialization::toJson(project.state());
    destData.replaceAll(json.toRawUTF8(), json.getNumBytesAsUTF8());
}

void HPSamplerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto json = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    juce::MessageManager::callAsync([this, json]
    {
        ProjectState s;
        juce::String err;
        if (!Serialization::fromJson(json, s, err)) { setStatus("State not restored: " + err); return; }
        project.replaceState(s, true);
        for (int i = 0; i < kNumSources; ++i)
        {
            const auto& src = s.sources[(size_t) i];
            if (src.empty()) continue;
            const juce::File f(src.filePath);
            if (f.existsAsFile()) { loading[(size_t) i] = true; restoredSlot[(size_t) i] = true; loader.load(i, f, src.id); }
            else setStatus("SRC " + juce::String(i + 1) + " missing: " + src.filePath + " - use LOAD to relink");
        }
    });
}
} // namespace hps

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new hps::HPSamplerProcessor(); }
