#include "audio/Engine.h"

#include <algorithm>

namespace hps
{
namespace
{
inline void svfCoeffs(float fc, double sr, float& g, float& k)
{
    fc = juce::jlimit(10.0f, (float) (sr * 0.45), fc);
    g = std::tan(juce::MathConstants<float>::pi * fc / (float) sr);
    k = 1.41421356f; // Q = 0.707
}

// TPT state-variable filter step; returns (lp, hp)
inline void svfTick(float x, float g, float k, float& ic1, float& ic2, float& lp, float& hp)
{
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + g * a1 * v3;
    const float v2 = ic2 + g * v1;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    lp = v2;
    hp = x - k * v1 - v2;
}
} // namespace

Engine::Engine()
{
    for (auto& h : padHits) h.store(0);
    for (auto& v : padVoices) v.store(0);
}

void Engine::publishPadSound(int pad, std::unique_ptr<PadSound> s)
{
    if (pad < 0 || pad >= kNumPads) return;
    sounds[(size_t) pad].publish(std::move(s));
}

void Engine::publishPreviewSource(int slot, std::unique_ptr<PreviewSource> p)
{
    if (slot < 0 || slot >= kNumSources) return;
    previews[(size_t) slot].publish(std::move(p));
}

void Engine::collectGarbage()
{
    snapshot.collect();
    for (auto& s : sounds) s.collect([](const PadSound& p) { return p.users.load() == 0; });
    for (auto& p : previews) p.collect();
}

void Engine::pushCmd(const Cmd& c)
{
    const juce::ScopedLock sl(producerLock);
    int s1, n1, s2, n2;
    cmdFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 > 0) cmdBuf[(size_t) s1] = c;
    else if (n2 > 0) cmdBuf[(size_t) s2] = c;
    cmdFifo.finishedWrite(n1 + n2);
}

void Engine::padDown(int pad, float velocity, int pointer)
{
    Cmd c; c.type = Cmd::PadOn; c.pad = pad; c.vel = velocity; c.token = kGuiToken | (uint32_t) (pointer & 0xff);
    pushCmd(c);
}

void Engine::padUp(int pad, int pointer)
{
    Cmd c; c.type = Cmd::PadOff; c.pad = pad; c.token = kGuiToken | (uint32_t) (pointer & 0xff);
    pushCmd(c);
}

void Engine::previewRange(int slot, int64_t start, int64_t end, bool loop)
{
    Cmd c; c.type = Cmd::Preview; c.slot = slot; c.a = start; c.b = end; c.flag = loop;
    pushCmd(c);
}

void Engine::stopPreview() { Cmd c; c.type = Cmd::PreviewStop; pushCmd(c); }

void Engine::setInternalPlaying(bool shouldPlay, bool fromStart)
{
    Cmd c; c.type = shouldPlay ? Cmd::Play : Cmd::Stop; c.flag = fromStart;
    pushCmd(c);
}

void Engine::playFrom(int64_t loopTick)
{
    Cmd c; c.type = Cmd::Play; c.flag = true; c.a = std::max<int64_t>(0, loopTick);
    pushCmd(c);
}

void Engine::playWithCountIn()
{
    Cmd c; c.type = Cmd::Play; c.flag = true; c.a = -(int64_t) kTicksPerBar;
    pushCmd(c);
}

void Engine::stopAll() { Cmd c; c.type = Cmd::StopAll; pushCmd(c); }

void Engine::panic() { Cmd c; c.type = Cmd::Panic; pushCmd(c); }

bool Engine::popRecordedHit(RecordedHit& out)
{
    int s1, n1, s2, n2;
    recFifo.prepareToRead(1, s1, n1, s2, n2);
    if (n1 + n2 == 0) return false;
    out = n1 > 0 ? recBuf[(size_t) s1] : recBuf[(size_t) s2];
    recFifo.finishedRead(n1 + n2);
    return true;
}

void Engine::prepare(double sampleRate, int)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;
    for (auto& v : voices) if (v.active) freeVoice(v);
    for (auto& o : offs) o.active = false;
}

void Engine::freeVoice(Voice& v)
{
    if (!v.active) return;
    if (v.sound) v.sound->users.fetch_sub(1);
    padVoices[(size_t) v.pad].fetch_sub(1);
    v.active = false;
    v.sound = nullptr;
}

void Engine::fadeVoice(Voice& v, float seconds)
{
    if (!v.active || v.stage == Stage::Fade) return;
    v.stage = Stage::Fade;
    v.fadeStep = std::max(v.env / (float) (seconds * sr), 1.0e-5f);
}

