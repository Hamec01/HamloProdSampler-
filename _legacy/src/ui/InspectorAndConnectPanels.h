#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/PluginProcessor.h"

namespace hps
{
// Slide-out Pad Inspector panel: SLICE / PLAY / PITCH / ENV / MIX / FX / MIDI
class PadInspectorPanel final : public juce::Component
{
public:
    explicit PadInspectorPanel(HPSamplerAudioProcessor& proc) : processor(proc)
    {
        auto cfgSlider = [this](juce::Slider& s, double lo, double hi, double step, const juce::String& suffix) {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 20);
            s.setRange(lo, hi, step);
            s.setTextValueSuffix(suffix);
            addAndMakeVisible(s);
        };

        cfgSlider(pitchSlider,  -24.0, 24.0, 0.5, " st");
        cfgSlider(gainSlider,   -24.0, 12.0, 0.5, " dB");
        cfgSlider(panSlider,    -1.0,  1.0,  0.05, "");
        cfgSlider(attackSlider,  0.0,  500.0, 1.0, " ms");
        cfgSlider(releaseSlider, 5.0,  2000.0, 5.0, " ms");
        cfgSlider(cutoffSlider,  80.0, 18000.0, 10.0, " Hz");

        playbackBox.addItem("Follow Global", 1);
        playbackBox.addItem("Gate", 2);
        playbackBox.addItem("One Shot", 3);
        playbackBox.addItem("Toggle", 4);
        addAndMakeVisible(playbackBox);

        chokeBox.addItem("Choke: Off", 1);
        for (int i = 1; i <= 4; ++i) chokeBox.addItem("Choke Group " + juce::String(i), i + 1);
        addAndMakeVisible(chokeBox);

        addAndMakeVisible(reverseBtn);
        addAndMakeVisible(loopBtn);
        addAndMakeVisible(filterBtn);
        addAndMakeVisible(locateBtn);
        addAndMakeVisible(closeBtn);

        auto apply = [this] { applyToSelectedPad(); };
        pitchSlider.onValueChange   = apply;
        gainSlider.onValueChange    = apply;
        panSlider.onValueChange     = apply;
        attackSlider.onValueChange  = apply;
        releaseSlider.onValueChange = apply;
        cutoffSlider.onValueChange  = apply;
        playbackBox.onChange        = apply;
        chokeBox.onChange           = apply;
        reverseBtn.onClick          = apply;
        loopBtn.onClick             = apply;
        filterBtn.onClick           = apply;
        locateBtn.onClick = [this] { if (onLocateInSource) onLocateInSource(flatPadIndex); };
        closeBtn.onClick  = [this] { setVisible(false); if (onClosed) onClosed(); };
    }

    void inspectPad(int flatIndex)
    {
        flatPadIndex = juce::jlimit(0, kTotalPads - 1, flatIndex);
        syncFromModel();
        repaint();
    }

    std::function<void(int)> onLocateInSource;
    std::function<void()>    onClosed;
    std::function<void()>    onPadModified;

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        auto top = r.removeFromTop(26);
        closeBtn.setBounds(top.removeFromRight(28));
        locateBtn.setBounds(top.removeFromRight(92).reduced(2, 0));
        r.removeFromTop(8);

        auto row = [&](juce::Component& c, int h = 24) { c.setBounds(r.removeFromTop(h)); r.removeFromTop(6); };
        row(playbackBox);
        row(chokeBox);
        auto toggles = r.removeFromTop(24);
        reverseBtn.setBounds(toggles.removeFromLeft(80));
        loopBtn.setBounds(toggles.removeFromLeft(70));
        filterBtn.setBounds(toggles);
        r.removeFromTop(8);
        row(pitchSlider);
        row(gainSlider);
        row(panSlider);
        row(attackSlider);
        row(releaseSlider);
        row(cutoffSlider);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff181c23));
        g.setColour(juce::Colour(0xff38404d));
        g.drawRect(getLocalBounds(), 1);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(juce::String::formatted("PAD %02d INSPECTOR", (flatPadIndex % kPadsPerBank) + 1),
                   10, 10, 140, 24, juce::Justification::centredLeft);
    }

