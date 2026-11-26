#include "ui/PadGrid.h"

namespace hps
{
int PadButton::flatIndex() const { return grid.proc.ui.bank * kPadsPerBank + indexInBank; }

PadButton::PadButton(PadGrid& g, int index) : grid(g), indexInBank(index)
{
    int k = 0;
    for (auto* knob : { &vol, &pan, &flt })
    {
        knob->setCaptionVisible(true);
        knob->onGestureStart = [this] { grid.proc.project.beginGesture("Pad mix"); };
        knob->onGestureEnd = [this] { grid.proc.project.endGesture(); };
        knob->onChange = [this, k](float v)
        {
            const int pad = flatIndex();
            if (!grid.proc.project.state().pads[(size_t) pad].assigned) return;
            grid.proc.project.edit(padLabel(pad) + (k == 0 ? " volume" : k == 1 ? " pan" : " filter"), [&](ProjectState& s)
            {
                auto& ps = s.pads[(size_t) pad].s;
                if (k == 0) ps.gainDb = v;
                else if (k == 1) ps.pan = v;
                else filterKnobToHz(v, ps.hpHz, ps.lpHz);
            });
        };
        addChildComponent(knob);
        ++k;
    }
}

bool PadButton::knobsShown() const
{
    return getWidth() >= 118 && getHeight() >= 92 && grid.proc.project.state().pads[(size_t) flatIndex()].assigned;
}

void PadButton::resized()
{
    // bottom-right corner of the pad, next to the badges
    auto r = getLocalBounds().reduced(6, 8).removeFromBottom(34);
    auto row = r.removeFromRight(juce::jmin(96, r.getWidth() / 2 + 18));
    const int w = row.getWidth() / 3;
    for (auto* knob : { &vol, &pan, &flt }) knob->setBounds(row.removeFromLeft(w));
    updateKnobs();
}

void PadButton::updateKnobs()
{
    const auto& st = grid.proc.project.state();
    const auto& p = st.pads[(size_t) flatIndex()];
    const bool shown = knobsShown();
    for (auto* knob : { &vol, &pan, &flt })
    {
        knob->setVisible(shown);
        if (shown) knob->setAccent(st.sources[(size_t) p.src].colour);
    }
    if (!shown) return;
    vol.setValueQuietly(p.s.gainDb);
    pan.setValueQuietly(p.s.pan);
    flt.setValueQuietly(hzToFilterKnob(p.s.hpHz, p.s.lpHz));
}

void PadButton::paint(juce::Graphics& g)
{
    const auto& st = grid.proc.project.state();
    const int pad = flatIndex();
    const auto& p = st.pads[(size_t) pad];
    auto r = getLocalBounds().toFloat().reduced(3.0f);
    const bool selected = grid.proc.ui.selectedPad == pad;
    const bool sounding = grid.proc.engine.padVoices[(size_t) pad].load() > 0;
    const auto col = p.assigned ? st.sources[(size_t) p.src].colour : theme::panel;

    // body
    juce::ColourGradient grad(theme::panelHi, r.getX(), r.getY(), theme::panel.darker(0.2f), r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(r, 6.0f);
    if (p.assigned)
    {
        g.setColour(col.withAlpha(0.22f + 0.5f * flash + (sounding ? 0.12f : 0.0f)));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(col);
        g.fillRoundedRectangle(r.getX() + 6, r.getY() + 5, r.getWidth() - 12, 3.0f, 1.5f);
    }
    if (flash > 0.01f)
    {
        g.setColour(juce::Colours::white.withAlpha(flash * 0.35f));
        g.fillRoundedRectangle(r, 6.0f);
    }
    g.setColour(dropHover ? theme::accent : (selected ? juce::Colours::white.withAlpha(0.85f) : juce::Colours::black.withAlpha(0.6f)));
    g.drawRoundedRectangle(r, 6.0f, dropHover || selected ? 2.0f : 1.0f);

    // labels
    auto inner = r.reduced(7.0f, 10.0f).toNearestInt();
    g.setColour(theme::text);
    g.setFont(theme::mono(12.0f, true));
    g.drawText(padLabel(pad), inner.removeFromTop(15), juce::Justification::topLeft);
    if (p.assigned)
    {
        g.setFont(theme::mono(10.0f, true));
        g.setColour(col);
        auto topRow = inner.withHeight(15).translated(0, -15);
        g.drawText("S" + juce::String(p.src + 1), topRow.removeFromRight(18), juce::Justification::topRight);
        if (p.s.chokeGroup > 0) // always visible
        {
            const auto cell = topRow.removeFromRight(34).withHeight(13);
            g.setColour(theme::rec.withAlpha(0.85f));
            g.fillRoundedRectangle(cell.toFloat(), 3.0f);
            g.setColour(juce::Colours::white);
            g.drawText("CUT" + juce::String(p.s.chokeGroup), cell, juce::Justification::centred);
        }
        g.setColour(theme::text.withAlpha(0.85f));
        g.setFont(theme::ui(11.0f));
        g.drawFittedText(p.name, inner.removeFromTop(knobsShown() ? 14 : 28), juce::Justification::topLeft, knobsShown() ? 1 : 2, 0.8f);

        // badges: mode, repeat, cut group, loop, reverse
        const auto mode = effectivePlayMode(p, st.settings);
        juce::StringArray badges;
        badges.add(mode == PlayMode::Gate ? "GATE" : mode == PlayMode::OneShot ? "1SHOT" : "TGL");
        if (p.s.repeat == RepeatMode::Layer) badges.add("LYR");
        if (std::abs(p.s.speed - 1.0f) > 0.005f) badges.add("x" + juce::String(p.s.speed, p.s.speed < 1.0f ? 2 : 1));
        if (p.s.loop) badges.add("LOOP");
        if (p.s.reverse) badges.add("REV");
        auto br = inner.removeFromBottom(14);
        if (knobsShown()) br = br.withWidth(juce::jmax(40, br.getWidth() - 98));
        g.setFont(theme::mono(9.5f, true));
        for (const auto& b : badges)
        {
            const int w = (int) (b.length() * 6.2f) + 6;
            if (w > br.getWidth()) break;
            auto cell = br.removeFromLeft(w);
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRoundedRectangle(cell.toFloat(), 3.0f);
            g.setColour(theme::text.withAlpha(0.8f));
            g.drawText(b, cell, juce::Justification::centred);
            br.removeFromLeft(3);
        }
    }
    else
    {
        const bool mark = grid.proc.engine.previewFrame.load() >= 0.0 && grid.proc.ui.padMode == UiState::PadMode::Perform;
        g.setColour(mark ? theme::ok.withAlpha(0.8f) : theme::textDim.withAlpha(0.6f));
        g.setFont(theme::ui(10.5f));
        g.drawFittedText(mark ? "hit = cut here" : "empty\ndrop a selection", inner, juce::Justification::centred, 2);
    }
    if (p.assigned && grid.proc.isPadRendering(pad)) // still the old sound
    {
        const auto chip = r.withSizeKeepingCentre(juce::jmin(84.0f, r.getWidth() - 12.0f), 16.0f);
        g.setColour(theme::screen.withAlpha(0.85f));
        g.fillRoundedRectangle(chip, 3.0f);
        g.setColour(theme::accent);
        g.setFont(theme::mono(9.5f, true));
        g.drawText("UPDATING...", chip, juce::Justification::centred);
    }
    if (grid.proc.ui.padMode == UiState::PadMode::Cut && p.assigned && p.s.chokeGroup == grid.proc.ui.cutPaint)
    {
        g.setColour(theme::rec.withAlpha(0.28f));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(theme::rec);
        g.drawRoundedRectangle(r, 6.0f, 2.0f);
    }
    if (grid.proc.ui.padMode == UiState::PadMode::Assign)
    {
        g.setColour(theme::accent.withAlpha(0.18f));
        g.fillRoundedRectangle(r, 6.0f);
    }
}

void PadButton::mouseDown(const juce::MouseEvent& e)
{
    const int pad = flatIndex();
    dragStarted = false;
    if (e.mods.isPopupMenu()) { grid.showPadMenu(pad, this); return; } // never makes a sound
    auto& ui = grid.proc.ui;
    switch (ui.padMode)
    {
        case UiState::PadMode::Perform:
            grid.proc.triggerPad(pad, 0.9f, e.source.getIndex()); // immediate NoteOn, no debounce
            playing = true;
            flash = 1.0f;
            repaint();
            break;
        case UiState::PadMode::Edit:
            grid.selectForEdit(pad);
            break;
        case UiState::PadMode::Cut:
            grid.beginCutSweep(pad);
            break;
        case UiState::PadMode::Assign:
        {
            const int slot = ui.view;
            if (slot >= 0)
            {
                const auto& v = ui.src[(size_t) slot];
                if (v.selStart >= 0 && v.selEnd > v.selStart) grid.proc.assignToPad(pad, slot, v.selStart, v.selEnd);
            }
            ui.padMode = UiState::PadMode::Perform;
            grid.refresh();
            break;
        }
    }
}

void PadButton::mouseUp(const juce::MouseEvent& e)
{
    grid.endCutSweep();
    if (playing)
    {
        grid.proc.releasePad(flatIndex(), e.source.getIndex());
        playing = false;
    }
}

void PadButton::mouseDrag(const juce::MouseEvent& e)
{
    if (grid.proc.ui.padMode == UiState::PadMode::Cut) { grid.cutSweepTo(e.getEventRelativeTo(&grid).getPosition()); return; }
    if (dragStarted || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 12) return;
    const auto& p = grid.proc.project.state().pads[(size_t) flatIndex()];
    if (!p.assigned) return;
    if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this))
    {
        dragStarted = true;
        if (playing) { grid.proc.releasePad(flatIndex(), e.source.getIndex()); playing = false; }
        juce::Image img(juce::Image::ARGB, 60, 24, true);
        {
            juce::Graphics ig(img);
            ig.setColour(grid.proc.project.state().sources[(size_t) p.src].colour.withAlpha(0.9f));
            ig.fillRoundedRectangle(0, 0, 60, 24, 4);
            ig.setColour(theme::screen);
            ig.setFont(theme::mono(12.0f, true));
            ig.drawText(padLabel(flatIndex()), 0, 0, 60, 24, juce::Justification::centred);
        }
        c->startDragging("pad:" + juce::String(flatIndex()), this, juce::ScaledImage(img), true);
    }
}

