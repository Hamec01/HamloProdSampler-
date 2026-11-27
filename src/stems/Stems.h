#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Project.h"

namespace hps
{
// ============================================================================================
// Stems (optional): HPStems.exe splits a source into DRUMS / BASS / VOX / INST with HTDemucs v4
// in its own process (a crash or a 3 GB peak never touches the DAW). Results are cached on disk
// (Documents/HamloProd/HPSAMPLER/Stems/<key>) and reused; the project stores only the mix.
// ============================================================================================

enum StemIndex : int { kStemDrums = 0, kStemBass = 1, kStemVox = 2, kStemInst = 3 };
juce::String stemLabel(int stem);        // "DRUMS" ...
juce::String stemFileName(int stem);     // HTDemucs names: drums, bass, vocals, other

struct StemSet
{
    juce::String sourceId;
    double sampleRate = 44100.0;
    std::array<std::shared_ptr<const juce::AudioBuffer<float>>, kNumStems> stems;
    std::shared_ptr<const SourceNotes> notes; // notes.json of the cache (null = none)
};

// notes.json (hp.notes.v1) -> notes incl. the bass line; null when missing / unreadable.
std::shared_ptr<SourceNotes> readStemNotes(const juce::File& notesJson);
// One bass line from the BASS stem's notes: low register only, octave ghosts dropped, monophonic
// (a new attack ends the sounding note).
std::vector<SourceNote> bassLineOf(std::vector<SourceNote> notes);
// The BASS stem can be heard in this slot's mix (its notes count for HPDG).
bool bassStemAudible(const SourceSlot& slot);

// Mixed audio of a source: sum of the stems at the slot's levels (message thread or worker).
std::shared_ptr<juce::AudioBuffer<float>> mixStems(const StemSet& set, const SourceSlot& slot, int64_t frames);

class StemSeparator
{
public:
    enum class State { None, Cached, Separating, Loading, Ready, Failed };

    StemSeparator();
    ~StemSeparator();

    // Where the helper is: $HPSTEMS_DIR, <plug-in dir>/HPStems, the VST3 bundle's Resources/HPStems,
    // Documents/HamloProd/HPSAMPLER/HPStems. Empty when the engine is not installed.
    static juce::File helperExe();
    static juce::File installHint();      // the folder the warning names
    static juce::File cacheDirFor(const SourceSlot& slot, const SourceData& data);
    static bool hasCache(const juce::File& dir);

    // Split (writes the decoded source, runs HPStems, then loads). Replaces any job of this slot.
    void separate(int slot, std::shared_ptr<const SourceData> data, const juce::File& cacheDir);
    // Load stems a previous run left in the cache.
    void load(int slot, std::shared_ptr<const SourceData> data, const juce::File& cacheDir);
    void cancel(int slot);
    void forget(int slot);                // source replaced / cleared

    State state(int slot) const;
    float progress(int slot) const;       // 0..1 while separating
    juce::String error(int slot) const;
    juce::String sourceIdOf(int slot) const;
    std::shared_ptr<const StemSet> stems(int slot) const;

    // message thread
    std::function<void(int slot)> onChanged;

private:
    struct Job;
    void start(int slot, std::shared_ptr<const SourceData> data, const juce::File& cacheDir, bool split);
    std::array<std::unique_ptr<Job>, kNumSources> jobs;
    std::array<std::shared_ptr<const StemSet>, kNumSources> ready;
    std::array<State, kNumSources> states {};
    std::array<juce::String, kNumSources> errors, ids;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};
} // namespace hps
