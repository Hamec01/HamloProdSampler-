#include "app/PluginEditor.h"

namespace hps
{
HPSamplerAudioProcessorEditor::HPSamplerAudioProcessorEditor(HPSamplerAudioProcessor& p)
    : AudioProcessorEditor(&p),
      processor(p),
      sourceDisplay(p),
      mainDisplay(p),
      padGrid(p),
      padInspector(p),
      connectPanel(p)
{
    setSize(1120, 720);
    setResizable(true, true);
    setResizeLimits(880, 560, 1920, 1200);

    auto styleBtn = [](juce::TextButton& b, juce::Colour bg = juce::Colour(0xff23272e)) {
        b.setColour(juce::TextButton::buttonColourId, bg);
        b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe6edf3));
    };

    styleBtn(mainBtn);
    addAndMakeVisible(mainBtn);
    mainBtn.onClick = [this] { setNavTarget(NavTarget::Main); };

    for (int i = 0; i < kNumVisibleSources; ++i)
    {
        srcBtns[static_cast<size_t>(i)].setButtonText("SRC " + juce::String(i + 1));
        styleBtn(srcBtns[static_cast<size_t>(i)]);
        addAndMakeVisible(srcBtns[static_cast<size_t>(i)]);
        srcBtns[static_cast<size_t>(i)].onClick = [this, i] {
            const auto target = static_cast<NavTarget>(static_cast<int>(NavTarget::Src1) + i);
            // Clicking active SRC toggles back to MAIN (Spec #4)
            setNavTarget(navTarget == target ? NavTarget::Main : target);
        };
    }

    addAndMakeVisible(sourceDisplay);
    addChildComponent(mainDisplay);
    addAndMakeVisible(padGrid);

    for (auto* b : { &bpmBtn, &keyBtn, &gridBtn, &syncBtn,
                     &playBtn, &stopBtn, &recBtn, &quantBtn, &padEditBtn, &connectBtn, &undoBtn, &redoBtn,
                     &modeBtn, &overlapBtn })
    {
        styleBtn(*b);
        addAndMakeVisible(*b);
    }

    const char* bankNames[4] = { "BANK A", "BANK B", "BANK C", "BANK D" };
    for (int b = 0; b < kNumBanks; ++b)
    {
        bankBtns[static_cast<size_t>(b)].setButtonText(bankNames[b]);
        styleBtn(bankBtns[static_cast<size_t>(b)]);
        addAndMakeVisible(bankBtns[static_cast<size_t>(b)]);
        bankBtns[static_cast<size_t>(b)].onClick = [this, b] { padGrid.setBank(b); refreshGlobalDisplays(); };
    }

    // 4 Global Parameter Display Buttons
    bpmBtn.onClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "80 BPM (Boom Bap)");
        m.addItem(2, "90 BPM (Underground)");
        m.addItem(3, "120 BPM (House/Anchor)");
        m.addItem(4, "140 BPM (Trap/Drill)");
        m.addItem(5, "170 BPM (Jungle/DnB)");
        m.addSeparator();
        m.addItem(10, processor.getProject().state().hostSyncEnabled ? "Host Sync: ON (Click to disable)" : "Host Sync: OFF (Click to enable)");
        m.showMenuAsync(juce::PopupMenu::Options(), [this](int res) {
            auto& st = processor.getProject().mutableState();
            if (res == 1) st.projectBpm = 80.0;
            else if (res == 2) st.projectBpm = 90.0;
            else if (res == 3) st.projectBpm = 120.0;
            else if (res == 4) st.projectBpm = 140.0;
            else if (res == 5) st.projectBpm = 170.0;
            else if (res == 10) st.hostSyncEnabled = !st.hostSyncEnabled;
            processor.getProject().bumpRevision();
            processor.publishProjectToAudio();
            refreshGlobalDisplays();
        });
    };

    keyBtn.onClick = [this] {
        juce::PopupMenu m;
        static constexpr const char* kNotes[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        m.addItem(1, "Unknown / Off");
        for (int i = 0; i < 12; ++i) m.addItem(10 + i, juce::String(kNotes[i]) + " minor");
        for (int i = 0; i < 12; ++i) m.addItem(30 + i, juce::String(kNotes[i]) + " major");
        m.showMenuAsync(juce::PopupMenu::Options(), [this](int res) {
            auto& k = processor.getProject().mutableState().projectKey;
            if (res == 1) { k.tonicPitchClass.reset(); k.origin = hpbridge::ContextOrigin::Unknown; }
            else if (res >= 10 && res < 22) { k.tonicPitchClass = res - 10; k.mode = "minor"; k.confidence = 1.0f; k.origin = hpbridge::ContextOrigin::UserOverride; }
            else if (res >= 30 && res < 42) { k.tonicPitchClass = res - 30; k.mode = "major"; k.confidence = 1.0f; k.origin = hpbridge::ContextOrigin::UserOverride; }
            processor.getProject().bumpRevision();
            refreshGlobalDisplays();
        });
    };

    gridBtn.onClick = [this] {
        auto& g = processor.getProject().mutableState().gridDivision;
        g = static_cast<GridDivision>((static_cast<int>(g) + 1) % 7);
        refreshGlobalDisplays();
    };

    syncBtn.onClick = [this] {
        auto& s = processor.getProject().mutableState().globalSyncMode;
        s = static_cast<SyncMode>((static_cast<int>(s) + 1) % 3);
        processor.publishProjectToAudio();
        refreshGlobalDisplays();
    };

    // Transport & Commands
    playBtn.onClick = [this] { processor.getVoiceEngine().setPlaying(true); refreshGlobalDisplays(); };
    stopBtn.onClick = [this] {
        processor.getVoiceEngine().setPlaying(false);
        processor.getVoiceEngine().setRecording(false);
        processor.getVoiceEngine().requestAllNotesOff();
        processor.getVoiceEngine().seekToTick(0);
        refreshGlobalDisplays();
    };
    recBtn.onClick = [this] {
        const bool next = !processor.getVoiceEngine().isRecording();
        processor.getVoiceEngine().setRecording(next);
        if (next) processor.getVoiceEngine().setPlaying(true);
        refreshGlobalDisplays();
    };
    quantBtn.onClick = [this] {
        auto& st = processor.getProject().mutableState();
        st.recordQuantizeOn = !st.recordQuantizeOn;
        processor.getProject().reapplyRecordQuantizeAll();
        processor.publishProjectToAudio();
        refreshGlobalDisplays();
    };
    padEditBtn.onClick = [this] {
        const bool next = !padGrid.isEditSelectMode();
        padGrid.setEditSelectMode(next);
        padInspector.setVisible(next);
        if (next) padInspector.inspectPad(padGrid.getSelectedPad());
        refreshGlobalDisplays();
    };
    connectBtn.onClick = [this] {
        connectPanel.setVisible(!connectPanel.isVisible());
        if (connectPanel.isVisible()) connectPanel.refreshPeers();
    };
    undoBtn.onClick = [this] { if (processor.getProject().undo()) { processor.publishProjectToAudio(); refreshGlobalDisplays(); repaint(); } };
    redoBtn.onClick = [this] { if (processor.getProject().redo()) { processor.publishProjectToAudio(); refreshGlobalDisplays(); repaint(); } };

    modeBtn.onClick = [this] {
        auto& pm = processor.getProject().mutableState().globalPlayback;
        pm = (pm == PlaybackMode::OneShot ? PlaybackMode::Gate : (pm == PlaybackMode::Gate ? PlaybackMode::Toggle : PlaybackMode::OneShot));
        processor.publishProjectToAudio();
        refreshGlobalDisplays();
    };
    overlapBtn.onClick = [this] {
        auto& ov = processor.getProject().mutableState().globalOverlap;
        ov = static_cast<OverlapPolicy>((static_cast<int>(ov) + 1) % 6);
        processor.publishProjectToAudio();
        refreshGlobalDisplays();
    };

    // Wiring callbacks
    sourceDisplay.onStateChanged = [this] { refreshGlobalDisplays(); padGrid.repaint(); mainDisplay.repaint(); };
    sourceDisplay.onAssignModeRequested = [this] { padGrid.setAssignPendingMode(!padGrid.isAssignPendingMode()); };

    padGrid.onPadSelectedForEdit = [this](int flat) {
        padInspector.inspectPad(flat);
        padInspector.setVisible(true);
        mainDisplay.setSelectedPadForDraw(flat);
    };
    padGrid.onAssignRequestedToPad = [this](int flat) {
        auto sl = sourceDisplay.createSliceFromSelection();
        if (sl.id.isNotEmpty())
        {
            processor.getProject().assignSliceToPad(sl.id, flat, true);
            processor.publishProjectToAudio();
        }
        padGrid.setAssignPendingMode(false);
        sourceDisplay.repaint();
        padGrid.repaint();
        mainDisplay.repaint();
    };
    padGrid.onLocatePadInSource = [this](int flat) {
        const auto& pad = processor.getProject().getPad(flat);
        if (!pad.assignment.has_value()) return;
        const int slot = processor.getProject().findSourceSlotById(pad.assignment->sourceId);
        if (slot < 0) return;
        setNavTarget(static_cast<NavTarget>(static_cast<int>(NavTarget::Src1) + slot));
        if (const auto* sl = processor.getProject().getSliceById(pad.assignment->sliceId))
            sourceDisplay.focusRange(sl->startFrame, sl->endFrameExclusive);
    };
    padGrid.onPadsMutated = [this] { sourceDisplay.repaint(); mainDisplay.repaint(); };

    padInspector.onLocateInSource = padGrid.onLocatePadInSource;
    padInspector.onClosed = [this] { padGrid.setEditSelectMode(false); refreshGlobalDisplays(); };
    padInspector.onPadModified = [this] { padGrid.repaint(); mainDisplay.repaint(); };
    connectPanel.onStatusChanged = [this] { refreshGlobalDisplays(); mainDisplay.repaint(); };

    addChildComponent(padInspector);
    addChildComponent(connectPanel);

    processor.onProjectChanged = [this] {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<HPSamplerAudioProcessorEditor>(this)] {
            if (safe != nullptr) { safe->refreshGlobalDisplays(); safe->repaint(); }
        });
    };

    setNavTarget(NavTarget::Src1);
    refreshGlobalDisplays();
    startTimerHz(10);
}

