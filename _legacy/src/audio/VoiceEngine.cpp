#include "audio/VoiceEngine.h"

#include <algorithm>
#include <cmath>

namespace hps
{
namespace
{
inline float hermiteSample(const float* data, int64_t numFrames, double pos) noexcept
{
    const int64_t idx = static_cast<int64_t>(std::floor(pos));
    const float   t   = static_cast<float>(pos - static_cast<double>(idx));

    auto at = [&](int64_t i) noexcept -> float {
        if (i < 0 || i >= numFrames) return 0.0f;
        return data[i];
    };

    const float y0 = at(idx - 1);
    const float y1 = at(idx);
    const float y2 = at(idx + 1);
    const float y3 = at(idx + 2);

    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

inline bool isLaneAudible(const AudioRenderSnapshot& snap, int slot) noexcept
{
    if (slot < 0 || slot >= kNumVisibleSources) return true;
    bool anySolo = false;
    for (const auto& lm : snap.laneMix)
        if (lm.soloed) { anySolo = true; break; }
    if (anySolo) return snap.laneMix[static_cast<size_t>(slot)].soloed;
    return !snap.laneMix[static_cast<size_t>(slot)].muted;
}
} // namespace

VoiceEngine::VoiceEngine()
{
    activeSnapshot = std::make_shared<const AudioRenderSnapshot>();
}

void VoiceEngine::prepare(double sr, int maxBlock)
{
    sampleRate   = sr > 8000.0 ? sr : 44100.0;
    maxBlockSize = std::max(64, maxBlock);
    voiceScratch.setSize(2, maxBlockSize, false, true, true);
    for (auto& v : voices)
        v.padFilter.prepare(sampleRate, maxBlockSize, 2);
    masterFx.prepare(sampleRate, maxBlockSize, 2);
    reset();
}

void VoiceEngine::reset() noexcept
{
    for (auto& v : voices)
    {
        v.active = false;
        v.envStage = EnvStage::Idle;
        v.envLevel = 0.0f;
        v.padFilter.reset();
    }
    for (auto& p : pendingRecNotes) p.active = false;
    for (auto& s : scheduledOffs) s.active = false;
    masterFx.reset();
    activeVoiceCount.store(0, std::memory_order_relaxed);
    activePadMaskLow.store(0, std::memory_order_relaxed);
}

std::shared_ptr<const AudioRenderSnapshot> VoiceEngine::buildSnapshot(const SamplerProject& project)
{
    auto snap = std::make_shared<AudioRenderSnapshot>();
    const auto& st = project.state();

    snap->projectBpm         = st.projectBpm;
    snap->patternLengthTicks = project.getPatternLengthTicks();
    snap->swingAmount        = st.pattern.swingAmount;
    snap->hostSyncEnabled    = st.hostSyncEnabled;
    snap->laneMix            = st.laneMix;
    snap->masterFx           = st.masterFx;
    snap->revision           = st.revision;

    for (int i = 0; i < kNumVisibleSources; ++i)
    {
        if (const auto* src = project.getSourceBySlot(i))
        {
            snap->sourceAudio[static_cast<size_t>(i)]       = src->audio;
            snap->sourceSampleRates[static_cast<size_t>(i)] = src->sampleRate;
        }
    }

    for (int i = 0; i < kTotalPads; ++i)
    {
        const auto& padSlot = st.pads[static_cast<size_t>(i)];
        auto& dst = snap->pads[static_cast<size_t>(i)];
        dst.midiNote = padSlot.defaultMidiNote;

        if (!padSlot.assignment.has_value()) continue;
        const auto& a = *padSlot.assignment;
        const auto* slice = project.getSliceById(a.sliceId);
        const auto* src   = project.getSourceById(a.sourceId);
        if (slice == nullptr || src == nullptr || src->audio == nullptr || src->numFrames <= 0)
            continue;

        dst.assigned          = true;
        dst.sourceSlot        = src->visibleSlot;
        dst.audio             = src->audio;
        dst.sourceSampleRate  = src->sampleRate;
        dst.startFrame        = a.trimOverride.has_value() ? a.trimOverride->startFrame : slice->startFrame;
        dst.endFrameExclusive = a.trimOverride.has_value() ? a.trimOverride->endFrameExclusive : slice->endFrameExclusive;
        dst.startFrame        = juce::jlimit<int64_t>(0, src->numFrames - 1, dst.startFrame);
        dst.endFrameExclusive = juce::jlimit<int64_t>(dst.startFrame + 1, src->numFrames, dst.endFrameExclusive);

        dst.playbackMode    = a.settings.playbackModeOverride.value_or(st.globalPlayback);
        dst.overlapPolicy   = a.settings.overlapOverride.value_or(st.globalOverlap);
        dst.retriggerMode   = a.settings.retriggerMode;
        dst.chokeGroup      = a.settings.chokeGroup;
        dst.loop            = a.settings.loop;
        dst.reverse         = a.settings.reverse;
        dst.pitchSemitones  = a.settings.pitchSemitones;
        dst.fineCents       = a.settings.fineCents;
        dst.stretchRatio    = a.settings.stretchRatio;
        dst.syncMode        = a.settings.syncMode != SyncMode::Off ? a.settings.syncMode : st.globalSyncMode;
        dst.sourceBpm       = src->analysis.bpm.value_or(0.0);
        dst.attackMs        = a.settings.attackMs;
        dst.decayMs         = a.settings.decayMs;
        dst.sustainLevel    = a.settings.sustainLevel;
        dst.releaseMs       = a.settings.releaseMs;
        dst.fadeInMs        = a.settings.fadeInMs;
        dst.fadeOutMs       = a.settings.fadeOutMs;
        dst.gainLinear      = juce::Decibels::decibelsToGain(a.settings.gainDb);
        dst.pan             = a.settings.pan;
        dst.filterEnabled   = a.settings.filterEnabled;
        dst.filterType      = a.settings.filterType;
        dst.filterCutoffHz  = a.settings.filterCutoffHz;
        dst.filterResonance = a.settings.filterResonance;
        dst.filterMix       = a.settings.filterMix;
        dst.midiNote        = a.settings.midiNote;
        dst.maxVoices       = juce::jlimit(1, kMaxVoices, a.settings.maxVoices);
    }

    snap->patternEvents.reserve(st.pattern.events.size());
    for (const auto& ev : st.pattern.events)
    {
        const int flat = project.findPadFlatIndexById(ev.padId);
        if (flat < 0) continue;
        snap->patternEvents.push_back({
            flat,
            ev.startTick + ev.microOffsetTicks,
            ev.durationTicks,
            ev.velocity,
            ev.pitchOverrideSemitones.value_or(0)
        });
    }

    return snap;
}

void VoiceEngine::publishSnapshot(std::shared_ptr<const AudioRenderSnapshot> snapshot)
{
    if (snapshot != nullptr)
        std::atomic_store_explicit(&activeSnapshot, std::move(snapshot), std::memory_order_release);
}

bool VoiceEngine::isPadActive(int flatPadIndex) const noexcept
{
    if (flatPadIndex < 0 || flatPadIndex >= 64) return false;
    return (activePadMaskLow.load(std::memory_order_relaxed) & (uint64_t(1) << flatPadIndex)) != 0;
}

void VoiceEngine::quickFadeVoice(Voice& v) noexcept
{
    if (v.active && v.envStage != EnvStage::QuickFadeOut)
    {
        v.envStage      = EnvStage::QuickFadeOut;
        v.quickFadeRate = 1.0f / std::max(16.0f, static_cast<float>(sampleRate * 0.004)); // 4 ms de-click
    }
}

int VoiceEngine::allocateVoiceSlot() noexcept
{
    for (int i = 0; i < kMaxVoices; ++i)
        if (!voices[static_cast<size_t>(i)].active)
            return i;

    // Steal oldest releasing voice first, else oldest active voice
    int best = 0;
    uint64_t bestAge = UINT64_MAX;
    bool foundReleasing = false;

    for (int i = 0; i < kMaxVoices; ++i)
    {
        const auto& v = voices[static_cast<size_t>(i)];
        const bool isRel = (v.envStage == EnvStage::Release || v.envStage == EnvStage::QuickFadeOut);
        if (isRel && !foundReleasing)
        {
            foundReleasing = true;
            best = i;
            bestAge = v.age;
        }
        else if (isRel == foundReleasing && v.age < bestAge)
        {
            best = i;
            bestAge = v.age;
        }
    }
    return best;
}

void VoiceEngine::releaseVoicesMatching(int flatPadIndex, TriggerOrigin origin, uint32_t token) noexcept
{
    for (auto& v : voices)
    {
        if (!v.active) continue;
        if (v.flatPadIndex == flatPadIndex && v.origin == origin && v.token == token)
        {
            // OneShot & Toggle ignore NoteOff! Only Gate releases on NoteOff.
            if (v.playbackMode == PlaybackMode::Gate)
                v.envStage = EnvStage::Release;
        }
    }
}

void VoiceEngine::startVoice(const PadTriggerMessage& msg, const AudioRenderSnapshot& snap) noexcept
{
    // Direct source audition (previewing waveform range before assigning to pad)
    if (msg.origin == TriggerOrigin::SourceAudition)
    {
        if (msg.auditionSlot < 0 || msg.auditionSlot >= kNumVisibleSources) return;
        const auto& buf = snap.sourceAudio[static_cast<size_t>(msg.auditionSlot)];
        if (buf == nullptr || buf->getNumSamples() <= 0) return;

        // Stop any previous source audition voice
        for (auto& v : voices)
            if (v.active && v.origin == TriggerOrigin::SourceAudition)
                quickFadeVoice(v);

        const int slot = allocateVoiceSlot();
        auto& v = voices[static_cast<size_t>(slot)];
        v.active            = true;
        v.flatPadIndex      = -1;
        v.sourceSlot        = msg.auditionSlot;
        v.origin            = TriggerOrigin::SourceAudition;
        v.token             = msg.token;
        v.age               = voiceAgeCounter.fetch_add(1, std::memory_order_relaxed);
        v.playbackMode      = PlaybackMode::OneShot;
        v.chokeGroup        = 0;
        v.loop              = false;
        v.loopPassesRemaining = 1;
        v.reverse           = false;
        v.audio             = buf;
        v.startFrame        = juce::jlimit<int64_t>(0, buf->getNumSamples() - 1, msg.auditionStartFrame);
        v.endFrameExclusive = msg.auditionEndFrame > v.startFrame
                            ? juce::jlimit<int64_t>(v.startFrame + 1, buf->getNumSamples(), msg.auditionEndFrame)
                            : buf->getNumSamples();
        v.position          = static_cast<double>(v.startFrame);
        v.increment         = snap.sourceSampleRates[static_cast<size_t>(msg.auditionSlot)] / sampleRate;
        v.velocity          = 1.0f;
        v.gainL             = 0.85f;
        v.gainR             = 0.85f;
        v.envStage          = EnvStage::Sustain;
        v.envLevel          = 1.0f;
        v.sustainLevel      = 1.0f;
        v.releaseRate       = 1.0f / std::max(32.0f, static_cast<float>(sampleRate * 0.01));
        v.fadeInSamples     = static_cast<int>(sampleRate * 0.002);
        v.fadeOutSamples    = static_cast<int>(sampleRate * 0.005);
        v.samplesRendered   = 0;
        v.padFilter.setParameters(false, 0, 12000.0f, 0.707f, 1.0f);
        return;
    }

    if (msg.flatPadIndex < 0 || msg.flatPadIndex >= kTotalPads) return;
    const auto& pad = snap.pads[static_cast<size_t>(msg.flatPadIndex)];
    if (!pad.assigned || pad.audio == nullptr) return;
    if (!isLaneAudible(snap, pad.sourceSlot)) return;

    // TOGGLE mode: if an active voice for this pad exists, turn it off instead of starting a new one!
    if (pad.playbackMode == PlaybackMode::Toggle)
    {
        bool toggledOff = false;
        for (auto& v : voices)
        {
            if (v.active && v.flatPadIndex == msg.flatPadIndex && v.envStage != EnvStage::QuickFadeOut && v.envStage != EnvStage::Release)
            {
                quickFadeVoice(v);
                toggledOff = true;
            }
        }
        if (toggledOff) return;
    }

    // Apply explicit Overlap / Choke / Retrigger policy BEFORE allocating new voice
    for (auto& v : voices)
    {
        if (!v.active || v.origin == TriggerOrigin::SourceAudition) continue;

        if (pad.overlapPolicy == OverlapPolicy::GlobalMono)
        {
            quickFadeVoice(v);
        }
        else if (pad.overlapPolicy == OverlapPolicy::SourceMono && v.sourceSlot == pad.sourceSlot)
        {
            quickFadeVoice(v);
        }
        else if (pad.chokeGroup > 0 && v.chokeGroup == pad.chokeGroup)
        {
            quickFadeVoice(v);
        }
        else if (v.flatPadIndex == msg.flatPadIndex)
        {
            if (pad.overlapPolicy == OverlapPolicy::Retrigger
                || pad.overlapPolicy == OverlapPolicy::MonoPad
                || (pad.overlapPolicy == OverlapPolicy::ChokeGroup && pad.retriggerMode == RetriggerMode::Restart))
            {
                quickFadeVoice(v);
            }
        }
    }

    const int slot = allocateVoiceSlot();
    auto& v = voices[static_cast<size_t>(slot)];
    v.active            = true;
    v.flatPadIndex      = msg.flatPadIndex;
    v.sourceSlot        = pad.sourceSlot;
    v.origin            = msg.origin;
    v.token             = msg.token;
    v.age               = voiceAgeCounter.fetch_add(1, std::memory_order_relaxed);
    v.playbackMode      = pad.playbackMode;
    v.chokeGroup        = pad.chokeGroup;
    v.loop              = pad.loop;
    // Safe OneShot + Loop semantics: bounded to 2 passes so it never creates accidental infinite hanging voices.
    v.loopPassesRemaining = (pad.loop && pad.playbackMode == PlaybackMode::OneShot) ? 2 : 1;
    v.reverse           = pad.reverse;
    v.audio             = pad.audio;
    v.startFrame        = pad.startFrame;
    v.endFrameExclusive = pad.endFrameExclusive;
    v.position          = pad.reverse ? static_cast<double>(pad.endFrameExclusive - 1)
                                      : static_cast<double>(pad.startFrame);

    double rateRatio = pad.sourceSampleRate / sampleRate;
    const double totalSemis = static_cast<double>(pad.pitchSemitones + msg.pitchOffset) + static_cast<double>(pad.fineCents) / 100.0;
    if (pad.syncMode == SyncMode::Repitch && pad.sourceBpm > 20.0 && snap.projectBpm > 20.0)
        rateRatio *= (snap.projectBpm / pad.sourceBpm);
    rateRatio *= std::pow(2.0, totalSemis / 12.0);
    v.increment = std::max(0.05, std::min(8.0, rateRatio)) * (pad.reverse ? -1.0 : 1.0);

    const float laneGain = (pad.sourceSlot >= 0 && pad.sourceSlot < kNumVisibleSources)
                         ? juce::Decibels::decibelsToGain(snap.laneMix[static_cast<size_t>(pad.sourceSlot)].gainDb)
                         : 1.0f;
    const float panAngle = (juce::jlimit(-1.0f, 1.0f, pad.pan) + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    v.velocity = juce::jlimit(0.01f, 1.0f, msg.velocity);
    v.gainL    = pad.gainLinear * laneGain * v.velocity * std::cos(panAngle) * 1.41421356f;
    v.gainR    = pad.gainLinear * laneGain * v.velocity * std::sin(panAngle) * 1.41421356f;

    v.attackRate     = 1.0f / std::max(1.0f, static_cast<float>(sampleRate * (pad.attackMs * 0.001)));
    v.decayRate      = pad.decayMs > 0.5f ? (1.0f / std::max(1.0f, static_cast<float>(sampleRate * (pad.decayMs * 0.001)))) : 1.0f;
    v.sustainLevel   = juce::jlimit(0.0f, 1.0f, pad.sustainLevel);
    v.releaseRate    = 1.0f / std::max(16.0f, static_cast<float>(sampleRate * (pad.releaseMs * 0.001)));
    v.envStage       = pad.attackMs > 0.5f ? EnvStage::Attack : EnvStage::Sustain;
    v.envLevel       = pad.attackMs > 0.5f ? 0.0f : v.sustainLevel;
    v.fadeInSamples  = std::max(8, static_cast<int>(sampleRate * (pad.fadeInMs * 0.001)));
    v.fadeOutSamples = std::max(16, static_cast<int>(sampleRate * (pad.fadeOutMs * 0.001)));
    v.samplesRendered = 0;

    v.padFilter.reset();
    v.padFilter.setParameters(pad.filterEnabled, pad.filterType, pad.filterCutoffHz, pad.filterResonance, pad.filterMix);
    totalNoteOnCount.fetch_add(1, std::memory_order_relaxed);
}

void VoiceEngine::handleTrigger(const PadTriggerMessage& msg, const AudioRenderSnapshot& snap, int64_t currentTick) noexcept
{
    if (msg.isNoteOn)
    {
        startVoice(msg, snap);

        // Live Record: ONLY record GuiMouse & MidiNote (NEVER Sequencer playback -> zero feedback loop!)
        if (recording.load(std::memory_order_acquire)
            && playing.load(std::memory_order_acquire)
            && (msg.origin == TriggerOrigin::GuiMouse || msg.origin == TriggerOrigin::MidiNote)
            && msg.flatPadIndex >= 0 && msg.flatPadIndex < kTotalPads)
        {
            const auto& pad = snap.pads[static_cast<size_t>(msg.flatPadIndex)];
            const bool isGate = (pad.playbackMode == PlaybackMode::Gate);
            if (!isGate)
            {
                recordedQueue.push({ msg.flatPadIndex, currentTick, kTicksPerQuarter / 2, msg.velocity, false });
            }
            else
            {
                for (auto& pr : pendingRecNotes)
                {
                    if (!pr.active)
                    {
                        pr = { true, msg.flatPadIndex, msg.origin, msg.token, currentTick, msg.velocity, true };
                        break;
                    }
                }
            }
        }
    }
    else
    {
        releaseVoicesMatching(msg.flatPadIndex, msg.origin, msg.token);

        // Finalize pending Gate recording duration
        if (msg.origin == TriggerOrigin::GuiMouse || msg.origin == TriggerOrigin::MidiNote)
        {
            for (auto& pr : pendingRecNotes)
            {
                if (pr.active && pr.flatPadIndex == msg.flatPadIndex && pr.origin == msg.origin && pr.token == msg.token)
                {
                    int64_t dur = currentTick - pr.startTick;
                    if (dur <= 0) dur += std::max<int64_t>(kTicksPerQuarter / 4, snap.patternLengthTicks);
                    dur = juce::jlimit<int64_t>(60, std::max<int64_t>(60, snap.patternLengthTicks), dur);
                    recordedQueue.push({ pr.flatPadIndex, pr.startTick, dur, pr.velocity, true });
                    pr.active = false;
                    break;
                }
            }
        }
    }
}

void VoiceEngine::processBlock(juce::AudioBuffer<float>& audioOut,
                               juce::MidiBuffer& midiInOut,
                               bool hostIsPlaying,
                               double hostBpm,
                               double hostPpqPosition) noexcept
{
    const int numSamples  = audioOut.getNumSamples();
    const int numChannels = audioOut.getNumChannels();
    audioOut.clear();
    if (numSamples <= 0 || numChannels <= 0) return;

    auto snap = std::atomic_load_explicit(&activeSnapshot, std::memory_order_acquire);
    if (snap == nullptr) return;

    if (allNotesOffRequested.exchange(false, std::memory_order_acq_rel))
    {
        for (auto& v : voices) quickFadeVoice(v);
        for (auto& s : scheduledOffs) s.active = false;
    }

    const double effectiveBpm = (snap->hostSyncEnabled && hostBpm >= 20.0 && hostBpm <= 300.0)
                              ? hostBpm
                              : juce::jlimit(20.0, 300.0, snap->projectBpm);
    const double ticksPerSample = (effectiveBpm / 60.0) * static_cast<double>(kTicksPerQuarter) / sampleRate;
    const int64_t patternLen    = std::max<int64_t>(kTicksPerQuarter, snap->patternLengthTicks);

    const bool isTransportPlaying = (snap->hostSyncEnabled && hostIsPlaying) || playing.load(std::memory_order_acquire);
    double blockStartTick = playheadTick.load(std::memory_order_acquire);
    if (snap->hostSyncEnabled && hostIsPlaying && hostPpqPosition >= 0.0)
    {
        const double rawTicks = hostPpqPosition * static_cast<double>(kTicksPerQuarter);
        blockStartTick = std::fmod(rawTicks, static_cast<double>(patternLen));
        if (blockStartTick < 0.0) blockStartTick += static_cast<double>(patternLen);
    }

    const int64_t currentTickInt = static_cast<int64_t>(blockStartTick) % patternLen;

    // 1. Drain GUI trigger queue (immediate, zero debounce)
    PadTriggerMessage guiMsg;
    while (guiQueue.pop(guiMsg))
        handleTrigger(guiMsg, *snap, currentTickInt);

    // 2. Process incoming MIDI events (sample-accurate; NoteOn vel=0 -> NoteOff)
    const int bank = activeBank.load(std::memory_order_acquire);
    for (const auto metadata : midiInOut)
    {
        const auto m = metadata.getMessage();
        const int offset = juce::jlimit(0, numSamples - 1, metadata.samplePosition);
        const int64_t evTick = static_cast<int64_t>(blockStartTick + offset * ticksPerSample) % patternLen;

        if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices) quickFadeVoice(v);
            continue;
        }

        const bool isOn  = m.isNoteOn() && m.getVelocity() > 0;
        const bool isOff = m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0);
        if (!isOn && !isOff) continue;

