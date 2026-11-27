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
#include "stems/Stems.h"

namespace hps
{
// Non-undoable editor state that must survive closing / reopening the plugin window.
struct UiState
{
    int view = -1;                      // -1 = MAIN, 0..3 = SRC n
    int mainTab = 0;                    // 0 = PATTERN, 1 = RESULT
    int bank = 0;
    int selectedPad = 0;
    int editBank = -1;                  // >= 0: PAD EDIT and the pad strip change every pad of this bank
    int editingPad = -1;                // pad whose range is being edited on its source wave
    enum class PadMode { Perform, Edit, Assign, Cut } padMode = PadMode::Perform;
    int cutPaint = 1;                   // PadMode::Cut: the group that clicked pads join
    bool inspectorOpen = false;
    struct SrcView
    {
        int64_t selStart = -1, selEnd = -1; // selection in source frames (-1 = none)
        double viewStart = 0.0, viewLen = 0.0; // visible range in frames (0 = whole file)
        int display = 0;                    // 0 WAVE, 1 SPEC, 2 SPLIT
        std::vector<ChopCandidate> candidates;
        int selectedCandidate = -1;
        int64_t rangeFrom = -1, rangeTo = -1; // range of the last chop run (a clicked candidate is not a new range)
    };
    std::array<SrcView, kNumSources> src;
    SnapMode snap = SnapMode::Transient;
    bool autoAudition = true;
    bool loopAudition = false;
    LengthMode chopLength = LengthMode::Found;
    bool cutThrough = true;             // pads that play to the end of the sample cut each other
    bool hearFitted = false;            // listen to sources at the project tempo / key
    bool showGrid = true;
    std::vector<juce::String> selectedEvents;
    // MAIN editing (FL Piano-roll tools) and view
    enum class Tool { Draw, Paint, StepPaint, Delete, Mute, Slice, Select, Zoom, Play };
    Tool tool = Tool::Draw;
    bool padRows = false;               // false = 4 source lanes, true = one row per pad
    bool showVelocity = true;
    bool followPlayhead = true;
    double mainStart = 0.0, mainTicks = 0.0; // visible time (0 ticks = the whole pattern)
    double mainVZoom = 1.0;
    int mainScrollY = 0;
    bool showGuide = true;
    int64_t mainCursorTick = 0;         // MAIN's marker (last click in the ruler / notes): PLAY starts here
    enum class AutoPlay { Off, Whole, Edge };
    AutoPlay autoPlay = AutoPlay::Whole; // SRC: what plays after the selection changed (Edge: 2 s at the moved edge)
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
    int  mapCandidatesToEmptyPads(int slot, const std::vector<ChopCandidate>& c, int fromPad, bool replace = false);
    // What PAD EDIT changes: the selected pad, or all assigned pads of ui.editBank. The last change wins, per parameter.
    std::vector<int> editTargets() const;
    int editRefPad() const;             // the pad whose values are shown
    juce::String editScopeName() const;
    void editPads(const juce::String& label, const std::function<void(PadSettings&, int pad)>& fn);
    // Quick cut groups. Pads that all are in the group already leave it.
    enum class CutScope { Bank, Source, All };
    std::vector<int> cutScopePads(CutScope scope) const;
    int setCutGroup(CutScope scope, int group, bool toggle);
    bool padPlaysToEnd(int pad) const;
    void togglePadToEnd(int pad);
    void listenersChanged() { listeners.call([](Listener& l) { l.projectChanged(); }); } // redraw after a UiState change
    void updateFit() { syncEngine(); }
    bool fitReady(int slot) const { return fitPublished[(size_t) slot] != 0; }
    void clearPad(int pad);
    int  clearBank(int bank);           // every pad of the bank and its MAIN notes, one undo step; returns pads cleared
    void copyPad(int pad) { clipboard = project.state().pads[(size_t) pad]; hasClipboard = true; }
    void pastePad(int pad);
    void triggerPad(int pad, float velocity, int pointer); // GUI NoteOn (+ focus)
    void releasePad(int pad, int pointer);
    PadSyncInfo padSyncInfo(int pad) const;
    double padSoundSeconds(int pad) const;
    bool isPadRendering(int pad) const { return renderer.isRendering(pad); }

    // transport / recording
    void togglePlay();
    void stop();
    // Space: whatever the plug-in itself sounds (audition, hand-played pads, MAIN) stops; returns false when nothing did
    bool stopEverything();
    void playMain(int64_t fromTick = -1); // -1 = from MAIN's marker; with REC armed and 1-2-3-4 on, a count-in first
    // DAW -> MAIN: notes of the last take (played while a transport ran) that MAIN does not have yet
    int  captureCount() const;
    void captureTake();
    struct NowPlaying { enum class Who { Nothing, Pad, Source, Main, Daw } who = Who::Nothing; juce::String title, detail; };
    NowPlaying nowPlaying() const;
    void setRecordArmed(bool armed);
    bool isRecordArmed() const { return recordArmed; }
    void requantizeRecorded();

    // project tempo / key
    void useSourceAsAnchor(int slot);

