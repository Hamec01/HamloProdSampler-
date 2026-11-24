#pragma once

#include <functional>
#include <memory>
#include <vector>

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

class PadRenderer : private juce::Thread
{
public:
    PadRenderer();
    ~PadRenderer() override;

    static PadRenderJob makeJob(const Project& project, int pad, double targetRate, PadSyncInfo* info = nullptr);
    static std::unique_ptr<PadSound> render(const PadRenderJob& job);

    // Queue a job; a newer job for the same pad replaces a pending one.
    void submit(PadRenderJob job);
    // Delivered on the message thread.
    std::function<void(int pad, std::unique_ptr<PadSound> sound)> onRendered;

private:
    void run() override;
    juce::CriticalSection lock;
    std::vector<PadRenderJob> pending;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};
} // namespace hps
