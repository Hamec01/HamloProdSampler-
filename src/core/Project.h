#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "core/Types.h"

namespace hps
{
// ============================================================================================
// Undoable project state. Plain values only: copying it is the undo snapshot. Audio buffers and
// analysis results live in SourceData (immutable, shared, not part of undo).
// ============================================================================================

struct SourceSlot
{
    juce::String id;          // empty = slot empty
    juce::String name;
    juce::String filePath;
    juce::Colour colour;
    // user overrides of the analysis (0 / -2 = use detected)
    double bpmOverride    = 0.0;  // > 0: user tempo
    double originOverride = -1.0; // >= 0: user beat-1 position in seconds
    int    keyOverrideRoot = -2;  // -2 = detected, -1 = force "no key", 0..11 = user key
    int    keyOverrideMode = 0;
    // source lane mix (applied to every pad of this source)
    float  gainDb = 0.0f;
    float  pan = 0.0f;
    float  lowCutHz = 20.0f;     // high-pass of the lane (20 = off)
    float  highCutHz = 20000.0f; // low-pass of the lane (20000 = off)

    bool empty() const noexcept { return id.isEmpty(); }
};

struct Slice // accepted region in a source (SliceLibrary). Pads copy the range on assignment.
{
    juce::String id;
    int          src = 0;
    int64_t      start = 0, end = 0; // source frames, [start, end)
    juce::String name;
};

struct PadSettings
{
    int   playMode   = -1;   // -1 = project default, else PlayMode
    RepeatMode repeat = RepeatMode::Retrigger;
    int   chokeGroup = 0;    // 0 = none, 1..8: hits in a group cut the others of that group
    bool  loop       = false;
    bool  reverse    = false;
    int   sync       = -1;   // -1 = project default, else SyncMode
    bool  followKey  = true; // auto-transpose to project key (when both keys are known)
    float speed      = 1.0f; // user speed: 2 = twice as fast (STRETCH/OFF keep pitch, REPITCH changes it)
    float pitch      = 0.0f; // user transpose, semitones
    float fine       = 0.0f; // cents
    float gainDb     = 0.0f;
    float pan        = 0.0f;
    float attackMs   = 1.0f;
    float decayMs    = 0.0f;  // 0 = no decay stage
    float sustain    = 1.0f;
    float releaseMs  = 40.0f;
    float fadeInMs   = 1.0f;
    float fadeOutMs  = 4.0f;
    float hpHz       = 20.0f;
    float lpHz       = 20000.0f;
};

struct Pad
{
    bool         assigned = false;
    juce::String sliceId;
    int          src = 0;
    int64_t      start = 0, end = 0; // pad-local copy of the range (editing one pad never moves another)
    juce::String name;
    PadSettings  s;
};

struct PatternEvent
{
    juce::String id;
    int     pad = 0;           // flat pad index 0..63
    int64_t tick = 0;          // position used for playback (may be quantized)
    int64_t rawTick = -1;      // recorded human timing (>= 0 for recorded events)
    int64_t length = kPPQ / 4; // trigger length (Gate: note-off; OneShot: ignored by audio)
    float   velocity = 0.9f;
    bool    muted = false;
};

struct ProjectSettings
{
    double     bpm = 120.0;
    MusicalKey key;                 // project target key (unknown = no auto transpose)
    int        anchorSrc = -1;      // source the BPM/key was taken from (display only)
    bool       followHostTempo = true;
    GridDiv    grid = GridDiv::Sixteenth;
    bool       snap = true;
    SyncMode   sync = SyncMode::Stretch;
    PlayMode   defaultMode = PlayMode::OneShot;
    MonoMode   mono = MonoMode::Off;
    int        patternBars = 4;
    float      swing = 0.0f;        // 0..1 (shifts off-beat 1/16)
    float      recQuantize = 0.0f;  // 0 = off (keep groove), 1 = hard
    GridDiv    recGrid = GridDiv::Sixteenth;
    bool       metronome = false;
    MetroSound metroSound = MetroSound::Click;
    float      metroDb = 0.0f;
    float      masterDb = 0.0f;
    bool       countIn = true;      // 1-2-3-4 before recording
    HostPlays  hostPlays = HostPlays::Both;
    bool       mainNeedsWindow = true; // the DAW's Play starts MAIN only while the plug-in window is open
    HostCapture hostCapture = HostCapture::Off; // DAW Play records only with REC lit (maintainer 2026-11-26)
};

struct ProjectState
{
    std::array<SourceSlot, kNumSources> sources;
    std::vector<Slice>                  slices;
    std::array<Pad, kNumPads>           pads;
    std::vector<PatternEvent>           events;
    ProjectSettings                     settings;
};

// ============================================================================================
// Immutable per-source data (decoded audio + analysis). Shared by UI, renderer and engine.
// ============================================================================================

struct SourceAnalysis
{
    bool   ready = false;
    double bpm = 0.0;          // 0 = unknown / free time
    float  bpmConfidence = 0.0f;
    double originSeconds = 0.0; // beat 1 of bar 1
    int    bars = 0;
    bool   exactLoop = false;
    float  swingPercent = 50.0f;
    MusicalKey key;
    float  keyConfidence = 0.0f;
    bool   atonal = false;
    double tuningCents = 0.0;
    float  drumLikelihood = 0.0f;
    juce::String labelInfo;      // what the file name / acid chunk said
    juce::String summary;

