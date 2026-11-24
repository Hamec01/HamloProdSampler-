#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "analysis/ClassicalAnalyzer.h"
#include "app/PluginProcessor.h"

namespace hps
{
// Large central display for SRC 1..4:
// - WAVE / SPECTROGRAM / SPLIT with shared time<->sample coordinates & zoom/pan
// - Manual selection (IN/OUT handles), quick preview, drag-and-drop audio files
// - Compact Chop Toolbar: MANUAL | FIND SPOTS | SLICER | CHOP | PREVIEW | ACCEPT | AUTO MAP | ASSIGN | ANCHOR
class SourceDisplayComponent final : public juce::Component,
                                     public juce::FileDragAndDropTarget
{
public:
    explicit SourceDisplayComponent(HPSamplerAudioProcessor& proc) : processor(proc)
    {
        auto styleBtn = [](juce::TextButton& b, juce::Colour bg = juce::Colour(0xff252a33)) {
            b.setColour(juce::TextButton::buttonColourId, bg);
            b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe6edf3));
        };

        for (auto* b : { &loadBtn, &anchorBtn, &waveBtn, &specBtn, &splitBtn,
                         &findSpotsBtn, &slicerBtn, &chopBtn, &previewBtn,
                         &acceptBtn, &autoMapBtn, &assignBtn })
        {
            styleBtn(*b);
            addAndMakeVisible(*b);
        }

        loadBtn.onClick = [this] { openFilePicker(); };
        anchorBtn.onClick = [this] {
            processor.getProject().setProjectAnchorFromSource(slotIndex);
            processor.publishProjectToAudio();
            if (onStateChanged) onStateChanged();
        };

        waveBtn.onClick  = [this] { displayMode = DisplayMode::Wave; repaint(); };
        specBtn.onClick  = [this] { displayMode = DisplayMode::Spectrogram; repaint(); };
        splitBtn.onClick = [this] { displayMode = DisplayMode::Split; repaint(); };

        findSpotsBtn.onClick = [this] { runChop(ChopAlgorithm::FindSpotsHybrid); };
        slicerBtn.onClick    = [this] { runChop(ChopAlgorithm::SlicerBeat); };
        chopBtn.onClick      = [this] { runChop(ChopAlgorithm::ChopRegions); };