bool PadButton::isInterestedInDragSource(const SourceDetails& d)
{
    return d.description.toString().startsWith("sel:");
}

void PadButton::itemDropped(const SourceDetails& d)
{
    dropHover = false;
    const auto parts = juce::StringArray::fromTokens(d.description.toString(), ":", "");
    if (parts.size() != 4) return;
    const int slot = parts[1].getIntValue();
    const int64_t s = parts[2].getLargeIntValue(), e = parts[3].getLargeIntValue();
    const int pad = flatIndex();
    auto& proc = grid.proc;
    if (proc.project.state().pads[(size_t) pad].assigned)
    {
        juce::PopupMenu m;
        m.addItem(1, "Replace " + padLabel(pad) + " (" + proc.project.state().pads[(size_t) pad].name + ")");
        m.addItem(2, "Put on the next free pad instead");
        m.addItem(0, "Cancel");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [&proc, pad, slot, s, e](int r)
        {
            if (r == 1) proc.assignToPad(pad, slot, s, e);
            else if (r == 2) proc.assignToNextEmptyPad(slot, s, e);
        });
    }
    else proc.assignToPad(pad, slot, s, e);
    repaint();
}

// =============================================================================================

PadGrid::PadGrid(HPSamplerProcessor& p) : proc(p)
{
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        auto* b = pads.add(new PadButton(*this, i));
        addAndMakeVisible(b);
    }
    for (int i = 0; i < kNumBanks; ++i)
    {
        banks[i].setButtonText("BANK " + juce::String::charToString(juce::juce_wchar('A' + i)));
        banks[i].onClick = [this, i]
        {
            if (std::exchange(rightClick, false)) return;
            proc.ui.bank = i;
            if (proc.ui.editBank >= 0 && onEditBank) onEditBank(i); // the bank being edited follows
            else refresh();
        };
        banks[i].setTooltip("Right-click: edit all pads of this bank at once");
        banks[i].addMouseListener(this, false);
        banks[i].setWantsKeyboardFocus(false);
        addAndMakeVisible(banks[i]);
    }
    performBtn.setTooltip("Normal play: every click is a hit, two quick clicks = two hits");
    editBtn.setTooltip("Click a pad to select it for editing - it does NOT play");
    performBtn.onClick = [this] { proc.ui.padMode = UiState::PadMode::Perform; refresh(); };
    editBtn.onClick = [this] { proc.ui.padMode = UiState::PadMode::Edit; refresh(); };
    for (auto* b : { &performBtn, &editBtn, &gateBtn, &oneShotBtn, &toggleBtn, &retrigBtn, &layerBtn, &loopBtn, &revBtn, &inspectBtn })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    addAndMakeVisible(padName);
    padName.setFont(theme::mono(13.0f, true));
    addAndMakeVisible(cutLabel);
    cutLabel.setText("CUT GROUP", juce::dontSendNotification);
    cutLabel.setFont(theme::ui(11.0f, true));
    cutLabel.setColour(juce::Label::textColourId, theme::textDim);
    cutLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(hint);
    hint.setFont(theme::ui(11.0f));
    hint.setColour(juce::Label::textColourId, theme::textDim);

    gateBtn.setTooltip("Plays while held, stops on release");
    oneShotBtn.setTooltip("Plays to the end on a single hit");
    toggleBtn.setTooltip("First hit starts, next hit stops");
    retrigBtn.setTooltip("A new hit on this pad restarts it (the old one fades out)");
    layerBtn.setTooltip("New hits on this pad overlap the previous ones");
    gateBtn.onClick = [this] { setPadField("Gate", [](PadSettings& s) { s.playMode = (int) PlayMode::Gate; }); };
    oneShotBtn.onClick = [this] { setPadField("One shot", [](PadSettings& s) { s.playMode = (int) PlayMode::OneShot; }); };
    toggleBtn.onClick = [this] { setPadField("Toggle", [](PadSettings& s) { s.playMode = (int) PlayMode::Toggle; }); };
    retrigBtn.onClick = [this] { setPadField("Retrigger", [](PadSettings& s) { s.repeat = RepeatMode::Retrigger; }); };
    layerBtn.onClick = [this] { setPadField("Layer", [](PadSettings& s) { s.repeat = RepeatMode::Layer; }); };
    for (int i = 0; i < 5; ++i)
    {
        cutBtns[i].setButtonText(i == 0 ? "OFF" : juce::String(i));
        cutBtns[i].setWantsKeyboardFocus(false);
        cutBtns[i].setTooltip(i == 0 ? juce::String("This pad cuts nothing. Right-click: the same for many pads") : "Pads in cut group " + juce::String(i)
                              + " stop each other: a new hit silences the others of the group. Right-click: whole bank, whole source or all pads");
        cutBtns[i].addMouseListener(this, false);
        cutBtns[i].getProperties().set("led", (juce::int64) theme::rec.getARGB());
        cutBtns[i].onClick = [this, i]
        {
            if (std::exchange(rightClick, false)) return;
            if (proc.ui.padMode == UiState::PadMode::Cut) { proc.ui.cutPaint = i; refresh(); }
            else setPadField("Cut group", [i](PadSettings& s) { s.chokeGroup = i; });
        };
        addAndMakeVisible(cutBtns[i]);
    }
    for (auto* b : { &cutBankBtn, &cutPickBtn })
    {
        b->setWantsKeyboardFocus(false);
        b->getProperties().set("led", (juce::int64) theme::rec.getARGB());
        addAndMakeVisible(*b);
    }
    cutBankBtn.setTooltip("One click: all pads of this bank cut each other (again: off)");
    cutBankBtn.onClick = [this]
    {
        const int shown = proc.ui.padMode == UiState::PadMode::Cut ? proc.ui.cutPaint
                                                                  : proc.project.state().pads[(size_t) proc.editRefPad()].s.chokeGroup;
        proc.setCutGroup(HPSamplerProcessor::CutScope::Bank, shown > 0 ? shown : 1, true);
    };
    cutPickBtn.setTooltip("Choose a group (1-4), then click pads or sweep over them with the button held. No sound. Click PICK again or Esc when done.");
    cutPickBtn.onClick = [this]
    {
        const bool on = proc.ui.padMode != UiState::PadMode::Cut;
        proc.ui.padMode = on ? UiState::PadMode::Cut : UiState::PadMode::Perform;
        if (on) if (const int g = proc.project.state().pads[(size_t) proc.ui.selectedPad].s.chokeGroup; g >= 1 && g <= 4) proc.ui.cutPaint = g;
        refresh();
    };
    loopBtn.onClick = [this]
    {
        const bool v = !proc.project.state().pads[(size_t) proc.editRefPad()].s.loop;
        setPadField("Loop", [v](PadSettings& s) { s.loop = v; });
    };
    revBtn.onClick = [this]
    {
        const bool v = !proc.project.state().pads[(size_t) proc.editRefPad()].s.reverse;
        setPadField("Reverse", [v](PadSettings& s) { s.reverse = v; });
    };
    inspectBtn.onClick = [this] { if (onOpenInspector) onOpenInspector(); };
    startTimerHz(30);
    refresh();
}

