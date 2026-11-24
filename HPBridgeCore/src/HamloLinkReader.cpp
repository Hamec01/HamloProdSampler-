#include "hpbridge/HamloLinkReader.h"
#include "hpbridge/BridgeValidator.h"

#include <cstring>

namespace hpbridge
{
namespace
{
constexpr std::uint16_t kHamloLinkMajor = 1;
constexpr int           kMaxSlotEvents  = 1024;
constexpr int           kMaxSlotRoles   = 32;
constexpr int           kRoleChars      = 24;
constexpr int           kNameChars      = 64;
constexpr juce::int64   kAliveMs        = 2000;

#pragma pack(push, 1)
struct RawEvent
{
    std::uint8_t lane = 0;
    std::uint8_t flags = 0;
    std::uint8_t velocity = 0;
    std::uint8_t roleId = 0;
    std::int32_t tick = 0;
    std::int32_t lengthTicks = 0;
};

struct RawSnapshot
{
    char          magic[8];
    std::uint16_t versionMajor;
    std::uint16_t versionMinor;
    std::uint32_t sequence;
    std::int64_t  heartbeatMs;
    std::uint64_t instanceId;
    char          instanceName[kNameChars];
    float         bpm;
    std::int32_t  genre;
    std::int32_t  substyle;
    float         swingPercent;
    float         timing, humanize, density, velocityAmount;
    std::int32_t  intensity;
    std::int32_t  keyRoot;
    std::int32_t  scaleMode;
    std::int32_t  bars;
    std::int32_t  phraseLengthBars;
    std::int32_t  seed;
    std::int32_t  generationCounter;
    std::int32_t  mutationCounter;
    std::int32_t  eventCount;
    RawEvent      events[kMaxSlotEvents];
    char          roleNames[kMaxSlotRoles][kRoleChars];
};
#pragma pack(pop)

bool readRawSlotFile(const juce::File& file, RawSnapshot& out)
{
    if (!file.existsAsFile() || file.getSize() < static_cast<juce::int64>(sizeof(RawSnapshot)))
        return false;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        juce::MemoryBlock mb;
        if (!file.loadFileAsData(mb) || mb.getSize() < sizeof(RawSnapshot))
            return false;

        std::memcpy(&out, mb.getData(), sizeof(RawSnapshot));
        if (std::memcmp(out.magic, "HAMLOLNK", 8) != 0)
            return false;
        if ((out.sequence & 1u) == 0u)
            return true;
    }
    return false;
}

DrumLaneClass mapHpdgLane(std::uint8_t lane) noexcept
{
    // HPDG TrackType order: 0 HiHat, 1 OpenHat, 2 Snare, 3 Clap, 4 Kick, 5 GhostKick, 6 Ride, 7 Cymbal, 8 Perc, 9 HatFx, 10 Sub808
    switch (lane)
    {
        case 4: case 5: return DrumLaneClass::Kick;
        case 2: case 3: return DrumLaneClass::Snare;
        case 0: case 1: case 6: case 7: case 9: return DrumLaneClass::Hat;
        case 8: return DrumLaneClass::Perc;
        default: return DrumLaneClass::Other;
    }
}
} // namespace

juce::File HamloLinkReader::linkDirectory()
{
   #if JUCE_WINDOWS
    // HPDG writes its link slots to %LOCALAPPDATA% (HamloLinkProtocol::linkDirectory), not to Roaming
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
   #else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
   #endif
        .getChildFile("HamloProd")
        .getChildFile("Link");
}

