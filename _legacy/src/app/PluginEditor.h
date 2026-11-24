#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/PluginProcessor.h"
#include "ui/InspectorAndConnectPanels.h"
#include "ui/MainDisplayComponent.h"
#include "ui/PadGridComponent.h"
#include "ui/SourceDisplayComponent.h"

namespace hps
{
class HPSamplerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            public juce::DragAndDropContainer,
                                            private juce::Timer
{
public:
    explicit HPSamplerAudioProcessorEditor(HPSamplerAudioProcessor&);
    ~HPSamplerAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setNavTarget(NavTarget target);
    void refreshGlobalDisplays();

    HPSamplerAudioProcessor& processor;
    NavTarget navTarget = NavTarget::Src1;

    // Top row: MAIN | SRC 1..4
    juce::TextButton mainBtn { "MAIN" };
    std::array<juce::TextButton, kNumVisibleSources> srcBtns;

    // Central displays
    SourceDisplayComponent sourceDisplay;
    MainDisplayComponent   mainDisplay;

    // 4 global parameter buttons: PROJECT BPM | PROJECT KEY | GRID | SYNC
    juce::TextButton bpmBtn  { "BPM 120.0" };
    juce::TextButton keyBtn  { "KEY Unknown" };
    juce::TextButton gridBtn { "GRID 1/16" };
    juce::TextButton syncBtn { "SYNC OFF" };

    // Command bar: Transport | PAD EDIT | CONNECT HPDG | UNDO/REDO
    juce::TextButton playBtn    { "PLAY" };
    juce::TextButton stopBtn    { "STOP" };
    juce::TextButton recBtn     { "REC" };
    juce::TextButton quantBtn   { "QUANT OFF" };
    juce::TextButton padEditBtn { "PAD EDIT" };
    juce::TextButton connectBtn { "CONNECT HPDG" };
    juce::TextButton undoBtn    { "UNDO" };
    juce::TextButton redoBtn    { "REDO" };

    // Bottom bank / playback / overlap strip
    std::array<juce::TextButton, kNumBanks> bankBtns;
    juce::TextButton modeBtn    { "ONE SHOT" };
    juce::TextButton overlapBtn { "RETRIGGER" };

    // 16 physical pads
    PadGridComponent padGrid;

    // Slide-out panels
    PadInspectorPanel padInspector;
    ConnectHpdgPanel  connectPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HPSamplerAudioProcessorEditor)
};
} // namespace hps