PadGrid::~PadGrid() { stopTimer(); }

void PadGrid::setPadField(const juce::String& label, const std::function<void(PadSettings&)>& fn)
{
    proc.editPads(label, [&](PadSettings& s, int) { fn(s); });
}

void PadGrid::mouseDown(const juce::MouseEvent& e)
{
    rightClick = e.mods.isPopupMenu();
    if (!rightClick) return;
    for (int i = 0; i < 5; ++i)
    {
        if (e.eventComponent != &cutBtns[i]) continue;
        using Scope = HPSamplerProcessor::CutScope;
        const juce::String to = i == 0 ? juce::String("no cut") : "cut group " + juce::String(i);
        auto count = [this](Scope s) { return juce::String((int) proc.cutScopePads(s).size()); };
        const auto& sel = proc.project.state().pads[(size_t) proc.ui.selectedPad];
        juce::PopupMenu m;
        m.addItem(1, "All pads of BANK " + juce::String::charToString(juce::juce_wchar('A' + proc.ui.bank)) + " (" + count(Scope::Bank) + "): " + to);
        m.addItem(2, "All pads of SRC " + juce::String(sel.src + 1) + " (" + count(Scope::Source) + "): " + to, sel.assigned);
        m.addItem(3, "All pads (" + count(Scope::All) + "): " + to);
        m.addSeparator();
        m.addItem(4, "Pick pads with the mouse...");
        juce::Component::SafePointer<PadGrid> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&cutBtns[i]), [safe, i](int r)
        {
            if (safe == nullptr || r == 0) return;
            if (r == 4) { safe->proc.ui.cutPaint = i; safe->proc.ui.padMode = UiState::PadMode::Cut; safe->refresh(); }
            else safe->proc.setCutGroup(r == 1 ? Scope::Bank : r == 2 ? Scope::Source : Scope::All, i, false);
        });
        return;
    }
    for (int i = 0; i < kNumBanks; ++i)
    {
        if (e.eventComponent != &banks[i]) continue;
        const juce::String name = "BANK " + juce::String::charToString(juce::juce_wchar('A' + i));
        int n = 0;
        for (int p = i * kPadsPerBank; p < (i + 1) * kPadsPerBank; ++p) n += proc.project.state().pads[(size_t) p].assigned ? 1 : 0;
        juce::PopupMenu m;
        m.addItem(1, "Edit " + name + "  (" + juce::String(n) + " pads at once)...", n > 0, proc.ui.editBank == i);
        m.addItem(2, "Stop editing the bank (back to one pad)", proc.ui.editBank >= 0);
        juce::Component::SafePointer<PadGrid> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&banks[i]), [safe, i](int r)
        {
            if (safe == nullptr) return;
            if (r == 1 && safe->onEditBank) safe->onEditBank(i);
            else if (r == 2) { safe->proc.ui.editBank = -1; safe->proc.listenersChanged(); }
        });
    }
}