        previewBtn.onClick = [this] { auditionSelection(); };
        acceptBtn.onClick  = [this] {
            auto accepted = processor.getProject().acceptAllCandidates(slotIndex);
            if (accepted.empty()) createSliceFromSelection();
            processor.publishProjectToAudio();
            if (onStateChanged) onStateChanged();
            repaint();
        };
        autoMapBtn.onClick = [this] {
            auto& proj = processor.getProject();
            auto accepted = proj.acceptAllCandidates(slotIndex);
            std::vector<juce::String> ids;
            for (const auto& s : accepted) ids.push_back(s.id);
            if (!ids.empty())
            {
                proj.autoMapSlicesToEmptyPads(ids, processor.getVoiceEngine().getActiveBank(), 0, false);
                processor.publishProjectToAudio();
                if (onStateChanged) onStateChanged();
                repaint();
            }
        };
        assignBtn.onClick = [this] {
            if (onAssignModeRequested) onAssignModeRequested();
        };
    }

    void setSlotIndex(int idx)
    {
        slotIndex = juce::jlimit(0, kNumVisibleSources - 1, idx);
        zoomFactor = 1.0;
        scrollNorm = 0.0;
        resetSelectionToDefault();
        repaint();
    }

    int getSlotIndex() const noexcept { return slotIndex; }
    int64_t getSelectionStart() const noexcept { return selStartFrame; }
    int64_t getSelectionEnd() const noexcept { return selEndFrame; }

    void focusRange(int64_t startF, int64_t endF)
    {
        selStartFrame = startF;
        selEndFrame   = endF;
        repaint();
    }

    SliceDefinition createSliceFromSelection()
    {
        const auto* src = processor.getProject().getSourceBySlot(slotIndex);
        if (src == nullptr || src->numFrames <= 0) return {};
        const int64_t s = juce::jlimit<int64_t>(0, src->numFrames - 1, selStartFrame);
        const int64_t e = juce::jlimit<int64_t>(s + 1, src->numFrames, selEndFrame > s ? selEndFrame : src->numFrames);
        return processor.getProject().createSlice(src->id, s, e, src->name.substring(0, 8));
    }

    std::function<void()> onStateChanged;
    std::function<void()> onAssignModeRequested;

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (const auto& f : files)
            if (f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".aiff") || f.endsWithIgnoreCase(".aif") || f.endsWithIgnoreCase(".flac"))
                return true;
        return false;
    }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        if (files.isEmpty()) return;
        processor.loadSourceFile(slotIndex, juce::File(files[0]));
        resetSelectionToDefault();
        if (onStateChanged) onStateChanged();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        auto topBar = area.removeFromTop(26);
        loadBtn.setBounds(topBar.removeFromLeft(90).reduced(2));
        anchorBtn.setBounds(topBar.removeFromLeft(108).reduced(2));
        topBar.removeFromLeft(8);
        splitBtn.setBounds(topBar.removeFromRight(58).reduced(2));
        specBtn.setBounds(topBar.removeFromRight(58).reduced(2));
        waveBtn.setBounds(topBar.removeFromRight(58).reduced(2));

        auto chopBar = area.removeFromBottom(28);
        findSpotsBtn.setBounds(chopBar.removeFromLeft(90).reduced(2));
        slicerBtn.setBounds(chopBar.removeFromLeft(70).reduced(2));
        chopBtn.setBounds(chopBar.removeFromLeft(70).reduced(2));
        chopBar.removeFromLeft(8);
        previewBtn.setBounds(chopBar.removeFromLeft(76).reduced(2));
        acceptBtn.setBounds(chopBar.removeFromLeft(70).reduced(2));
        autoMapBtn.setBounds(chopBar.removeFromLeft(84).reduced(2));
        assignBtn.setBounds(chopBar.removeFromRight(106).reduced(2));

        canvasBounds = area.reduced(0, 4);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff101318));
        const auto& proj = processor.getProject();
        const auto* src  = proj.getSourceBySlot(slotIndex);
        const auto col   = SourceColor::defaultForSlot(slotIndex);

        // Source metadata header strip
        auto headerArea = getLocalBounds().reduced(6).removeFromTop(26).withTrimmedLeft(206).withTrimmedRight(186);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.setColour(col.colour);
        if (src != nullptr)
        {
            juce::String meta = col.badge + " " + src->name;
            meta << "  |  BPM: " << (src->analysis.bpm.has_value() ? juce::String(*src->analysis.bpm, 1) : juce::String("Unknown"));
            meta << "  |  KEY: " << src->analysis.key.label();
            g.drawText(meta, headerArea, juce::Justification::centredLeft, true);
        }
        else
        {
            g.drawText(col.badge + " Empty Slot — Drop WAV / AIFF / FLAC or click Load", headerArea, juce::Justification::centredLeft);
        }

        // Canvas background
        g.setColour(juce::Colour(0xff0b0e12));
        g.fillRoundedRectangle(canvasBounds.toFloat(), 5.0f);
        g.setColour(col.colour.withAlpha(0.35f));
        g.drawRoundedRectangle(canvasBounds.toFloat(), 5.0f, 1.0f);

        if (src == nullptr || src->audio == nullptr || src->numFrames <= 0)
        {
            g.setColour(juce::Colour(0xff6b7280));
            g.setFont(juce::FontOptions(14.0f));
            g.drawText("Drop an audio file here (WAV / AIFF / FLAC) to start chopping", canvasBounds, juce::Justification::centred);
            return;
        }

        if (displayMode == DisplayMode::Wave)
            drawWaveform(g, *src, canvasBounds);
        else if (displayMode == DisplayMode::Spectrogram)
            drawSpectrogram(g, *src, canvasBounds);
        else
        {
            auto top = canvasBounds.withHeight(canvasBounds.getHeight() / 2);
            auto bot = canvasBounds.withTrimmedTop(canvasBounds.getHeight() / 2);
            drawWaveform(g, *src, top);
            drawSpectrogram(g, *src, bot);
        }

        drawOverlays(g, *src, canvasBounds);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!canvasBounds.contains(e.getPosition())) return;
        const auto* src = processor.getProject().getSourceBySlot(slotIndex);
        if (src == nullptr || src->numFrames <= 0) return;

        const int64_t f = xToFrame(e.position.x, src->numFrames);
        selStartFrame = f;
        selEndFrame   = std::min(src->numFrames, f + static_cast<int64_t>(src->sampleRate * 0.5));
        draggingSel   = true;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!draggingSel) return;
        const auto* src = processor.getProject().getSourceBySlot(slotIndex);
        if (src == nullptr || src->numFrames <= 0) return;
        selEndFrame = xToFrame(e.position.x, src->numFrames);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!draggingSel) return;
        draggingSel = false;
        if (selEndFrame < selStartFrame) std::swap(selStartFrame, selEndFrame);
        if (selEndFrame - selStartFrame < 64)
            if (const auto* src = processor.getProject().getSourceBySlot(slotIndex))
                selEndFrame = std::min(src->numFrames, selStartFrame + static_cast<int64_t>(src->sampleRate * 0.5));
        auditionSelection();
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (!canvasBounds.contains(e.getPosition())) return;
        zoomFactor = juce::jlimit(1.0, 32.0, zoomFactor * (wheel.deltaY > 0 ? 1.25 : 0.8));
        scrollNorm = juce::jlimit(0.0, std::max(0.0, 1.0 - 1.0 / zoomFactor), scrollNorm);
        repaint();
    }