void Engine::releaseVoice(Voice& v, const EnginePad& p)
{
    if (!v.active || v.stage == Stage::Release || v.stage == Stage::Fade) return;
    v.stage = Stage::Release;
    v.relStep = std::max(v.env / (float) (std::max(0.001f, p.releaseS) * sr), 1.0e-6f);
}

void Engine::noteOn(const EngineSnapshot& s, int pad, float vel, uint32_t token, bool live)
{
    juce::ignoreUnused(live);
    if (pad < 0 || pad >= kNumPads) return;
    const auto& p = s.pads[(size_t) pad];
    const PadSound* snd = sounds[(size_t) pad].get();
    if (!p.assigned || snd == nullptr || snd->audio.getNumSamples() < 2) return;
    padHits[(size_t) pad].fetch_add(1);

    if (p.mode == PlayMode::Toggle)
    {
        bool stopped = false;
        for (auto& v : voices)
            if (v.active && v.pad == pad && v.toggle && v.stage != Stage::Release && v.stage != Stage::Fade)
            {
                releaseVoice(v, p);
                stopped = true;
            }
        if (stopped) return;
    }

    // overlap / cut policies (explicit; nothing cuts other pads by default)
    int sameLayer = 0;
    for (auto& v : voices)
    {
        if (!v.active || v.stage == Stage::Fade) continue;
        bool cut = false;
        if (s.mono == MonoMode::All) cut = true;
        else if (s.mono == MonoMode::PerSource && v.src == p.src) cut = true;
        if (p.choke > 0 && v.pad != pad && s.pads[(size_t) v.pad].choke == p.choke) cut = true;
        if (v.pad == pad && p.repeat == RepeatMode::Retrigger) cut = true;
        if (cut) fadeVoice(v, 0.005f);
        else if (v.pad == pad) ++sameLayer;
    }
    if (sameLayer >= 8) // layer limit per pad: drop the oldest
    {
        Voice* oldest = nullptr;
        for (auto& v : voices)
            if (v.active && v.pad == pad && v.stage != Stage::Fade && (!oldest || v.order < oldest->order)) oldest = &v;
        if (oldest) fadeVoice(*oldest, 0.005f);
    }

    Voice* slot = nullptr;
    for (auto& v : voices) if (!v.active) { slot = &v; break; }
    if (slot == nullptr)
    {
        for (auto& v : voices) if (v.stage == Stage::Fade && (!slot || v.order < slot->order)) slot = &v;
        if (slot == nullptr) for (auto& v : voices) if (!slot || v.order < slot->order) slot = &v;
        freeVoice(*slot);
    }
    Voice& v = *slot;
    v = Voice {};
    v.active = true;
    v.pad = pad;
    v.src = p.src;
    v.sound = snd;
    snd->users.fetch_add(1);
    padVoices[(size_t) pad].fetch_add(1);
    v.pos = 0.0;
    v.vel = juce::jlimit(0.0f, 1.0f, vel);
    v.token = token;
    v.order = voiceOrder++;
    v.loop = p.loop && p.mode != PlayMode::OneShot; // OneShot + Loop = single pass (never infinite)
    v.toggle = p.mode == PlayMode::Toggle;
    if (p.attackS <= 0.0005f) { v.env = 1.0f; v.stage = p.decayS > 0.0f ? Stage::Decay : Stage::Sustain; }
    else { v.env = 0.0f; v.stage = Stage::Attack; }
    // filter coefficients are set per block in renderVoices (pad + source filters, live knobs)
}

void Engine::noteOff(const EngineSnapshot& s, int pad, uint32_t token)
{
    if (pad < 0 || pad >= kNumPads) return;
    const auto& p = s.pads[(size_t) pad];
    if (p.mode != PlayMode::Gate) return; // OneShot plays to the end, Toggle ignores NoteOff
    for (auto& v : voices)
        if (v.active && v.pad == pad && v.token == token) releaseVoice(v, p);
}