        int matchedPad = -1;
        for (int p = 0; p < kPadsPerBank; ++p)
        {
            const int flat = bank * kPadsPerBank + p;
            if (snap->pads[static_cast<size_t>(flat)].midiNote == m.getNoteNumber())
            {
                matchedPad = flat;
                break;
            }
        }
        if (matchedPad < 0) continue;

        const uint32_t midiToken = (static_cast<uint32_t>(m.getChannel()) << 8) | static_cast<uint32_t>(m.getNoteNumber());
        PadTriggerMessage t;
        t.isNoteOn     = isOn;
        t.flatPadIndex = matchedPad;
        t.velocity     = isOn ? m.getFloatVelocity() : 0.0f;
        t.sampleOffset = offset;
        t.origin       = TriggerOrigin::MidiNote;
        t.token        = midiToken;
        handleTrigger(t, *snap, evTick);
    }

    // 3. Pattern Sequencer (sample-accurate loop scheduling)
    if (isTransportPlaying)
    {
        const double blockEndTick = blockStartTick + static_cast<double>(numSamples) * ticksPerSample;

        auto inWindow = [&](int64_t t) noexcept -> bool {
            const double td = static_cast<double>(t);
            if (blockEndTick < static_cast<double>(patternLen))
                return td >= blockStartTick && td < blockEndTick;
            // Loop wrap
            return (td >= blockStartTick && td < static_cast<double>(patternLen))
                || (td >= 0.0 && td < (blockEndTick - static_cast<double>(patternLen)));
        };

        // Process scheduled sequencer gate note-offs first (Off -> On ordering on identical tick)
        for (auto& so : scheduledOffs)
        {
            if (so.active && inWindow(so.offTick))
            {
                releaseVoicesMatching(so.flatPadIndex, TriggerOrigin::Sequencer, so.token);
                so.active = false;
            }
        }

        // Trigger pattern events in this block
        uint32_t seqIdx = 1;
        for (const auto& ev : snap->patternEvents)
        {
            int64_t evTick = ev.startTick % patternLen;
            if (evTick < 0) evTick += patternLen;

            // Apply 1/16th swing on odd 16th steps
            if (snap->swingAmount > 0.001f)
            {
                const int64_t step16 = kTicksPerQuarter / 4;
                if (((evTick / step16) & 1) == 1)
                    evTick = (evTick + static_cast<int64_t>(snap->swingAmount * step16 * 0.5f)) % patternLen;
            }

            if (inWindow(evTick))
            {
                PadTriggerMessage t;
                t.isNoteOn     = true;
                t.flatPadIndex = ev.flatPadIndex;
                t.velocity     = ev.velocity;
                t.origin       = TriggerOrigin::Sequencer;
                t.token        = 0x80000000u | seqIdx;
                t.pitchOffset  = ev.pitchOverride;
                handleTrigger(t, *snap, evTick);

                // Schedule Gate NoteOff if pad is Gate mode
                if (ev.flatPadIndex >= 0 && ev.flatPadIndex < kTotalPads
                    && snap->pads[static_cast<size_t>(ev.flatPadIndex)].playbackMode == PlaybackMode::Gate)
                {
                    const int64_t offT = (evTick + std::max<int64_t>(60, ev.durationTicks)) % patternLen;
                    for (auto& so : scheduledOffs)
                    {
                        if (!so.active)
                        {
                            so = { true, ev.flatPadIndex, t.token, offT };
                            break;
                        }
                    }
                }
            }
            ++seqIdx;
        }

        double nextTick = blockEndTick;
        if (looping.load(std::memory_order_acquire))
            nextTick = std::fmod(nextTick, static_cast<double>(patternLen));
        else if (nextTick >= static_cast<double>(patternLen))
        {
            nextTick = 0.0;
            playing.store(false, std::memory_order_release);
        }
        playheadTick.store(nextTick, std::memory_order_release);
    }

