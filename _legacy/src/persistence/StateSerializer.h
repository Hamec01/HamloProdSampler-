#pragma once

#include "model/SamplerModel.h"

namespace hps
{
struct LoadProjectResult
{
    bool                      ok = true;
    juce::String              error;
    std::vector<int>          missingSourceSlots; // slots whose original file needs Relink
};

class StateSerializer
{
public:
    static constexpr const char* kStateSchemaV1 = "hpsampler.project.v1";

    static juce::String serializeToJson(const SamplerProject& project);
    static LoadProjectResult deserializeFromJson(const juce::String& jsonText,
                                                 SamplerProject& project,
                                                 bool reloadAudioFiles = true);

    static bool loadAudioFileIntoSource(const juce::File& file, int slotIndex, SourceAsset& outAsset, juce::String* errorOut = nullptr);
};
} // namespace hps
