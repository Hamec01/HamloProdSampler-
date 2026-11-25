#include "app/PluginEditor.h"

#include <BinaryData.h>

namespace hps
{
void NavButton::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    const auto& st = proc.project.state();
    const bool isMain = slot < 0;
    const auto col = isMain ? theme::accent : st.sources[(size_t) slot].colour;
    g.setColour(active ? theme::panelHi : theme::panel);
    g.fillRoundedRectangle(r, 5.0f);
    if (hover) { g.setColour(col.withAlpha(0.3f)); g.fillRoundedRectangle(r, 5.0f); }
    g.setColour(active ? col : juce::Colours::black.withAlpha(0.5f));
    g.drawRoundedRectangle(r, 5.0f, active ? 2.0f : 1.0f);
    g.setColour(col);
    g.fillRoundedRectangle(r.getX() + 6, r.getBottom() - 5, r.getWidth() - 12, 2.5f, 1.0f);
    auto in = r.reduced(8, 3).toNearestInt();
    g.setFont(theme::ui(13.0f, true));
    g.setColour(active ? juce::Colours::white : theme::text);
    if (isMain)
    {
        g.drawText("MAIN", in, juce::Justification::centred);
        return;
    }
    const auto& s = st.sources[(size_t) slot];
    g.drawText("SRC " + juce::String(slot + 1), in.removeFromTop(16), juce::Justification::centredLeft);
    g.setFont(theme::ui(10.5f));
    g.setColour(theme::textDim);
    juce::String sub = s.empty() ? "drop file" : s.name;
    if (proc.isSourceLoading(slot)) sub = "analysing...";
    else if (!s.empty() && proc.project.sourceDataForSlot(slot) == nullptr) sub = "MISSING - relink";
    g.drawFittedText(sub, in, juce::Justification::centredLeft, 1, 0.8f);
    const int pads = (int) proc.project.padsUsingSource(slot).size();
    if (pads > 0)
    {
        g.setColour(col);
        g.setFont(theme::mono(10.0f, true));
        g.drawText(juce::String(pads) + " pads", r.reduced(8, 3).toNearestInt(), juce::Justification::topRight);
    }
}

// =============================================================================================

HPSamplerEditor::HPSamplerEditor(HPSamplerProcessor& p) : juce::AudioProcessorEditor(p), proc(p)
{
    setLookAndFeel(&lnf);
    juce::LookAndFeel::setDefaultLookAndFeel(&lnf);
    setWantsKeyboardFocus(true);

    addAndMakeVisible(mainBtn);
    mainBtn.onClick = [this] { showView(-1); };
    mainBtn.setTooltip("MAIN: the 4 source lanes of your pattern");
    for (int i = 0; i < kNumSources; ++i)
    {
        auto* b = srcBtns.add(new NavButton(proc, i));
        b->setTooltip("Show source " + juce::String(i + 1) + " (drop a file here to load it). Click again to go back to MAIN.");
        b->onClick = [this, i] { showView(proc.ui.view == i ? -1 : i); };
        addAndMakeVisible(b);
    }
    for (auto* b : { &undoBtn, &redoBtn, &bridgeBtn })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    undoBtn.onClick = [this] { proc.project.undo(); };
    redoBtn.onClick = [this] { proc.project.redo(); };
    bridgeBtn.setTooltip("HP Bridge to HPDG: export the MAIN context, import / link the HPDG drum guide");
    bridgeBtn.onClick = [this] { bridgeButtonClicked(); };

    addChildComponent(sourceView);
    addChildComponent(patternView);
    addAndMakeVisible(controls);
    addAndMakeVisible(padGrid);
    addChildComponent(inspector);
    for (auto* l : { &statusLabel, &bridgeLabel })
    {
        l->setFont(theme::ui(12.0f));
        l->setColour(juce::Label::textColourId, theme::textDim);
        addAndMakeVisible(*l);
    }
    bridgeLabel.setJustificationType(juce::Justification::centredRight);

    padGrid.onOpenInspector = [this] { editPad(proc.ui.selectedPad); };
    padGrid.onLocate = [this](int pad) { locatePad(pad); };
    inspector.onClose = [this] { openInspector(false); };
    inspector.onScopeChanged = [this] { padGrid.refresh(); if (sourceView.isVisible()) sourceView.refresh(); };
    padGrid.onEditBank = [this](int bank)
    {
        proc.ui.bank = bank;
        proc.ui.editBank = bank;
        proc.ui.editingPad = -1;
        openInspector(true);
        projectChanged();
    };
    inspector.onLocate = [this](int pad) { editPad(pad); };
    patternView.onShowSource = [this](int slot) { showView(slot); };

    proc.addListener(this);
    setResizable(true, true);
    setResizeLimits(1100, 700, 2600, 1600);
    setSize(1360, 860);
    showView(proc.ui.view);
    openInspector(proc.ui.inspectorOpen);
    projectChanged();
    statusChanged();
    startTimerHz(10);
}

