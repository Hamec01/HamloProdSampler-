#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include "hpbridge/BridgeTypes.h"

namespace hps
{
inline constexpr int kNumVisibleSources = 4;
inline constexpr int kNumBanks          = 4;  // A, B, C, D
inline constexpr int kPadsPerBank       = 16;
inline constexpr int kTotalPads         = kNumBanks * kPadsPerBank;
inline constexpr int kTicksPerQuarter   = 960;
inline constexpr int kBaseMidiNote      = 36; // C1

enum class DisplayMode { Wave, Spectrogram, Split };
enum class MainViewMode { Perform, Pattern, Result };
enum class NavTarget { Main, Src1, Src2, Src3, Src4 };

enum class PlaybackMode { Gate, OneShot, Toggle };
enum class RetriggerMode { Restart, Layer };
enum class OverlapPolicy { PolyLayer, Retrigger, ChokeGroup, MonoPad, SourceMono, GlobalMono };
enum class SyncMode { Off, Repitch, Stretch };
enum class GridDivision { Quarter, Eighth, Sixteenth, ThirtySecond, TripletEighth, TripletSixteenth, Off };
enum class AnchorSource { Manual, Host, Src1, Src2, Src3, Src4, HpdgGuide };

enum class AnalysisState { Empty, Analyzing, Ready, Unknown, Failed };

enum class ChopAlgorithm
{
    Manual,
    FindSpotsAttack,
    FindSpotsTonal,
    FindSpotsPhrase,
    FindSpotsHybrid,
    SlicerBeat,
    SlicerEqualTime,
    ChopThreshold,
    ChopRegions,
    ChopBpm
};

struct SourceColor
{
    juce::Colour colour;
    juce::String badge; // "S1".."S4" for color-blind accessibility

    static SourceColor defaultForSlot(int slotIndex)
    {
        switch (slotIndex)
        {
            case 0: return { juce::Colour(0xff4da3ff), "S1" }; // Blue
            case 1: return { juce::Colour(0xfff5a623), "S2" }; // Amber
            case 2: return { juce::Colour(0xffb06cff), "S3" }; // Violet
            case 3: return { juce::Colour(0xff39d98a), "S4" }; // Green
            default: return { juce::Colour(0xff9aa5b1), "S?" };
        }
    }
};

struct KeyEstimate
{
    std::optional<int>   tonicPitchClass; // 0=C .. 11=B, nullopt = Unknown
    juce::String         mode;            // "minor", "major"
    float                confidence = 0.0f;
    hpbridge::ContextOrigin origin  = hpbridge::ContextOrigin::Unknown;

    juce::String label() const
    {
        if (!tonicPitchClass.has_value()) return "Unknown";
        static constexpr const char* kNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return juce::String(kNames[*tonicPitchClass % 12]) + (mode == "major" ? " maj" : " min");
    }
};

struct BpmCandidate
{
    double bpm        = 0.0;
    float  confidence = 0.0f;
};

struct OnsetPoint
{
    int64_t frame    = 0;
    float   strength = 0.0f;
    int     band     = 1; // 0=low, 1=mid, 2=high
};

struct AnalysisResult
{
    AnalysisState             state = AnalysisState::Empty;
    int                       algorithmVersion = 1;
    std::optional<double>     bpm;
    float                     bpmConfidence = 0.0f;
    hpbridge::ContextOrigin   bpmOrigin     = hpbridge::ContextOrigin::Unknown;
    std::vector<BpmCandidate> bpmCandidates;

    KeyEstimate               key;
    bool                      isAtonalOrPercussive = false;
    float                     averageFlatness      = 0.0f;

    std::vector<OnsetPoint>   onsets;
    std::array<float, 12>     chroma {};
    std::vector<float>        rmsEnvelope;    // downsampled ~10ms frames
    int                       hopSamples = 256;
    std::vector<float>        spectrogramCols; // flattened [col * specBins + bin], normalized 0..1
    int                       specCols = 0;
    int                       specBins = 64;
};

struct SourceAsset
{
    juce::String                            id;
    int                                     visibleSlot = 0; // 0..3
    juce::String                            name;
    juce::String                            filePath;
    juce::String                            contentHash;
    bool                                    missingFile = false;
    SourceColor                             color;
    double                                  sampleRate = 44100.0;
    int64_t                                 numFrames  = 0;
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // immutable after load
    AnalysisResult                          analysis;
    uint64_t                                revision = 1;
};

struct SliceCandidate
{
    juce::String  id;
    juce::String  sourceId;
    int64_t       startFrame        = 0;
    int64_t       endFrameExclusive = 0;
    ChopAlgorithm algorithm         = ChopAlgorithm::FindSpotsHybrid;
    juce::String  originLabel;      // "Attack", "Tonal", "Phrase", "Beat", "Region", "Threshold"
    float         score             = 0.0f;
    bool          pinned            = false;
    uint64_t      sourceRevision    = 0;
};

struct SliceDefinition
{
    juce::String id;
    juce::String sourceId;
    juce::String name;
    int64_t      startFrame        = 0;
    int64_t      endFrameExclusive = 0;
    float        fadeInMs          = 2.0f;
    float        fadeOutMs         = 5.0f;
    bool         pinned            = false;
};

struct LocalTrimOverride
{
    int64_t startFrame        = 0;
    int64_t endFrameExclusive = 0;
};

struct PadSettings
{
    std::optional<PlaybackMode>  playbackModeOverride; // nullopt -> follow global
    std::optional<OverlapPolicy> overlapOverride;      // nullopt -> follow global
    RetriggerMode                retriggerMode = RetriggerMode::Restart;
    int                          chokeGroup    = 0;    // 0 = none, 1..8 = group
    bool                         loop          = false;
    bool                         reverse       = false;

