#pragma once

#include "ui/Theme.h"

namespace hps
{
// Small rotary knob used on the source lanes and on the pads: VOL / PAN / FILTER.
// Vertical or horizontal drag, wheel after a click on it, double-click = reset (a knob, never a pad: the pad never sees the click).
class MixKnob : public juce::Slider
{
public:
    enum class Kind { Volume, Pan, Filter, LowCut, HighCut };

    explicit MixKnob(Kind k) : kind(k)
    {
        setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        setMouseDragSensitivity(140);
        setWantsKeyboardFocus(false);
        setPopupDisplayEnabled(true, true, nullptr, 900);
        wheelAfterClick(*this);
        switch (kind)
        {
            case Kind::Volume: setRange(-36.0, 12.0, 0.1); setSkewFactorFromMidPoint(-6.0); setDoubleClickReturnValue(true, 0.0); getProperties().set("caption", "VOL"); break;
            case Kind::Pan:    setRange(-1.0, 1.0, 0.01); setDoubleClickReturnValue(true, 0.0); getProperties().set("caption", "PAN"); break;
            case Kind::Filter: setRange(-1.0, 1.0, 0.01); setDoubleClickReturnValue(true, 0.0); getProperties().set("caption", "FILTER"); break;
            case Kind::LowCut: setRange(20.0, 2000.0, 1.0); setSkewFactorFromMidPoint(200.0); setDoubleClickReturnValue(true, 20.0); getProperties().set("caption", "LO CUT"); break;
            case Kind::HighCut: setRange(500.0, 20000.0, 10.0); setSkewFactorFromMidPoint(4000.0); setDoubleClickReturnValue(true, 20000.0); getProperties().set("caption", "HI CUT"); break;
        }
        getProperties().set("bipolar", kind == Kind::Pan || kind == Kind::Filter);
        setTooltip(kind == Kind::Volume ? "Volume (double-click: 0 dB)"
                 : kind == Kind::Pan ? "Pan (double-click: centre)"
                 : kind == Kind::LowCut ? "Low cut: removes the lows below this frequency (double-click: off)"
                 : kind == Kind::HighCut ? "High cut: removes the highs above this frequency (double-click: off)"
                 : "Filter: left = low-pass, right = high-pass (double-click: off)");
    }

    void setAccent(juce::Colour c)
    {
        setColour(juce::Slider::rotarySliderFillColourId, c);
        repaint();
    }

    void setCaptionVisible(bool shown) { getProperties().set("noCaption", !shown); repaint(); }

    juce::String getTextFromValue(double v) override
    {
        switch (kind)
        {
            case Kind::Volume: return v <= -35.9 ? juce::String("-inf dB") : juce::String(v, 1) + " dB";
            case Kind::Pan:    return std::abs(v) < 0.005 ? juce::String("C") : (v < 0 ? "L " : "R ") + juce::String(juce::roundToInt(std::abs(v) * 100));
            case Kind::Filter:
            {
                if (std::abs(v) < 0.005) return "OFF";
                float hp, lp;
                filterKnobToHz((float) v, hp, lp);
                return v < 0 ? "LP " + juce::String(juce::roundToInt(lp)) + " Hz" : "HP " + juce::String(juce::roundToInt(hp)) + " Hz";
            }
            case Kind::LowCut:  return v <= 25.0 ? juce::String("OFF") : juce::String(juce::roundToInt(v)) + " Hz";
            case Kind::HighCut: return v >= 19000.0 ? juce::String("OFF") : v >= 1000.0 ? juce::String(v / 1000.0, 1) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz";
        }
        return {};
    }

    // Undo: one entry per drag (and one per double-click / wheel step).
    std::function<void(float)> onChange;
    std::function<void()> onGestureStart, onGestureEnd;

    void startedDragging() override { dragging = true; if (onGestureStart) onGestureStart(); }
    void stoppedDragging() override { dragging = false; if (onGestureEnd) onGestureEnd(); }
    void valueChanged() override { if (onChange && !updating) onChange((float) getValue()); }

    void setValueQuietly(double v)
    {
        if (dragging) return;
        updating = true;
        setValue(v, juce::dontSendNotification);
        updating = false;
    }

private:
    Kind kind;
    bool dragging = false, updating = false;
};
} // namespace hps
