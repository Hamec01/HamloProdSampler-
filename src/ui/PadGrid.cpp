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
        g.drawText("S" + juce::String(p.src + 1), inner.withHeight(15).translated(0, -15), juce::Justification::topRight);
        g.setColour(theme::text.withAlpha(0.85f));
        g.setFont(theme::ui(11.0f));
        g.drawFittedText(p.name, inner.removeFromTop(knobsShown() ? 14 : 28), juce::Justification::topLeft, knobsShown() ? 1 : 2, 0.8f);

        // badges: mode, repeat, cut group, loop, reverse
        const auto mode = effectivePlayMode(p, st.settings);
        juce::StringArray badges;
        badges.add(mode == PlayMode::Gate ? "GATE" : mode == PlayMode::OneShot ? "1SHOT" : "TGL");
        if (p.s.repeat == RepeatMode::Layer) badges.add("LYR");
        if (p.s.chokeGroup > 0) badges.add("CUT" + juce::String(p.s.chokeGroup));
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
            const bool cut = b.startsWith("CUT");
            g.setColour(cut ? theme::rec.withAlpha(0.85f) : juce::Colours::black.withAlpha(0.45f));
            g.fillRoundedRectangle(cell.toFloat(), 3.0f);
            g.setColour(cut ? juce::Colours::white : theme::text.withAlpha(0.8f));
            g.drawText(b, cell, juce::Justification::centred);
            br.removeFromLeft(3);
        }
    }
    else
    {
        g.setColour(theme::textDim.withAlpha(0.6f));
        g.setFont(theme::ui(10.5f));
        g.drawFittedText("empty\ndrop a selection", inner, juce::Justification::centred, 2);
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
    if (playing)
    {
        grid.proc.releasePad(flatIndex(), e.source.getIndex());
        playing = false;
    }
}

void PadButton::mouseDrag(const juce::MouseEvent& e)
{
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
        banks[i].onClick = [this, i] { proc.ui.bank = i; refresh(); };
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
        cutBtns[i].setTooltip(i == 0 ? "This pad cuts nothing" : "Pads in cut group " + juce::String(i) + " stop each other: a new hit silences the others of the group");
        cutBtns[i].getProperties().set("led", (juce::int64) theme::rec.getARGB());
        cutBtns[i].onClick = [this, i] { setPadField("Cut group", [i](PadSettings& s) { s.chokeGroup = i; }); };
        addAndMakeVisible(cutBtns[i]);
    }
    loopBtn.onClick = [this] { setPadField("Loop", [](PadSettings& s) { s.loop = !s.loop; }); };
    revBtn.onClick = [this] { setPadField("Reverse", [](PadSettings& s) { s.reverse = !s.reverse; }); };
    inspectBtn.onClick = [this] { if (onOpenInspector) onOpenInspector(); };
    startTimerHz(30);
    refresh();
}

PadGrid::~PadGrid() { stopTimer(); }

void PadGrid::setPadField(const juce::String& label, const std::function<void(PadSettings&)>& fn)
{
    const int pad = proc.ui.selectedPad;
    if (!proc.project.state().pads[(size_t) pad].assigned) return;
    proc.project.edit(label + " " + padLabel(pad), [&](ProjectState& s) { fn(s.pads[(size_t) pad].s); });
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
    for (int i = 0; i < kNumBanks; ++i) banks[i].setToggleState(proc.ui.bank == i, juce::dontSendNotification);
    performBtn.setToggleState(proc.ui.padMode == UiState::PadMode::Perform, juce::dontSendNotification);
    editBtn.setToggleState(proc.ui.padMode == UiState::PadMode::Edit, juce::dontSendNotification);
    updateStrip();
    for (auto* p : pads) { p->updateKnobs(); p->repaint(); }
    repaint();
}

void PadGrid::updateStrip()
{
    const auto& st = proc.project.state();
    const int pad = proc.ui.selectedPad;
    const auto& p = st.pads[(size_t) pad];
    padName.setText(padLabel(pad) + (p.assigned ? "  " + p.name : juce::String("  (empty)")), juce::dontSendNotification);
    padName.setColour(juce::Label::textColourId, p.assigned ? st.sources[(size_t) p.src].colour : theme::textDim);
    const auto mode = effectivePlayMode(p, st.settings);
    gateBtn.setToggleState(p.assigned && mode == PlayMode::Gate, juce::dontSendNotification);
    oneShotBtn.setToggleState(p.assigned && mode == PlayMode::OneShot, juce::dontSendNotification);
    toggleBtn.setToggleState(p.assigned && mode == PlayMode::Toggle, juce::dontSendNotification);
    retrigBtn.setToggleState(p.assigned && p.s.repeat == RepeatMode::Retrigger, juce::dontSendNotification);
    layerBtn.setToggleState(p.assigned && p.s.repeat == RepeatMode::Layer, juce::dontSendNotification);
    for (int i = 0; i < 5; ++i) cutBtns[i].setToggleState(p.assigned && p.s.chokeGroup == i, juce::dontSendNotification);
    loopBtn.setToggleState(p.assigned && p.s.loop, juce::dontSendNotification);
    revBtn.setToggleState(p.assigned && p.s.reverse, juce::dontSendNotification);
    for (auto* b : { &gateBtn, &oneShotBtn, &toggleBtn, &retrigBtn, &layerBtn, &loopBtn, &revBtn, &inspectBtn }) b->setEnabled(p.assigned);
    for (auto& b : cutBtns) b.setEnabled(p.assigned);
    juce::String h;
    if (proc.ui.padMode == UiState::PadMode::Edit) h = "EDIT SELECT: click a pad to edit it (no sound)";
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
    for (auto* p : pads)
    {
        const int pad = p->flatIndex();
        const uint32_t hits = proc.engine.padHits[(size_t) pad].load();
        bool dirty = false;
        if (hits != p->lastHits) { p->lastHits = hits; p->flash = 1.0f; dirty = true; }
        if (p->flash > 0.0f) { p->flash = std::max(0.0f, p->flash - 0.12f); dirty = true; }
        if (proc.engine.padVoices[(size_t) pad].load() > 0) dirty = true;
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
    padName.setBounds(strip.removeFromLeft(190));
    for (auto* b : { &gateBtn, &oneShotBtn, &toggleBtn }) b->setBounds(strip.removeFromLeft(64).reduced(2));
    strip.removeFromLeft(8);
    for (auto* b : { &retrigBtn, &layerBtn }) b->setBounds(strip.removeFromLeft(64).reduced(2));
    strip.removeFromLeft(4);
    cutLabel.setBounds(strip.removeFromLeft(74));
    for (auto& b : cutBtns) b.setBounds(strip.removeFromLeft(b.getButtonText() == "OFF" ? 40 : 28).reduced(2));
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