    float                        pitchSemitones = 0.0f;
    float                        fineCents      = 0.0f;
    float                        stretchRatio   = 1.0f;
    SyncMode                     syncMode       = SyncMode::Off;

    float                        attackMs       = 1.0f;
    float                        decayMs        = 0.0f;
    float                        sustainLevel   = 1.0f;
    float                        releaseMs      = 25.0f;
    float                        fadeInMs       = 2.0f;
    float                        fadeOutMs      = 5.0f;

    float                        gainDb         = 0.0f;
    float                        pan            = 0.0f; // -1..1

    // Per-pad Filter/EQ Lite
    bool                         filterEnabled  = false;
    int                          filterType     = 0;    // 0=LP, 1=HP, 2=BP
    float                        filterCutoffHz = 12000.0f;
    float                        filterResonance = 0.707f;
    float                        filterMix      = 1.0f;

    int                          midiNote       = kBaseMidiNote;
    int                          midiChannel    = 0;    // 0 = Omni
    int                          maxVoices      = 4;
};

struct PadAssignment
{
    juce::String                     sliceId;
    juce::String                     sourceId;
    juce::String                     label;
    PadSettings                      settings;
    std::optional<LocalTrimOverride> trimOverride;
};

struct PadSlot
{
    juce::String                 padId;
    int                          bankIndex = 0; // 0..3
    int                          padIndex  = 0; // 0..15
    int                          defaultMidiNote = kBaseMidiNote;
    std::optional<PadAssignment> assignment;
};

struct PatternEvent
{
    juce::String       id;
    juce::String       padId;
    int64_t            startTick          = 0;
    int64_t            rawStartTick       = 0; // preserved for non-destructive Record Quantize
    int64_t            durationTicks      = 480;
    float              velocity           = 0.9f;
    int32_t            microOffsetTicks   = 0;
    std::optional<int> pitchOverrideSemitones;
};

struct Pattern
{
    juce::String              id;
    juce::String              name = "Pattern 1";
    int                       bars = 2; // 1..4 (up to 16)
    float                     swingAmount = 0.0f; // 0..0.5
    std::vector<PatternEvent> events;
};

struct SourceLaneMixState
{
    bool  muted  = false;
    bool  soloed = false;
    float gainDb = 0.0f;
};

struct MasterFxSettings
{
    bool  warmTubeCompEnabled = false;
    float driveDb             = 2.0f;
    float compMix             = 0.5f;
    bool  limiterEnabled      = true;
    float masterGainDb        = 0.0f;
};

struct ProjectSnapshot
{
    juce::String                                 projectId;
    juce::String                                 instanceId;
    double                                       projectBpm        = 120.0;
    int                                          meterNumerator    = 4;
    int                                          meterDenominator  = 4;
    KeyEstimate                                  projectKey;
    AnchorSource                                 anchorSource      = AnchorSource::Manual;
    bool                                         bpmLocked         = false;
    bool                                         keyLocked         = false;
    bool                                         hostSyncEnabled   = true;
    GridDivision                                 gridDivision      = GridDivision::Sixteenth;
    SyncMode                                     globalSyncMode    = SyncMode::Off;
    PlaybackMode                                 globalPlayback    = PlaybackMode::OneShot;
    OverlapPolicy                                globalOverlap     = OverlapPolicy::Retrigger;
    bool                                         recordQuantizeOn  = false;
    float                                        recordQuantizeAmt = 1.0f;
    bool                                         overdubMode       = true;

    std::array<std::optional<SourceAsset>, kNumVisibleSources> sources;
    std::array<SourceLaneMixState, kNumVisibleSources>         laneMix;
    std::vector<SliceDefinition>                               slices;
    std::array<PadSlot, kTotalPads>                            pads;
    Pattern                                                    pattern;
    MasterFxSettings                                           masterFx;
    uint64_t                                                   revision = 1;
};

int64_t gridDivisionToTicks(GridDivision div) noexcept;
juce::String gridDivisionLabel(GridDivision div);
juce::String makeUuid();

class SamplerProject
{
public:
    SamplerProject();

