#include "ui/Theme.h"

namespace hps
{
HPLookAndFeel::HPLookAndFeel()
{
    using namespace theme;
    setColour(juce::ResizableWindow::backgroundColourId, body);
    setColour(juce::TextButton::buttonColourId, panel);
    setColour(juce::TextButton::buttonOnColourId, panelHi);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, accent);
    setColour(juce::ComboBox::backgroundColourId, screen);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::outlineColourId, panelHi);
    setColour(juce::ComboBox::arrowColourId, textDim);
    setColour(juce::PopupMenu::backgroundColourId, bodyLight);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, panelHi);
    setColour(juce::PopupMenu::highlightedTextColourId, accent);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Slider::thumbColourId, accent);
    setColour(juce::Slider::trackColourId, panelHi);
    setColour(juce::Slider::backgroundColourId, screen);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxBackgroundColourId, screen);
    setColour(juce::Slider::textBoxOutlineColourId, panelHi);
    setColour(juce::TextEditor::backgroundColourId, screen);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, panelHi);
    setColour(juce::AlertWindow::backgroundColourId, bodyLight);
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::ScrollBar::thumbColourId, panelHi);
    setColour(juce::ToggleButton::textColourId, text);
    setColour(juce::ToggleButton::tickColourId, accent);
    setColour(juce::TooltipWindow::backgroundColourId, bodyLight);
    setColour(juce::TooltipWindow::textColourId, text);
}

void HPLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool over, bool down)
{
    using namespace theme;
    auto r = b.getLocalBounds().toFloat().reduced(1.0f);
    const bool on = b.getToggleState();
    auto c = base;
    if (on) c = b.findColour(juce::TextButton::buttonOnColourId);
    if (down) c = c.brighter(0.15f);
    else if (over) c = c.brighter(0.07f);
    g.setColour(c);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawLine(r.getX() + 3, r.getY() + 1, r.getRight() - 3, r.getY() + 1);
    if (on)
    {
        // LED strip on active buttons
        const auto led = b.getProperties()["led"].isVoid() ? accent : juce::Colour((juce::uint32) (juce::int64) b.getProperties()["led"]);
        g.setColour(led);
        g.fillRoundedRectangle(r.getX() + 4, r.getBottom() - 3.5f, r.getWidth() - 8, 2.0f, 1.0f);
    }
}

void HPLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.setColour(b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.4f));
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(4, 1), juce::Justification::centred, 2, 0.8f);
}

juce::Font HPLookAndFeel::getTextButtonFont(juce::TextButton&, int h)
{
    return theme::ui(juce::jlimit(11.0f, 14.0f, h * 0.48f), true);
}

void HPLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    using namespace theme;
    auto r = juce::Rectangle<float>(0, 0, (float) w, (float) h).reduced(1.0f);
    g.setColour(screen);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(panelHi);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
    juce::Path p;
    const float ax = (float) w - 12.0f, ay = (float) h * 0.5f;
    p.addTriangle(ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.fillPath(p);
}

void HPLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                     juce::Slider::SliderStyle style, juce::Slider& s)
{
    using namespace theme;
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, pos, 0, 0, style, s);
        return;
    }
    const float cy = y + h * 0.5f;
    auto track = juce::Rectangle<float>((float) x, cy - 2.5f, (float) w, 5.0f);
    g.setColour(screen);
    g.fillRoundedRectangle(track, 2.5f);
    // fill from the zero point for bipolar sliders
    const double zero = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 ? s.valueToProportionOfLength(0.0) : 0.0;
    const float zx = x + (float) zero * w;
    g.setColour(s.findColour(juce::Slider::thumbColourId).withAlpha(0.65f));
    g.fillRoundedRectangle(juce::Rectangle<float>(std::min(zx, pos), cy - 2.5f, std::abs(pos - zx), 5.0f), 2.5f);
    g.setColour(text);
    g.fillRoundedRectangle(juce::Rectangle<float>(pos - 3.0f, cy - 7.0f, 6.0f, 14.0f), 2.0f);
}
} // namespace hps
