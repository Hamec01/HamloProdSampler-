#include "hpbridge/FileSnapshotTransport.h"
#include "hpbridge/BridgeValidator.h"

namespace hpbridge
{
FileSnapshotTransport::FileSnapshotTransport(juce::File exchangeDirectory)
    : exchangeDir(std::move(exchangeDirectory))
{
}

juce::File FileSnapshotTransport::defaultExchangeDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("HamloProd")
        .getChildFile("HPBridge");
}

std::vector<BridgePeerInfo> FileSnapshotTransport::discoverPeers()
{
    std::vector<BridgePeerInfo> peers;
    if (!exchangeDir.isDirectory())
        return peers;

    for (const auto& f : exchangeDir.findChildFiles(juce::File::findFiles, false, "*.json"))
    {
        BridgePeerInfo info;
        info.backingFile = f;
        info.displayName = f.getFileName();
        info.instanceId  = f.getFileNameWithoutExtension();
        info.pluginType  = f.getFileName().containsIgnoreCase("drum-guide") ? "HPDG" : "HPSAMPLER";
        info.alive       = true;
        info.readOnlyLink = true;
        peers.push_back(std::move(info));
    }
    return peers;
}

ValidationResult FileSnapshotTransport::publishSamplerContext(const SamplerContextSnapshot& snapshot,
                                                              const juce::File& targetOrHint)
{
    const auto check = BridgeValidator::validate(snapshot);
    if (!check.ok)
        return check;

    const auto json = BridgeValidator::toJson(snapshot);
    if (json.getNumBytesAsUTF8() > kMaxPayloadBytes)
        return ValidationResult::failure("Serialized SamplerContextSnapshot exceeds 512 KB");

    juce::File outFile = targetOrHint.getFullPathName().isNotEmpty()
        ? targetOrHint
        : exchangeDir.getChildFile("hp.sample-context.v1.json");

    outFile.getParentDirectory().createDirectory();
    if (!outFile.replaceWithText(json))
        return ValidationResult::failure("Failed to write file: " + outFile.getFullPathName());

    return ValidationResult::success();
}

ValidationResult FileSnapshotTransport::loadSamplerContext(const juce::File& source,
                                                           SamplerContextSnapshot& outSnapshot)
{
    if (!source.existsAsFile())
        return ValidationResult::failure("File does not exist: " + source.getFullPathName());
    if (source.getSize() > static_cast<juce::int64>(kMaxPayloadBytes))
        return ValidationResult::failure("Snapshot file exceeds 512 KB");

    return BridgeValidator::fromJson(source.loadFileAsString(), outSnapshot);
}

ValidationResult FileSnapshotTransport::publishDrumGuide(const DrumGuideSnapshot& snapshot,
                                                         const juce::File& targetOrHint)
{
    const auto check = BridgeValidator::validate(snapshot);
    if (!check.ok)
        return check;

    const auto json = BridgeValidator::toJson(snapshot);
    if (json.getNumBytesAsUTF8() > kMaxPayloadBytes)
        return ValidationResult::failure("Serialized DrumGuideSnapshot exceeds 512 KB");

    juce::File outFile = targetOrHint.getFullPathName().isNotEmpty()
        ? targetOrHint
        : exchangeDir.getChildFile("hp.drum-guide.v1.json");

    outFile.getParentDirectory().createDirectory();
    if (!outFile.replaceWithText(json))
        return ValidationResult::failure("Failed to write file: " + outFile.getFullPathName());

    return ValidationResult::success();
}

ValidationResult FileSnapshotTransport::loadDrumGuide(const juce::File& source,
                                                      DrumGuideSnapshot& outSnapshot)
{
    if (!source.existsAsFile())
        return ValidationResult::failure("File does not exist: " + source.getFullPathName());
    if (source.getSize() > static_cast<juce::int64>(kMaxPayloadBytes))
        return ValidationResult::failure("Drum guide file exceeds 512 KB");

    return BridgeValidator::fromJson(source.loadFileAsString(), outSnapshot);
}
} // namespace hpbridge