    const ProjectSnapshot& state() const noexcept { return current; }
    ProjectSnapshot&       mutableState() noexcept { return current; }

    // Undo / Redo (NEVER called for NoteOn/NoteOff)
    void pushUndoPoint(const juce::String& actionName = {});
    bool canUndo() const noexcept { return !undoStack.empty(); }
    bool canRedo() const noexcept { return !redoStack.empty(); }
    bool undo();
    bool redo();

    // Source management
    SourceAsset*       getSourceBySlot(int slotIndex);
    const SourceAsset* getSourceBySlot(int slotIndex) const;
    SourceAsset*       getSourceById(const juce::String& sourceId);
    const SourceAsset* getSourceById(const juce::String& sourceId) const;
    int                findSourceSlotById(const juce::String& sourceId) const;
    int                countDependentPadsForSource(const juce::String& sourceId) const;

    void setSourceInSlot(int slotIndex, SourceAsset asset);
    void clearSourceSlot(int slotIndex, bool clearDependentPads);
    void setProjectAnchorFromSource(int slotIndex);
    void clearProjectAnchor();

    // SliceCandidateSet (temporary, per source slot, never mutates SliceLibrary or Pads)
    std::vector<SliceCandidate>&       candidatesForSlot(int slotIndex) { return candidates[static_cast<size_t>(juce::jlimit(0, 3, slotIndex))]; }
    const std::vector<SliceCandidate>& candidatesForSlot(int slotIndex) const { return candidates[static_cast<size_t>(juce::jlimit(0, 3, slotIndex))]; }
    void replaceCandidatesForSlot(int slotIndex, std::vector<SliceCandidate> newCandidates);

    // SliceLibrary (non-destructive accepted ranges)
    const SliceDefinition* getSliceById(const juce::String& sliceId) const;
    SliceDefinition*       getSliceById(const juce::String& sliceId);
    SliceDefinition        createSlice(const juce::String& sourceId, int64_t startFrame, int64_t endFrameExclusive, const juce::String& name = {});
    SliceDefinition        acceptCandidate(int slotIndex, const juce::String& candidateId);
    std::vector<SliceDefinition> acceptAllCandidates(int slotIndex);

    // Pad management
    PadSlot&       getPad(int flatIndex);
    const PadSlot& getPad(int flatIndex) const;
    PadSlot*       getPadById(const juce::String& padId);
    const PadSlot* getPadById(const juce::String& padId) const;
    int            findPadFlatIndexById(const juce::String& padId) const;
    int            findPadFlatIndexByMidiNote(int bankIndex, int midiNote) const;

    bool assignSliceToPad(const juce::String& sliceId, int flatPadIndex, bool allowOverwrite = true);
    int  autoMapSlicesToEmptyPads(const std::vector<juce::String>& sliceIds, int startBank, int startPadInBank, bool overwriteOccupied = false);
    void clearPad(int flatPadIndex);
    void copyPad(int fromFlatIndex, int toFlatIndex);
    SourceColor getPadColor(int flatPadIndex) const;
    int         getPadSourceSlot(int flatPadIndex) const;

    // Pattern & 4-lane ArrangementView projection
    int64_t getPatternLengthTicks() const noexcept;
    void    addPatternEvent(PatternEvent ev, bool recordUndo = true);
    void    removePatternEvent(const juce::String& eventId);
    void    clearPattern();
    void    applyRecordQuantizeToEvent(PatternEvent& ev) const;
    void    reapplyRecordQuantizeAll();
    int     getEventSourceLane(const PatternEvent& ev) const; // returns 0..3 or -1

    // External HPDG Drum Guide (read-only overlay, NEVER a 5th source lane)
    void setExternalDrumGuide(std::optional<hpbridge::DrumGuideSnapshot> guide);
    const std::optional<hpbridge::DrumGuideSnapshot>& getExternalDrumGuide() const noexcept { return externalDrumGuide; }
    bool isDrumGuideFrozen() const noexcept { return drumGuideFrozen; }
    void setDrumGuideFrozen(bool frozen) noexcept { drumGuideFrozen = frozen; }

    // Build validated HPBridge SamplerContextSnapshot from MAIN MIX or ACTIVE SRC
    hpbridge::SamplerContextSnapshot buildSamplerContextSnapshot(hpbridge::ContextScope scope, int activeSourceSlot) const;

    void bumpRevision() noexcept { ++current.revision; }

private:
    ProjectSnapshot                                              current;
    std::vector<ProjectSnapshot>                                 undoStack;
    std::vector<ProjectSnapshot>                                 redoStack;
    std::array<std::vector<SliceCandidate>, kNumVisibleSources>  candidates;
    std::optional<hpbridge::DrumGuideSnapshot>                   externalDrumGuide;
    bool                                                         drumGuideFrozen = false;
};
} // namespace hps
