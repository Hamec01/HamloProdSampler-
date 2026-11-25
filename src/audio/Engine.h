#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "core/Types.h"

namespace hps
{
// ============================================================================================
// Realtime-safe publication: the message thread publishes immutable objects, the audio thread
// reads raw pointers. Replaced objects are deleted on the message thread after a grace period
// (and, for pad sounds, once no voice uses them). The audio thread never allocates or locks.
// ============================================================================================
template <class T>
class Published
{
public:
    ~Published() { delete current.load(); }
    T* get() const noexcept { return current.load(std::memory_order_acquire); }
    void publish(std::unique_ptr<T> next)
    {
        T* old = current.exchange(next.release(), std::memory_order_acq_rel);
        if (old != nullptr) retired.push_back({ std::unique_ptr<T>(old), juce::Time::getMillisecondCounter() });
    }
    template <class CanFree>
    void collect(CanFree canFree)
    {
        const auto now = juce::Time::getMillisecondCounter();
        retired.erase(std::remove_if(retired.begin(), retired.end(), [&](auto& r)
        {
            return now - r.t > 1500 && canFree(*r.p);
        }), retired.end());
    }
    void collect() { collect([](const T&) { return true; }); }

private:
    struct Retired { std::unique_ptr<T> p; juce::uint32 t; };
    std::atomic<T*> current { nullptr };
    std::vector<Retired> retired;
};

// Pre-rendered pad audio at the engine sample rate (slice -> stretch/pitch -> reverse -> fades).
struct PadSound
{
    juce::AudioBuffer<float> audio; // 2 channels
    double sampleRate = 44100.0;
    uint64_t renderKey = 0;
    int fadeIn = 0, fadeOut = 0; // frames, applied by the voice (shared renders; other sounds have them baked in)
    std::shared_ptr<juce::AudioBuffer<float>> master; // set when `audio` only points into a render shared by several pads
    mutable std::atomic<int> users { 0 };
};

struct EnginePad
{
    bool     assigned = false;
    PlayMode mode = PlayMode::OneShot;
    RepeatMode repeat = RepeatMode::Retrigger;
    int      choke = 0;
    bool     loop = false;
    int      src = 0;
    float    gainL = 1.0f, gainR = 1.0f;
    float    attackS = 0.001f, decayS = 0.0f, sustain = 1.0f, releaseS = 0.04f;
    float    hpHz = 20.0f, lpHz = 20000.0f;
};

struct EngineEvent
{
    int     pad = 0;
    int64_t tick = 0;
    int64_t length = 240;
    float   velocity = 0.9f;
    int64_t rawTick = -1;   // played timing of a recorded note (used to never play one hit twice)
};

struct EngineSnapshot
{
    std::array<EnginePad, kNumPads> pads;
    struct SourceMix { float gainL = 1.0f, gainR = 1.0f, hpHz = 20.0f, lpHz = 20000.0f; };
    std::array<SourceMix, kNumSources> sources;
    MonoMode mono = MonoMode::Off;
    std::vector<EngineEvent> events; // sorted by tick
    int64_t  loopTicks = kTicksPerBar * 4;
    double   bpm = 120.0;
    float    swing = 0.0f;
    bool     metronome = false;
    float    masterGain = 1.0f;
    bool mainNeedsWindow = false;                  // MAIN follows the DAW only while windowOpen
    HostPlays hostPlays = HostPlays::Both;         // what sounds while the DAW plays
    HostCapture hostCapture = HostCapture::WhenRecording;
};

struct PreviewSource
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double sampleRate = 44100.0;
    double ratio = 1.0; // buffer frames per source frame (a source rendered at the project tempo; positions stay source frames)
};

