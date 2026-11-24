#pragma once

#include "core/Project.h"
#include "hpbridge/BridgeTypes.h"

namespace hps
{
// Drum guide as HPSAMPLER shows it: an overlay over the 4 source lanes, never a fifth lane.
struct GuideOverlay
{
    bool present = false;
    juce::String origin;   // "file" / "HamloLink: <instance>"
    int64_t revision = 0;
    double bpm = 0.0;
    int loopBars = 0;
    struct Hit { int64_t tick; hpbridge::DrumLaneClass lane; float velocity; };
    std::vector<Hit> hits; // ticks in 960 PPQ from the guide origin
};

struct BridgeAdapter
{
    // MAIN mix context: project tempo / key, the actual pattern, accents from the arranged hits.
    static hpbridge::SamplerContextSnapshot buildSamplerContext(const Project& project, int64_t revision,
                                                                const juce::String& instanceId);
    static GuideOverlay fromDrumGuide(const hpbridge::DrumGuideSnapshot& g, const juce::String& origin);
};
} // namespace hps