    // Feature frames over the whole file (hop = featureHop frames)
    int featureHop = 256;
    std::vector<float> onsetCurve;          // 0..1 spectral flux
    std::vector<float> rms;                 // 0..1
    std::vector<std::array<float, 12>> chroma;
    std::vector<int64_t> onsets;            // frames, peak-picked
    std::vector<float>   onsetStrength;
    // Spectrogram for display: specCols x specBins, 0..1, hop = specHop frames
    int specHop = 512, specBins = 96, specCols = 0;
    std::vector<uint8_t> spectrogram;
};

struct WaveformPeaks
{
    int framesPerPeak = 64;
    std::vector<float> mins, maxs; // mono summary
};

struct SourceData
{
    juce::String id;
    double sampleRate = 44100.0;
    int64_t numFrames = 0;
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // stereo (mono duplicated)
    WaveformPeaks peaks;
    SourceAnalysis analysis;
    juce::String error;
};

// Same threshold HPDG uses (SampleAnalyzer: tempoConfidence >= 0.35 -> tempo usable).
inline constexpr float kReliableBpmConfidence = 0.35f;

// Effective values: user override first, then analysis. An unreliable tempo stays a suggestion.
double     effectiveSourceBpm(const SourceSlot& slot, const SourceData* data);
double     effectiveSourceOrigin(const SourceSlot& slot, const SourceData* data);
MusicalKey effectiveSourceKey(const SourceSlot& slot, const SourceData* data);
PlayMode   effectivePlayMode(const Pad& pad, const ProjectSettings& s);
SyncMode   effectiveSync(const Pad& pad, const ProjectSettings& s);

juce::String newId();

// ============================================================================================
// Project: message-thread owner of state + undo history + source data store.
// ============================================================================================
class Project
{
public:
    Project();

    const ProjectState& state() const noexcept { return current; }

    // Undoable edit. `label` names the transaction. Within a gesture (begin/endGesture) only the
    // state before the gesture is pushed, so a whole drag is one Undo step.
    void edit(const juce::String& label, const std::function<void(ProjectState&)>& fn);
    void beginGesture(const juce::String& label);
    void endGesture();
    // Non-undoable change (host tempo follow, analysis-driven updates).
    void mutate(const std::function<void(ProjectState&)>& fn) { fn(current); changed(); }
    // Non-undoable replace (load / restore).
    void replaceState(const ProjectState& s, bool clearHistory);

    bool canUndo() const noexcept { return !undoStack.empty(); }
    bool canRedo() const noexcept { return !redoStack.empty(); }
    juce::String undoLabel() const { return undoStack.empty() ? juce::String() : undoStack.back().label; }
    juce::String redoLabel() const { return redoStack.empty() ? juce::String() : redoStack.back().label; }
    void undo();
    void redo();

    // Source data store (not undoable)
    std::shared_ptr<const SourceData> sourceData(const juce::String& id) const;
    std::shared_ptr<const SourceData> sourceDataForSlot(int slot) const;
    void setSourceData(std::shared_ptr<const SourceData> data);

    uint64_t revision() const noexcept { return rev; }

    // Change notification (message thread)
    std::function<void()> onChanged;

    // Helpers
    int firstEmptyPad(int fromFlat) const;
    std::vector<int> padsUsingSource(int slot) const;

private:
    struct Entry { ProjectState state; juce::String label; };
    void changed();

    ProjectState current;
    std::vector<Entry> undoStack, redoStack;
    bool inGesture = false;
    bool gesturePushed = false;
    juce::String gestureLabel;
    std::map<juce::String, std::shared_ptr<const SourceData>> store;
    uint64_t rev = 1;
    static constexpr size_t kMaxUndo = 200;
};
} // namespace hps
