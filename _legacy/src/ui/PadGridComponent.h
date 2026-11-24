#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

#include "app/PluginProcessor.h"

namespace hps
{
// 16 physical tactile pads (2x8 or 1x16 adaptive) + Bank A/B/C/D.
// CRITICAL RULE (#7): NEVER use double-click to edit a pad!
// - In Perform mode: mouseDown = immediate PadNoteOn, mouseUp = PadNoteOff.
// - In Edit Select mode: single click selects pad for Inspector WITHOUT triggering audio.
// - Right-click always opens context menu WITHOUT triggering audio.
// - Dragging a pad onto MAIN drops a PatternEvent onto its source lane.
class PadGridComponent final : public juce::Component,
                               private juce::Timer
{
public:
    explicit PadGridComponent(HPSamplerAudioProcessor& proc) : processor(proc)
    {
        startTimerHz(30);
    }

    void setBank(int b) { bank = juce::jlimit(0, kNumBanks - 1, b); processor.getVoiceEngine().setActiveBank(bank); repaint(); }
    int  getBank() const noexcept { return bank; }

    void setEditSelectMode(bool on) { editSelectMode = on; repaint(); }
    bool isEditSelectMode() const noexcept { return editSelectMode; }

    void setAssignPendingMode(bool on) { assignPendingMode = on; repaint(); }
    bool isAssignPendingMode() const noexcept { return assignPendingMode; }

    void setSelectedPad(int flatIndex) { selectedFlatPad = flatIndex; repaint(); }
    int  getSelectedPad() const noexcept { return selectedFlatPad; }

    std::function<void(int flatPadIndex)> onPadSelectedForEdit;
    std::function<void(int flatPadIndex)> onAssignRequestedToPad;
    std::function<void(int flatPadIndex)> onLocatePadInSource;
    std::function<void()>                 onPadsMutated;

    void paint(juce::Graphics& g) override
    {
        const auto& project = processor.getProject();
        const bool singleRow = getWidth() >= 1180;
        const int cols = singleRow ? 16 : 8;
        const int rows = singleRow ? 1 : 2;
        const int gap  = 6;
        const float cellW = (getWidth() - gap * (cols + 1)) / static_cast<float>(cols);
        const float cellH = (getHeight() - gap * (rows + 1)) / static_cast<float>(rows);

        for (int p = 0; p < kPadsPerBank; ++p)
        {
            const int flat = bank * kPadsPerBank + p;
            const int c = p % cols;
            const int r = p / cols;
            const juce::Rectangle<float> rect(gap + c * (cellW + gap), gap + r * (cellH + gap), cellW, cellH);

            const auto& slot = project.getPad(flat);
            const bool assigned = slot.assignment.has_value();
            const bool active   = processor.getVoiceEngine().isPadActive(flat);
            const bool selected = (flat == selectedFlatPad);
            const auto srcCol   = project.getPadColor(flat);

            juce::Colour bg = assigned ? srcCol.colour.withMultipliedBrightness(active ? 0.55f : 0.24f)
                                       : juce::Colour(0xff23272e);
            if (pressedFlatPad == flat) bg = bg.brighter(0.25f);

            g.setColour(bg);
            g.fillRoundedRectangle(rect, 6.0f);

            // Border
            juce::Colour border = assigned ? srcCol.colour.withAlpha(active ? 1.0f : 0.72f) : juce::Colour(0xff3c424d);
            if (assignPendingMode) border = juce::Colour(0xffffd166);
            else if (editSelectMode && selected) border = juce::Colour(0xffffffff);
            else if (selected) border = border.brighter(0.4f);

            g.setColour(border);
            g.drawRoundedRectangle(rect.reduced(0.5f), 6.0f, (selected || active || assignPendingMode) ? 2.2f : 1.2f);

            // Top left: PAD number; Top right: S1..S4 badge for color-blind accessibility
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.setColour(assigned ? juce::Colours::white : juce::Colour(0xff7d8590));
            g.drawText(juce::String::formatted("P%02d", p + 1), rect.reduced(6, 4).removeFromTop(14), juce::Justification::centredLeft);

            if (assigned)
            {
                g.setColour(srcCol.colour);
                g.drawText(srcCol.badge, rect.reduced(6, 4).removeFromTop(14), juce::Justification::centredRight);

                g.setFont(juce::FontOptions(11.0f));
                g.setColour(juce::Colour(0xffe6edf3));
                g.drawText(slot.assignment->label, rect.reduced(6, 18), juce::Justification::centredLeft, true);

                // Playback mode pill
                const auto pm = slot.assignment->settings.playbackModeOverride.value_or(project.state().globalPlayback);
                const juce::String pmTag = pm == PlaybackMode::Gate ? "GATE" : (pm == PlaybackMode::Toggle ? "TOG" : "1SH");
                g.setFont(juce::FontOptions(9.5f));
                g.setColour(juce::Colour(0xffb0bac5));
                g.drawText(pmTag, rect.reduced(6, 4).removeFromBottom(12), juce::Justification::centredRight);
            }
            else if (assignPendingMode)
            {
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.setColour(juce::Colour(0xffffd166));
                g.drawText("CLICK TO ASSIGN", rect.reduced(4), juce::Justification::centred);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int flat = hitTestPad(e.position);
        if (flat < 0) return;

        // Right-click NEVER triggers sound
        if (e.mods.isPopupMenu())
        {
            showPadContextMenu(flat);
            return;
        }

        // Assign pending mode: single click assigns selected slice to this pad without playing
        if (assignPendingMode)
        {
            if (onAssignRequestedToPad) onAssignRequestedToPad(flat);
            return;
        }

        // Edit Select mode: single click selects pad for Inspector WITHOUT triggering audio
        if (editSelectMode)
        {
            selectedFlatPad = flat;
            if (onPadSelectedForEdit) onPadSelectedForEdit(flat);
            repaint();
            return;
        }

        // Perform mode: IMMEDIATE NoteOn on mouseDown (zero debounce, NEVER double-click edit)
        pressedFlatPad = flat;
        selectedFlatPad = flat;
        activeMouseToken = ++mouseTokenCounter;
        dragStartPos = e.position;
        dragStarted  = false;

        PadTriggerMessage msg;
        msg.isNoteOn     = true;
        msg.flatPadIndex = flat;
        msg.velocity     = 0.92f;
        msg.origin       = TriggerOrigin::GuiMouse;
        msg.token        = activeMouseToken;
        processor.getVoiceEngine().pushGuiTrigger(msg);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (pressedFlatPad < 0 || dragStarted || editSelectMode || assignPendingMode) return;
        if (e.position.getDistanceFrom(dragStartPos) > 14.0f)
        {
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                dragStarted = true;
                dnd->startDragging("PAD:" + juce::String(pressedFlatPad), this);
            }
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (pressedFlatPad >= 0)
        {
            PadTriggerMessage msg;
            msg.isNoteOn     = false;
            msg.flatPadIndex = pressedFlatPad;
            msg.origin       = TriggerOrigin::GuiMouse;
            msg.token        = activeMouseToken;
            processor.getVoiceEngine().pushGuiTrigger(msg);
            pressedFlatPad = -1;
            repaint();
        }
    }

private:
    void timerCallback() override { repaint(); }

    int hitTestPad(juce::Point<float> pos) const
    {
        const bool singleRow = getWidth() >= 1180;
        const int cols = singleRow ? 16 : 8;
        const int rows = singleRow ? 1 : 2;
        const int gap  = 6;
        const float cellW = (getWidth() - gap * (cols + 1)) / static_cast<float>(cols);
        const float cellH = (getHeight() - gap * (rows + 1)) / static_cast<float>(rows);

        for (int p = 0; p < kPadsPerBank; ++p)
        {
            const int c = p % cols;
            const int r = p / cols;
            const juce::Rectangle<float> rect(gap + c * (cellW + gap), gap + r * (cellH + gap), cellW, cellH);
            if (rect.contains(pos))
                return bank * kPadsPerBank + p;
        }
        return -1;
    }

    void showPadContextMenu(int flat)
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Edit Pad (Inspector)");
        menu.addItem(2, "Assign Current Source Selection");
        menu.addItem(3, "Locate in Source");
        menu.addSeparator();
        menu.addItem(4, "Copy Pad");
        menu.addItem(5, "Paste Pad", clipboardFlatPad >= 0);
        menu.addItem(6, "Clear Pad");

        menu.showMenuAsync(juce::PopupMenu::Options(), [this, flat](int res) {
            if (res == 1) { selectedFlatPad = flat; if (onPadSelectedForEdit) onPadSelectedForEdit(flat); }
            else if (res == 2) { if (onAssignRequestedToPad) onAssignRequestedToPad(flat); }
            else if (res == 3) { if (onLocatePadInSource) onLocatePadInSource(flat); }
            else if (res == 4) { clipboardFlatPad = flat; }
            else if (res == 5 && clipboardFlatPad >= 0) { processor.getProject().copyPad(clipboardFlatPad, flat); processor.publishProjectToAudio(); if (onPadsMutated) onPadsMutated(); }
            else if (res == 6) { processor.getProject().clearPad(flat); processor.publishProjectToAudio(); if (onPadsMutated) onPadsMutated(); }
            repaint();
        });
    }

    HPSamplerAudioProcessor& processor;
    int      bank              = 0;
    bool     editSelectMode    = false;
    bool     assignPendingMode = false;
    int      selectedFlatPad   = 0;
    int      pressedFlatPad    = -1;
    int      clipboardFlatPad  = -1;
    uint32_t mouseTokenCounter = 1;
    uint32_t activeMouseToken  = 0;
    juce::Point<float> dragStartPos;
    bool     dragStarted       = false;
};
} // namespace hps