void PadGrid::beginCutSweep(int pad)
{
    const auto& p = proc.project.state().pads[(size_t) pad];
    if (!p.assigned) return;
    sweepGroup = p.s.chokeGroup == proc.ui.cutPaint ? 0 : proc.ui.cutPaint; // the first pad decides: add or remove
    proc.project.beginGesture("Cut group");
    applySweep(pad);
}

void PadGrid::applySweep(int pad)
{
    const auto& p = proc.project.state().pads[(size_t) pad];
    if (sweepGroup < 0 || !p.assigned || p.s.chokeGroup == sweepGroup) return;
    proc.project.edit("Cut group", [&](ProjectState& s) { s.pads[(size_t) pad].s.chokeGroup = sweepGroup; });
}

void PadGrid::cutSweepTo(juce::Point<int> pos)
{
    if (sweepGroup < 0) return;
    for (auto* b : pads) if (b->getBounds().contains(pos)) { applySweep(b->flatIndex()); break; }
}

void PadGrid::endCutSweep()
{
    if (sweepGroup < 0) return;
    sweepGroup = -1;
    proc.project.endGesture();
}

void PadGrid::selectForEdit(int pad)
{
    proc.ui.selectedPad = pad;
    refresh();
    if (onOpenInspector) onOpenInspector();
}

