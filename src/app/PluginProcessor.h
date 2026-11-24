#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "analysis/ChopEngine.h"
#include "analysis/SourceAnalysis.h"
#include "audio/Engine.h"
#include "bridge/BridgeAdapter.h"
#include "hpbridge/BridgeSession.h"
#include "core/Project.h"
#include "render/PadRenderer.h"

namespace hps
{
// Non-undoable editor state that must survive closing / reopening the plugin window.
struct UiState
{
    int view = -1;                      // -1 = MAIN, 0..3 = SRC n
    int mainTab = 0;                    // 0 = PATTERN, 1 = RESULT
    int bank = 0;
    int selectedPad = 0;
    int editingPad = -1;                // pad whose range is being edited on its source wave
    enum class PadMode { Perform, Edit, Assign } padMode = PadMode::Perform;
    bool inspectorOpen = false;
    struct SrcView
    {
        int64_t selStart = -1, selEnd = -1; // selection in source frames (-1 = none)
        double viewStart = 0.0, viewLen = 0.0; // visible range in frames (0 = whole file)
        int display = 0;                    // 0 WAVE, 1 SPEC, 2 SPLIT
        std::vector<ChopCandidate> candidates;
        int selectedCandidate = -1;
    };
    std::array<SrcView, kNumSources> src;
    SnapMode snap = SnapMode::Transient;
    bool autoAudition = true;
    bool loopAudition = false;
    std::vector<juce::String> selectedEvents;
    bool drawTool = true;
    bool showGuide = true;
};

class HPSamplerProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    HPSamplerProcessor();
    ~HPSamplerProcessor() override;

    // AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "HPSAMPLER"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // ---------------- app API (message thread) ----------------
    Project project;
    Engine engine;
    UiState ui;
    GuideOverlay guide;

    struct Listener { virtual ~Listener() = default; virtual void projectChanged() = 0; virtual void statusChanged() {} };
    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

    void loadSource(int slot, const juce::File& file);
    void clearSource(int slot);
    bool isSourceLoading(int slot) const { return loading[(size_t) slot]; }
    SourceGrid gridForSource(int slot) const;

    // pads
    void assignToPad(int pad, int slot, int64_t start, int64_t end, const juce::String& name = {});
    int  assignToNextEmptyPad(int slot, int64_t start, int64_t end);
    int  mapCandidatesToEmptyPads(int slot, const std::vector<ChopCandidate>& c, int fromPad);
    void clearPad(int pad);
    void copyPad(int pad) { clipboard = project.state().pads[(size_t) pad]; hasClipboard = true; }
    void pastePad(int pad);
    void triggerPad(int pad, float velocity, int pointer); // GUI NoteOn (+ focus)
    void releasePad(int pad, int pointer);
    PadSyncInfo padSyncInfo(int pad) const;
    double padSoundSeconds(int pad) const;

    // transport / recording
    void togglePlay();
    void stop();
    void setRecordArmed(bool armed);
    bool isRecordArmed() const { return recordArmed; }
    void requantizeRecorded();

    // project tempo / key
    void useSourceAsAnchor(int slot);

    // export
    bool exportWav(const juce::File& f, juce::String& error);
    bool exportMidi(const juce::File& f, juce::String& error);
    void renderResult(); // RESULT view (offline, message thread is fine for short loops)
    std::shared_ptr<juce::AudioBuffer<float>> resultAudio;
    uint64_t resultRevision = 0;

    // bridge (live: one connection, both directions; file / HamloLink read-only stay as fallbacks)
    std::unique_ptr<hpbridge::BridgeSession> session;
    bool autoSyncContext = true;   // HPSAMPLER -> HPDG: send the MAIN context when it changes (never regenerates HPDG)
    bool followGuide = true;       // HPDG -> HPSAMPLER: accept new drum guides (off = Freeze Guide)
    void sendContextNow();
    juce::String bridgeStatus;
    bool exportSamplerContext(juce::File& written, juce::String& error);
    bool importDrumGuide(const juce::File& f, juce::String& error);
    std::vector<hpbridge::BridgePeerInfo> discoverHpdg();
    bool linkHpdgGuide(const hpbridge::BridgePeerInfo& peer, juce::String& error);
    void unlinkGuide();

    // status line
    juce::String status;
    void setStatus(const juce::String& s);

    const juce::String instanceId = juce::Uuid().toDashedString();

private:
    void timerCallback() override;
    void onProjectChanged();
    void syncEngine();
    void handleRecordedHits();
    std::unique_ptr<EngineSnapshot> buildSnapshot() const;
    void mutateNoUndo(const std::function<void(ProjectState&)>& fn);

    juce::ListenerList<Listener> listeners;
    SourceLoader loader;
    PadRenderer renderer;
    std::array<uint64_t, kNumPads> submittedKeys {};
    std::array<double, kNumPads> soundSeconds {};
    std::array<const void*, kNumSources> publishedPreview {};
    std::array<bool, kNumSources> loading {};
    double engineRate = 44100.0;
    bool recordArmed = false;
    struct PendingRec { uint32_t token; int pad; juce::String eventId; int64_t onTick; };
    std::vector<PendingRec> pendingRec;
    Pad clipboard;
    bool hasClipboard = false;
    juce::File linkedSlot;
    int64_t contextRevision = 0;
    int guidePoll = 0;
    bool contextDirty = true;
    juce::uint32 lastProjectChangeMs = 0;
    void updateBridgeName();
    juce::String bridgeName;
    std::array<bool, kNumSources> restoredSlot {}; // restored from DAW state: never auto-anchor
};
} // namespace hps