std::vector<BridgePeerInfo> HamloLinkReader::discoverLiveHpdgInstances(juce::int64 nowMs)
{
    std::vector<BridgePeerInfo> peers;
    const auto dir = linkDirectory();
    if (!dir.isDirectory())
        return peers;

    for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "hpdg-*.slot"))
    {
        RawSnapshot raw {};
        if (!readRawSlotFile(f, raw))
            continue;
        if (raw.versionMajor != kHamloLinkMajor)
            continue;

        const bool alive = raw.heartbeatMs > 0 && (nowMs - raw.heartbeatMs) <= kAliveMs;
        BridgePeerInfo info;
        info.instanceId   = "hpdg-" + juce::String::toHexString(static_cast<juce::int64>(raw.instanceId));
        info.displayName  = juce::String::fromUTF8(raw.instanceName, static_cast<int>(strnlen(raw.instanceName, kNameChars)));
        if (info.displayName.isEmpty()) info.displayName = "HPDG (" + info.instanceId + ")";
        info.pluginType   = "HPDG";
        info.bpm          = static_cast<double>(raw.bpm);
        info.alive        = alive;
        info.readOnlyLink = true;
        info.backingFile  = f;
        peers.push_back(std::move(info));
    }
    return peers;
}

ValidationResult HamloLinkReader::readDrumGuideFromSlot(const juce::File& slotFile,
                                                        DrumGuideSnapshot& outGuide,
                                                        juce::int64 nowMs)
{
    RawSnapshot raw {};
    if (!readRawSlotFile(slotFile, raw))
        return ValidationResult::failure("Unable to read valid HamloLink slot: " + slotFile.getFullPathName());

    if (raw.versionMajor != kHamloLinkMajor)
        return ValidationResult::failure("Incompatible HamloLink major version");

    if (raw.heartbeatMs <= 0 || (nowMs - raw.heartbeatMs) > kAliveMs)
        return ValidationResult::failure("HPDG slot is stale or closed");

    DrumGuideSnapshot g;
    g.schema         = kDrumGuideSchemaV1;
    g.revision       = std::max<int64_t>(1, static_cast<int64_t>(raw.generationCounter + raw.mutationCounter + 1));
    g.hpdgInstanceId = "hpdg-" + juce::String::toHexString(static_cast<juce::int64>(raw.instanceId));
    g.patternId      = "hpdg-gen-" + juce::String(raw.generationCounter) + "-mut-" + juce::String(raw.mutationCounter);

    g.timebase.ticksPerQuarter = 960;
    g.timebase.originTick      = 0;

    g.transport.bpm              = juce::jlimit(20.0, 300.0, static_cast<double>(raw.bpm));
    g.transport.meterNumerator   = 4;
    g.transport.meterDenominator = 4;
    g.transport.loopBars         = juce::jlimit(1, 64, static_cast<int>(raw.bars));

    const int64_t ticksPerBar = 960 * 4;
    const int64_t totalTicks  = ticksPerBar * g.transport.loopBars;
    for (int bar = 0; bar < g.transport.loopBars; ++bar)
    {
        g.beatGrid.downbeatTicks.push_back(bar * ticksPerBar);
        for (int beat = 0; beat < 4; ++beat)
            g.beatGrid.beatTicks.push_back(bar * ticksPerBar + beat * 960);
    }

    g.groove.subdivision = "1/16";
    g.groove.swingAmount = juce::jlimit(0.0f, 1.0f, (raw.swingPercent - 50.0f) / 25.0f);

    const int count = juce::jlimit(0, kMaxSlotEvents, static_cast<int>(raw.eventCount));
    for (int i = 0; i < count; ++i)
    {
        const auto& ev = raw.events[i];
        if (ev.lane == 10) // Sub808 is not a drum lane accent
            continue;
        const int64_t clampedTick = juce::jlimit<int64_t>(0, totalTicks, ev.tick);
        const float vel           = juce::jlimit(0.0f, 1.0f, static_cast<float>(ev.velocity) / 127.0f);
        const bool isAccent       = (ev.flags & 8u) != 0u;
        const bool isGhost        = (ev.flags & 1u) != 0u;
        const float accent        = isAccent ? 1.0f : (isGhost ? vel * 0.5f : vel);
        g.drumEvents.push_back({ clampedTick, mapHpdgLane(ev.lane), vel, accent });
    }

    g.phrases.push_back({ 0, totalTicks, 0.5f });

    const auto check = BridgeValidator::validate(g);
    if (!check.ok) return check;
    outGuide = std::move(g);
    return ValidationResult::success();
}
} // namespace hpbridge