    // 4. Render active voices -> per-pad FilterEqLite -> sum into audioOut
    int activeCnt = 0;
    uint64_t padMask = 0;
    const int renderN = std::min(numSamples, voiceScratch.getNumSamples());

    for (auto& v : voices)
    {
        if (!v.active || v.audio == nullptr) continue;

        voiceScratch.clear(0, renderN);
        float* scratchL = voiceScratch.getWritePointer(0);
        float* scratchR = voiceScratch.getWritePointer(1);

        const int srcCh = v.audio->getNumChannels();
        const int64_t srcTotal = v.audio->getNumSamples();
        const float* srcL = v.audio->getReadPointer(0);
        const float* srcR = srcCh > 1 ? v.audio->getReadPointer(1) : srcL;

        for (int i = 0; i < renderN; ++i)
        {
            // Check boundaries / loop
            if ((!v.reverse && v.position >= static_cast<double>(v.endFrameExclusive))
                || (v.reverse && v.position < static_cast<double>(v.startFrame)))
            {
                const bool canLoop = v.loop && (v.playbackMode == PlaybackMode::Gate
                                                || v.playbackMode == PlaybackMode::Toggle
                                                || v.loopPassesRemaining > 1);
                if (canLoop && v.envStage != EnvStage::Release && v.envStage != EnvStage::QuickFadeOut)
                {
                    if (v.playbackMode == PlaybackMode::OneShot) --v.loopPassesRemaining;
                    v.position = v.reverse ? static_cast<double>(v.endFrameExclusive - 1)
                                           : static_cast<double>(v.startFrame);
                }
                else
                {
                    v.active = false;
                    v.envStage = EnvStage::Idle;
                    break;
                }
            }

            // Envelope update
            switch (v.envStage)
            {
                case EnvStage::Attack:
                    v.envLevel += v.attackRate;
                    if (v.envLevel >= 1.0f) { v.envLevel = 1.0f; v.envStage = EnvStage::Decay; }
                    break;
                case EnvStage::Decay:
                    if (v.envLevel > v.sustainLevel)
                    {
                        v.envLevel -= v.decayRate;
                        if (v.envLevel <= v.sustainLevel) { v.envLevel = v.sustainLevel; v.envStage = EnvStage::Sustain; }
                    }
                    else v.envStage = EnvStage::Sustain;
                    break;
                case EnvStage::Sustain:
                    break;
                case EnvStage::Release:
                    v.envLevel -= v.releaseRate;
                    if (v.envLevel <= 1.0e-5f) { v.envLevel = 0.0f; v.active = false; v.envStage = EnvStage::Idle; }
                    break;
                case EnvStage::QuickFadeOut:
                    v.envLevel -= v.quickFadeRate;
                    if (v.envLevel <= 1.0e-5f) { v.envLevel = 0.0f; v.active = false; v.envStage = EnvStage::Idle; }
                    break;
                case EnvStage::Idle:
                    v.active = false;
                    break;
            }
            if (!v.active) break;

            // Edge de-click fades
            float edgeFade = 1.0f;
            if (v.samplesRendered < v.fadeInSamples)
                edgeFade *= static_cast<float>(v.samplesRendered) / static_cast<float>(v.fadeInSamples);
            const double distToEnd = v.reverse ? (v.position - static_cast<double>(v.startFrame))
                                               : (static_cast<double>(v.endFrameExclusive) - v.position);
            if (!v.loop && distToEnd < static_cast<double>(v.fadeOutSamples))
                edgeFade *= juce::jlimit(0.0f, 1.0f, static_cast<float>(distToEnd / static_cast<double>(v.fadeOutSamples)));

            const float sL = hermiteSample(srcL, srcTotal, v.position);
            const float sR = hermiteSample(srcR, srcTotal, v.position);
            const float env = v.envLevel * edgeFade;

            scratchL[i] = sL * v.gainL * env;
            scratchR[i] = sR * v.gainR * env;

            v.position += v.increment;
            ++v.samplesRendered;
        }

        v.padFilter.process(voiceScratch, 0, renderN);

        audioOut.addFrom(0, 0, voiceScratch, 0, 0, renderN);
        if (numChannels > 1)
            audioOut.addFrom(1, 0, voiceScratch, 1, 0, renderN);

        if (v.active)
        {
            ++activeCnt;
            if (v.flatPadIndex >= 0 && v.flatPadIndex < 64)
                padMask |= (uint64_t(1) << v.flatPadIndex);
        }
    }