HPSamplerEditor::~HPSamplerEditor()
{
    proc.removeListener(this);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

void HPSamplerEditor::showView(int view)
{
    proc.ui.view = view;
    mainBtn.active = view < 0;
    for (int i = 0; i < kNumSources; ++i) srcBtns[i]->active = view == i;
    sourceView.setVisible(view >= 0);
    patternView.setVisible(view < 0);
    if (view >= 0)
    {
        sourceView.setSlot(view);
        sourceView.grabKeyboardFocus();
    }
    else
    {
        proc.ui.editingPad = -1;
        proc.engine.stopPreview();
        patternView.refresh();
        patternView.grabKeyboardFocus();
    }
    mainBtn.repaint();
    for (auto* b : srcBtns) b->repaint();
}

void HPSamplerEditor::openInspector(bool open)
{
    proc.ui.inspectorOpen = open;
    if (!open && proc.ui.editBank >= 0) { proc.ui.editBank = -1; padGrid.refresh(); }
    inspector.setVisible(open);
    if (open) inspector.refresh();
    resized();
}

void HPSamplerEditor::editPad(int pad)
{
    proc.ui.selectedPad = pad;
    proc.ui.editBank = -1; // one pad
    padGrid.refresh();
    openInspector(true);
    if (!proc.project.state().pads[(size_t) pad].assigned) return;
    proc.ui.editingPad = pad;
    locatePad(pad);
}

void HPSamplerEditor::locatePad(int pad)
{
    const auto& p = proc.project.state().pads[(size_t) pad];
    if (!p.assigned) return;
    proc.ui.selectedPad = pad;
    showView(p.src);
    auto& v = proc.ui.src[(size_t) p.src];
    v.selStart = p.start;
    v.selEnd = p.end;
    const double len = (double) (p.end - p.start);
    v.viewLen = len * 1.6;
    v.viewStart = (double) p.start - len * 0.3;
    sourceView.refresh();
}

void HPSamplerEditor::projectChanged()
{
    undoBtn.setEnabled(proc.project.canUndo());
    redoBtn.setEnabled(proc.project.canRedo());
    undoBtn.setTooltip(proc.project.canUndo() ? "Undo " + proc.project.undoLabel() + "  (Ctrl+Z)" : "Nothing to undo");
    redoBtn.setTooltip(proc.project.canRedo() ? "Redo " + proc.project.redoLabel() + "  (Ctrl+Y)" : "Nothing to redo");
    if (sourceView.isVisible()) sourceView.refresh();
    if (patternView.isVisible()) patternView.refresh();
    controls.refresh();
    padGrid.refresh();
    if (inspector.isVisible()) inspector.refresh();
    mainBtn.repaint();
    for (auto* b : srcBtns) b->repaint();
}

void HPSamplerEditor::statusChanged()
{
    statusLabel.setText(proc.status, juce::dontSendNotification);
    bridgeLabel.setText(proc.bridgeStatus, juce::dontSendNotification);
    bridgeLabel.setColour(juce::Label::textColourId, proc.session->getState() == hpbridge::BridgeConnectionState::Connected ? theme::ok : theme::textDim);
    // the button shows the real connection state (never "connected" without a confirmed handshake)
    const auto st = proc.session->getState();
    const bool on = st == hpbridge::BridgeConnectionState::Connected;
    bridgeBtn.setButtonText(on ? "HPDG CONNECTED" : st == hpbridge::BridgeConnectionState::Connecting ? "CONNECTING..."
                            : st == hpbridge::BridgeConnectionState::Stale ? "HPDG STALE"
                            : hpdgFound > 0 ? "CONNECT HPDG (" + juce::String(hpdgFound) + ")" : "CONNECT HPDG");
    bridgeBtn.setTooltip(on ? "Connected to " + proc.session->getPartner().label() + " - click for options / Disconnect"
                            : hpdgFound == 1 ? "One HPDG is running: click to connect"
                            : hpdgFound > 1 ? "Several HPDG are running: click to choose one"
                            : "No HPDG with Connect support is running (insert HPDG, or click for file options)");
    bridgeBtn.setToggleState(on || st == hpbridge::BridgeConnectionState::Stale, juce::dontSendNotification);
    bridgeBtn.getProperties().set("led", (juce::int64) (on ? theme::ok : theme::warn).getARGB());
    padGrid.refresh();
    if (inspector.isVisible()) inspector.refresh();
    for (auto* b : srcBtns) b->repaint();
}

void HPSamplerEditor::timerCallback()
{
    // keep loading indicators fresh
    for (int i = 0; i < kNumSources; ++i) if (proc.isSourceLoading(i)) srcBtns[i]->repaint();
    // every 2 s: how many HPDG can be connected (shown on the button)
    if (++bridgeTicks >= 20)
    {
        bridgeTicks = 0;
        int n = 0;
        for (const auto& p : proc.session->discover()) if (!p.busy && !p.incompatible) ++n;
        if (n != hpdgFound) { hpdgFound = n; statusChanged(); }
    }
}

void HPSamplerEditor::bridgeButtonClicked()
{
    // One click connects when exactly one free HPDG is running; otherwise the menu lets you choose / disconnect.
    const auto st = proc.session->getState();
    if (st == hpbridge::BridgeConnectionState::Disconnected || st == hpbridge::BridgeConnectionState::Error)
    {
        std::vector<hpbridge::BridgePeer> free;
        for (const auto& p : proc.session->discover()) if (!p.busy && !p.incompatible) free.push_back(p);
        if (free.size() == 1)
        {
            proc.session->connectTo(free.front().id);
            proc.setStatus("Connecting to " + free.front().label() + "...");
            statusChanged();
            return;
        }
    }
    showBridgeMenu();
}

void HPSamplerEditor::showBridgeMenu()
{
    juce::PopupMenu m, live, files;
    auto& s = *proc.session;
    const auto st = s.getState();
    const bool connected = st == hpbridge::BridgeConnectionState::Connected;
    m.addSectionHeader(s.getStatusText());
    const auto bridgePeers = s.discover();
    if (st == hpbridge::BridgeConnectionState::Disconnected || st == hpbridge::BridgeConnectionState::Error)
    {
        for (int i = 0; i < (int) bridgePeers.size(); ++i)
        {
            const auto& p = bridgePeers[(size_t) i];
            m.addItem(200 + i, "Connect to " + p.label() + "  " + juce::String(p.bpm, 1) + " BPM" + (p.busy ? "  (busy)" : ""),
                      !p.busy && !p.incompatible);
        }
        if (bridgePeers.empty()) m.addItem(199, "No HPDG with Connect support is running", false);
    }
    else m.addItem(5, "Disconnect from " + s.getPartner().label());
    m.addSeparator();
    m.addItem(6, "Send MAIN context now  (rev " + juce::String(s.getPublishedRevision()) + ", HPDG has "
                     + juce::String(s.getPartnerAckRevision()) + ")", connected);
    m.addItem(7, "Auto-send context when MAIN changes (HPDG never regenerates by itself)", true, proc.autoSyncContext);
    m.addItem(8, "Follow HPDG drum guide (off = freeze the current guide)", true, proc.followGuide);
    m.addItem(3, "Remove guide overlay", proc.guide.present);
    // fallbacks: files and the read-only HPDG Link of older HPDG builds
    files.addItem(1, "Export MAIN context snapshot (hp.sample-context.v1)");
    files.addItem(2, "Import drum guide file (hp.drum-guide.v1)...");
    const auto peers = proc.discoverHpdg();
    for (int i = 0; i < (int) peers.size(); ++i)
        live.addItem(100 + i, peers[(size_t) i].displayName + "  " + juce::String(peers[(size_t) i].bpm, 1) + " BPM");
    if (peers.empty()) live.addItem(99, "no running HPDG found", false);
    files.addSubMenu("Read HPDG Link guide (read-only, any HPDG)", live);
    m.addSubMenu("Without connection", files);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&bridgeBtn), [this, peers, bridgePeers](int r)
    {
        juce::String err;
        if (r >= 200 && r - 200 < (int) bridgePeers.size())
        {
            proc.session->connectTo(bridgePeers[(size_t) (r - 200)].id);
            proc.setStatus("Connecting to " + bridgePeers[(size_t) (r - 200)].label() + "...");
        }
        else if (r == 5) proc.session->disconnect();
        else if (r == 6) proc.sendContextNow();
        else if (r == 7) proc.autoSyncContext = !proc.autoSyncContext;
        else if (r == 8) proc.followGuide = !proc.followGuide;
        else if (r == 1)
        {
            juce::File f;
            proc.setStatus(proc.exportSamplerContext(f, err) ? "Context written to " + f.getFullPathName() : "Export failed: " + err);
        }
        else if (r == 2)
        {
            chooser = std::make_unique<juce::FileChooser>("Import HPDG drum guide", juce::File(), "*.json");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
            {
                juce::String e;
                const auto f = fc.getResult();
                if (f.existsAsFile()) proc.setStatus(proc.importDrumGuide(f, e) ? "Drum guide imported" : "Guide rejected: " + e);
            });
        }
        else if (r == 3) proc.unlinkGuide();
        else if (r >= 100 && r - 100 < (int) peers.size())
            proc.setStatus(proc.linkHpdgGuide(peers[(size_t) (r - 100)], err) ? "HPDG drum guide linked (read-only, follows HPDG)" : "Link failed: " + err);
        statusChanged();
    });
}