HPSamplerAudioProcessorEditor::~HPSamplerAudioProcessorEditor()
{
    processor.onProjectChanged = nullptr;
    stopTimer();
}

void HPSamplerAudioProcessorEditor::setNavTarget(NavTarget target)
{
    navTarget = target;
    const bool isMain = (navTarget == NavTarget::Main);
    mainDisplay.setVisible(isMain);
    sourceDisplay.setVisible(!isMain);
    if (!isMain)
        sourceDisplay.setSlotIndex(static_cast<int>(navTarget) - static_cast<int>(NavTarget::Src1));
    refreshGlobalDisplays();
    repaint();
}

void HPSamplerAudioProcessorEditor::refreshGlobalDisplays()
{
    const auto& st = processor.getProject().state();

    // Top nav colors
    mainBtn.setColour(juce::TextButton::buttonColourId, navTarget == NavTarget::Main ? juce::Colour(0xff3b82f6) : juce::Colour(0xff23272e));
    for (int i = 0; i < kNumVisibleSources; ++i)
    {
        const auto col = SourceColor::defaultForSlot(i).colour;
        const bool active = (navTarget == static_cast<NavTarget>(static_cast<int>(NavTarget::Src1) + i));
        srcBtns[static_cast<size_t>(i)].setColour(juce::TextButton::buttonColourId, active ? col.withMultipliedBrightness(0.65f) : juce::Colour(0xff23272e));
        const auto* src = processor.getProject().getSourceBySlot(i);
        srcBtns[static_cast<size_t>(i)].setButtonText("S" + juce::String(i + 1) + (src != nullptr ? (" " + src->name.substring(0, 8)) : ""));
    }

    bpmBtn.setButtonText("BPM " + juce::String(st.projectBpm, 1) + (st.hostSyncEnabled ? " [H]" : ""));
    keyBtn.setButtonText("KEY " + st.projectKey.label());
    gridBtn.setButtonText("GRID " + gridDivisionLabel(st.gridDivision));
    syncBtn.setButtonText(st.globalSyncMode == SyncMode::Off ? "SYNC OFF" : (st.globalSyncMode == SyncMode::Repitch ? "SYNC REPITCH" : "SYNC STRETCH"));

    playBtn.setColour(juce::TextButton::buttonColourId, processor.getVoiceEngine().isPlaying() ? juce::Colour(0xff2ea043) : juce::Colour(0xff23272e));
    recBtn.setColour(juce::TextButton::buttonColourId, processor.getVoiceEngine().isRecording() ? juce::Colour(0xffda3633) : juce::Colour(0xff23272e));
    quantBtn.setButtonText(st.recordQuantizeOn ? "QUANT ON" : "QUANT OFF");
    padEditBtn.setColour(juce::TextButton::buttonColourId, padGrid.isEditSelectMode() ? juce::Colour(0xffd29922) : juce::Colour(0xff23272e));
    padEditBtn.setButtonText(padGrid.isEditSelectMode() ? "EDIT SELECT" : "PAD EDIT");

    connectBtn.setButtonText("HPDG: " + hpbridge::toString(processor.getBridgeState()));
    connectBtn.setColour(juce::TextButton::buttonColourId,
        processor.getBridgeState() == hpbridge::BridgeConnectionState::GuideLinkedReadOnly ? juce::Colour(0xff1f6feb) : juce::Colour(0xff23272e));

    for (int b = 0; b < kNumBanks; ++b)
        bankBtns[static_cast<size_t>(b)].setColour(juce::TextButton::buttonColourId, padGrid.getBank() == b ? juce::Colour(0xff388bfd) : juce::Colour(0xff23272e));

    modeBtn.setButtonText(st.globalPlayback == PlaybackMode::Gate ? "GATE" : (st.globalPlayback == PlaybackMode::Toggle ? "TOGGLE" : "ONE SHOT"));
    static constexpr const char* kOverlapNames[6] = { "POLY/LAYER", "RETRIGGER", "CHOKE GRP", "MONO PAD", "SRC MONO", "GLOBAL MONO" };
    overlapBtn.setButtonText(kOverlapNames[static_cast<int>(st.globalOverlap)]);
}

