#pragma once

#include "BridgeTypes.h"

namespace hpbridge
{
class BridgeValidator
{
public:
    static ValidationResult validate(const SamplerContextSnapshot& snapshot);
    static ValidationResult validate(const DrumGuideSnapshot& snapshot);

    static juce::String toJson(const SamplerContextSnapshot& snapshot);
    static juce::String toJson(const DrumGuideSnapshot& snapshot);

    static ValidationResult fromJson(const juce::String& jsonText, SamplerContextSnapshot& out);
    static ValidationResult fromJson(const juce::String& jsonText, DrumGuideSnapshot& out);
};
} // namespace hpbridge