void Engine::renderVoices(float* L, float* R, int n, const EngineSnapshot& s)
{
    for (auto& v : voices)
    {
        if (!v.active) continue;
        const auto& p = s.pads[(size_t) v.pad];
        const auto& a = v.sound->audio;
        const int len = a.getNumSamples();
        const float* inL = a.getReadPointer(0);
        const float* inR = a.getReadPointer(a.getNumChannels() > 1 ? 1 : 0);
        const int fadeIn = v.sound->fadeIn, fadeOut = v.sound->fadeOut;
        const float atk = 1.0f / (float) (std::max(0.0005f, p.attackS) * sr);
        const float dec = p.decayS > 0.0f ? (1.0f - p.sustain) / (float) (p.decayS * sr) : 1.0f;
        const auto& m = s.sources[(size_t) juce::jlimit(0, kNumSources - 1, v.src)];
        const float gl = p.gainL * m.gainL * v.vel, gr = p.gainR * m.gainR * v.vel;
        const float hpHz = std::max(p.hpHz, m.hpHz), lpHz = std::min(p.lpHz, m.lpHz);
        v.useHp = hpHz > 25.0f;
        v.useLp = lpHz < 19000.0f;
        if (v.useHp) svfCoeffs(hpHz, sr, v.hpG, v.hpK);
        if (v.useLp) svfCoeffs(lpHz, sr, v.lpG, v.lpK);
        for (int i = 0; i < n; ++i)
        {
            int ip = (int) v.pos;
            if (ip >= len)
            {
                if (v.loop && v.stage != Stage::Release && v.stage != Stage::Fade) { v.pos = 0.0; ip = 0; }
                else { freeVoice(v); break; }
            }
            switch (v.stage)
            {
                case Stage::Attack:  v.env += atk; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = p.decayS > 0.0f ? Stage::Decay : Stage::Sustain; } break;
                case Stage::Decay:   v.env -= dec; if (v.env <= p.sustain) { v.env = p.sustain; v.stage = Stage::Sustain; } break;
                case Stage::Sustain: break;
                case Stage::Release: v.env -= v.relStep; break;
                case Stage::Fade:    v.env -= v.fadeStep; break;
            }
            if (v.env <= 0.0f && (v.stage == Stage::Release || v.stage == Stage::Fade)) { freeVoice(v); break; }

            float xl = inL[ip], xr = inR[ip];
            if (ip < fadeIn) { const float f = (float) ip / (float) fadeIn; xl *= f; xr *= f; }
            if (len - ip <= fadeOut) { const float f = (float) (len - ip - 1) / (float) fadeOut; xl *= f; xr *= f; }
            if (v.useHp)
            {
                float lp, hp;
                svfTick(xl, v.hpG, v.hpK, v.hp[0].ic1, v.hp[0].ic2, lp, hp); xl = hp;
                svfTick(xr, v.hpG, v.hpK, v.hp[1].ic1, v.hp[1].ic2, lp, hp); xr = hp;
            }
            if (v.useLp)
            {
                float lp, hp;
                svfTick(xl, v.lpG, v.lpK, v.lp[0].ic1, v.lp[0].ic2, lp, hp); xl = lp;
                svfTick(xr, v.lpG, v.lpK, v.lp[1].ic1, v.lp[1].ic2, lp, hp); xr = lp;
            }
            L[i] += xl * v.env * gl;
            R[i] += xr * v.env * gr;
            v.pos += 1.0;
        }
    }
}

void Engine::renderPreview(float* L, float* R, int n)
{
    if (!previewActive) return;
    const PreviewSource* ps = previews[(size_t) previewSrc].get();
    if (ps == nullptr || ps->audio == nullptr) { previewActive = false; previewFrame.store(-1.0); return; }
    const auto& a = *ps->audio;
    const int len = a.getNumSamples();
    const double rate = ps->sampleRate / sr / ps->ratio; // source frames per output sample
    const float* inL = a.getReadPointer(0);
    const float* inR = a.getReadPointer(a.getNumChannels() > 1 ? 1 : 0);
    const float rampStep = 1.0f / (float) (0.004 * sr);
    for (int i = 0; i < n; ++i)
    {
        if (previewStopping) { previewGain -= rampStep; if (previewGain <= 0.0f) { previewActive = false; previewFrame.store(-1.0); return; } }
        else if (previewGain < 1.0f) previewGain = std::min(1.0f, previewGain + rampStep);
        if (previewPos >= previewEnd - 1.0 || previewPos * ps->ratio >= len - 1)
        {
            if (previewLoop) previewPos = previewStart;
            else { previewStopping = true; previewPos = std::max(0.0, std::min(previewPos, ((double) len - 2) / ps->ratio)); }
        }
        // fade the last 3 ms of a non-looping audition
        float edge = 1.0f;
        if (!previewLoop)
        {
            const double remain = (previewEnd - previewPos) * ps->ratio / ps->sampleRate;
            if (remain < 0.003) edge = (float) juce::jmax(0.0, remain / 0.003);
        }
        const double bp = previewPos * ps->ratio;
        const int ip = std::min((int) bp, len - 1);
        const float fr = (float) (bp - ip);
        const int ip1 = std::min(ip + 1, len - 1);
        L[i] += (inL[ip] + fr * (inL[ip1] - inL[ip])) * previewGain * edge;
        R[i] += (inR[ip] + fr * (inR[ip1] - inR[ip])) * previewGain * edge;
        previewPos += rate;
    }
    previewFrame.store(previewPos);
}

