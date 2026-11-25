#pragma once

#include "app/PluginProcessor.h"
#include "ui/MixKnob.h"
#include "ui/Theme.h"

namespace hps
{
class PatternView;

// MAIN editor with Piano-roll tools. Rows: 4 source lanes (overlapping notes stacked) or one row per pad.
// Tools: Draw (P), Paint (B), Step paint (N), Delete (D), Mute (T), Slice (C), Select (E), Zoom (Z), Playback (Y)
// Draw:  LMB empty = place a note (last used length / velocity), keep dragging to move it; LMB note = select + move (pick)
//        right end = resize, Shift+drag = copy, Alt = no snap. RMB = delete (drag = sweep), Ctrl+drag = marquee in any tool
// Zoom:  Ctrl+wheel = time, Ctrl+Shift+wheel = row height, the overview strip pans, middle drag pans, PgUp / PgDn, F = fit
class ToolButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ToolButton(UiState::Tool t) : tool(t) {}
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    const UiState::Tool tool;
    bool active = false;
    std::function<void()> onClick;
};

class LaneCanvas : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget
{
public:
    explicit LaneCanvas(PatternView& o);
    void paint(juce::Graphics&) override;
    void resized() override { layoutKnobs(); }
    void layoutKnobs();   // the lane mix knobs follow the lane rectangles (drum guide on / off)
    void updateKnobs();   // values from the project
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseMagnify(const juce::MouseEvent& e, float f) override { zoomTime(f, e.x); }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override {}
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("pad:"); }
    void itemDragMove(const SourceDetails& d) override { dropX = d.localPosition.x; repaint(); }
    void itemDragExit(const SourceDetails&) override { dropX = -1; repaint(); }
    void itemDropped(const SourceDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    static constexpr int kLabelW = 150, kNavH = 14, kRulerH = 40, kGuideH = 16, kVelH = 58;
    struct Row { int src; int pad; int y, h; };   // pad -1 = whole source lane / a source without pads
    std::vector<Row> rows() const;
    juce::Rectangle<int> notesArea() const;       // rows only (no overview / ruler / guide / velocity)
    juce::Rectangle<int> velocityArea() const;
    const Row* rowAt(const std::vector<Row>& rs, int y) const;
    int contentHeight() const;
    double tickToX(double t) const;
    double xToTick(double x) const;
    int64_t snapTick(double t, bool floorIt = false) const;

    double visibleTicks() const;
    void zoomTime(double factor, int anchorX);    // > 1 = in
    void zoomRows(double factor, int anchorY);
    void fitAll();
    void fitSelection();
    void showTicks(double start, double len);
    void clampView();
    void followPlayhead();
    bool isDragging() const { return drag != Drag::None; }

    int64_t cursorTick = 0;                  // paste position (last click)

private:
    struct Placed { int index; juce::Rectangle<float> trig, tail; };
    std::vector<Placed> layout() const;
    int eventAt(juce::Point<int> p, bool& onRightEdge) const;
    juce::Rectangle<float> rectOf(const juce::String& id) const;
    void audition(int pad);
    void stopAudition();

    PatternView& owner;
    enum class Drag { None, Move, Resize, Marquee, Erase, Paint, Step, Mute, Slice, ZoomBox, Playback, Velocity, Nav, Pan } drag = Drag::None;
    juce::Point<int> downPos;
    void eraseAt(juce::Point<int> p);
    void muteAt(juce::Point<int> p);
    void paintNotesTo(int x);
    void setVelocityAt(juce::Point<int> p);
    void addNote(int pad, int64_t tick, int64_t length);
    int padForRow(const Row& r) const;
    void dragNav(int x);
    void drawNotes(juce::Graphics&);
    void drawVelocity(juce::Graphics&);
    void drawNav(juce::Graphics&);
    struct Orig { juce::String id; int64_t tick; int64_t length; int pad; };
    std::vector<Orig> orig;
    int64_t anchorTick = 0, anchorLen = 0, grabOffset = 0;
    juce::Rectangle<int> band;
    bool bandAdd = false;
    int dropX = -1;
    int paintPad = -1; int64_t paintLast = -1;
    bool stepErase = false, muteTo = false;
    juce::Point<int> dragPos, lastPos;
    double panStart = 0; int panScroll = 0;
    int downRow = 0, lastPadShift = 0;
    int auditionPad = -1;
    std::array<std::array<std::unique_ptr<MixKnob>, 3>, kNumSources> laneKnobs; // VOL / PAN / FILTER per source
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
    void chopSelected();          // Ctrl+U: every selected note into grid steps
    void toggleMuteSelected();    // Alt+M
    void selectTool(UiState::Tool t);
    void snapMenu();
    int64_t gridStep() const;

    int64_t lastLength = kPPQ / 4;   // FL: the last placed / clicked note's length is used for new notes
    float lastVelocity = 0.9f;

private:
    void timerCallback() override;
    void mouseDown(const juce::MouseEvent&) override { resultDragged = false; }
    void mouseDrag(const juce::MouseEvent&) override; // RESULT: drag the wave into the DAW
    void exportWav();
    void exportMidi();
    void showHelp();

    LaneCanvas lanes { *this };
    juce::TextButton patternTab { "PATTERN" }, resultTab { "RESULT" };
    juce::OwnedArray<ToolButton> tools;
    juce::TextButton helpBtn { "KEYS ?" }, snapBtn, rowsBtn { "PAD ROWS" }, velBtn { "VELOCITY" }, followBtn { "FOLLOW" };
    juce::TextButton zoomOutBtn { "-" }, zoomInBtn { "+" }, fitBtn { "FIT" }, rowOutBtn { "-" }, rowInBtn { "+" };
    juce::Label zoomLabel, rowLabel;
    juce::ComboBox barsBox;
    juce::TextButton guideBtn { "DRUM GUIDE" }, quantBtn { "QUANTIZE REC" }, clearBtn { "CLEAR" };
    // Click = save as, drag = the file goes straight into the DAW.
    struct DragFileButton : juce::TextButton
    {
        using juce::TextButton::TextButton;
        std::function<juce::File()> makeFile;
        bool dragged = false;
        void mouseDown(const juce::MouseEvent& e) override { dragged = false; juce::TextButton::mouseDown(e); }
        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (dragged) return;
            if (e.getDistanceFromDragStart() < 6) { juce::TextButton::mouseDrag(e); return; }
            dragged = true;
            setState(juce::Button::buttonNormal);
            startFileDrag(*this, makeFile);
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (dragged) { setState(juce::Button::buttonNormal); return; } // a drag is not a click
            juce::TextButton::mouseUp(e);
        }
    };
    static void startFileDrag(juce::Component& from, const std::function<juce::File()>& makeFile);
    juce::File dragFile(bool midi);
    bool resultDragged = false;
    DragFileButton wavBtn { "DRAG WAV" }, midiBtn { "DRAG MIDI" };
    juce::TextButton renderBtn { "RENDER" };
    juce::Label info;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<PatternEvent> clipboard;
    int lastTab = -1;
    bool wasPlaying = false;
};
} // namespace hps
