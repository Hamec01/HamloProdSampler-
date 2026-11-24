#pragma once

#include "app/PluginProcessor.h"
#include "ui/ControlStrip.h"
#include "ui/PadGrid.h"
#include "ui/PadInspector.h"
#include "ui/PatternView.h"
#include "ui/SourceView.h"
#include "ui/Theme.h"

namespace hps
{
class NavButton : public juce::Component, public juce::FileDragAndDropTarget, public juce::SettableTooltipClient
{
public:
    NavButton(HPSamplerProcessor& p, int s) : proc(p), slot(s) {}
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
    bool isInterestedInFileDrag(const juce::StringArray&) override { return slot >= 0; }
    void fileDragEnter(const juce::StringArray&, int, int) override { hover = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { hover = false; repaint(); }
    void filesDropped(const juce::StringArray& f, int, int) override { hover = false; if (!f.isEmpty()) proc.loadSource(slot, juce::File(f[0])); }
    std::function<void()> onClick;
    bool active = false;

private:
    HPSamplerProcessor& proc;
    int slot; // -1 = MAIN
    bool hover = false;
};

class HPSamplerEditor : public juce::AudioProcessorEditor,
                        public juce::DragAndDropContainer,
                        private HPSamplerProcessor::Listener,
                        private juce::Timer
{
public:
    explicit HPSamplerEditor(HPSamplerProcessor&);
    ~HPSamplerEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void projectChanged() override;
    void statusChanged() override;
    void timerCallback() override;
    void showView(int view);
    void showBridgeMenu();
    void bridgeButtonClicked();
    int bridgeTicks = 0, hpdgFound = 0;
    void openInspector(bool open);
    void locatePad(int pad);
    void editPad(int pad); // open the pad's piece of source in the big display + inspector

    HPSamplerProcessor& proc;
    HPLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };
    NavButton mainBtn { proc, -1 };
    juce::OwnedArray<NavButton> srcBtns;
    juce::TextButton undoBtn { "UNDO" }, redoBtn { "REDO" }, bridgeBtn { "CONNECT HPDG" };
    SourceView sourceView { proc };
    PatternView patternView { proc };
    ControlStrip controls { proc };
    PadGrid padGrid { proc };
    PadInspector inspector { proc };
    juce::Label statusLabel, bridgeLabel;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace hps
