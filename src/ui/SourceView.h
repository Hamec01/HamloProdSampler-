#pragma once

#include "app/PluginProcessor.h"
#include "ui/Theme.h"

namespace hps
{
class SourceView;

// The big waveform screen of one source: ruler + beat grid, pad regions, selection with handles,
// chop candidates, overview. Frame-accurate whatever the zoom.
class WaveCanvas : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit WaveCanvas(SourceView& owner);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override {} // never used

    double frameToX(double f) const;
    double xToFrame(double x) const;

private:
    enum class Drag { None, NewSel, SelStart, SelEnd, MoveSel, ToPad, Origin, CandStart, CandEnd, Overview, Seek };
    juce::Rectangle<int> rulerArea() const;
    juce::Rectangle<int> padBand() const;
    juce::Rectangle<int> waveArea() const;
    juce::Rectangle<int> candBand() const;
    juce::Rectangle<int> overviewArea() const;
    void drawGrid(juce::Graphics&, juce::Rectangle<int> area, bool ruler);
    void drawWave(juce::Graphics&, juce::Rectangle<int> area, const SourceData& d);
    void drawSpectrogram(juce::Graphics&, juce::Rectangle<int> area, const SourceData& d);
    int64_t snapped(double frame, bool force = false) const;
    int candidateAt(juce::Point<int> p, int& edge) const; // edge: -1 start, 1 end, 0 body
    int padRegionAt(juce::Point<int> p) const;
    void showContextMenu(const juce::MouseEvent&);

    SourceView& owner;
    Drag drag = Drag::None;
    int64_t anchorFrame = 0, dragOffset = 0;
    int dragCandidate = -1;
    bool moved = false;
    juce::Image specImage;
    const void* specFor = nullptr;
};

class SourceView : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit SourceView(HPSamplerProcessor& p);
    ~SourceView() override;

    void setSlot(int slot);
    int  getSlot() const noexcept { return slot; }
    void refresh(); // project changed

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void playFromKeyboard();
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray& files, int, int) override;

    // used by the canvas
    HPSamplerProcessor& proc;
    UiState::SrcView& view() { return proc.ui.src[(size_t) slot]; }
    std::shared_ptr<const SourceData> data() const { return proc.project.sourceDataForSlot(slot); }
    SourceGrid grid() const { return proc.gridForSource(slot); }
    void auditionSelection(bool keepPosition = false); // keepPosition: a playing audition goes on (edges moved)
    bool auditioningHere() const;
    void auditionFrom(int64_t frame);
    void stopAudition();
    void quickAssign();
    void zoomToSelection();
    void setSelection(int64_t s, int64_t e, bool audition);
    void clampView();
    void selectionChanged();
    int64_t markFrame = -1; // quick-chop mark while auditioning
    // Pad edit: the selection IS the pad's range while editingPad >= 0 (see UiState)
    int  editingPad() const;
    void candidatesEdited();      // an edge was dragged: re-apply LENGTH
    void commitPadEdit();
    void endPadEdit();

private:
    void timerCallback() override;
    void updateHeader();
    void showBpmMenu();
    void showKeyMenu();
    void runChopTool();
    void chopParamChanged();      // live: the candidates follow the tool's settings
    juce::String chopHint() const;
    void mapToPads(int fromPad, bool replace);
    void setLengthMode(LengthMode m);
    void tap();
    void finishTap(bool apply);
    double tappedBpm() const;
    bool tapArmed = false;
    std::vector<double> tapTimes, tapFrames; // seconds / source frames of the audition (-1 = silent)
    juce::uint32 tapLastMs = 0;
    int lastRunTool = 0;
    juce::TextButton tapBtn { "TAP" }, gridBtn { "GRID" }, hearBtn, toEndBtn { "TO END" };
    juce::ComboBox lengthBox;
    juce::TextButton cutBtn { "CUT" }, mapMenuBtn { "v" };
    void nudge(bool end, int64_t frames);
    void openFile();

    int slot = 0;
    int lastTool = 0;
    std::array<bool, kNumSources> wasLoading {};
    WaveCanvas canvas { *this };

    // header
    juce::TextButton loadBtn { "LOAD" }, clearBtn { "CLEAR" }, anchorBtn { "USE AS PROJECT ANCHOR" };
    juce::TextButton bpmBtn, keyBtn, halfBtn { "/2" }, doubleBtn { "x2" };
    juce::TextButton waveBtn { "WAVE" }, specBtn { "SPEC" }, splitBtn { "SPLIT" };
    juce::Label title, info;

    // chop toolbar
    juce::ComboBox toolBox, focusBox;
    juce::Slider amount;
    juce::Label amountLabel;
    juce::TextButton runBtn { "FIND" }, mapBtn { "-> PADS" }, acceptBtn { "ACCEPT" }, clearCandBtn { "CLEAR" };
    juce::Label candInfo;

    // selection bar
    juce::Label selInfo;
    juce::TextButton sMinus { "<" }, sPlus { ">" }, eMinus { "<" }, ePlus { ">" };
    juce::ComboBox snapBox;
    juce::TextButton zoomSelBtn { "ZOOM SEL" }, zoomAllBtn { "ALL" }, loopBtn { "LOOP" }, autoBtn { "AUTO PLAY" };
    juce::TextButton doneEditBtn { "DONE" }, playPadBtn { "PLAY PAD" };
    juce::Label editBanner;
    juce::TextButton assignBtn { "-> NEXT PAD  [Enter]" }, assignModeBtn { "ASSIGN TO..." };
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace hps
