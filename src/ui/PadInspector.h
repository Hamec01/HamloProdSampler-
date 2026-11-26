#pragma once

#include "app/PluginProcessor.h"
#include "ui/Theme.h"

namespace hps
{
class PadInspector : public juce::Component
{
public:
    explicit PadInspector(HPSamplerProcessor& p);
    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    std::function<void()> onClose;
    std::function<void()> onScopeChanged;
    std::function<void(int pad)> onLocate;

private:
    struct Row
    {
        juce::String section;
        std::unique_ptr<juce::Label> label;
        juce::Component* control = nullptr;
        int height = 26;
    };
    juce::Slider& addSlider(const juce::String& section, const juce::String& name, double lo, double hi, double step,
                            const juce::String& suffix, std::function<float(const PadSettings&)> get,
                            std::function<void(PadSettings&, float)> set, double skewMid = 0.0);
    juce::ComboBox& addCombo(const juce::String& section, const juce::String& name, const juce::StringArray& items,
                             std::function<int(const PadSettings&)> get, std::function<void(PadSettings&, int)> set);
    void edit(const juce::String& label, const std::function<void(PadSettings&)>& fn);

    HPSamplerProcessor& proc;
    juce::Label header, syncInfo, sliceInfo;
    template <class Get> bool mixed(const Get& get) const // the pads of the bank differ in this parameter
    {
        const auto t = proc.editTargets();
        const auto& pads = proc.project.state().pads;
        for (int i : t) if (get(pads[(size_t) i].s) != get(pads[(size_t) t.front()].s)) return true;
        return false;
    }
    static void markMixed(juce::Label& l, const juce::String& name, bool isMixed);
    juce::TextButton scopeBtn, clearBankBtn { "CLEAR" };
    juce::TextButton closeBtn { "X" }, locateBtn { "LOCATE IN SRC" }, playBtn { "PLAY" };
    juce::TextButton startM { "IN -" }, startP { "IN +" }, endM { "OUT -" }, endP { "OUT +" };
    juce::TextButton half { "1/2x" }, normal { "1x" }, dbl { "2x" };
    juce::ComboBox fitBox;
    juce::TextButton resetPitch { "RESET PITCH" }, resetEnv { "RESET ENV" }, resetMix { "RESET MIX" };
    juce::ToggleButton followKey { "Follow project key" }, reverse { "Reverse" }, loop { "Loop" };
    juce::Viewport viewport;
    struct Content : juce::Component
    {
        std::vector<std::pair<int, juce::String>> headers;
        void paint(juce::Graphics& g) override
        {
            for (const auto& [y, name] : headers)
            {
                g.setColour(theme::accent.withAlpha(0.85f));
                g.setFont(theme::ui(11.0f, true));
                g.drawText(name, 0, y, getWidth(), 14, juce::Justification::centredLeft);
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                g.drawHorizontalLine(y + 15, 0.0f, (float) getWidth());
            }
        }
    } content;
    juce::OwnedArray<juce::Component> extras;
    std::vector<Row> rows;
    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::ComboBox> combos;
    std::vector<std::function<void()>> updaters;
    bool updating = false;
};
} // namespace hps
