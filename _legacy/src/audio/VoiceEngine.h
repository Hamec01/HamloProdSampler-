#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "fx/FxLite.h"
#include "model/SamplerModel.h"

namespace hps
{
enum class TriggerOrigin : uint8_t
{
    GuiMouse,
    MidiNote,
    Sequencer,
    SourceAudition
};

struct PadTriggerMessage
{
    bool          isNoteOn     = true;
    int           flatPadIndex = 0;       // 0..63, or -1 for SourceAudition
    float         velocity     = 1.0f;
    int           sampleOffset = 0;
    TriggerOrigin origin       = TriggerOrigin::GuiMouse;
    uint32_t      token        = 0;       // distinct token per pointer / MIDI note+channel
    int           pitchOffset  = 0;
    // For direct source audition (previewing waveform selection or candidate before assign)
    int           auditionSlot       = -1;
    int64_t       auditionStartFrame = 0;
    int64_t       auditionEndFrame   = 0;
};

struct RecordedPadHit
{
    int     flatPadIndex  = 0;
    int64_t startTick     = 0;
    int64_t durationTicks = 480;
    float   velocity      = 1.0f;
    bool    isGate        = false;
};

// Immutable snapshot published from UI thread and read lock-free by audio thread.
struct AudioPadSnapshot
{
    bool                                            assigned = false;
    int                                             sourceSlot = -1;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double                                          sourceSampleRate = 44100.0;
    int64_t                                         startFrame = 0;
    int64_t                                         endFrameExclusive = 0;
    PlaybackMode                                    playbackMode = PlaybackMode::OneShot;
    OverlapPolicy                                   overlapPolicy = OverlapPolicy::Retrigger;
    RetriggerMode                                   retriggerMode = RetriggerMode::Restart;
    int                                             chokeGroup = 0;
    bool                                            loop = false;
    bool                                            reverse = false;
    float                                           pitchSemitones = 0.0f;
    float                                           fineCents = 0.0f;
    float                                           stretchRatio = 1.0f;
    SyncMode                                        syncMode = SyncMode::Off;
    double                                          sourceBpm = 0.0;
    float                                           attackMs = 1.0f;
    float                                           decayMs = 0.0f;
    float                                           sustainLevel = 1.0f;
    float                                           releaseMs = 25.0f;
    float                                           fadeInMs = 2.0f;
    float                                           fadeOutMs = 5.0f;
    float                                           gainLinear = 1.0f;
    float                                           pan = 0.0f;
    bool                                            filterEnabled = false;
    int                                             filterType = 0;
    float                                           filterCutoffHz = 12000.0f;
    float                                           filterResonance = 0.707f;
    float                                           filterMix = 1.0f;
    int                                             midiNote = kBaseMidiNote;
    int                                             maxVoices = 4;
};

struct AudioPatternEventSnapshot
{
    int     flatPadIndex  = 0;
    int64_t startTick     = 0;
    int64_t durationTicks = 480;
    float   velocity      = 1.0f;
    int     pitchOverride = 0;
};

struct AudioRenderSnapshot
{
    double                                                          projectBpm = 120.0;
    int64_t                                                         patternLengthTicks = kTicksPerQuarter * 8;
    float                                                           swingAmount = 0.0f;
    bool                                                            hostSyncEnabled = true;
    std::array<std::shared_ptr<const juce::AudioBuffer<float>>, kNumVisibleSources> sourceAudio;
    std::array<double, kNumVisibleSources>                          sourceSampleRates { 44100.0, 44100.0, 44100.0, 44100.0 };
    std::array<SourceLaneMixState, kNumVisibleSources>              laneMix;
    std::array<AudioPadSnapshot, kTotalPads>                        pads;
    std::vector<AudioPatternEventSnapshot>                          patternEvents;
    MasterFxSettings                                                masterFx;
    uint64_t                                                        revision = 0;
};

// Lock-free SPSC ring buffer for realtime-safe messaging
template <typename T, size_t Capacity>
class LockFreeQueue
{
public:
    bool push(const T& item) noexcept
    {
        const auto w = writeIdx.load(std::memory_order_relaxed);
        const auto next = (w + 1) % Capacity;
        if (next == readIdx.load(std::memory_order_acquire))
            return false;
        buffer[w] = item;
        writeIdx.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept
    {
        const auto r = readIdx.load(std::memory_order_relaxed);
        if (r == writeIdx.load(std::memory_order_acquire))
            return false;
        out = buffer[r];
        readIdx.store((r + 1) % Capacity, std::memory_order_release);
        return true;
    }

    void clear() noexcept
    {
        readIdx.store(writeIdx.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::array<T, Capacity> buffer {};
    std::atomic<size_t>     writeIdx { 0 };
    std::atomic<size_t>     readIdx  { 0 };
};

class VoiceEngine
{
public:
    static constexpr int kMaxVoices = 32;

    VoiceEngine();

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // Called from UI thread to publish a new immutable snapshot
    static std::shared_ptr<const AudioRenderSnapshot> buildSnapshot(const SamplerProject& project);
    void publishSnapshot(std::shared_ptr<const AudioRenderSnapshot> snapshot);

    // Realtime-safe triggers from UI thread
    bool pushGuiTrigger(const PadTriggerMessage& msg) noexcept { return guiQueue.push(msg); }
    void requestAllNotesOff() noexcept { allNotesOffRequested.store(true, std::memory_order_release); }

    // Transport & Recording controls (atomic, realtime-safe)
    void setPlaying(bool play) noexcept { playing.store(play, std::memory_order_release); }
    bool isPlaying() const noexcept { return playing.load(std::memory_order_acquire); }
    void setRecording(bool rec) noexcept { recording.store(rec, std::memory_order_release); }
    bool isRecording() const noexcept { return recording.load(std::memory_order_acquire); }
    void setLooping(bool loop) noexcept { looping.store(loop, std::memory_order_release); }
    bool isLooping() const noexcept { return looping.load(std::memory_order_acquire); }
    void setActiveBank(int bank) noexcept { activeBank.store(juce::jlimit(0, kNumBanks - 1, bank), std::memory_order_release); }
    int  getActiveBank() const noexcept { return activeBank.load(std::memory_order_acquire); }
    void seekToTick(int64_t tick) noexcept { playheadTick.store(static_cast<double>(std::max<int64_t>(0, tick)), std::memory_order_release); }
    double getPlayheadTick() const noexcept { return playheadTick.load(std::memory_order_acquire); }

    // Drain recorded live hits on UI thread
    bool popRecordedHit(RecordedPadHit& out) noexcept { return recordedQueue.pop(out); }

    // Main realtime audio callback (ZERO locks, ZERO allocs, ZERO disk/IPC)
    void processBlock(juce::AudioBuffer<float>& audioOut,
                      juce::MidiBuffer& midiInOut,
                      bool hostIsPlaying = false,
                      double hostBpm = 0.0,
                      double hostPpqPosition = -1.0) noexcept;

    int getActiveVoiceCount() const noexcept { return activeVoiceCount.load(std::memory_order_relaxed); }
    uint64_t getTotalNoteOnCount() const noexcept { return totalNoteOnCount.load(std::memory_order_relaxed); }
    bool isPadActive(int flatPadIndex) const noexcept;

    // Offline render for RESULT WAVEFORM and WAV/MIDI export
    static juce::AudioBuffer<float> renderPatternOffline(const SamplerProject& project, double sampleRate = 44100.0);
    static bool exportWav(const juce::File& outFile, const juce::AudioBuffer<float>& buffer, double sampleRate = 44100.0);
    static bool exportMidiTriggers(const juce::File& outFile, const SamplerProject& project);

private:
    enum class EnvStage : uint8_t { Idle, Attack, Decay, Sustain, Release, QuickFadeOut };

    struct Voice
    {
        bool                                            active = false;
        int                                             flatPadIndex = -1;
        int                                             sourceSlot = -1;
        TriggerOrigin                                   origin = TriggerOrigin::GuiMouse;
        uint32_t                                        token = 0;
        uint64_t                                        age = 0;
        PlaybackMode                                    playbackMode = PlaybackMode::OneShot;
        int                                             chokeGroup = 0;
        bool                                            loop = false;
        int                                             loopPassesRemaining = 1; // prevents accidental infinite OneShot+Loop
        bool                                            reverse = false;

        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        int64_t                                         startFrame = 0;
        int64_t                                         endFrameExclusive = 0;
        double                                          position = 0.0;
        double                                          increment = 1.0;

        float                                           velocity = 1.0f;
        float                                           gainL = 1.0f;
        float                                           gainR = 1.0f;

        EnvStage                                        envStage = EnvStage::Idle;
        float                                           envLevel = 0.0f;
        float                                           attackRate = 0.01f;
        float                                           decayRate = 0.001f;
        float                                           sustainLevel = 1.0f;
        float                                           releaseRate = 0.001f;
        float                                           quickFadeRate = 0.005f;
        int                                             fadeInSamples = 64;
        int                                             fadeOutSamples = 128;
        int64_t                                         samplesRendered = 0;

        FilterEqLite                                    padFilter;
    };

    struct PendingRecNote
    {
        bool          active = false;
        int           flatPadIndex = 0;
        TriggerOrigin origin = TriggerOrigin::GuiMouse;
        uint32_t      token = 0;
        int64_t       startTick = 0;
        float         velocity = 1.0f;
        bool          isGate = false;
    };

    void handleTrigger(const PadTriggerMessage& msg, const AudioRenderSnapshot& snap, int64_t currentTick) noexcept;
    void startVoice(const PadTriggerMessage& msg, const AudioRenderSnapshot& snap) noexcept;
    void releaseVoicesMatching(int flatPadIndex, TriggerOrigin origin, uint32_t token) noexcept;
    void quickFadeVoice(Voice& v) noexcept;
    int  allocateVoiceSlot() noexcept;

    double                                     sampleRate = 44100.0;
    int                                        maxBlockSize = 512;
    std::shared_ptr<const AudioRenderSnapshot> activeSnapshot;
    std::atomic<uint64_t>                      voiceAgeCounter { 1 };

    std::array<Voice, kMaxVoices>              voices;
    std::array<PendingRecNote, 16>             pendingRecNotes;
    WarmTubeCompLite                           masterFx;
    juce::AudioBuffer<float>                   voiceScratch;

    LockFreeQueue<PadTriggerMessage, 256>      guiQueue;
    LockFreeQueue<RecordedPadHit, 256>         recordedQueue;

    std::atomic<bool>     playing { false };
    std::atomic<bool>     recording { false };
    std::atomic<bool>     looping { true };
    std::atomic<bool>     allNotesOffRequested { false };
    std::atomic<int>      activeBank { 0 };
    std::atomic<double>   playheadTick { 0.0 };
    std::atomic<int>      activeVoiceCount { 0 };
    std::atomic<uint64_t> totalNoteOnCount { 0 };
    std::atomic<uint64_t> activePadMaskLow { 0 };

    // Scheduled gate note-offs from pattern sequencer
    struct ScheduledOff { bool active = false; int flatPadIndex = 0; uint32_t token = 0; int64_t offTick = 0; };
    std::array<ScheduledOff, 32> scheduledOffs;
};
} // namespace hps