void PadGrid::showPadMenu(int pad, juce::Component* target)
{
    const auto& st = proc.project.state();
    const auto& p = st.pads[(size_t) pad];
    proc.ui.selectedPad = pad;
    refresh();
    juce::PopupMenu m, mode, cut, rep;
    const bool a = p.assigned;
    m.addItem(1, "Edit pad...", a);
    m.addItem(2, "Locate in source", a);
    mode.addItem(10, "Project default", true, p.s.playMode < 0);
    mode.addItem(11, "Gate", true, p.s.playMode == (int) PlayMode::Gate);
    mode.addItem(12, "One shot", true, p.s.playMode == (int) PlayMode::OneShot);
    mode.addItem(13, "Toggle", true, p.s.playMode == (int) PlayMode::Toggle);
    m.addSubMenu("Play mode", mode, a);
    rep.addItem(20, "Retrigger (restart)", true, p.s.repeat == RepeatMode::Retrigger);
    rep.addItem(21, "Layer (overlap)", true, p.s.repeat == RepeatMode::Layer);
    m.addSubMenu("Repeat hits", rep, a);
    cut.addItem(30, "Off", true, p.s.chokeGroup == 0);
    for (int i = 1; i <= kMaxChokeGroups; ++i) cut.addItem(30 + i, "Group " + juce::String(i), true, p.s.chokeGroup == i);
    m.addSubMenu("Cut group (choke)", cut, a);
    m.addItem(9, "Play to the end of the sample", a, proc.padPlaysToEnd(pad));
    m.addItem(3, "Loop", a, p.s.loop);
    m.addItem(4, "Reverse", a, p.s.reverse);
    m.addSeparator();
    m.addItem(5, "Copy", a);
    m.addItem(6, "Paste");
    m.addItem(7, "Rename...", a);
    m.addItem(8, "Clear pad", a);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [this, pad](int r)
    {
        auto set = [this, pad](const juce::String& l, const std::function<void(PadSettings&)>& fn)
        {
            proc.project.edit(l + " " + padLabel(pad), [&](ProjectState& s) { fn(s.pads[(size_t) pad].s); });
        };
        if (r == 1) selectForEdit(pad);
        else if (r == 2) { if (onLocate) onLocate(pad); }
        else if (r >= 10 && r <= 13) set("Play mode", [r](PadSettings& s) { s.playMode = r - 11; });
        else if (r == 20 || r == 21) set("Repeat", [r](PadSettings& s) { s.repeat = r == 20 ? RepeatMode::Retrigger : RepeatMode::Layer; });
        else if (r >= 30 && r <= 30 + kMaxChokeGroups) set("Cut group", [r](PadSettings& s) { s.chokeGroup = r - 30; });
        else if (r == 3) set("Loop", [](PadSettings& s) { s.loop = !s.loop; });
        else if (r == 4) set("Reverse", [](PadSettings& s) { s.reverse = !s.reverse; });
        else if (r == 9) proc.togglePadToEnd(pad);
        else if (r == 5) proc.copyPad(pad);
        else if (r == 6) proc.pastePad(pad);
        else if (r == 8) proc.clearPad(pad);
        else if (r == 7)
        {
            auto* w = new juce::AlertWindow("Rename " + padLabel(pad), {}, juce::MessageBoxIconType::NoIcon, this);
            w->addTextEditor("n", proc.project.state().pads[(size_t) pad].name);
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, pad](int res)
            {
                if (res == 1)
                {
                    const auto name = w->getTextEditorContents("n").substring(0, 24);
                    proc.project.edit("Rename", [&](ProjectState& s) { s.pads[(size_t) pad].name = name; });
                }
            }), true);
        }
    });
}