void HPSamplerAudioProcessorEditor::timerCallback()
{
    refreshGlobalDisplays();
}

void HPSamplerAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff16191f)); // Matte graphite hardware box

    // Top bar brand
    g.setColour(juce::Colour(0xffe6edf3));
    g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    g.drawText("HPSAMPLER", 14, 10, 140, 26, juce::Justification::centredLeft);
    g.setColour(juce::Colour(0xff7d8590));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("HamloProd", 118, 12, 90, 22, juce::Justification::centredLeft);

    // Bottom status bar
    auto statusArea = getLocalBounds().removeFromBottom(22).reduced(12, 0);
    g.setColour(juce::Colour(0xff8b949e));
    g.setFont(juce::FontOptions(11.0f));
    const juce::String modeStr = padGrid.isEditSelectMode() ? "MODE: EDIT SELECT (Click pad to inspect)"
                               : padGrid.isAssignPendingMode() ? "MODE: ASSIGN (Click destination pad)"
                               : "MODE: PERFORM (Click/MIDI triggers immediately; NO double-click edit)";
    g.drawText(modeStr + "   |   Voices: " + juce::String(processor.getVoiceEngine().getActiveVoiceCount()),
               statusArea, juce::Justification::centredLeft);
}

void HPSamplerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(10);

    // 1. Top Navigation Row (MAIN | SRC 1..4)
    auto topBar = area.removeFromTop(32);
    topBar.removeFromLeft(200); // brand space
    const int navW = std::min(120, topBar.getWidth() / 5);
    mainBtn.setBounds(topBar.removeFromLeft(navW).reduced(2));
    for (int i = 0; i < kNumVisibleSources; ++i)
        srcBtns[static_cast<size_t>(i)].setBounds(topBar.removeFromLeft(navW).reduced(2));

    area.removeFromBottom(20); // status bar

    // 2. Bottom Pad Grid + Bank/Mode Strip
    const int padAreaH = getWidth() >= 1180 ? 112 : 176;
    auto padArea = area.removeFromBottom(padAreaH);
    padGrid.setBounds(padArea);

    auto bankRow = area.removeFromBottom(28);
    for (int b = 0; b < kNumBanks; ++b)
        bankBtns[static_cast<size_t>(b)].setBounds(bankRow.removeFromLeft(72).reduced(2));
    bankRow.removeFromLeft(12);
    modeBtn.setBounds(bankRow.removeFromLeft(96).reduced(2));
    overlapBtn.setBounds(bankRow.removeFromLeft(116).reduced(2));

    // 3. Command Row & 4 Global Parameter Displays
    auto cmdRow = area.removeFromBottom(30);
    playBtn.setBounds(cmdRow.removeFromLeft(56).reduced(2));
    stopBtn.setBounds(cmdRow.removeFromLeft(56).reduced(2));
    recBtn.setBounds(cmdRow.removeFromLeft(54).reduced(2));
    quantBtn.setBounds(cmdRow.removeFromLeft(88).reduced(2));
    cmdRow.removeFromLeft(8);
    padEditBtn.setBounds(cmdRow.removeFromLeft(96).reduced(2));
    connectBtn.setBounds(cmdRow.removeFromLeft(185).reduced(2));
    redoBtn.setBounds(cmdRow.removeFromRight(58).reduced(2));
    undoBtn.setBounds(cmdRow.removeFromRight(58).reduced(2));

    auto paramRow = area.removeFromBottom(32);
    const int pW = paramRow.getWidth() / 4;
    bpmBtn.setBounds(paramRow.removeFromLeft(pW).reduced(3, 2));
    keyBtn.setBounds(paramRow.removeFromLeft(pW).reduced(3, 2));
    gridBtn.setBounds(paramRow.removeFromLeft(pW).reduced(3, 2));
    syncBtn.setBounds(paramRow.reduced(3, 2));

    // 4. Large Central Display
    auto displayArea = area.reduced(0, 4);
    sourceDisplay.setBounds(displayArea);
    mainDisplay.setBounds(displayArea);

    // 5. Slide-out side panels (never cover pad grid!)
    padInspector.setBounds(displayArea.withLeft(displayArea.getRight() - 290));
    connectPanel.setBounds(displayArea.withLeft(displayArea.getRight() - 320));
}
} // namespace hps