void Engine::renderClick(float* L, float* R, int n)
{
    if (clickEnv < 1.0e-4f) return;
    const float inc = juce::MathConstants<float>::twoPi * clickFreq / (float) sr;
    for (int i = 0; i < n; ++i)
    {
        clickSeed = clickSeed * 1664525u + 1013904223u;
        const float noise = (float) (int32_t) clickSeed / 2147483648.0f;
        float x = 0.0f;
        switch (clickSound)
        {
            case MetroSound::Click:   x = std::sin(clickPhase); break;
            case MetroSound::HiHat:   // noise through two high-pass steps
            {
                clickHp = 0.55f * (clickHp + noise - clickPrev);
                clickPrev = noise;
                x = clickHp * 1.5f;
                break;
            }
            case MetroSound::Rim:     x = 0.75f * std::sin(clickPhase) + 0.35f * noise * clickEnv; break;
            case MetroSound::Cowbell: x = 0.4f * ((std::sin(clickPhase) > 0.0f ? 1.0f : -1.0f) + (std::sin(clickPhase * 1.504f) > 0.0f ? 1.0f : -1.0f)); break;
        }
        x *= clickEnv * clickGain;
        clickPhase += inc;
        clickEnv *= clickDecay;
        L[i] += x; R[i] += x;
    }
}

void Engine::startClick(bool accent, const EngineSnapshot& s)
{
    struct Def { float lo, hi, ms, accentMs, gain; };
    static constexpr Def defs[] = { { 1000.0f, 1500.0f, 14.0f, 14.0f, 0.5f }, { 0.0f, 0.0f, 22.0f, 55.0f, 0.6f },
                                    { 1650.0f, 2100.0f, 9.0f, 12.0f, 0.6f }, { 545.0f, 680.0f, 55.0f, 75.0f, 0.4f } };
    const auto& d = defs[juce::jlimit(0, 3, (int) s.metroSound)];
    clickSound = s.metroSound;
    clickFreq = accent ? d.hi : d.lo;
    clickDecay = std::exp(-1.0f / (float) ((accent ? d.accentMs : d.ms) * 0.001 * sr));
    clickGain = d.gain * s.metroGain * (accent ? 1.0f : 0.75f);
    clickEnv = 1.0f;
    clickPhase = 0.0f;
}

void Engine::pushMark(int pad, int offset)
{
    const PreviewSource* ps = previews[(size_t) previewSrc].get();
    if (ps == nullptr) return;
    RecordedHit h;
    h.type = RecordedHit::Mark;
    h.pad = pad;
    h.slot = previewSrc;
    h.frame = previewPos + offset * ps->sampleRate / sr / ps->ratio;
    int s1, n1, s2, n2;
    recFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 > 0) recBuf[(size_t) s1] = h; else if (n2 > 0) recBuf[(size_t) s2] = h;
    recFifo.finishedWrite(n1 + n2);
}

void Engine::pushHit(RecordedHit::Type type, const Ev& e, bool rec)
{
    const double absTick = blockStartTick + e.offset * ticksPerSample;
    if (absTick < -(double) (kPPQ / 4)) return;      // count-in: free playing
    const int64_t tick = absTick < 0.0 ? 0 : currentLoopTick(e.offset); // a hit just before the "one" belongs to it
    RecordedHit h { type, e.pad, tick, e.vel, e.token, rec, hostDriving, pass };
    if (rec && type == RecordedHit::On)
    {
        recents[(size_t) recentPos] = { e.pad, h.tick, absTick };
        recentPos = (recentPos + 1) % (int) recents.size();
    }
    int s1, n1, s2, n2;
    recFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 > 0) recBuf[(size_t) s1] = h; else if (n2 > 0) recBuf[(size_t) s2] = h;
    recFifo.finishedWrite(n1 + n2);
}

int64_t Engine::currentLoopTick(int offset) const noexcept
{
    const double t = blockStartTick + offset * ticksPerSample;
    const double L = (double) std::max<int64_t>(1, loopTicks);
    double m = std::fmod(t, L);
    if (m < 0) m += L;
    return (int64_t) m;
}

