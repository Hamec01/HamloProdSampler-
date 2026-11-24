#pragma once

#include "BridgeTypes.h"

namespace hpbridge
{
// Read-only adapter for live HPDG (C:\DRUMENGINE) HamloLink slots in
// %LOCALAPPDATA%\HamloProd\Link\hpdg-*.slot.
// Converts HPDG's live seqlock snapshot into a validated hp.drum-guide.v1 DTO
// without modifying HPDG's binaries or faking a two-way handshake.
class HamloLinkReader
{
public:
    static juce::File linkDirectory();
    static std::vector<BridgePeerInfo> discoverLiveHpdgInstances(juce::int64 nowMs = juce::Time::currentTimeMillis());
    static ValidationResult readDrumGuideFromSlot(const juce::File& slotFile,
                                                  DrumGuideSnapshot& outGuide,
                                                  juce::int64 nowMs = juce::Time::currentTimeMillis());
};
} // namespace hpbridge