    activeVoiceCount.store(activeCnt, std::memory_order_relaxed);
    activePadMaskLow.store(padMask, std::memory_order_relaxed);

    // 5. Master Gain + WarmTubeCompLite + Safety Limiter
    const float masterGain = juce::Decibels::decibelsToGain(snap->masterFx.masterGainDb);
    audioOut.applyGain(masterGain);
    masterFx.setParameters(snap->masterFx.warmTubeCompEnabled,
                           snap->masterFx.driveDb,
                           snap->masterFx.compMix,
                           snap->masterFx.limiterEnabled);
    masterFx.process(audioOut, 0, numSamples);
}

juce::AudioBuffer<float> VoiceEngine::renderPatternOffline(const SamplerProject& project, double sr)
{
    const auto& st = project.state();
    const int64_t lenTicks = project.getPatternLengthTicks();
    const double bpm = juce::jlimit(20.0, 300.0, st.projectBpm);
    const double durationSec = (static_cast<double>(lenTicks) / static_cast<double>(kTicksPerQuarter)) * (60.0 / bpm);
    const int totalSamples = std::max(512, static_cast<int>(std::ceil(durationSec * sr)));

    juce::AudioBuffer<float> result(2, totalSamples);
    result.clear();

    if (st.pattern.events.empty())
        return result;

    VoiceEngine offline;
    offline.prepare(sr, 512);
    auto snap = buildSnapshot(project);
    auto mutSnap = std::make_shared<AudioRenderSnapshot>(*snap);
    mutSnap->hostSyncEnabled = false;
    offline.publishSnapshot(mutSnap);
    offline.setLooping(false);
    offline.seekToTick(0);
    offline.setPlaying(true);

    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer emptyMidi;
    int pos = 0;
    while (pos < totalSamples)
    {
        const int n = std::min(512, totalSamples - pos);
        block.setSize(2, n, false, false, true);
        offline.processBlock(block, emptyMidi, false, bpm, -1.0);
        for (int ch = 0; ch < 2; ++ch)
            result.copyFrom(ch, pos, block, ch, 0, n);
        pos += n;
    }

    return result;
}

