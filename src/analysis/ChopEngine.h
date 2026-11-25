#pragma once

#include "core/Project.h"

namespace hps
{
// A chop proposal on a source. Never a SliceDefinition by itself: the user previews / edits /
// removes candidates, then ACCEPTs (slices) or MAPs them to empty pads.
struct ChopCandidate
{
    int64_t start = 0, end = 0;  // source frames [start, end)
    float   score = 0.0f;        // 0..1
    juce::String origin;         // "Attack", "Tonal", "Phrase", "Beat", "Region", "Threshold", "Grid"
    int64_t foundEnd = 0;        // end the tool (or the user's drag) gave; `end` is what the LENGTH mode makes of it
    bool    manualEnd = false;   // the user dragged the end: LENGTH modes leave it alone
};

// How long a found chop sounds.
enum class LengthMode : int { Found = 0, ToNext = 1, ToEnd = 2 };

enum class SpotFocus { Hybrid, Attack, Tonal, Phrase };

struct SpotParams
{
    SpotFocus focus = SpotFocus::Hybrid;
    int    maxSpots = 16;
    double minSeconds = 0.12;
    double maxSeconds = 4.0;
    float  sensitivity = 0.5f; // 0..1, higher = more spots
};

// Snap modes for boundaries in the source view.
enum class SnapMode { Off, Grid, Transient, ZeroCross };

struct SourceGrid // musical grid of a source (from effective BPM / origin)
{
    double bpm = 0.0;     // 0 = no grid
    double origin = 0.0;  // seconds of beat 1
    double sampleRate = 44100.0;
    bool tentative = false; // no reliable source tempo: the grid is a guess (detected or project tempo)
    bool valid() const noexcept { return bpm > 20.0; }
    double framesPerBeat() const noexcept { return sampleRate * 60.0 / bpm; }
    double originFrame() const noexcept { return origin * sampleRate; }
};

class ChopEngine
{
public:
    static std::vector<ChopCandidate> findSpots(const SourceData& data, int64_t from, int64_t to,
                                                const SpotParams& params, const SourceGrid& grid);

    // SLICER — Beat: `count` consecutive slices of `beatsEach` beats from `from` (needs a grid).
    static std::vector<ChopCandidate> beatSlicer(const SourceData& data, int64_t from, int count,
                                                 double beatsEach, const SourceGrid& grid);
    // SLICER / CHOP Regions — equal time parts of [from, to).
    static std::vector<ChopCandidate> equalRegions(int64_t from, int64_t to, int count);
    // `count` equal parts of a musical length (the one closest to (to - from) / count), starting on the grid.
    static std::vector<ChopCandidate> equalRegionsOnGrid(int64_t from, int64_t to, int count, int64_t numFrames,
                                                         const SourceGrid& grid, double* beatsEach = nullptr);
    static void applyLength(std::vector<ChopCandidate>& c, LengthMode mode, int64_t numFrames);
    // Period of tapped beats (same unit as `taps`), 0 = not enough. A missed beat does not spoil the result.
    static double tapPeriod(const std::vector<double>& taps);
    // CHOP Threshold — attack-driven cut points inside [from, to).
    static std::vector<ChopCandidate> threshold(const SourceData& data, int64_t from, int64_t to,
                                                float sensitivity, double minSeconds);
    // CHOP BPM — cut on the source's musical grid inside [from, to).
    static std::vector<ChopCandidate> gridChop(const SourceData& data, int64_t from, int64_t to,
                                               const SourceGrid& grid, GridDiv div);

    // Boundary snapping (frame-accurate, independent of zoom).
    static int64_t snap(const SourceData& data, int64_t frame, SnapMode mode, const SourceGrid& grid,
                        GridDiv div, int64_t maxDistanceFrames);
    static int64_t nearestOnset(const SourceData& data, int64_t frame, int64_t maxDistance);
};
} // namespace hps
