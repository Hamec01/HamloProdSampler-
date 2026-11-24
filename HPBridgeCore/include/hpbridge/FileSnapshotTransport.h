#pragma once

#include "BridgeTypes.h"

namespace hpbridge
{
class FileSnapshotTransport final : public IHPBridgeTransport
{
public:
    explicit FileSnapshotTransport(juce::File exchangeDirectory = defaultExchangeDirectory());

    static juce::File defaultExchangeDirectory();

    BridgeConnectionState getState() const noexcept override { return state; }
    std::vector<BridgePeerInfo> discoverPeers() override;

    ValidationResult publishSamplerContext(const SamplerContextSnapshot& snapshot,
                                           const juce::File& targetOrHint = {}) override;
    ValidationResult loadSamplerContext(const juce::File& source,
                                        SamplerContextSnapshot& outSnapshot) override;

    ValidationResult publishDrumGuide(const DrumGuideSnapshot& snapshot,
                                      const juce::File& targetOrHint = {}) override;
    ValidationResult loadDrumGuide(const juce::File& source,
                                   DrumGuideSnapshot& outSnapshot) override;

    juce::File getExchangeDirectory() const { return exchangeDir; }

private:
    juce::File            exchangeDir;
    BridgeConnectionState state = BridgeConnectionState::Disconnected;
};
} // namespace hpbridge