void PadGrid::refresh()
{
    for (int i = 0; i < kNumBanks; ++i)
    {
        banks[i].setToggleState(proc.ui.bank == i, juce::dontSendNotification);
        banks[i].setButtonText((proc.ui.editBank == i ? "EDIT " : "BANK ") + juce::String::charToString(juce::juce_wchar('A' + i)));
    }
    performBtn.setToggleState(proc.ui.padMode == UiState::PadMode::Perform, juce::dontSendNotification);
    editBtn.setToggleState(proc.ui.padMode == UiState::PadMode::Edit, juce::dontSendNotification);
    updateStrip();
    for (auto* p : pads) { p->updateKnobs(); p->repaint(); }
    repaint();
}

void PadGrid::updateStrip()
{
    const auto& st = proc.project.state();
    const int pad = proc.editRefPad();
    const auto& p = st.pads[(size_t) pad];
    const bool bank = proc.ui.editBank >= 0;
    if (bank) padName.setText(proc.editScopeName() + ": ALL " + juce::String((int) proc.editTargets().size()) + " PADS", juce::dontSendNotification);
    else padName.setText(padLabel(pad) + (p.assigned ? "  " + p.name : juce::String("  (empty)")), juce::dontSendNotification);
    padName.setColour(juce::Label::textColourId, bank ? theme::accent : p.assigned ? st.sources[(size_t) p.src].colour : theme::textDim);
    const auto mode = effectivePlayMode(p, st.settings);
    gateBtn.setToggleState(p.assigned && mode == PlayMode::Gate, juce::dontSendNotification);
    oneShotBtn.setToggleState(p.assigned && mode == PlayMode::OneShot, juce::dontSendNotification);
    toggleBtn.setToggleState(p.assigned && mode == PlayMode::Toggle, juce::dontSendNotification);
    retrigBtn.setToggleState(p.assigned && p.s.repeat == RepeatMode::Retrigger, juce::dontSendNotification);
    layerBtn.setToggleState(p.assigned && p.s.repeat == RepeatMode::Layer, juce::dontSendNotification);
    const bool picking = proc.ui.padMode == UiState::PadMode::Cut;
    for (int i = 0; i < 5; ++i) cutBtns[i].setToggleState(picking ? proc.ui.cutPaint == i : p.assigned && p.s.chokeGroup == i, juce::dontSendNotification);
    cutPickBtn.setToggleState(picking, juce::dontSendNotification);
    const auto bankPads = proc.cutScopePads(HPSamplerProcessor::CutScope::Bank);
    const int g0 = bankPads.empty() ? 0 : st.pads[(size_t) bankPads.front()].s.chokeGroup;
    cutBankBtn.setToggleState(g0 > 0 && std::all_of(bankPads.begin(), bankPads.end(), [&](int i) { return st.pads[(size_t) i].s.chokeGroup == g0; }),
                              juce::dontSendNotification);
    cutBankBtn.setEnabled(!bankPads.empty());
    loopBtn.setToggleState(p.assigned && p.s.loop, juce::dontSendNotification);
    revBtn.setToggleState(p.assigned && p.s.reverse, juce::dontSendNotification);
    for (auto* b : { &gateBtn, &oneShotBtn, &toggleBtn, &retrigBtn, &layerBtn, &loopBtn, &revBtn, &inspectBtn }) b->setEnabled(p.assigned);
    for (auto& b : cutBtns) b.setEnabled(p.assigned || picking);
    juce::String h;
    if (picking) h = proc.ui.cutPaint > 0 ? "PICK: click or sweep over pads - they join cut group " + juce::String(proc.ui.cutPaint) + " (again: leave it). PICK or Esc = done"
                                          : juce::String("PICK: click or sweep over pads to take them out of their cut group. PICK or Esc = done");
    else if (bank) h = "Every change goes to all pads of " + proc.editScopeName() + ". The knobs on a pad change only that pad.";
    else if (proc.ui.padMode == UiState::PadMode::Edit) h = "EDIT SELECT: click a pad to edit it (no sound)";
    else if (proc.ui.padMode == UiState::PadMode::Assign) h = "ASSIGN: click the pad that gets the selection";
    else if (p.assigned && p.s.chokeGroup > 0)
    {
        juce::StringArray others;
        for (int i = 0; i < kNumPads; ++i)
            if (i != pad && st.pads[(size_t) i].assigned && st.pads[(size_t) i].s.chokeGroup == p.s.chokeGroup) others.add(padLabel(i));
        h = others.isEmpty() ? "Cut group " + juce::String(p.s.chokeGroup) + ": put another pad in this group to make them cut each other"
                             : "Cut group " + juce::String(p.s.chokeGroup) + ": stops " + others.joinIntoString(", ") + " (and they stop it)";
    }
    else h = "Hit = play (MIDI C1 = A01). Right-click = options. Drag a pad onto MAIN to place it.";
    hint.setText(h, juce::dontSendNotification);
}

