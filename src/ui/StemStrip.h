#pragma once

#include "app/PluginProcessor.h"
#include "ui/Theme.h"

namespace hps
{
// SRC header: the stem rebalance of one source. Four cells (DRUMS / BASS / VOX / INST), each an icon
// with its level line: drag up / down = level, click = mute, double-click = 0 dB, right-click = menu.
// Before the stems exist the strip offers the split (with a warning: it is a heavy, optional step).
class StemStrip : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit StemStrip(HPSamplerProcessor& p);
    void setSlot(int s) { slot = s; refresh(); }
    void refresh();

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    // level <-> fader position (0 = off, 0.75 = 0 dB, 1 = +12 dB)
    static float dbToPos(float db);
    static float posToDb(float pos);

private:
    enum class Mode { NoSource, Offer, Cached, Working, Ready };
    Mode mode() const;
    void timerCallback() override;
    juce::Rectangle<float> powerArea() const;
    juce::Rectangle<float> cellArea(int stem) const;
    int cellAt(juce::Point<float> p) const;
    void askToSplit();
    void showMenu(int stem);
    void setLevel(int stem, float db);
    void drawIcon(juce::Graphics&, int stem, juce::Rectangle<float> r, juce::Colour c) const;
    static juce::Colour stemColour(int stem);
    juce::String levelText(int stem) const;

    HPSamplerProcessor& proc;
    int slot = 0;
    int hover = -1, dragStem = -1;
    float dragStartPos = 0.0f;
    bool dragged = false;
    Mode lastMode = Mode::NoSource;
    float lastProgress = -1.0f;
};
} // namespace hps