bool VoiceEngine::exportWav(const juce::File& outFile, const juce::AudioBuffer<float>& buffer, double sr)
{
    outFile.getParentDirectory().createDirectory();
    if (outFile.existsAsFile()) outFile.deleteFile();
    auto stream = outFile.createOutputStream();
    if (stream == nullptr) return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), sr, static_cast<unsigned int>(buffer.getNumChannels()), 24, {}, 0));
    if (writer == nullptr) return false;
    stream.release(); // writer owns stream
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

bool VoiceEngine::exportMidiTriggers(const juce::File& outFile, const SamplerProject& project)
{
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(kTicksPerQuarter);

    juce::MidiMessageSequence seq;
    const auto& st = project.state();
    const double usPerQuarter = 60000000.0 / juce::jlimit(20.0, 300.0, st.projectBpm);
    seq.addEvent(juce::MidiMessage::tempoMetaEvent(static_cast<int>(std::llround(usPerQuarter))), 0.0);

    for (const auto& ev : st.pattern.events)
    {
        const auto* pad = project.getPadById(ev.padId);
        if (pad == nullptr) continue;
        const int note = pad->assignment.has_value() ? pad->assignment->settings.midiNote : pad->defaultMidiNote;
        const auto vel = static_cast<uint8_t>(juce::jlimit(1, 127, static_cast<int>(std::llround(ev.velocity * 127.0f))));
        const double startT = static_cast<double>(std::max<int64_t>(0, ev.startTick + ev.microOffsetTicks));
        const double durT   = static_cast<double>(std::max<int64_t>(60, ev.durationTicks));

        seq.addEvent(juce::MidiMessage::noteOn(1, note, vel), startT);
        seq.addEvent(juce::MidiMessage::noteOff(1, note), startT + durT);
    }
    seq.updateMatchedPairs();
    midiFile.addTrack(seq);

    outFile.getParentDirectory().createDirectory();
    if (outFile.existsAsFile()) outFile.deleteFile();
    auto stream = outFile.createOutputStream();
    if (stream == nullptr) return false;
    return midiFile.writeTo(*stream);
}
} // namespace hps