    // export
    bool exportWav(const juce::File& f, juce::String& error, double tailSeconds = 2.0, int onlySource = -1); // onlySource: one MAIN lane
    bool exportMidi(const juce::File& f, juce::String& error);
    // A file to drop into the DAW. It stays on disk (Documents/HamloProd/HPSAMPLER/Drag): the DAW project refers to it.
    juce::File writeDragFile(bool midi, juce::String& error, int onlySource = -1);
    void renderResult(); // RESULT view (offline, message thread is fine for short loops)
    std::shared_ptr<juce::AudioBuffer<float>> resultAudio;
    uint64_t resultRevision = 0;

    // bridge (live: one connection, both directions; file / HamloLink read-only stay as fallbacks)
    std::unique_ptr<hpbridge::BridgeSession> session;
    bool autoSyncContext = true;   // HPSAMPLER -> HPDG: send the MAIN context when it changes (never regenerates HPDG)
    bool followGuide = true;       // HPDG -> HPSAMPLER: accept new drum guides (off = Freeze Guide)
    void sendContextNow();
    void publishContext(hpbridge::SamplerContextSnapshot snap);
    juce::ThreadPool contextPool { juce::ThreadPoolOptions {}.withThreadName("HPSAMPLER context").withNumberOfThreads(1)
                                       .withDesiredThreadPriority(juce::Thread::Priority::low) };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    bool contextJobRunning = false;
    uint64_t contextGeneration = 0;
    juce::String bridgeStatus;
    bool exportSamplerContext(juce::File& written, juce::String& error);
    bool importDrumGuide(const juce::File& f, juce::String& error);
    std::vector<hpbridge::BridgePeerInfo> discoverHpdg();
    bool linkHpdgGuide(const hpbridge::BridgePeerInfo& peer, juce::String& error);
    void unlinkGuide();

    // stems (optional, HPStems.exe): DRUMS / BASS / VOX / INST, rebalanced with SourceSlot::stemDb
    StemSeparator stems;
    bool stemEngineInstalled() const { return StemSeparator::helperExe() != juce::File(); }
    bool stemsCached(int slot) const;           // a previous split of this exact file is on disk
    double stemEstimateSeconds(int slot) const; // rough CPU time of a split
    void startStems(int slot);                  // after the user agreed to the warning
    void loadCachedStems(int slot);
    void cancelStems(int slot);
    void deleteStems(int slot);                 // drops the cache and goes back to the original

    // status line
    juce::String status;
    void setStatus(const juce::String& s);

    const juce::String instanceId = juce::Uuid().toDashedString();
    const bool hostIsFL = juce::PluginHostType().isFruityLoops();

private:
    void timerCallback() override;
    void onProjectChanged();
    void syncEngine();
    void handleRecordedHits();
    void markPad(int pad, int slot, double frame); // slicing on the fly: an empty pad was hit during an audition
    int markSlot = -1, markPadIndex = -1, markCount = 0;
    std::unique_ptr<EngineSnapshot> buildSnapshot() const;
    void mutateNoUndo(const std::function<void(ProjectState&)>& fn);

    juce::ListenerList<Listener> listeners;
    SourceLoader loader;
    PadRenderer renderer;
    std::array<uint64_t, kNumPads> submittedKeys {};
    std::array<double, kNumPads> soundSeconds {};
    std::array<const void*, kNumSources> publishedPreview {};
    std::array<uint64_t, kNumSources> fitSubmitted {}, fitPublished {}; // 0 = the original is heard
    int throughCutGroup(const ProjectState& s, int slot, int64_t end) const; // 0 = no cut
    std::array<bool, kNumSources> loading {};
    double engineRate = 44100.0;
    bool recordArmed = false;
    struct PendingRec { uint32_t token; int pad; juce::String eventId; int64_t onTick; };
    bool hasSameHit(int pad, int64_t tick) const;
    PatternEvent eventFromHit(int pad, int64_t rawTick, float velocity) const;
    struct TakeNote { int pad; int64_t tick, length; float velocity; uint32_t token; bool open; };
    std::vector<TakeNote> take;
    uint32_t takePass = 0;
    bool dawWasPlaying = false, dawGesture = false;
    int dawRecorded = 0;
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
    // stems: the decoded source as loaded; the project's store holds the rebalanced copy while a mix is on
    std::array<std::shared_ptr<const SourceData>, kNumSources> loaderData;   // as decoded / analysed
    std::array<std::shared_ptr<const SourceData>, kNumSources> originalData; // loaderData + the stems' notes
    void refreshOriginal(int slot);
    std::array<uint64_t, kNumSources> stemMixApplied {}; // 0 = the original is published
    std::array<bool, kNumSources> stemMixBusy {};
    uint64_t stemMixKey(int slot) const;
    void updateStemMix(int slot);
    void stemsChanged(int slot);
    juce::ThreadPool stemMixPool { juce::ThreadPoolOptions {}.withThreadName("HPSAMPLER stem mix").withNumberOfThreads(1)
                                       .withDesiredThreadPriority(juce::Thread::Priority::low) };
};
} // namespace hps
