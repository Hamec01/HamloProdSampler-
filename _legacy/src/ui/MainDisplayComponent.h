#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/PluginProcessor.h"

namespace hps
{
// MAIN central display:
// - PERFORM / PATTERN / RESULT sub-modes
// - PATTERN ALWAYS shows exactly 4 horizontal SRC lanes (SRC 1..4) with source colors,
//   unified time ruler, optional HPDG DRUM GUIDE accent strip on top (NOT a 5th lane!),
//   clickable placement, and DragAndDropTarget for dropping Pads onto MAIN.
// - RESULT displays the real offline-rendered stereo waveform (never fake when empty).
class MainDisplayComponent final : public juce::Component,
                                   public juce::DragAndDropTarget,
                                   private juce::Timer
{
public:
    explicit MainDisplayComponent(HPSamplerAudioProcessor& proc) : processor(proc)
    {
        auto styleBtn = [](juce::TextButton& b) {
            b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff252a33));
            b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe6edf3));
        };
        for (auto* b : { &performBtn, &patternBtn, &resultBtn, &barsBtn, &clearBtn, &exportWavBtn, &exportMidiBtn, &guideToggleBtn })
        {
            styleBtn(*b);
            addAndMakeVisible(*b);
        }

        performBtn.onClick = [this] { viewMode = MainViewMode::Perform; repaint(); };
        patternBtn.onClick = [this] { viewMode = MainViewMode::Pattern; repaint(); };
        resultBtn.onClick  = [this] {
            viewMode = MainViewMode::Result;
            processor.rebuildResultWaveformCache();
            repaint();
        };

        barsBtn.onClick = [this] {
            auto& pat = processor.getProject().mutableState().pattern;
            pat.bars = (pat.bars == 1 ? 2 : (pat.bars == 2 ? 4 : 1));
            processor.getProject().bumpRevision();
            processor.publishProjectToAudio();
            barsBtn.setButtonText(juce::String(pat.bars) + " BARS");
            repaint();
        };
        clearBtn.onClick = [this] {
            processor.getProject().clearPattern();
            processor.publishProjectToAudio();
            repaint();
        };
        guideToggleBtn.onClick = [this] {
            showDrumGuide = !showDrumGuide;
            repaint();
        };
        exportWavBtn.onClick  = [this] { exportWavAsync(); };
        exportMidiBtn.onClick = [this] { exportMidiAsync(); };

        startTimerHz(30);
    }

    void setSelectedPadForDraw(int flatPad) noexcept { selectedFlatPad = flatPad; }
    std::function<void(int flatPadIndex)> onEventPadSelected;

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return details.description.toString().startsWith("PAD:");
    }

    void itemDropped(const SourceDetails& details) override
    {
        const int flat = details.description.toString().fromFirstOccurrenceOf("PAD:", false, false).getIntValue();
        if (flat < 0 || flat >= kTotalPads || !lanesArea.contains(details.localPosition))
            return;

        auto& proj = processor.getProject();
        const auto& pad = proj.getPad(flat);
        if (!pad.assignment.has_value()) return;

        const int64_t totalTicks = proj.getPatternLengthTicks();
        const double normX = juce::jlimit(0.0, 0.999, static_cast<double>(details.localPosition.x - lanesArea.getX()) / std::max(1, lanesArea.getWidth()));
        int64_t tick = static_cast<int64_t>(normX * static_cast<double>(totalTicks));
        const int64_t step = gridDivisionToTicks(proj.state().gridDivision);
        if (step > 0) tick = (tick / step) * step;

        PatternEvent ev;
        ev.id            = makeUuid();
        ev.padId         = pad.padId;
        ev.rawStartTick  = tick;
        ev.startTick     = tick;
        ev.durationTicks = std::max<int64_t>(240, step > 0 ? step : 480);
        ev.velocity      = 0.9f;
        proj.addPatternEvent(std::move(ev), true);
        processor.publishProjectToAudio();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        auto top = area.removeFromTop(26);
        performBtn.setBounds(top.removeFromLeft(78).reduced(2));
        patternBtn.setBounds(top.removeFromLeft(78).reduced(2));
        resultBtn.setBounds(top.removeFromLeft(74).reduced(2));
        top.removeFromLeft(10);
        barsBtn.setBounds(top.removeFromLeft(66).reduced(2));
        guideToggleBtn.setBounds(top.removeFromLeft(96).reduced(2));
        exportMidiBtn.setBounds(top.removeFromRight(84).reduced(2));
        exportWavBtn.setBounds(top.removeFromRight(84).reduced(2));
        clearBtn.setBounds(top.removeFromRight(60).reduced(2));

        bodyArea  = area.reduced(0, 4);
        rulerArea = bodyArea.withHeight(22).withTrimmedLeft(108);
        lanesArea = bodyArea.withTrimmedTop(24).withTrimmedLeft(108);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff101318));
        barsBtn.setButtonText(juce::String(processor.getProject().state().pattern.bars) + " BARS");

        g.setColour(juce::Colour(0xff0b0e12));
        g.fillRoundedRectangle(bodyArea.toFloat(), 5.0f);
        g.setColour(juce::Colour(0xff2c333f));
        g.drawRoundedRectangle(bodyArea.toFloat(), 5.0f, 1.0f);

        if (viewMode == MainViewMode::Result)
        {
            drawResultWaveform(g);
            return;
        }

        drawFourSourceLanes(g);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (viewMode == MainViewMode::Result) return;

        // Check Mute/Solo click on lane headers
        auto headerCol = bodyArea.withTrimmedTop(24).withWidth(104);
        if (headerCol.contains(e.getPosition()))
        {
            const int laneH = std::max(1, headerCol.getHeight() / kNumVisibleSources);
            const int lane = juce::jlimit(0, kNumVisibleSources - 1, (e.getPosition().y - headerCol.getY()) / laneH);
            auto& lm = processor.getProject().mutableState().laneMix[static_cast<size_t>(lane)];
            if (e.mods.isPopupMenu() || e.mods.isShiftDown()) lm.soloed = !lm.soloed;
            else lm.muted = !lm.muted;
            processor.publishProjectToAudio();
            repaint();
            return;
        }

        if (!lanesArea.contains(e.getPosition())) return;

        auto& proj = processor.getProject();
        const int64_t totalTicks = proj.getPatternLengthTicks();
        const double normX = juce::jlimit(0.0, 0.999, static_cast<double>(e.getPosition().x - lanesArea.getX()) / std::max(1, lanesArea.getWidth()));
        const int64_t clickTick = static_cast<int64_t>(normX * static_cast<double>(totalTicks));
        const int laneH = std::max(1, lanesArea.getHeight() / kNumVisibleSources);
        const int clickedLane = juce::jlimit(0, kNumVisibleSources - 1, (e.getPosition().y - lanesArea.getY()) / laneH);

        // Right-click deletes event under cursor
        if (e.mods.isPopupMenu())
        {
            for (const auto& ev : proj.state().pattern.events)
            {
                if (proj.getEventSourceLane(ev) == clickedLane && clickTick >= ev.startTick && clickTick <= ev.startTick + std::max<int64_t>(240, ev.durationTicks))
                {
                    proj.removePatternEvent(ev.id);
                    processor.publishProjectToAudio();
                    repaint();
                    return;
                }
            }
            return;
        }

        // Left-click: pick a pad belonging to this SRC lane (prefer selectedFlatPad if same SRC)
        int chosenPad = -1;
        if (proj.getPadSourceSlot(selectedFlatPad) == clickedLane)
            chosenPad = selectedFlatPad;
        else
        {
            for (int i = 0; i < kTotalPads; ++i)
                if (proj.getPadSourceSlot(i) == clickedLane) { chosenPad = i; break; }
        }
        if (chosenPad < 0) return;

        const int64_t step = gridDivisionToTicks(proj.state().gridDivision);
        const int64_t snapped = step > 0 ? (clickTick / step) * step : clickTick;
        PatternEvent ev;
        ev.id            = makeUuid();
        ev.padId         = proj.getPad(chosenPad).padId;
        ev.rawStartTick  = snapped;
        ev.startTick     = snapped;
        ev.durationTicks = std::max<int64_t>(240, step > 0 ? step : 480);
        ev.velocity      = 0.9f;
        proj.addPatternEvent(std::move(ev), true);
        processor.publishProjectToAudio();
        if (onEventPadSelected) onEventPadSelected(chosenPad);
        repaint();
    }