private:
    void syncFromModel()
    {
        updating = true;
        const auto& pad = processor.getProject().getPad(flatPadIndex);
        if (pad.assignment.has_value())
        {
            const auto& s = pad.assignment->settings;
            pitchSlider.setValue(s.pitchSemitones, juce::dontSendNotification);
            gainSlider.setValue(s.gainDb, juce::dontSendNotification);
            panSlider.setValue(s.pan, juce::dontSendNotification);
            attackSlider.setValue(s.attackMs, juce::dontSendNotification);
            releaseSlider.setValue(s.releaseMs, juce::dontSendNotification);
            cutoffSlider.setValue(s.filterCutoffHz, juce::dontSendNotification);
            reverseBtn.setToggleState(s.reverse, juce::dontSendNotification);
            loopBtn.setToggleState(s.loop, juce::dontSendNotification);
            filterBtn.setToggleState(s.filterEnabled, juce::dontSendNotification);
            playbackBox.setSelectedId(s.playbackModeOverride.has_value() ? (static_cast<int>(*s.playbackModeOverride) + 2) : 1, juce::dontSendNotification);
            chokeBox.setSelectedId(s.chokeGroup + 1, juce::dontSendNotification);
        }
        updating = false;
    }

    void applyToSelectedPad()
    {
        if (updating) return;
        auto& pad = processor.getProject().getPad(flatPadIndex);
        if (!pad.assignment.has_value()) return;
        auto& s = pad.assignment->settings;
        s.pitchSemitones = static_cast<float>(pitchSlider.getValue());
        s.gainDb         = static_cast<float>(gainSlider.getValue());
        s.pan            = static_cast<float>(panSlider.getValue());
        s.attackMs       = static_cast<float>(attackSlider.getValue());
        s.releaseMs      = static_cast<float>(releaseSlider.getValue());
        s.filterCutoffHz = static_cast<float>(cutoffSlider.getValue());
        s.reverse        = reverseBtn.getToggleState();
        s.loop           = loopBtn.getToggleState();
        s.filterEnabled  = filterBtn.getToggleState();
        const int pbId   = playbackBox.getSelectedId();
        if (pbId <= 1) s.playbackModeOverride.reset();
        else s.playbackModeOverride = static_cast<PlaybackMode>(pbId - 2);
        s.chokeGroup = std::max(0, chokeBox.getSelectedId() - 1);

        processor.getProject().bumpRevision();
        processor.publishProjectToAudio();
        if (onPadModified) onPadModified();
    }

    HPSamplerAudioProcessor& processor;
    int  flatPadIndex = 0;
    bool updating     = false;

    juce::Slider pitchSlider, gainSlider, panSlider, attackSlider, releaseSlider, cutoffSlider;
    juce::ComboBox playbackBox, chokeBox;
    juce::ToggleButton reverseBtn { "Reverse" }, loopBtn { "Loop" }, filterBtn { "Filter FX" };
    juce::TextButton locateBtn { "Locate SRC" }, closeBtn { "X" };
};

