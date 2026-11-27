#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

namespace hps
{
inline constexpr int kNumSources   = 4;
inline constexpr int kNumBanks     = 4;
inline constexpr int kPadsPerBank  = 16;
inline constexpr int kNumPads      = kNumBanks * kPadsPerBank;
inline constexpr int kPPQ          = 960;
inline constexpr int kTicksPerBar  = kPPQ * 4; // 4/4 in v1
inline constexpr int kBaseMidiNote = 36;       // C1 = bank A pad 1
inline constexpr int kMaxChokeGroups = 8;
inline constexpr int kNumStems     = 4;        // DRUMS, BASS, VOX, INST (HTDemucs drums / bass / vocals / other)
inline constexpr float kStemMinDb  = -60.0f;   // at or below: the stem is off
inline constexpr float kStemMaxDb  = 12.0f;

// How a pad reacts to NoteOn / NoteOff.
enum class PlayMode : int { Gate = 0, OneShot = 1, Toggle = 2 };

// What a repeated hit on the SAME pad does.
enum class RepeatMode : int { Retrigger = 0, Layer = 1 };

// Cross-pad cut policy (explicit, never the default).
enum class MonoMode : int { Off = 0, PerSource = 1, All = 2 };

// How a pad follows the project tempo.
enum class SyncMode : int { Off = 0, Repitch = 1, Stretch = 2 };

// What sounds while the DAW transport runs.
enum class HostPlays : int { Both = 0, Main = 1, DawNotes = 2 };

enum class MetroSound : int { Click = 0, HiHat = 1, Rim = 2, Cowbell = 3 };

// When notes played while the DAW runs are written into MAIN by themselves.
enum class HostCapture : int { Off = 0, WhenRecording = 1, Always = 2 };

// The same pad within this distance is the same hit (a note recorded in the DAW and in MAIN).
inline constexpr int64_t kSameHitTicks = 60;

enum class GridDiv : int { Bar, Half, Quarter, Eighth, Sixteenth, ThirtySecond, EighthT, SixteenthT, Off };

inline int gridTicks(GridDiv g) noexcept
{
    switch (g)
    {
        case GridDiv::Bar:          return kTicksPerBar;
        case GridDiv::Half:         return kPPQ * 2;
        case GridDiv::Quarter:      return kPPQ;
        case GridDiv::Eighth:       return kPPQ / 2;
        case GridDiv::Sixteenth:    return kPPQ / 4;
        case GridDiv::ThirtySecond: return kPPQ / 8;
        case GridDiv::EighthT:      return kPPQ / 3;
        case GridDiv::SixteenthT:   return kPPQ / 6;
        case GridDiv::Off:          return 0;
    }
    return kPPQ / 4;
}

inline juce::String gridName(GridDiv g)
{
    switch (g)
    {
        case GridDiv::Bar: return "1 BAR";
        case GridDiv::Half: return "1/2";
        case GridDiv::Quarter: return "1/4";
        case GridDiv::Eighth: return "1/8";
        case GridDiv::Sixteenth: return "1/16";
        case GridDiv::ThirtySecond: return "1/32";
        case GridDiv::EighthT: return "1/8T";
        case GridDiv::SixteenthT: return "1/16T";
        case GridDiv::Off: return "OFF";
    }
    return {};
}

struct MusicalKey
{
    int root = -1; // -1 = unknown / off, 0 = C ... 11 = B
    int mode = 0;  // 0 = minor, 1 = major

    bool known() const noexcept { return root >= 0 && root < 12; }
    bool operator==(const MusicalKey& o) const noexcept { return root == o.root && (root < 0 || mode == o.mode); }

    juce::String label() const
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        if (!known()) return "--";
        return juce::String(names[root]) + (mode == 1 ? " maj" : " min");
    }
};

// Smallest transpose (semitones, -6..+5) that moves `from` onto `to`. Different modes are
// matched through the relative key (C major -> A minor = 0), because a chord's quality cannot
// be changed by transposition anyway.
inline int keyTransposeSemitones(MusicalKey from, MusicalKey to) noexcept
{
    if (!from.known() || !to.known()) return 0;
    int fromRoot = from.root;
    if (from.mode != to.mode)
        fromRoot = from.mode == 1 ? (from.root + 9) % 12 : (from.root + 3) % 12;
    int d = ((to.root - fromRoot) % 12 + 12) % 12;
    if (d > 5) d -= 12;
    return d;
}

inline juce::Colour sourceColour(int slot)
{
    switch (slot)
    {
        case 0: return juce::Colour(0xff4ea4ff); // blue
        case 1: return juce::Colour(0xfff2a93b); // amber
        case 2: return juce::Colour(0xffb27cff); // violet
        case 3: return juce::Colour(0xff3fd48c); // green
        default: return juce::Colour(0xff8a96a3);
    }
}

inline juce::String padLabel(int flatPad)
{
    const int bank = flatPad / kPadsPerBank;
    const int pad  = flatPad % kPadsPerBank;
    return juce::String::charToString(juce::juce_wchar('A' + bank)) + juce::String(pad + 1).paddedLeft('0', 2);
}

// One-knob filter: -1 .. 0 = low-pass 200 Hz .. off, 0 .. 1 = off .. high-pass 2 kHz.
inline void filterKnobToHz(float f, float& hpHz, float& lpHz) noexcept
{
    hpHz = 20.0f;
    lpHz = 20000.0f;
    if (f < -0.001f) lpHz = 20000.0f * std::pow(0.01f, -f);
    else if (f > 0.001f) hpHz = 20.0f * std::pow(100.0f, f);
}
inline float hzToFilterKnob(float hpHz, float lpHz) noexcept
{
    if (lpHz < 19900.0f) return std::log(lpHz / 20000.0f) / std::log(100.0f); // negative: low-pass side
    if (hpHz > 20.5f) return std::log(hpHz / 20.0f) / std::log(100.0f);
    return 0.0f;
}

inline double dbToGain(float db) noexcept { return db <= -60.0f ? 0.0 : std::pow(10.0, db / 20.0); }
} // namespace hps