void PadGrid::timerCallback()
{
    if (const bool m = proc.engine.previewFrame.load() >= 0.0; m != marking) { marking = m; for (auto* p : pads) p->repaint(); }
    for (auto* p : pads)
    {
        const int pad = p->flatIndex();
        const uint32_t hits = proc.engine.padHits[(size_t) pad].load();
        bool dirty = false;
        if (hits != p->lastHits) { p->lastHits = hits; p->flash = 1.0f; dirty = true; }
        if (p->flash > 0.0f) { p->flash = std::max(0.0f, p->flash - 0.12f); dirty = true; }
        // only when it starts / stops sounding: a pad that plays on and on is not redrawn 30 times a second
        if (const bool snd = proc.engine.padVoices[(size_t) pad].load() > 0; snd != p->wasSounding) { p->wasSounding = snd; dirty = true; }
        if (const bool r = proc.isPadRendering(pad); r || p->wasRendering) { p->wasRendering = r; dirty = true; }
        if (dirty) p->repaint();
    }
}

void PadGrid::paint(juce::Graphics& g)
{
    g.fillAll(theme::bodyLight);
    g.setColour(juce::Colours::black.withAlpha(0.4f));
    g.drawHorizontalLine(0, 0, (float) getWidth());
}

void PadGrid::resized()
{
    auto r = getLocalBounds().reduced(6, 4);
    // selected pad strip
    auto strip = r.removeFromTop(30);
    padName.setBounds(strip.removeFromLeft(getWidth() < 1200 ? 140 : 190));
    for (auto* b : { &gateBtn, &oneShotBtn, &toggleBtn }) b->setBounds(strip.removeFromLeft(64).reduced(2));
    strip.removeFromLeft(8);
    for (auto* b : { &retrigBtn, &layerBtn }) b->setBounds(strip.removeFromLeft(64).reduced(2));
    strip.removeFromLeft(4);
    cutLabel.setBounds(strip.removeFromLeft(74));
    for (auto& b : cutBtns) b.setBounds(strip.removeFromLeft(b.getButtonText() == "OFF" ? 40 : 28).reduced(2));
    cutBankBtn.setBounds(strip.removeFromLeft(48).reduced(2));
    cutPickBtn.setBounds(strip.removeFromLeft(48).reduced(2));
    strip.removeFromLeft(8);
    loopBtn.setBounds(strip.removeFromLeft(52).reduced(2));
    revBtn.setBounds(strip.removeFromLeft(46).reduced(2));
    inspectBtn.setBounds(strip.removeFromRight(100).reduced(2));
    hint.setBounds(strip.reduced(6, 0));

    auto left = r.removeFromLeft(96);
    const int bh = juce::jmin(28, left.getHeight() / 6);
    for (auto& b : banks) b.setBounds(left.removeFromTop(bh).reduced(2));
    left.removeFromTop(6);
    performBtn.setBounds(left.removeFromTop(bh).reduced(2));
    editBtn.setBounds(left.removeFromTop(bh).reduced(2));

    // one row of 16 when wide enough, else 2 x 8
    const bool oneRow = r.getWidth() / 16 >= 80 && r.getHeight() < 140;
    const int cols = oneRow ? 16 : 8, rows = oneRow ? 1 : 2;
    const int w = r.getWidth() / cols, h = r.getHeight() / rows;
    for (int i = 0; i < kPadsPerBank; ++i)
        pads[i]->setBounds(r.getX() + (i % cols) * w, r.getY() + (i / cols) * h, w, h);
}
} // namespace hps
