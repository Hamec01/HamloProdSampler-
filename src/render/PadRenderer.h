#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <juce_events/juce_events.h>

#include "audio/Engine.h"
#include "core/Project.h"

namespace hps
{
// Everything a pad's rendered sound depends on.
struct PadRenderJob
{
    int pad = 0;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double sourceRate = 44100.0, targetRate = 44100.0;
    int64_t start = 0, end = 0;
    bool reverse = false;
    float fadeInMs = 1.0f, fadeOutMs = 4.0f;
    SyncMode sync = SyncMode::Stretch;
    double timeRatio = 1.0;   // output duration / input duration (Stretch)
    double speed = 1.0;       // playback-rate factor (Repitch)
    float transpose = 0.0f;   // semitones incl. fine and key follow (Off / Stretch)
    bool shareable() const;   // long sound: one render of the source serves all pads with the same settings
    uint64_t key() const;
};

// What the UI shows about a pad's sync (why it sounds the way it does).
struct PadSyncInfo
{
    SyncMode sync = SyncMode::Stretch;
    bool   tempoKnown = false;
    double sourceBpm = 0.0;
    double userSpeed = 1.0;
    double timeRatio = 1.0;
    int    keyShift = 0;      // automatic key-follow semitones
    float  totalTranspose = 0.0f;
    bool   keyFollowActive = false;
    bool   extreme = false;   // ratio outside 0.5 .. 2 -> artifacts likely
    double outputSeconds = 0.0;
};

class PadRenderer : private juce::AsyncUpdater
{
public:
    PadRenderer();
    ~PadRenderer() override;

    static PadRenderJob makeJob(const Project& project, int pad, double targetRate, PadSyncInfo* info = nullptr);
    static std::unique_ptr<PadSound> render(const PadRenderJob& job);
    // Whole source at the project tempo / key (for listening before anything is on a pad). Pad index = kNumPads + slot.
    static PadRenderJob makeFitJob(const Project& project, int slot, double targetRate);

    // Queue a job; a newer job for the same pad replaces a pending one.
    void submit(PadRenderJob job);
    // Delivered on the message thread.
    std::function<void(int pad, std::unique_ptr<PadSound> sound)> onRendered;

    // The pad the user plays is rendered first.
    void setFocus(int pad);
    bool isRendering(int pad) const;    // a newer sound of this pad is on its way
    bool isBusy() const;                // anything left, incl. the tails of long sounds
    void finish(int timeoutMs);         // message thread: wait and deliver (exports)

private:
    struct Master;
    struct Worker;
    struct Item { PadRenderJob job; uint64_t serial = 0; std::shared_ptr<Master> master; };
    struct Ready { int pad = 0; uint64_t serial = 0; std::unique_ptr<PadSound> sound; };
    static constexpr size_t kSlots = (size_t) (kNumPads + kNumSources);
    bool workOnce();
    void deliver();
    void handleAsyncUpdate() override { deliver(); }

    juce::CriticalSection lock;
    std::vector<Item> pending;
    std::vector<Ready> ready;
    std::vector<std::shared_ptr<Master>> masters;
    std::array<std::shared_ptr<Master>, kSlots> wanted;
    std::array<uint64_t, kSlots> submitted {}, delivered {};
    uint64_t serialCounter = 0;
    int focus = -1;
    juce::OwnedArray<Worker> workers;
};
} // namespace hps
