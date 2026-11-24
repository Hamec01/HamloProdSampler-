#pragma once

#include "core/Project.h"

namespace hps
{
// hpsampler.project.v2 — JSON. Audio is referenced by path, never embedded.
struct Serialization
{
    static constexpr const char* kSchema = "hpsampler.project.v2";
    static juce::String toJson(const ProjectState& s);
    static bool fromJson(const juce::String& json, ProjectState& out, juce::String& error);

    // MIDI file of the pattern's pad triggers (one note per event, pad -> note kBaseMidiNote + pad).
    static juce::MidiFile patternToMidi(const ProjectState& s);
};
} // namespace hps