// Connect to HPDG Panel:
// - Discovers live HPDG instances via HamloLinkReader (read-only guide)
// - Exports hp.sample-context.v1 JSON & imports hp.drum-guide.v1 JSON (FileSnapshotTransport)
// - Honest connection status (never fakes two-way Connected without handshake)
class ConnectHpdgPanel final : public juce::Component
{
public:
    explicit ConnectHpdgPanel(HPSamplerAudioProcessor& proc) : processor(proc)
    {
        for (auto* b : { &scanBtn, &linkSelectedBtn, &disconnectBtn, &exportCtxBtn, &importGuideBtn, &closeBtn })
            addAndMakeVisible(*b);
        addAndMakeVisible(peersBox);
        addAndMakeVisible(freezeGuideBtn);
        addAndMakeVisible(masterWarmBtn);

        scanBtn.onClick = [this] { refreshPeers(); };
        linkSelectedBtn.onClick = [this] {
            const int idx = peersBox.getSelectedItemIndex();
            if (idx >= 0 && idx < static_cast<int>(peers.size()))
            {
                juce::String err;
                if (!processor.linkToLiveHpdgSlot(peers[static_cast<size_t>(idx)], &err))
                    statusText = "Link failed: " + err;
                else
                    statusText = "Linked read-only to " + peers[static_cast<size_t>(idx)].displayName;
                if (onStatusChanged) onStatusChanged();
                repaint();
            }
        };
        disconnectBtn.onClick = [this] {
            processor.disconnectBridge();
            statusText = "Disconnected";
            if (onStatusChanged) onStatusChanged();
            repaint();
        };
        exportCtxBtn.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser>("Export hp.sample-context.v1 JSON",
                processor.getFileTransport().getExchangeDirectory().getChildFile("hp.sample-context.v1.json"), "*.json");
            fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc) {
                    const auto f = fc.getResult();
                    if (f.getFullPathName().isNotEmpty())
                    {
                        juce::String err;
                        const bool ok = processor.exportSamplerSnapshot(f, hpbridge::ContextScope::MainMix, 0, &err);
                        statusText = ok ? ("Exported: " + f.getFileName()) : ("Error: " + err);
                        repaint();
                    }
                });
        };
        importGuideBtn.onClick = [this] {
            fileChooser = std::make_unique<juce::FileChooser>("Import hp.drum-guide.v1 JSON",
                processor.getFileTransport().getExchangeDirectory(), "*.json");
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                [this](const juce::FileChooser& fc) {
                    const auto f = fc.getResult();
                    if (f.existsAsFile())
                    {
                        juce::String err;
                        const bool ok = processor.importDrumGuideSnapshot(f, &err);
                        statusText = ok ? ("Imported DrumGuide: " + f.getFileName()) : ("Error: " + err);
                        if (onStatusChanged) onStatusChanged();
                        repaint();
                    }
                });
        };
        freezeGuideBtn.onClick = [this] { processor.getProject().setDrumGuideFrozen(freezeGuideBtn.getToggleState()); };
        masterWarmBtn.onClick  = [this] {
            processor.getProject().mutableState().masterFx.warmTubeCompEnabled = masterWarmBtn.getToggleState();
            processor.publishProjectToAudio();
        };
        closeBtn.onClick = [this] { setVisible(false); };
        refreshPeers();
    }

    std::function<void()> onStatusChanged;

    void refreshPeers()
    {
        peers = processor.discoverBridgePeers();
        peersBox.clear();
        int id = 1;
        for (const auto& p : peers)
            peersBox.addItem(p.displayName + (p.alive ? " [LIVE]" : " [STALE]"), id++);
        if (peersBox.getNumItems() > 0) peersBox.setSelectedId(1);
        statusText = "Status: " + hpbridge::toString(processor.getBridgeState());
        masterWarmBtn.setToggleState(processor.getProject().state().masterFx.warmTubeCompEnabled, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        auto top = r.removeFromTop(26);
        closeBtn.setBounds(top.removeFromRight(28));
        r.removeFromTop(8);
        peersBox.setBounds(r.removeFromTop(24));
        r.removeFromTop(6);
        auto row1 = r.removeFromTop(26);
        scanBtn.setBounds(row1.removeFromLeft(90).reduced(2));
        linkSelectedBtn.setBounds(row1.removeFromLeft(110).reduced(2));
        disconnectBtn.setBounds(row1.reduced(2));
        r.removeFromTop(8);
        exportCtxBtn.setBounds(r.removeFromTop(26).reduced(2));
        r.removeFromTop(4);
        importGuideBtn.setBounds(r.removeFromTop(26).reduced(2));
        r.removeFromTop(8);
        freezeGuideBtn.setBounds(r.removeFromTop(24));
        r.removeFromTop(8);
        masterWarmBtn.setBounds(r.removeFromTop(24));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff181c23));
        g.setColour(juce::Colour(0xff38404d));
        g.drawRect(getLocalBounds(), 1);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("HP BRIDGE / MASTER FX", 10, 10, 200, 24, juce::Justification::centredLeft);

        g.setFont(juce::FontOptions(11.0f));
        g.setColour(juce::Colour(0xff9aa5b1));
        g.drawFittedText(statusText, getLocalBounds().reduced(10).removeFromBottom(44), juce::Justification::centredLeft, 3);
    }

private:
    HPSamplerAudioProcessor& processor;
    std::vector<hpbridge::BridgePeerInfo> peers;
    juce::String statusText;

    juce::ComboBox peersBox;
    juce::TextButton scanBtn { "Discover" }, linkSelectedBtn { "Link HPDG" }, disconnectBtn { "Disconnect" };
    juce::TextButton exportCtxBtn { "Export Sampler Snapshot (JSON)" }, importGuideBtn { "Import HPDG Drum Guide (JSON)" };
    juce::ToggleButton freezeGuideBtn { "Freeze Drum Guide" }, masterWarmBtn { "Master Warm Tube Comp Lite" };
    juce::TextButton closeBtn { "X" };
    std::unique_ptr<juce::FileChooser> fileChooser;
};
} // namespace hps