// What the audio thread reports to the message thread while recording.
struct RecordedHit
{
    enum Type : uint8_t { On, Off } type = On;
    int      pad = 0;
    int64_t  tick = 0;    // position inside the pattern loop (raw, unquantized)
    float    velocity = 0.9f;
    uint32_t token = 0;
    bool     rec = false;   // recording was on (else: only remembered for CAPTURE)
    bool     host = false;  // the DAW transport was running
    uint32_t pass = 0;      // increments on every transport start
};

struct HostTransport
{
    bool   valid = false;   // host gave a position
    bool   playing = false;
    double ppq = 0.0;
    double bpm = 120.0;
    bool   recording = false; // the DAW records
    bool   offline = false;   // the DAW renders to a file
};

class Engine
{
public:
    static constexpr int kMaxVoices = 48;
    static constexpr uint32_t kGuiToken = 0x10000u, kMidiToken = 0x20000u, kSeqToken = 0x40000u;
    static constexpr int kAuditionPointer = 0x40; // GUI pointers with this bit only listen: never recorded

    Engine();

    // ----- message thread -----
    void publishSnapshot(std::unique_ptr<EngineSnapshot> s) { snapshot.publish(std::move(s)); }
    void publishPadSound(int pad, std::unique_ptr<PadSound> s);
    void publishPreviewSource(int slot, std::unique_ptr<PreviewSource> p);
    void collectGarbage();

    void padDown(int pad, float velocity, int pointer = 0);   // immediate NoteOn (GUI)
    void padUp(int pad, int pointer = 0);                     // NoteOff (GUI)
    void previewRange(int slot, int64_t start, int64_t end, bool loop);
    void stopPreview();
    void setInternalPlaying(bool shouldPlay, bool fromStart = false);
    void playFrom(int64_t loopTick); // internal transport from a position (Playback tool)
    void playWithCountIn();          // one bar of clicks (1-2-3-4), then MAIN from bar 1
    void stopAll();                  // audition + live pads + MAIN; what the DAW plays goes on
    void setRecording(bool shouldRecord) { recording.store(shouldRecord); }
    void panic();

    bool popRecordedHit(RecordedHit& out);
    const PadSound* padSound(int pad) const noexcept { return sounds[(size_t) pad].get(); }

    // ----- UI read-outs (any thread) -----
    std::atomic<double> playheadTick { 0.0 };   // inside the loop
    std::atomic<bool>   transportPlaying { false };
    std::atomic<bool>   windowOpen { false };   // set by the editor
    std::atomic<bool>   hostDriven { false };   // the DAW transport is running (the plug-in follows it)
    std::atomic<bool>   hostRecording { false };
    std::atomic<int>    countInBeat { 0 };      // 1..4 while the count-in runs, else 0
    std::atomic<int>    liveVoices { 0 };       // pads played by hand (not by MAIN / the DAW)
    std::atomic<int>    livePad { -1 };         // the last of them
    std::atomic<uint32_t> hostMidiNotes { 0 };  // notes received from the DAW while it plays
    std::atomic<double> previewFrame { -1.0 };  // source frame of the audition, -1 = none
    std::atomic<int>    previewSlot { -1 };
    std::atomic<float>  outputPeak { 0.0f };
    std::array<std::atomic<uint32_t>, kNumPads> padHits {};   // increments on every NoteOn
    std::array<std::atomic<int>, kNumPads>      padVoices {}; // currently sounding voices
    std::atomic<bool>   recording { false };
    std::atomic<double> hostBpm { 0.0 };

    // ----- audio thread -----
    void prepare(double sampleRate, int maxBlock);
    void process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, const HostTransport& host);
    double getSampleRate() const noexcept { return sr; }

    // Offline: render the pattern of `snap` with `padSounds` into `out` (bars of the loop).
    static void renderOffline(const EngineSnapshot& snap, const std::array<const PadSound*, kNumPads>& padSounds,
                              double sampleRate, juce::AudioBuffer<float>& out, double tailSeconds);