void Engine::process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, const HostTransport& host)
{
    const int n = out.getNumSamples();
    out.clear();
    if (out.getNumChannels() < 1 || n <= 0) return;
    const EngineSnapshot* snap = snapshot.get();
    static const EngineSnapshot empty {};
    const EngineSnapshot& s = snap != nullptr ? *snap : empty;
    int nev = 0;
    auto addEv = [&](int off, uint8_t kind, int pad, float vel, uint32_t token)
    {
        if (nev < (int) evs.size()) evs[(size_t) nev++] = { juce::jlimit(0, n - 1, off), kind, pad, vel, token };
    };

    // One thing at a time (as in a hardware sampler): a hand-played pad ends the audition, an audition ends hand-played
    // pads and MAIN, MAIN ends both. The DAW is above all of them: when it starts it takes the transport over, and
    // nothing in the plug-in can start MAIN next to it.
    hostDriving = host.valid && host.playing;
    auto fadeTokens = [&](uint32_t mask) { for (auto& v : voices) if (v.active && (v.token & mask) != 0) fadeVoice(v, 0.008f); };
    const uint32_t handTokens = kGuiToken | (hostDriving ? 0u : kMidiToken);
    auto dropPendingOns = [&] { for (int i = 0; i < nev; ++i) if (evs[(size_t) i].kind == 1) evs[(size_t) i].kind = 0; };

    // ---- commands from the GUI ----
    {
        int s1, n1, s2, n2;
        cmdFifo.prepareToRead(cmdFifo.getNumReady(), s1, n1, s2, n2);
        auto handle = [&](const Cmd& c)
        {
            switch (c.type)
            {
                case Cmd::PadOn:
                    if (c.pad >= 0 && c.pad < kNumPads)
                    {
                        if (s.pads[(size_t) c.pad].assigned) stopPreviewNow();
                        else if (previewActive && !previewStopping && (c.token & (uint32_t) kAuditionPointer) == 0) pushMark(c.pad, 0);
                    }
                    addEv(0, 1, c.pad, c.vel, c.token); break;
                case Cmd::PadOff: addEv(0, 0, c.pad, 0.0f, c.token); break;
                case Cmd::Preview:
                    previewActive = true; previewStopping = false; previewSrc = juce::jlimit(0, kNumSources - 1, c.slot);
                    previewStart = (double) c.a; previewEnd = (double) std::max(c.a + 2, c.b); previewPos = previewStart;
                    previewLoop = c.flag; previewGain = 0.0f; previewSlot.store(previewSrc);
                    previewFrame.store(previewStart);
                    if (internalPlaying && !hostDriving) { internalPlaying = false; fadeTokens(kSeqToken); }
                    fadeTokens(handTokens);
                    dropPendingOns(); // a pad and an audition in the same block: the later command wins
                    break;
                case Cmd::PreviewStop: stopPreviewNow(); break;
                case Cmd::Play:
                    if (hostDriving) break; // the DAW leads
                    fadeTokens(kSeqToken | handTokens);
                    for (auto& o : offs) o.active = false;
                    stopPreviewNow();
                    internalPlaying = true;
                    if (c.flag) internalTick = (double) c.a;
                    lastEndTick = internalTick;
                    break;
                case Cmd::Stop:
                    internalPlaying = false; internalTick = 0.0;
                    if (!hostDriving) fadeTokens(kSeqToken);
                    break;
                case Cmd::StopAll:
                    internalPlaying = false; internalTick = 0.0;
                    stopPreviewNow();
                    fadeTokens(handTokens | (hostDriving ? 0u : kSeqToken));
                    dropPendingOns();
                    break;
                case Cmd::Panic:
                    for (auto& v : voices) fadeVoice(v, 0.004f);
                    for (auto& o : offs) o.active = false;
                    stopPreviewNow();
                    break;
            }
        };
        for (int i = 0; i < n1; ++i) handle(cmdBuf[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) handle(cmdBuf[(size_t) (s2 + i)]);
        cmdFifo.finishedRead(n1 + n2);
    }

    // ---- live MIDI (sample-accurate) ----
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        const int off = meta.samplePosition;
        if (m.isNoteOn())
        {
            const int pad = m.getNoteNumber() - kBaseMidiNote;
            if (pad < 0 || pad >= kNumPads) continue;
            if (hostDriving) hostMidiNotes.fetch_add(1);
            else if (s.pads[(size_t) pad].assigned) stopPreviewNow();
            else if (previewActive && !previewStopping) pushMark(pad, off); // slicing while listening
            addEv(off, 1, pad, m.getFloatVelocity(), kMidiToken | ((uint32_t) m.getChannel() << 8) | (uint32_t) m.getNoteNumber());
        }
        else if (m.isNoteOff()) // includes NoteOn velocity 0
        {
            const int pad = m.getNoteNumber() - kBaseMidiNote;
            addEv(off, 0, pad, 0.0f, kMidiToken | ((uint32_t) m.getChannel() << 8) | (uint32_t) m.getNoteNumber());
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices) fadeVoice(v, 0.004f);
        }
    }

    // ---- transport ----
    bool playing = false;
    double bpm = s.bpm > 20.0 ? s.bpm : 120.0;
    double startTick = 0.0;
    if (hostDriving)
    {
        if (!wasHostDriving) // the DAW starts: it takes over from MAIN / audition
        {
            if (internalPlaying) { internalPlaying = false; fadeTokens(kSeqToken); }
            internalTick = 0.0;
            stopPreviewNow();
        }
        playing = true;
        bpm = host.bpm > 20.0 ? host.bpm : bpm;
        startTick = host.ppq * kPPQ;
    }
    else if (internalPlaying)
    {
        playing = true;
        startTick = internalTick;
    }
    // DAW stopped: everything it played stops (an offline render keeps its tails)
    if (!hostDriving && wasHostDriving && !host.offline) fadeTokens(kSeqToken | kMidiToken);
    hostDriven.store(hostDriving);
    hostRecording.store(hostDriving && host.recording);
    hostBpm.store(host.valid ? host.bpm : 0.0);
    ticksPerSample = bpm / 60.0 * kPPQ / sr;
    loopTicks = std::max<int64_t>(kPPQ, s.loopTicks);
    blockStartTick = startTick;
    const double endTick = startTick + n * ticksPerSample;

    auto releaseSequenced = [&]
    {
        for (auto& v : voices)
            if (v.active && (v.token & kSeqToken) != 0 && s.pads[(size_t) v.pad].mode == PlayMode::Gate)
                releaseVoice(v, s.pads[(size_t) v.pad]);
        for (auto& o : offs) o.active = false;
    };
    const bool jumped = playing && wasPlaying && std::abs(startTick - lastEndTick) > 4.0;
    if ((!playing && wasPlaying) || jumped || hostDriving != wasHostDriving) releaseSequenced();
    if (playing && (!wasPlaying || hostDriving != wasHostDriving)) ++pass;
    if (!playing || !wasPlaying || jumped) for (auto& x : recents) x.pad = -1;

    const bool rec = playing && (recording.load()
                     || (hostDriving && (s.hostCapture == HostCapture::Always
                                         || (s.hostCapture == HostCapture::WhenRecording && host.recording))));
    // Closed window: the DAW's Play does not start MAIN (DAW notes still play the pads). An export keeps what was before it.
    if (!host.offline) windowLatched = windowOpen.load();
    const bool gated = hostDriving && s.mainNeedsWindow && !windowLatched;
    if (gated && !wasGated) // the window was closed while MAIN played
    {
        fadeTokens(kSeqToken);
        for (auto& o : offs) o.active = false;
    }
    wasGated = gated;
    const bool patternAudible = playing && !gated && !(hostDriving && s.hostPlays == HostPlays::DawNotes);
    // the DAW sends a note that MAIN plays anyway (recorded in both): one sound, MAIN's
    auto playedByMain = [&](int pad, int64_t tick)
    {
        for (const auto& e : s.events)
        {
            if (e.pad != pad) continue;
            for (const int64_t t : { e.tick, e.rawTick })
            {
                if (t < 0) continue;
                const int64_t d = std::abs(t - tick);
                if (std::min(d, loopTicks - d) <= kSameHitTicks) return true;
            }
        }
        return false;
    };

    if (playing)
    {
        // scheduled note-offs
        for (auto& o : offs)
            if (o.active && o.tick < endTick)
            {
                addEv((int) ((o.tick - startTick) / ticksPerSample), 0, o.pad, 0.0f, o.token);
                o.active = false;
            }
        // pattern events (loop of loopTicks)
        const double L = (double) loopTicks;
        const int64_t k0 = (int64_t) std::floor(startTick / L), k1 = (int64_t) std::floor((endTick - 1e-9) / L);
        for (int64_t k = k0; patternAudible && k <= k1; ++k)
        {
            if (k < 0) continue; // count-in
            const double base = (double) k * L;
            for (size_t ei = 0; ei < s.events.size(); ++ei)
            {
                const auto& e = s.events[ei];
                double t = base + (double) e.tick;
                const int64_t inSixteenth = e.tick % (kPPQ / 2);
                if (s.swing > 0.0f && std::abs(inSixteenth - kPPQ / 4) <= 8) t += s.swing * (kPPQ / 6.0); // off-beat 1/16 swing
                if (t < startTick || t >= endTick) continue;
                if (e.rawTick >= 0) // just recorded and quantized forward: it was already heard
                {
                    bool heard = false;
                    for (const auto& x : recents)
                        heard = heard || (x.pad == e.pad && x.loopTick == e.rawTick && std::abs(t - x.absTick) < L * 0.5);
                    if (heard) continue;
                }
                const int off = (int) ((t - startTick) / ticksPerSample);
                const uint32_t token = kSeqToken | (uint32_t) (ei & 0xffff);
                addEv(off, 1, e.pad, e.velocity, token);
                if (e.pad >= 0 && e.pad < kNumPads && s.pads[(size_t) e.pad].mode == PlayMode::Gate)
                {
                    const double offTick = t + (double) std::max<int64_t>(1, e.length);
                    if (offTick < endTick) addEv((int) ((offTick - startTick) / ticksPerSample), 0, e.pad, 0.0f, token);
                    else
                        for (auto& o : offs) if (!o.active) { o = { true, e.pad, token, offTick }; break; }
                }
            }
        }
        // metronome clicks on beats
        const bool counting = !hostDriving && startTick < 0.0;
        if ((s.metronome && !gated) || counting)
        {
            const int64_t b0 = (int64_t) std::ceil(startTick / kPPQ), b1 = (int64_t) std::floor((endTick - 1e-9) / kPPQ);
            for (int64_t b = b0; b <= b1; ++b)
                if (s.metronome || b < 0) addEv((int) (((double) b * kPPQ - startTick) / ticksPerSample), 2, (b % 4) == 0 ? 1 : 0, 0.0f, 0);
        }
    }

    // metronome of a source audition: on the beats of the source's grid, at the tempo that is heard
    if (s.metronome && !playing && previewActive && !previewStopping)
    {
        const auto& m = s.sources[(size_t) previewSrc];
        const PreviewSource* ps = previews[(size_t) previewSrc].get();
        if (m.beatFrames > 100.0 && ps != nullptr)
        {
            const double rate = ps->sampleRate / sr / ps->ratio;
            const double p0 = (previewPos - m.beatOrigin) / m.beatFrames;
            const double p1 = (std::min(previewEnd, previewPos + n * rate) - m.beatOrigin) / m.beatFrames;
            for (int64_t b = (int64_t) std::ceil(p0); (double) b < p1; ++b)
                addEv((int) (((double) b - p0) * m.beatFrames / rate), 2, (((b % 4) + 4) % 4) == 0 ? 1 : 0, 0.0f, 0);
        }
    }

    // ---- order: by offset, offs before ons at the same offset ----
    std::sort(evs.begin(), evs.begin() + nev, [](const Ev& a, const Ev& b)
    {
        return a.offset != b.offset ? a.offset < b.offset : a.kind < b.kind;
    });

    float* L = out.getWritePointer(0);
    float* R = out.getNumChannels() > 1 ? out.getWritePointer(1) : nullptr;
    float* RR = R != nullptr ? R : L; // mono bus: both channels summed into L

    int pos = 0;
    for (int i = 0; i <= nev; ++i)
    {
        const int until = i < nev ? evs[(size_t) i].offset : n;
        if (until > pos)
        {
            renderVoices(L + pos, RR + pos, until - pos, s);
            renderClick(L + pos, RR + pos, until - pos);
            pos = until;
        }
        if (i == nev) break;
        const auto& e = evs[(size_t) i];
        const bool live = (e.token & kSeqToken) == 0;
        const bool audition = (e.token & kGuiToken) != 0 && (e.token & (uint32_t) kAuditionPointer) != 0;
        const bool report = playing && live && !audition && e.pad >= 0 && e.pad < kNumPads && s.pads[(size_t) e.pad].assigned;
        if (e.kind == 1)
        {
            const bool fromDaw = hostDriving && (e.token & kMidiToken) != 0;
            const bool muted = fromDaw && ((s.hostPlays == HostPlays::Main && !rec)
                                           || (s.hostPlays != HostPlays::DawNotes && !gated && playedByMain(e.pad, currentLoopTick(e.offset))));
            if (!muted) noteOn(s, e.pad, e.vel, e.token, live);
            if (live && !fromDaw && e.pad >= 0) livePad.store(e.pad);
            if (report) pushHit(RecordedHit::On, e, rec);
        }
        else if (e.kind == 0)
        {
            noteOff(s, e.pad, e.token);
            if (report) pushHit(RecordedHit::Off, e, rec);
        }
        else
        {
            startClick(e.pad == 1, s);
        }
    }
    renderPreview(L, RR, n);

    // master gain + gentle safety clip
    float peak = 0.0f;
    const float g = s.masterGain;
    for (int c = 0; c < std::min(2, out.getNumChannels()); ++c)
    {
        float* d = out.getWritePointer(c);
        for (int i = 0; i < n; ++i)
        {
            float x = d[i] * g;
            if (!std::isfinite(x)) x = 0.0f;
            if (std::abs(x) > 0.9f) x = std::copysign(0.9f + 0.1f * std::tanh((std::abs(x) - 0.9f) * 10.0f), x);
            d[i] = x;
            peak = std::max(peak, std::abs(x));
        }
    }
    for (int c = 2; c < out.getNumChannels(); ++c) out.clear(c, 0, n);
    outputPeak.store(std::max(peak, outputPeak.load() * 0.9f));

    if (playing)
    {
        if (!(host.valid && host.playing)) internalTick = endTick;
        double m = std::fmod(endTick, (double) loopTicks);
        if (m < 0) m += (double) loopTicks;
        playheadTick.store(!hostDriving && endTick < 0.0 ? 0.0 : m);
    }
    countInBeat.store(playing && !hostDriving && endTick < 0.0
                          ? juce::jlimit(1, 4, (int) std::floor((endTick + kTicksPerBar) / kPPQ) + 1) : 0);
    int hand = 0;
    for (const auto& v : voices)
        if (v.active && v.stage != Stage::Fade && v.stage != Stage::Release && (v.token & handTokens) != 0) ++hand;
    liveVoices.store(hand);
    transportPlaying.store(playing);
    wasHostDriving = hostDriving;
    wasPlaying = playing;
    lastEndTick = endTick;
}