// Space is one switch for the whole plug-in: anything the plug-in itself sounds stops, otherwise the visible view plays.
bool HPSamplerEditor::handleSpace()
{
    if (proc.stopEverything()) return true;
    if (proc.ui.view >= 0) { sourceView.playFromKeyboard(); return true; }
    if (proc.engine.hostDriven.load()) { proc.setStatus("The DAW is playing and leads MAIN - stop it in the DAW"); return false; } // the DAW gets the key
    proc.playMain();
    return true;
}

bool HPSamplerEditor::keyPressed(const juce::KeyPress& k)
{
    if (k.getModifiers().isCtrlDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
    {
        if (k.getModifiers().isShiftDown()) proc.project.redo(); else proc.project.undo();
        return true;
    }
    if (k.getModifiers().isCtrlDown() && (k.getKeyCode() == 'Y' || k.getKeyCode() == 'y')) { proc.project.redo(); return true; }
    if (k == juce::KeyPress::escapeKey && proc.ui.padMode == UiState::PadMode::Cut) { proc.ui.padMode = UiState::PadMode::Perform; padGrid.refresh(); return true; }
    if (k == juce::KeyPress::spaceKey) return handleSpace();
    // a click on a pad moves the keyboard focus away from the view: its keys still work
    if (proc.ui.view >= 0) { if (!sourceView.hasKeyboardFocus(true) && sourceView.keyPressed(k)) return true; }
    else if (!patternView.hasKeyboardFocus(true) && patternView.keyPressed(k)) return true;
    if (k == juce::KeyPress::escapeKey) { proc.ui.padMode = UiState::PadMode::Perform; padGrid.refresh(); return true; }
    return false;
}

void HPSamplerEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme::body);
    auto top = getLocalBounds().removeFromTop(48);
    g.setColour(theme::bodyLight);
    g.fillRect(top);
    // HamloProd mark (the white logo of the HamloProd plug-ins) + product name
    auto logo = top.removeFromLeft(200).reduced(12, 7);
    // NOT a function-level static: a static image outlives the editor and is destroyed when the host unloads the DLL,
    // under the loader lock - releasing its Direct2D resources there froze FL Studio (remove plug-in / quit).
    if (!logoImage.isValid())
        logoImage = juce::ImageFileFormat::loadFrom(BinaryData::hamloprod_logo_white_png, BinaryData::hamloprod_logo_white_pngSize);
    const auto& mark = logoImage;
    if (mark.isValid())
    {
        const auto area = logo.removeFromLeft(30).toFloat();
        g.setOpacity(0.92f);
        g.drawImage(mark, area, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
        g.setOpacity(1.0f);
    }
    g.setColour(theme::text);
    g.setFont(theme::ui(19.0f, true));
    g.drawText("HPSAMPLER", logo.withTrimmedLeft(8), juce::Justification::centredLeft);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawHorizontalLine(48, 0, (float) getWidth());
}

void HPSamplerEditor::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop(48);
    top.removeFromLeft(200);
    bridgeBtn.setBounds(top.removeFromRight(130).reduced(4, 8));
    redoBtn.setBounds(top.removeFromRight(64).reduced(3, 8));
    undoBtn.setBounds(top.removeFromRight(64).reduced(3, 8));
    top.removeFromRight(12);
    mainBtn.setBounds(top.removeFromLeft(90).reduced(3, 5));
    top.removeFromLeft(6);
    const int sw = juce::jmin(200, top.getWidth() / kNumSources);
    for (auto* b : srcBtns) b->setBounds(top.removeFromLeft(sw).reduced(3, 5));

    auto status = r.removeFromBottom(22).reduced(10, 0);
    bridgeLabel.setBounds(status.removeFromRight(420));
    statusLabel.setBounds(status);

    const int padH = juce::jlimit(190, 300, getHeight() / 3);
    padGrid.setBounds(r.removeFromBottom(padH));
    controls.setBounds(r.removeFromBottom(64));
    if (inspector.isVisible()) inspector.setBounds(r.removeFromRight(340));
    sourceView.setBounds(r);
    patternView.setBounds(r);
}
} // namespace hps
