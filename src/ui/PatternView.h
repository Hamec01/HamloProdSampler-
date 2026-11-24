#pragma once

#include "app/PluginProcessor.h"
#include "ui/Theme.h"

namespace hps
{
class PatternView;

// MAIN editor: the 4 source lanes (overlapping notes of a lane are stacked), with FL Studio Piano-roll editing logic.
// Mouse:  LMB empty = place the lane's current pad (last used length / velocity), keep dragging to move it
//         LMB note = select + move (pick: it becomes the lane's pad, its length / velocity the next ones)
//         right end = resize, Shift+drag = copy, Alt = no snap
//         RMB note = delete (RMB drag = sweep-delete), Ctrl+drag = marquee select (Ctrl+Shift adds)
//         wheel = scroll time, Ctrl+wheel = zoom, Alt+wheel on a note = velocity, click the ruler = paste position
class LaneCanvas : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget
{
public:
    explicit LaneCanvas(PatternView& o) : owner(o) { setWantsKeyboardFocus(false); }
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override {}
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("pad:"); }
    void itemDragMove(const SourceDetails& d) override { dropX = d.localPosition.x; repaint(); }
    void itemDragExit(const SourceDetails&) override { dropX = -1; repaint(); }
    void itemDropped(const SourceDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    static constexpr int kLabelW = 150, kRulerH = 26, kGuideH = 16;
    juce::Rectangle<int> laneRect(int lane) const;
    int laneAt(int y) const;
    double tickToX(double t) const;
    double xToTick(double x) const;
    int64_t snapTick(double t, bool floorIt = false) const;

    double viewStart = 0.0, viewTicks = 0.0; // 0 = whole pattern
    int64_t cursorTick = 0;                  // paste position (last click)

private:
    struct Placed { int index; juce::Rectangle<float> trig, tail; };
    std::vector<Placed> layout() const;
    int eventAt(juce::Point<int> p, bool& onRightEdge) const;
    juce::Rectangle<float> rectOf(const juce::String& id) const;
    void audition(int pad);
    void stopAudition();

    PatternView& owner;
    enum class Drag { None, Move, Resize, Marquee, Erase } drag = Drag::None;
    juce::Point<int> downPos;
    struct Orig { juce::String id; int64_t tick; int64_t length; };
    std::vector<Orig> orig;
    int64_t anchorTick = 0, anchorLen = 0, grabOffset = 0;
    juce::Rectangle<int> band;
    bool bandAdd = false;
    int dropX = -1;
    int auditionPad = -1;
};

class PatternView : public juce::Component, private juce::Timer
{
public:
    explicit PatternView(HPSamplerProcessor& p);
    ~PatternView() override;
    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    HPSamplerProcessor& proc;
    std::function<void(int slot)> onShowSource;
    int64_t loopTicks() const { return (int64_t) proc.project.state().settings.patternBars * kTicksPerBar; }
    double tailTicks(int pad, int64_t length) const;
    bool isSelected(const juce::String& id) const;
    void deleteSelected();
    void duplicateSelected();   // Ctrl+B: right after the selection
    void copySelected(bool cut);
    void paste();
    void nudgeSelected(int64_t ticks, int padStep); // padStep: previous / next pad of the same source (like pitch)
    int lanePad(int lane) const;                     // the pad a click on this lane places
    std::array<int, kNumSources> laneLastPad { { -1, -1, -1, -1 } };
    void quantizeSelected();
    int64_t gridStep() const;

    int64_t lastLength = kPPQ / 4;   // FL: the last placed / clicked note's length is used for new notes
    float lastVelocity = 0.9f;

private:
    void timerCallback() override;
    void exportWav();
    void exportMidi();
    void showHelp();

    LaneCanvas lanes { *this };
    juce::TextButton patternTab { "PATTERN" }, resultTab { "RESULT" };
    juce::TextButton drawBtn { "DRAW" }, selectBtn { "SELECT" }, helpBtn { "KEYS ?" };
    juce::ComboBox barsBox;
    juce::TextButton guideBtn { "DRUM GUIDE" }, quantBtn { "QUANTIZE REC" }, clearBtn { "CLEAR" };
    juce::TextButton wavBtn { "EXPORT WAV" }, midiBtn { "EXPORT MIDI" }, renderBtn { "RENDER" };
    juce::Label info;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<PatternEvent> clipboard;
    int lastTab = -1;
};
} // namespace hps