private:
    enum class Stage : uint8_t { Attack, Decay, Sustain, Release, Fade };
    struct Svf { float ic1 = 0, ic2 = 0; };
    struct Voice
    {
        bool active = false;
        int pad = 0, src = 0;
        const PadSound* sound = nullptr;
        double pos = 0.0;
        float vel = 1.0f;
        uint32_t token = 0;
        uint64_t order = 0;
        Stage stage = Stage::Attack;
        float env = 0.0f;
        float fadeStep = 0.0f;  // for Fade stage
        float relStep = 0.0f;
        bool loop = false;
        bool toggle = false;
        Svf hp[2], lp[2];
        float hpG = 0, hpK = 0, lpG = 0, lpK = 0;
        bool useHp = false, useLp = false;
    };
    struct Cmd
    {
        enum Type : uint8_t { PadOn, PadOff, Preview, PreviewStop, Play, Stop, Panic, StopAll } type = PadOn;
        int pad = 0;
        float vel = 1.0f;
        uint32_t token = 0;
        int slot = 0;
        int64_t a = 0, b = 0;
        bool flag = false;
    };
    struct Ev { int offset; uint8_t kind; int pad; float vel; uint32_t token; }; // kind 0=off,1=on,2=click

    void pushCmd(const Cmd& c);
    void noteOn(const EngineSnapshot& s, int pad, float vel, uint32_t token, bool live);
    void noteOff(const EngineSnapshot& s, int pad, uint32_t token);
    void fadeVoice(Voice& v, float seconds = 0.004f);
    void releaseVoice(Voice& v, const EnginePad& p);
    void renderVoices(float* L, float* R, int n, const EngineSnapshot& s);
    void renderPreview(float* L, float* R, int n);
    void renderClick(float* L, float* R, int n);
    void freeVoice(Voice& v);
    void stopPreviewNow() { if (previewActive) previewStopping = true; }
    void pushHit(RecordedHit::Type type, const Ev& e, bool rec);
    int64_t currentLoopTick(int offset) const noexcept;

    double sr = 44100.0;
    Published<EngineSnapshot> snapshot;
    std::array<Published<PadSound>, kNumPads> sounds;
    std::array<Published<PreviewSource>, kNumSources> previews;

    std::array<Voice, kMaxVoices> voices {};
    uint64_t voiceOrder = 1;

    // command FIFO (message -> audio)
    juce::AbstractFifo cmdFifo { 1024 };
    std::array<Cmd, 1024> cmdBuf {};
    juce::CriticalSection producerLock; // GUI producers only (never taken on the audio thread)

    // recorded hits FIFO (audio -> message)
    juce::AbstractFifo recFifo { 2048 };
    std::array<RecordedHit, 2048> recBuf {};

    // transport
    bool internalPlaying = false;
    double internalTick = 0.0;    // absolute ticks
    double blockStartTick = 0.0;  // absolute ticks at block start
    double ticksPerSample = 0.0;
    bool wasPlaying = false;
    bool wasHostDriving = false;
    bool hostDriving = false;
    bool windowLatched = false, wasGated = false;
    uint32_t pass = 0;
    struct Recent { int pad = -1; int64_t loopTick = 0; double absTick = 0; }; // hits of this pass: not played again by MAIN
    std::array<Recent, 64> recents {};
    int recentPos = 0;
    double lastEndTick = -1.0;
    int64_t loopTicks = kTicksPerBar * 4;

    struct ScheduledOff { bool active = false; int pad = 0; uint32_t token = 0; double tick = 0; };
    std::array<ScheduledOff, 256> offs {};

    // preview
    bool previewActive = false;
    int previewSrc = 0;
    double previewPos = 0, previewStart = 0, previewEnd = 0, previewFadeIn = 0;
    bool previewLoop = false;
    float previewGain = 0.0f;
    bool previewStopping = false;

    // metronome click
    float clickPhase = 0.0f, clickEnv = 0.0f, clickFreq = 1000.0f;

    std::array<Ev, 1024> evs {};
};
} // namespace hps
