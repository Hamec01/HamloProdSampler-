#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Types.h"

namespace hps::theme
{
// Dark graphite "hardware box" palette.
inline const juce::Colour body      { 0xff1b1d21 };
inline const juce::Colour bodyLight { 0xff24272c };
inline const juce::Colour panel     { 0xff2b2f35 };
inline const juce::Colour panelHi   { 0xff353a42 };
inline const juce::Colour screen    { 0xff0b0e12 };
inline const juce::Colour screenLine{ 0xff1a2029 };
inline const juce::Colour wave      { 0xff7fb4d8 };
inline const juce::Colour text      { 0xffd9dee5 };
inline const juce::Colour textDim   { 0xff8b939e };
inline const juce::Colour accent    { 0xffffb347 };  // amber LED
inline const juce::Colour rec       { 0xffff4b4b };
inline const juce::Colour ok        { 0xff52d37f };
inline const juce::Colour warn      { 0xffffc94d };
inline const juce::Colour selection { 0xffffffff };

inline juce::Font mono(float h, bool bold = false)
{
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), h, bold ? juce::Font::bold : juce::Font::plain));
}
inline juce::Font ui(float h, bool bold = false)
{
    return juce::Font(juce::FontOptions(h, bold ? juce::Font::bold : juce::Font::plain));
}

// Small rounded "LED display" chip used for BPM / KEY etc.
inline void drawChip(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour edge, bool active)
{
    g.setColour(active ? screen.brighter(0.05f) : screen);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(edge.withAlpha(active ? 0.9f : 0.35f));
    g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
}
} // namespace hps::theme

namespace hps
{
class HPLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HPLookAndFeel();
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override { return theme::ui(13.0f); }
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getPopupMenuFont() override { return theme::ui(14.0f); }
};
} // namespace hps