private:
    void timerCallback() override { repaint(); }

    void drawFourSourceLanes(juce::Graphics& g)
    {
        const auto& proj = processor.getProject();
        const auto& st   = proj.state();
        const int64_t totalTicks = proj.getPatternLengthTicks();
        const int bars = std::max(1, st.pattern.bars);

        // 1. Unified Time Ruler + optional HPDG Drum Guide overlay (NEVER a 5th lane!)
        g.setColour(juce::Colour(0xff171b22));
        g.fillRect(rulerArea);
        for (int b = 0; b <= bars * 4; ++b)
        {
            const float x = rulerArea.getX() + (static_cast<float>(b) / static_cast<float>(bars * 4)) * rulerArea.getWidth();
            const bool isBar = (b % 4 == 0);
            g.setColour(isBar ? juce::Colour(0xff4c566a) : juce::Colour(0xff262c36));
            g.drawVerticalLine(static_cast<int>(x), static_cast<float>(rulerArea.getY()), static_cast<float>(lanesArea.getBottom()));
            if (isBar && b < bars * 4)
            {
                g.setColour(juce::Colour(0xffc9d1d9));
                g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
                g.drawText("BAR " + juce::String(b / 4 + 1), static_cast<int>(x) + 4, rulerArea.getY() + 2, 48, 16, juce::Justification::centredLeft);
            }
        }

        // HPDG Drum Guide accent markers on top of the ruler
        if (showDrumGuide && proj.getExternalDrumGuide().has_value())
        {
            const auto& guide = *proj.getExternalDrumGuide();
            for (const auto& de : guide.drumEvents)
            {
                const float x = rulerArea.getX() + (static_cast<float>(de.tick % totalTicks) / static_cast<float>(totalTicks)) * rulerArea.getWidth();
                const juce::Colour c = de.laneClass == hpbridge::DrumLaneClass::Kick  ? juce::Colour(0xffff5c5c)
                                     : de.laneClass == hpbridge::DrumLaneClass::Snare ? juce::Colour(0xffffd166)
                                                                                      : juce::Colour(0x889aa5b1);
                g.setColour(c);
                g.fillEllipse(x - 2.5f, static_cast<float>(rulerArea.getBottom() - 7), 5.0f, 5.0f);
            }
        }

        // 2. Exactly 4 horizontal SRC lanes (SRC 1..4)
        const int laneH = std::max(1, lanesArea.getHeight() / kNumVisibleSources);
        for (int lane = 0; lane < kNumVisibleSources; ++lane)
        {
            const int y = lanesArea.getY() + lane * laneH;
            const auto srcCol = SourceColor::defaultForSlot(lane);
            const auto* src   = proj.getSourceBySlot(lane);
            const auto& lm    = st.laneMix[static_cast<size_t>(lane)];

            // Lane header
            juce::Rectangle<int> hdr(bodyArea.getX() + 4, y + 2, 100, laneH - 4);
            g.setColour(srcCol.colour.withAlpha(lm.muted ? 0.08f : 0.18f));
            g.fillRoundedRectangle(hdr.toFloat(), 4.0f);
            g.setColour(srcCol.colour);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(srcCol.badge + " " + (src != nullptr ? src->name.substring(0, 9) : "Empty"),
                       hdr.reduced(5, 2).removeFromTop(16), juce::Justification::centredLeft, true);
            g.setFont(juce::FontOptions(9.5f));
            g.setColour(lm.muted ? juce::Colour(0xffff6b6b) : (lm.soloed ? juce::Colour(0xffffd166) : juce::Colour(0xff8b949e)));
            g.drawText(lm.muted ? "MUTE" : (lm.soloed ? "SOLO" : "Click:M / R:S"),
                       hdr.reduced(5, 2).removeFromBottom(14), juce::Justification::centredLeft);

            // Lane separator
            g.setColour(juce::Colour(0xff21262d));
            g.drawHorizontalLine(y + laneH - 1, static_cast<float>(lanesArea.getX()), static_cast<float>(lanesArea.getRight()));
        }

        // 3. Draw PatternEvents projected onto their true SRC lane (with overlap stacking!)
        std::array<std::vector<const PatternEvent*>, kNumVisibleSources> byLane;
        for (const auto& ev : st.pattern.events)
        {
            const int lane = proj.getEventSourceLane(ev);
            if (lane >= 0 && lane < kNumVisibleSources)
                byLane[static_cast<size_t>(lane)].push_back(&ev);
        }

        for (int lane = 0; lane < kNumVisibleSources; ++lane)
        {
            const auto srcCol = SourceColor::defaultForSlot(lane);
            const int yBase   = lanesArea.getY() + lane * laneH + 4;
            const int clipH   = std::max(14, laneH - 10);

            for (size_t i = 0; i < byLane[static_cast<size_t>(lane)].size(); ++i)
            {
                const auto* ev = byLane[static_cast<size_t>(lane)][i];
                const int flat = proj.findPadFlatIndexById(ev->padId);
                const float x0 = lanesArea.getX() + (static_cast<float>(ev->startTick) / static_cast<float>(totalTicks)) * lanesArea.getWidth();
                const float wTrig = std::max(10.0f, (static_cast<float>(ev->durationTicks) / static_cast<float>(totalTicks)) * lanesArea.getWidth());

                // Audible tail preview (semi-transparent extension)
                g.setColour(srcCol.colour.withAlpha(0.22f));
                g.fillRoundedRectangle(x0, static_cast<float>(yBase), std::min(wTrig * 1.4f, lanesArea.getRight() - x0), static_cast<float>(clipH), 3.0f);

                // Trigger block
                g.setColour(srcCol.colour.withAlpha(0.85f));
                g.fillRoundedRectangle(x0, static_cast<float>(yBase), wTrig, static_cast<float>(clipH), 3.0f);
                g.setColour(juce::Colours::black);
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText(flat >= 0 ? juce::String::formatted("P%02d", (flat % kPadsPerBank) + 1) : "PAD",
                           static_cast<int>(x0) + 3, yBase + 2, static_cast<int>(wTrig) - 4, clipH - 4, juce::Justification::centredLeft, true);
            }
        }

        // 4. Playhead cursor
        const double tick = processor.getVoiceEngine().getPlayheadTick();
        const float px = lanesArea.getX() + static_cast<float>(std::fmod(tick, static_cast<double>(totalTicks)) / static_cast<double>(totalTicks)) * lanesArea.getWidth();
        g.setColour(processor.getVoiceEngine().isRecording() ? juce::Colour(0xffff4d4d) : juce::Colours::white);
        g.drawVerticalLine(static_cast<int>(px), static_cast<float>(rulerArea.getY()), static_cast<float>(lanesArea.getBottom()));
    }

    void drawResultWaveform(juce::Graphics& g)
    {
        const auto& buf = processor.getResultWaveformCache();
        if (processor.getProject().state().pattern.events.empty() || buf.getNumSamples() <= 0)
        {
            g.setColour(juce::Colour(0xff8b949e));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText("MAIN RESULT is empty — record or place pads in PATTERN to render the mix", bodyArea, juce::Justification::centred);
            return;
        }

        const auto area = bodyArea.reduced(10);
        const int w = area.getWidth();
        const float* data = buf.getReadPointer(0);
        const int n = buf.getNumSamples();
        const float midY = area.getCentreY();
        const float halfH = area.getHeight() * 0.45f;

        g.setColour(juce::Colour(0xff58a6ff));
        for (int x = 0; x < w; ++x)
        {
            const int f0 = (x * n) / w;
            const int f1 = std::min(n, ((x + 1) * n) / w + 1);
            float mn = 0.0f, mx = 0.0f;
            for (int f = f0; f < f1; f += std::max(1, (f1 - f0) / 24))
            {
                mn = std::min(mn, data[f]);
                mx = std::max(mx, data[f]);
            }
            g.drawVerticalLine(area.getX() + x, midY - mx * halfH, std::max(midY - mx * halfH + 1.0f, midY - mn * halfH));
        }
    }

    void exportWavAsync()
    {
        fileChooser = std::make_unique<juce::FileChooser>("Export MAIN WAV", juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("hpsampler_main.wav"), "*.wav");
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& fc) {
                                     const auto f = fc.getResult();
                                     if (f.getFullPathName().isNotEmpty())
                                     {
                                         processor.rebuildResultWaveformCache();
                                         VoiceEngine::exportWav(f, processor.getResultWaveformCache());
                                     }
                                 });
    }

    void exportMidiAsync()
    {
        fileChooser = std::make_unique<juce::FileChooser>("Export MIDI Triggers", juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("hpsampler_triggers.mid"), "*.mid");
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& fc) {
                                     const auto f = fc.getResult();
                                     if (f.getFullPathName().isNotEmpty())
                                         VoiceEngine::exportMidiTriggers(f, processor.getProject());
                                 });
    }

    HPSamplerAudioProcessor& processor;
    MainViewMode viewMode = MainViewMode::Pattern;
    bool showDrumGuide    = true;
    int  selectedFlatPad  = 0;

    juce::Rectangle<int> bodyArea, rulerArea, lanesArea;
    juce::TextButton performBtn { "PERFORM" }, patternBtn { "PATTERN" }, resultBtn { "RESULT" };
    juce::TextButton barsBtn { "2 BARS" }, guideToggleBtn { "DRUM GUIDE" }, clearBtn { "Clear" };
    juce::TextButton exportWavBtn { "Export WAV" }, exportMidiBtn { "Export MIDI" };
    std::unique_ptr<juce::FileChooser> fileChooser;
};
} // namespace hps
