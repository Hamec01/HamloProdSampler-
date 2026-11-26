#pragma once

#include "app/PluginProcessor.h"
#include "ui/MixKnob.h"
#include "ui/Theme.h"

namespace hps
{
class PadGrid;

class PadButton : public juce::Component, public juce::DragAndDropTarget, public juce::SettableTooltipClient
{
public:
    PadButton(PadGrid& g, int index);
    void paint(juce::Graphics&) override;
    void resized() override;
    void updateKnobs();
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
    bool wasRendering = false;
    bool wasSounding = false;

private:
    PadGrid& grid;
    int indexInBank;
    bool dropHover = false;
    bool playing = false;
    bool dragStarted = false;
    MixKnob vol { MixKnob::Kind::Volume }, pan { MixKnob::Kind::Pan }, flt { MixKnob::Kind::Filter };
    bool knobsShown() const;
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
    // PadMode::Cut: click or sweep over pads puts them into / takes them out of ui.cutPaint
    void beginCutSweep(int pad);
    void cutSweepTo(juce::Point<int> posInGrid);
    void endCutSweep();

    HPSamplerProcessor& proc;
    std::function<void()> onOpenInspector;
    std::function<void(int bank)> onEditBank;
    void mouseDown(const juce::MouseEvent&) override; // right click on a bank button
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
    juce::TextButton cutBtns[5], cutBankBtn { "BANK" }, cutPickBtn { "PICK" };
    void applySweep(int pad);
    bool marking = false;     // a source is playing: an empty pad takes a cut
    int sweepGroup = -1;      // group being painted, -1 = no sweep
    bool rightClick = false;  // JUCE buttons also "click" with the right button: that one only opens the menu
    juce::TextButton loopBtn { "LOOP" }, revBtn { "REV" }, inspectBtn { "PAD EDIT >" };
    juce::Label hint;
};
} // namespace hps
