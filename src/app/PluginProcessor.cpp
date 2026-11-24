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
            if (!pos->getPpqPosition().hasValue()) host.playing = false;
        }
    }
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
    s->mono = st.settings.mono;
    s->loopTicks = (int64_t) st.settings.patternBars * kTicksPerBar;
    for (const auto& ev : st.events)
        if (ev.tick < s->loopTicks && st.pads[(size_t) ev.pad].assigned)
            s->events.push_back({ ev.pad, ev.tick, ev.length, ev.velocity });
    std::sort(s->events.begin(), s->events.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    s->bpm = st.settings.bpm;
    s->swing = st.settings.swing;
    s->metronome = st.settings.metronome;
    s->masterGain = (float) dbToGain(st.settings.masterDb);
    return s;
}

void HPSamplerProcessor::syncEngine()
{
    engine.publishSnapshot(buildSnapshot());
    for (int i = 0; i < kNumSources; ++i)
    {
        const auto d = project.sourceDataForSlot(i);
        const void* ptr = d ? d->audio.get() : nullptr;
        if (ptr != publishedPreview[(size_t) i])
        {
            publishedPreview[(size_t) i] = ptr;
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

    const auto& st = project.state();
    const double hb = engine.hostBpm.load();
    if (st.settings.followHostTempo && hb > 20.0 && std::abs(hb - st.settings.bpm) > 0.005)
        mutateNoUndo([hb](ProjectState& s) { s.settings.bpm = hb; });

    if (session != nullptr)
    {
        session->setBpm((float) st.settings.bpm);
        // debounce: send the MAIN context 400 ms after the last edit, only while connected (no regeneration in HPDG)
        if (autoSyncContext && contextDirty && session->getState() == hpbridge::BridgeConnectionState::Connected
            && juce::Time::getMillisecondCounter() - lastProjectChangeMs > 400)
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

int HPSamplerProcessor::mapCandidatesToEmptyPads(int slot, const std::vector<ChopCandidate>& c, int fromPad)
{
    int count = 0;
    project.edit("Map chops to pads", [&](ProjectState& s)
    {
        int next = fromPad;
        for (const auto& cand : c)
        {
            while (next < kNumPads && s.pads[(size_t) next].assigned) ++next;
            if (next >= kNumPads) break;
            Slice sl { newId(), slot, cand.start, cand.end, cand.origin };
            s.slices.push_back(sl);
            auto& p = s.pads[(size_t) next];
            p = Pad {};
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
    if (engine.transportPlaying.load() && !engine.hostDriven.load()) stop();
    else engine.setInternalPlaying(true, true);
}

void HPSamplerProcessor::stop()
{
    engine.setInternalPlaying(false);
    if (recordArmed) setRecordArmed(false);
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
        if (!engine.transportPlaying.load()) engine.setInternalPlaying(true, true);
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

void HPSamplerProcessor::handleRecordedHits()
{
    RecordedHit h;
    const auto& st = project.state();
    const int64_t loop = (int64_t) st.settings.patternBars * kTicksPerBar;
    auto quantize = [&](int64_t raw)
    {
        const int g = gridTicks(st.settings.recGrid);
        if (g <= 0 || st.settings.recQuantize <= 0.0f) return raw;
        const double q = std::round((double) raw / g) * g;
        int64_t t = (int64_t) std::llround(raw + (q - raw) * st.settings.recQuantize);
        return ((t % loop) + loop) % loop;
    };
    while (engine.popRecordedHit(h))
    {
        if (!recordArmed) continue;
        if (h.type == RecordedHit::On)
        {
            PatternEvent e;
            e.id = newId();
            e.pad = h.pad;
            e.rawTick = h.tick;
            e.tick = quantize(h.tick);
            e.velocity = h.velocity;
            const auto& p = st.pads[(size_t) h.pad];
            e.length = effectivePlayMode(p, st.settings) == PlayMode::Gate ? kPPQ / 8 : kPPQ / 4;
            project.edit("Record", [&](ProjectState& s) { s.events.push_back(e); });
            pendingRec.push_back({ h.token, h.pad, e.id, h.tick });
        }
        else
        {
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

bool HPSamplerProcessor::exportWav(const juce::File& f, juce::String& error)
{
    std::array<const PadSound*, kNumPads> sounds {};
    for (int i = 0; i < kNumPads; ++i) sounds[(size_t) i] = engine.padSound(i);
    juce::AudioBuffer<float> out;
    Engine::renderOffline(*buildSnapshot(), sounds, engineRate, out, 2.0);
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

void HPSamplerProcessor::renderResult()
{
    if (project.state().events.empty()) { resultAudio.reset(); resultRevision = project.revision(); return; }
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
    const auto snap = BridgeAdapter::buildSamplerContext(project, session->getPublishedRevision() + 1, instanceId);
    const auto check = hpbridge::BridgeValidator::validate(snap);
    if (!check.ok) { setStatus("Context not sent: " + check.error); contextDirty = false; return; }
    const auto rev = session->publish(hpbridge::kSamplerContextSchemaV1, hpbridge::BridgeValidator::toJson(snap));
    contextDirty = false;
    bridgeStatus = "HP Bridge: " + session->getStatusText() + "  |  context rev " + juce::String(rev)
                   + (session->getPartnerAckRevision() > 0 ? "  (HPDG has rev " + juce::String(session->getPartnerAckRevision()) + ")" : juce::String());
    listeners.call([](Listener& l) { l.statusChanged(); });
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
