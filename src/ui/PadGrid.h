#pragma once

#include "app/PluginProcessor.h"
#include "ui/Theme.h"

namespace hps
{
class PadGrid;

class PadButton : public juce::Component, public juce::DragAndDropTarget, public juce::SettableTooltipClient
{
public:
    PadButton(PadGrid& g, int index) : grid(g), indexInBank(index) {}
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override {} // two quick clicks = two hits (handled in mouseDown)
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit(const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped(const SourceDetails&) override;
    int flatIndex() const;

    float flash = 0.0f;
    uint32_t lastHits = 0;

private:
    PadGrid& grid;
    int indexInBank;
    bool dropHover = false;
    bool playing = false;
    bool dragStarted = false;
};

class PadGrid : public juce::Component, private juce::Timer
{
public:
    explicit PadGrid(HPSamplerProcessor& p);
    ~PadGrid() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh();
    void showPadMenu(int pad, juce::Component* target);
    void selectForEdit(int pad);

    HPSamplerProcessor& proc;
    std::function<void()> onOpenInspector;
    std::function<void(int pad)> onLocate; // switch to SRC view and show the region

private:
    void timerCallback() override;
    void updateStrip();
    void setPadField(const juce::String& label, const std::function<void(PadSettings&)>& fn);

    juce::OwnedArray<PadButton> pads;
    juce::TextButton banks[kNumBanks];
    juce::TextButton performBtn { "PERFORM" }, editBtn { "EDIT SELECT" };
    // selected-pad strip
    juce::Label padName;
    juce::TextButton gateBtn { "GATE" }, oneShotBtn { "1-SHOT" }, toggleBtn { "TOGGLE" };
    juce::TextButton retrigBtn { "RETRIG" }, layerBtn { "LAYER" };
    juce::Label cutLabel;
    juce::TextButton cutBtns[5];
    juce::TextButton loopBtn { "LOOP" }, revBtn { "REV" }, inspectBtn { "PAD EDIT >" };
    juce::Label hint;
};
} // namespace hps
