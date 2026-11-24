#pragma once

#include "app/PluginProcessor.h"
#include "ui/Theme.h"

namespace hps
{
// One of the four parameter "displays" above the pads.
class DisplayChip : public juce::Component, public juce::SettableTooltipClient
{
public:
    juce::String caption, value, sub;
    juce::Colour subColour = theme::textDim;
    std::function<void()> onClick;
    std::function<void(float delta, bool fine)> onWheel;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    int lastY = 0;
    bool dragged = false;
};

class ControlStrip : public juce::Component, private juce::Timer
{
public:
    explicit ControlStrip(HPSamplerProcessor& p);
    ~ControlStrip() override;
    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void bpmMenu();
    void keyMenu();
    void gridMenu();
    void syncMenu();

    HPSamplerProcessor& proc;
    DisplayChip bpm, key, grid, sync;
    juce::TextButton playBtn { "PLAY" }, stopBtn { "STOP" }, recBtn { "REC" }, metroBtn { "METRO" };
    juce::Label posLabel;
    juce::Label cutLabel, modeLabel, qLabel, swingLabel;
    juce::TextButton cutOff { "OFF" }, cutSrc { "PER SRC" }, cutAll { "ALL" };
    juce::TextButton gateDef { "GATE" }, oneDef { "1-SHOT" };
    juce::ComboBox recQ;
    juce::Slider swing, master;
};
} // namespace hps