private:
    void openFilePicker()
    {
        fileChooser = std::make_unique<juce::FileChooser>("Load Audio Source", juce::File{}, "*.wav;*.aiff;*.aif;*.flac");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& fc) {
                                     const auto f = fc.getResult();
                                     if (f.existsAsFile())
                                     {
                                         processor.loadSourceFile(slotIndex, f);
                                         resetSelectionToDefault();
                                         if (onStateChanged) onStateChanged();
                                         repaint();
                                     }
                                 });
    }

    void resetSelectionToDefault()
    {
        if (const auto* src = processor.getProject().getSourceBySlot(slotIndex))
        {
            selStartFrame = 0;
            selEndFrame   = std::min<int64_t>(src->numFrames, static_cast<int64_t>(src->sampleRate * 1.0));
        }
        else
        {
            selStartFrame = 0;
            selEndFrame   = 0;
        }
    }

    void auditionSelection()
    {
        const auto* src = processor.getProject().getSourceBySlot(slotIndex);
        if (src == nullptr || src->numFrames <= 0) return;
        PadTriggerMessage msg;
        msg.isNoteOn           = true;
        msg.flatPadIndex       = -1;
        msg.origin             = TriggerOrigin::SourceAudition;
        msg.auditionSlot       = slotIndex;
        msg.auditionStartFrame = std::min(selStartFrame, selEndFrame);
        msg.auditionEndFrame   = std::max(selStartFrame, selEndFrame);
        processor.getVoiceEngine().pushGuiTrigger(msg);
    }

    void runChop(ChopAlgorithm algo)
    {
        auto& proj = processor.getProject();
        const auto* src = proj.getSourceBySlot(slotIndex);
        if (src == nullptr) return;
        ChopParams p;
        p.algorithm     = algo;
        p.maxCandidates = 8;
        p.regionCount   = 8;
        p.sensitivity   = 0.55f;
        p.beatsPerSlice = 1.0;
        auto cands = ClassicalAnalyzer::generateChopCandidates(*src, p, proj.state().projectBpm);
        proj.replaceCandidatesForSlot(slotIndex, std::move(cands));
        repaint();
    }

    int64_t visibleStart(int64_t totalFrames) const noexcept
    {
        return static_cast<int64_t>(scrollNorm * static_cast<double>(totalFrames));
    }

    int64_t visibleLength(int64_t totalFrames) const noexcept
    {
        return std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(totalFrames) / zoomFactor));
    }

    float frameToX(int64_t frame, int64_t totalFrames) const noexcept
    {
        const double norm = static_cast<double>(frame - visibleStart(totalFrames)) / static_cast<double>(visibleLength(totalFrames));
        return canvasBounds.getX() + static_cast<float>(norm) * static_cast<float>(canvasBounds.getWidth());
    }

    int64_t xToFrame(float x, int64_t totalFrames) const noexcept
    {
        const double norm = juce::jlimit(0.0, 1.0, static_cast<double>(x - canvasBounds.getX()) / std::max(1.0, static_cast<double>(canvasBounds.getWidth())));
        return juce::jlimit<int64_t>(0, totalFrames, visibleStart(totalFrames) + static_cast<int64_t>(norm * static_cast<double>(visibleLength(totalFrames))));
    }

    void drawWaveform(juce::Graphics& g, const SourceAsset& src, juce::Rectangle<int> area)
    {
        const int w = area.getWidth();
        if (w <= 1) return;
        const float* data = src.audio->getReadPointer(0);
        const int64_t vStart = visibleStart(src.numFrames);
        const int64_t vLen   = visibleLength(src.numFrames);
        const float midY = area.getCentreY();
        const float halfH = area.getHeight() * 0.45f;

        g.setColour(src.color.colour.withAlpha(0.75f));
        for (int x = 0; x < w; ++x)
        {
            const int64_t f0 = vStart + (static_cast<int64_t>(x) * vLen) / w;
            const int64_t f1 = std::min<int64_t>(src.numFrames, vStart + (static_cast<int64_t>(x + 1) * vLen) / w + 1);
            float mn = 0.0f, mx = 0.0f;
            const int64_t step = std::max<int64_t>(1, (f1 - f0) / 32);
            for (int64_t f = f0; f < f1; f += step)
            {
                const float s = data[f];
                if (s < mn) mn = s;
                if (s > mx) mx = s;
            }
            const float yTop = midY - mx * halfH;
            const float yBot = midY - mn * halfH;
            g.drawVerticalLine(area.getX() + x, yTop, std::max(yTop + 1.0f, yBot));
        }
    }

    void drawSpectrogram(juce::Graphics& g, const SourceAsset& src, juce::Rectangle<int> area)
    {
        if (src.analysis.specCols <= 0 || src.analysis.specBins <= 0) return;
        const int cols = src.analysis.specCols;
        const int bins = src.analysis.specBins;
        const float cellW = static_cast<float>(area.getWidth()) / static_cast<float>(cols);
        const float cellH = static_cast<float>(area.getHeight()) / static_cast<float>(bins);

        for (int c = 0; c < cols; ++c)
        {
            for (int b = 0; b < bins; ++b)
            {
                const float v = src.analysis.spectrogramCols[static_cast<size_t>(c * bins + b)];
                if (v < 0.05f) continue;
                g.setColour(src.color.colour.withAlpha(v * 0.9f));
                g.fillRect(area.getX() + c * cellW, area.getBottom() - (b + 1) * cellH, std::ceil(cellW), std::ceil(cellH));
            }
        }
    }

    void drawOverlays(juce::Graphics& g, const SourceAsset& src, juce::Rectangle<int> area)
    {
        const auto& proj = processor.getProject();

        // 1. Candidate slices (temporary proposals)
        for (const auto& c : proj.candidatesForSlot(slotIndex))
        {
            const float x0 = frameToX(c.startFrame, src.numFrames);
            const float x1 = frameToX(c.endFrameExclusive, src.numFrames);
            if (x1 < area.getX() || x0 > area.getRight()) continue;
            g.setColour(juce::Colour(0x28ffd166));
            g.fillRect(x0, static_cast<float>(area.getY()), std::max(2.0f, x1 - x0), static_cast<float>(area.getHeight()));
            g.setColour(juce::Colour(0xd0ffd166));
            g.drawVerticalLine(static_cast<int>(x0), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(c.originLabel, static_cast<int>(x0) + 3, area.getBottom() - 16, 70, 14, juce::Justification::centredLeft);
        }

        // 2. Assigned PAD regions on this source
        for (int i = 0; i < kTotalPads; ++i)
        {
            const auto& pad = proj.getPad(i);
            if (!pad.assignment.has_value() || pad.assignment->sourceId != src.id) continue;
            const auto* sl = proj.getSliceById(pad.assignment->sliceId);
            if (sl == nullptr) continue;
            const float x0 = frameToX(sl->startFrame, src.numFrames);
            const float x1 = frameToX(sl->endFrameExclusive, src.numFrames);
            if (x1 < area.getX() || x0 > area.getRight()) continue;

            g.setColour(src.color.colour.withAlpha(0.18f));
            g.fillRect(x0, static_cast<float>(area.getY()), std::max(2.0f, x1 - x0), static_cast<float>(area.getHeight()));
            g.setColour(src.color.colour);
            g.drawVerticalLine(static_cast<int>(x0), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
            g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            g.drawText(juce::String::formatted("PAD %02d", pad.padIndex + 1), static_cast<int>(x0) + 3, area.getY() + 3, 54, 14, juce::Justification::centredLeft);
        }

        // 3. Current manual selection
        const int64_t s = std::min(selStartFrame, selEndFrame);
        const int64_t e = std::max(selStartFrame, selEndFrame);
        if (e > s)
        {
            const float x0 = frameToX(s, src.numFrames);
            const float x1 = frameToX(e, src.numFrames);
            g.setColour(juce::Colour(0x38ffffff));
            g.fillRect(x0, static_cast<float>(area.getY()), std::max(2.0f, x1 - x0), static_cast<float>(area.getHeight()));
            g.setColour(juce::Colours::white);
            g.drawVerticalLine(static_cast<int>(x0), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
            g.drawVerticalLine(static_cast<int>(x1), static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
        }
    }

    HPSamplerAudioProcessor& processor;
    int         slotIndex     = 0;
    DisplayMode displayMode   = DisplayMode::Wave;
    double      zoomFactor    = 1.0;
    double      scrollNorm    = 0.0;
    int64_t     selStartFrame = 0;
    int64_t     selEndFrame   = 0;
    bool        draggingSel   = false;

    juce::Rectangle<int> canvasBounds;
    juce::TextButton loadBtn { "Load File" }, anchorBtn { "Use as Anchor" };
    juce::TextButton waveBtn { "WAVE" }, specBtn { "SPEC" }, splitBtn { "SPLIT" };
    juce::TextButton findSpotsBtn { "FIND SPOTS" }, slicerBtn { "SLICER" }, chopBtn { "CHOP" };
    juce::TextButton previewBtn { "PREVIEW" }, acceptBtn { "ACCEPT" }, autoMapBtn { "AUTO MAP" }, assignBtn { "ASSIGN -> PAD" };
    std::unique_ptr<juce::FileChooser> fileChooser;
};
} // namespace hps
