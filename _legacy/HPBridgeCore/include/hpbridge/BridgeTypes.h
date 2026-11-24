#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace hpbridge
{
inline constexpr const char* kSamplerContextSchemaV1 = "hp.sample-context.v1";
inline constexpr const char* kDrumGuideSchemaV1      = "hp.drum-guide.v1";
inline constexpr std::size_t kMaxPayloadBytes        = 512 * 1024;
inline constexpr std::size_t kMaxEvents              = 4096;
inline constexpr int         kDefaultTicksPerQuarter = 960;

enum class BridgeConnectionState
{
    Disconnected,
    Available,
    Connecting,
    Connected,
    GuideLinkedReadOnly, // Live read-only HPDG guide via HamloLink (honest status, not full 2-way handshake)
    Stale,
    Incompatible,
    Error
};

enum class ContextOrigin
{
    Unknown,
    Analysis,
    UserOverride
};

enum class ContextScope
{
    MainMix,
    ActiveSrc
};

enum class DrumLaneClass
{
    Kick,
    Snare,
    Hat,
    Perc,
    Other
};

struct TimebaseInfo
{
    int     ticksPerQuarter = kDefaultTicksPerQuarter;
    int64_t originTick      = 0;
};

struct TransportInfo
{
    double bpm              = 120.0;
    int    meterNumerator   = 4;
    int    meterDenominator = 4;
    bool   hostSync         = true;
    int    loopBars         = 2;
};

struct MusicalKeyInfo
{
    std::optional<int>   tonicPitchClass; // 0..11 (C..B), nullopt = Unknown
    juce::String         mode;            // "minor", "major", or empty
    std::optional<float> confidence;      // 0..1
    ContextOrigin        origin = ContextOrigin::Unknown;
};

struct SourceSummary
{
    juce::String          sourceId;
    juce::String          name;
    std::optional<double> originalBpm;
};

struct PatternEventDto
{
    int64_t            tick          = 0;
    int64_t            durationTicks = 0;
    juce::String       padId;
    juce::String       sliceId;
    juce::String       sourceId;
    float              velocity      = 1.0f;
    std::optional<int> pitchSemitones;
};

struct BeatGridDto
{
    std::vector<int64_t> beatTicks;
    std::vector<int64_t> downbeatTicks;
};

struct GrooveDto
{
    juce::String         subdivision = "1/16";
    float                swingAmount = 0.0f;
    std::vector<int32_t> microOffsetTicks;
};

struct AccentDto
{
    int64_t      tick     = 0;
    float        strength = 0.0f;
    juce::String band     = "mid"; // "low", "mid", "high"
};

struct HarmonySegmentDto
{
    int64_t              startTick = 0;
    int64_t              endTick   = 0;
    juce::String         chord;
    std::optional<int>   rootPitchClass;
    std::optional<float> confidence;
    ContextOrigin        origin = ContextOrigin::Unknown;
};

struct PhraseDto
{
    int64_t startTick = 0;
    int64_t endTick   = 0;
    float   energy    = 0.5f;
};

struct LowEndDto
{
    int64_t            gridStepTicks = 240;
    std::vector<float> occupancy;
};

struct SamplerContextSnapshot
{
    juce::String schema = kSamplerContextSchemaV1;
    int64_t      revision = 1;
    juce::String projectId;
    juce::String samplerInstanceId;
    ContextScope scope = ContextScope::MainMix;

    TimebaseInfo              timebase;
    TransportInfo             transport;
    MusicalKeyInfo            key;
    std::vector<SourceSummary> sources;

    int                          patternBars = 2;
    std::vector<PatternEventDto> patternEvents;

    BeatGridDto                    beatGrid;
    GrooveDto                      groove;
    std::vector<AccentDto>         accents;
    std::vector<HarmonySegmentDto> harmony;
    std::vector<PhraseDto>         phrases;
    LowEndDto                      lowEnd;

    int          algorithmVersion = 1;
    juce::String unknownPolicy    = "null";
};

struct DrumGuideEventDto
{
    int64_t       tick      = 0;
    DrumLaneClass laneClass = DrumLaneClass::Other;
    float         velocity  = 0.8f;
    float         accent    = 0.8f;
};

struct DrumGuideSnapshot
{
    juce::String schema = kDrumGuideSchemaV1;
    int64_t      revision = 1;
    juce::String hpdgInstanceId;
    juce::String patternId;

    TimebaseInfo                   timebase;
    TransportInfo                  transport;
    BeatGridDto                    beatGrid;
    std::vector<DrumGuideEventDto> drumEvents;
    GrooveDto                      groove;
    std::vector<PhraseDto>         phrases;
    std::optional<juce::String>    audioReference;
};

struct BridgePeerInfo
{
    juce::String          instanceId;
    juce::String          displayName;
    juce::String          pluginType; // "HPDG" or "HPSAMPLER"
    double                bpm   = 0.0;
    bool                  alive = false;
    bool                  readOnlyLink = false;
    juce::File            backingFile;
};

struct ValidationResult
{
    bool         ok = true;
    juce::String error;

    static ValidationResult success() { return { true, {} }; }
    static ValidationResult failure(juce::String msg) { return { false, std::move(msg) }; }
};

class IHPBridgeTransport
{
public:
    virtual ~IHPBridgeTransport() = default;

    virtual BridgeConnectionState getState() const noexcept = 0;
    virtual std::vector<BridgePeerInfo> discoverPeers() = 0;

    virtual ValidationResult publishSamplerContext(const SamplerContextSnapshot& snapshot,
                                                   const juce::File& targetOrHint = {}) = 0;
    virtual ValidationResult loadSamplerContext(const juce::File& source,
                                                SamplerContextSnapshot& outSnapshot) = 0;

    virtual ValidationResult publishDrumGuide(const DrumGuideSnapshot& snapshot,
                                              const juce::File& targetOrHint = {}) = 0;
    virtual ValidationResult loadDrumGuide(const juce::File& source,
                                           DrumGuideSnapshot& outSnapshot) = 0;
};

juce::String toString(BridgeConnectionState state);
juce::String toString(ContextOrigin origin);
ContextOrigin contextOriginFromString(const juce::String& s);
juce::String toString(ContextScope scope);
ContextScope contextScopeFromString(const juce::String& s);
juce::String toString(DrumLaneClass lane);
DrumLaneClass drumLaneClassFromString(const juce::String& s);
} // namespace hpbridge