void Engine::renderOffline(const EngineSnapshot& snap, const std::array<const PadSound*, kNumPads>& padSounds,
                           double sampleRate, juce::AudioBuffer<float>& out, double tailSeconds)
{
    auto e = std::make_unique<Engine>();
    e->prepare(sampleRate, 512);
    e->publishSnapshot(std::make_unique<EngineSnapshot>(snap));
    for (int i = 0; i < kNumPads; ++i)
        if (padSounds[(size_t) i] != nullptr)
        {
            auto copy = std::make_unique<PadSound>();
            auto& shared = const_cast<juce::AudioBuffer<float>&>(padSounds[(size_t) i]->audio); // only read; outlives this call
            copy->audio.setDataToReferTo(shared.getArrayOfWritePointers(), shared.getNumChannels(), shared.getNumSamples());
            copy->sampleRate = padSounds[(size_t) i]->sampleRate;
            copy->fadeIn = padSounds[(size_t) i]->fadeIn;
            copy->fadeOut = padSounds[(size_t) i]->fadeOut;
            e->publishPadSound(i, std::move(copy));
        }
    const double bpm = snap.bpm > 20.0 ? snap.bpm : 120.0;
    const double loopSeconds = (double) snap.loopTicks / kPPQ * 60.0 / bpm;
    const int loopFrames = (int) std::ceil(loopSeconds * sampleRate);
    const int total = loopFrames + (int) (tailSeconds * sampleRate);
    out.setSize(2, total);
    out.clear();
    e->internalPlaying = true;
    e->internalTick = 0.0;
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    HostTransport none;
    for (int pos = 0; pos < total; pos += 512)
    {
        const int n = std::min(512, total - pos);
        if (pos + n > loopFrames && e->internalPlaying)
        {
            // stop exactly at the loop end: render up to it, then stop
            const int head = loopFrames - pos;
            if (head > 0)
            {
                juce::AudioBuffer<float> h(2, head);
                e->process(h, midi, none);
                for (int c = 0; c < 2; ++c) out.copyFrom(c, pos, h, c, 0, head);
            }
            e->internalPlaying = false;
            const int rest = n - std::max(0, head);
            if (rest > 0)
            {
                juce::AudioBuffer<float> t(2, rest);
                e->process(t, midi, none);
                for (int c = 0; c < 2; ++c) out.copyFrom(c, pos + std::max(0, head), t, c, 0, rest);
            }
            continue;
        }
        block.setSize(2, n, false, false, true);
        e->process(block, midi, none);
        for (int c = 0; c < 2; ++c) out.copyFrom(c, pos, block, c, 0, n);
    }
}
} // namespace hps
