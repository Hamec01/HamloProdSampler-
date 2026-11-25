#include "ui/ControlStrip.h"

namespace hps
{
void DisplayChip::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(3.0f);
    theme::drawChip(g, r, theme::accent, isMouseOverOrDragging());
    auto in = r.reduced(8.0f, 4.0f).toNearestInt();
    g.setColour(theme::textDim);
    g.setFont(theme::ui(10.0f, true));
    g.drawText(caption, in.removeFromTop(12), juce::Justification::topLeft);
    g.setColour(valueColour);
    g.setFont(theme::mono(19.0f, true));
    g.drawFittedText(value, in.removeFromTop(22), juce::Justification::centredLeft, 1, 0.7f);
    g.setColour(subColour);
    g.setFont(theme::ui(10.5f));
    g.drawFittedText(sub, in, juce::Justification::centredLeft, 1, 0.7f);
}

void DisplayChip::mouseDown(const juce::MouseEvent& e) { lastY = e.y; dragged = false; }

void DisplayChip::mouseDrag(const juce::MouseEvent& e)
{
    if (!onWheel) return;
    const int dy = lastY - e.y;
    if (std::abs(dy) >= 3)
    {
        dragged = true;
        onWheel((float) (dy / 3), e.mods.isShiftDown());
        lastY = e.y;
    }
}

void DisplayChip::mouseUp(const juce::MouseEvent& e)
{
    if (!dragged && !e.mouseWasDraggedSinceMouseDown() && onClick) onClick();
}

void DisplayChip::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (onWheel) onWheel(w.deltaY > 0 ? 1.0f : -1.0f, e.mods.isShiftDown());
}

// =============================================================================================

ControlStrip::ControlStrip(HPSamplerProcessor& p) : proc(p)
{
    for (auto* c : { &now, &bpm, &key, &grid, &sync }) addAndMakeVisible(*c);
    bpm.caption = "PROJECT BPM";
    key.caption = "PROJECT KEY";
    grid.caption = "GRID";
    sync.caption = "SYNC";
    now.caption = "NOW PLAYING";
    now.setTooltip("Who plays right now: the DAW, MAIN, a source audition or a pad. Click: what sounds while the DAW plays, and when DAW notes are written into MAIN.");
    now.onClick = [this] { nowMenu(); };
    bpm.setTooltip("Project tempo. Drag up/down or wheel (Shift = 0.1). Click for options.");
    key.setTooltip("Target key: tonal pads are transposed to it (SYNC STRETCH). Click to choose.");
    grid.setTooltip("Musical grid for SRC snapping and MAIN editing. Click to choose.");
    sync.setTooltip("How pads follow the project: STRETCH = tempo and key independent, REPITCH = vintage speed+pitch, OFF = as recorded.");
    bpm.onClick = [this] { bpmMenu(); };
    key.onClick = [this] { keyMenu(); };
    grid.onClick = [this] { gridMenu(); };
    sync.onClick = [this] { syncMenu(); };
    bpm.onWheel = [this](float d, bool fine)
    {
        const double b = juce::jlimit(40.0, 300.0, proc.project.state().settings.bpm + d * (fine ? 0.1 : 1.0));
        proc.project.beginGesture("Project BPM");
        proc.project.edit("Project BPM", [b](ProjectState& s) { s.settings.bpm = std::round(b * 10.0) / 10.0; s.settings.followHostTempo = false; });
        proc.project.endGesture();
    };
    key.onWheel = [this](float d, bool)
    {
        auto k = proc.project.state().settings.key;
        if (!k.known()) k = { 9, 0 };
        k.root = ((k.root + (d > 0 ? 1 : -1)) % 12 + 12) % 12;
        proc.project.edit("Project key", [k](ProjectState& s) { s.settings.key = k; });
    };

    for (auto* b : { &playBtn, &stopBtn, &recBtn, &metroBtn, &cutOff, &cutSrc, &cutAll, &gateDef, &oneDef })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    recBtn.getProperties().set("led", (juce::int64) theme::rec.getARGB());
    playBtn.getProperties().set("led", (juce::int64) theme::ok.getARGB());
    playBtn.onClick = [this] { proc.togglePlay(); };
    stopBtn.setTooltip("Stops everything in the plug-in (the DAW itself is stopped in the DAW)");
    stopBtn.onClick = [this] { proc.stop(); proc.engine.panic(); };
    recBtn.setTooltip("Record pad / MIDI hits into the pattern (overdub). Starts the transport if needed.");
    recBtn.onClick = [this] { proc.setRecordArmed(!proc.isRecordArmed()); refresh(); };
    countBtn.setTooltip("Count-in: four clicks (1-2-3-4) before REC starts MAIN. While the DAW plays, its own count-in is used.");
    countBtn.setWantsKeyboardFocus(false);
    addAndMakeVisible(countBtn);
    countBtn.onClick = [this] { proc.project.mutate([](ProjectState& s) { s.settings.countIn = !s.settings.countIn; }); };
    captureBtn.setTooltip("Writes what you just played (pads / DAW notes while the DAW or MAIN ran) into MAIN, even if REC was off");
    captureBtn.getProperties().set("led", (juce::int64) theme::warn.getARGB());
    captureBtn.setWantsKeyboardFocus(false);
    addAndMakeVisible(captureBtn);
    captureBtn.onClick = [this] { proc.captureTake(); };
    metroBtn.onClick = [this] { proc.project.mutate([](ProjectState& s) { s.settings.metronome = !s.settings.metronome; }); };

    cutLabel.setText("CUT", juce::dontSendNotification);
    cutLabel.setTooltip("Cross-pad cut: OFF = pads overlap freely; PER SRC = a new hit stops the other pads of the same source; ALL = one sound at a time");
    modeLabel.setText("DEFAULT", juce::dontSendNotification);
    modeLabel.setTooltip("Play mode for pads that do not have their own");
    qLabel.setText("REC Q", juce::dontSendNotification);
    swingLabel.setText("SWING", juce::dontSendNotification);
    for (auto* l : { &cutLabel, &modeLabel, &qLabel, &swingLabel })
    {
        l->setFont(theme::ui(10.0f, true));
        l->setColour(juce::Label::textColourId, theme::textDim);
        l->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*l);
    }
    cutOff.setTooltip("Pads never stop each other (use CUT GROUP on single pads)");
    cutSrc.setTooltip("A new hit stops the other pads of the same SRC");
    cutAll.setTooltip("A new hit stops everything (mono)");
    for (auto* b : { &cutOff, &cutSrc, &cutAll }) b->getProperties().set("led", (juce::int64) theme::rec.getARGB());
    auto setMono = [this](MonoMode m) { proc.project.edit("Cut mode", [m](ProjectState& s) { s.settings.mono = m; }); };
    cutOff.onClick = [setMono] { setMono(MonoMode::Off); };
    cutSrc.onClick = [setMono] { setMono(MonoMode::PerSource); };
    cutAll.onClick = [setMono] { setMono(MonoMode::All); };
    gateDef.onClick = [this] { proc.project.edit("Default mode", [](ProjectState& s) { s.settings.defaultMode = PlayMode::Gate; }); };
    oneDef.onClick = [this] { proc.project.edit("Default mode", [](ProjectState& s) { s.settings.defaultMode = PlayMode::OneShot; }); };

    recQ.addItem("OFF (groove)", 1);
    recQ.addItem("50 % 1/16", 2);
    recQ.addItem("100 % 1/16", 3);
    recQ.addItem("100 % 1/8", 4);
    recQ.addItem("100 % 1/16T", 5);
    recQ.setTooltip("Quantize for recorded hits. Non-destructive: the played timing is kept and can be restored.");
    recQ.onChange = [this]
    {
        const int id = recQ.getSelectedId();
        proc.project.edit("Rec quantize", [id](ProjectState& s)
        {
            s.settings.recQuantize = id == 1 ? 0.0f : id == 2 ? 0.5f : 1.0f;
            s.settings.recGrid = id == 4 ? GridDiv::Eighth : id == 5 ? GridDiv::SixteenthT : GridDiv::Sixteenth;
        });
    };
    addAndMakeVisible(recQ);
    swing.setSliderStyle(juce::Slider::LinearHorizontal);
    swing.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    swing.setRange(0.0, 1.0, 0.01);
    swing.setTooltip("Swing of the off-beat 1/16 in MAIN playback");
    swing.onDragStart = [this] { proc.project.beginGesture("Swing"); };
    swing.onDragEnd = [this] { proc.project.endGesture(); };
    swing.onValueChange = [this] { const float v = (float) swing.getValue(); proc.project.edit("Swing", [v](ProjectState& s) { s.settings.swing = v; }); };
    addAndMakeVisible(swing);
    master.setSliderStyle(juce::Slider::LinearHorizontal);
    master.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 16);
    master.setRange(-36.0, 6.0, 0.1);
    master.setTextValueSuffix(" dB");
    master.setTooltip("Master level");
    master.onDragStart = [this] { proc.project.beginGesture("Master"); };
    master.onDragEnd = [this] { proc.project.endGesture(); };
    master.onValueChange = [this] { const float v = (float) master.getValue(); proc.project.edit("Master", [v](ProjectState& s) { s.settings.masterDb = v; }); };
    addAndMakeVisible(master);
    startTimerHz(20);
    refresh();
}

ControlStrip::~ControlStrip() { stopTimer(); }

void ControlStrip::refresh()
{
    const auto& st = proc.project.state().settings;
    bpm.value = juce::String(st.bpm, 1);
    const double hb = proc.engine.hostBpm.load();
    if (st.followHostTempo && hb > 20.0) { bpm.sub = "following DAW"; bpm.subColour = theme::ok; }
    else if (hb > 20.0 && std::abs(hb - st.bpm) > 0.05) { bpm.sub = "DAW is " + juce::String(hb, 1) + "!"; bpm.subColour = theme::warn; }
    else if (st.anchorSrc >= 0) { bpm.sub = "anchor SRC " + juce::String(st.anchorSrc + 1); bpm.subColour = theme::textDim; }
    else { bpm.sub = "manual"; bpm.subColour = theme::textDim; }
    key.value = st.key.known() ? st.key.label() : "OFF";
    key.sub = st.key.known() ? (st.sync == SyncMode::Stretch ? "tonal pads follow" : "no auto (SYNC)") : "no transposition";
    grid.value = gridName(st.grid);
    grid.sub = st.snap ? "snap on" : "snap off";
    sync.value = st.sync == SyncMode::Stretch ? "STRETCH" : st.sync == SyncMode::Repitch ? "REPITCH" : "OFF";
    sync.sub = st.sync == SyncMode::Stretch ? "tempo + key independent" : st.sync == SyncMode::Repitch ? "speed = pitch (vintage)" : "as recorded";
    for (auto* c : { &bpm, &key, &grid, &sync }) c->repaint();

    cutOff.setToggleState(st.mono == MonoMode::Off, juce::dontSendNotification);
    cutSrc.setToggleState(st.mono == MonoMode::PerSource, juce::dontSendNotification);
    cutAll.setToggleState(st.mono == MonoMode::All, juce::dontSendNotification);
    gateDef.setToggleState(st.defaultMode == PlayMode::Gate, juce::dontSendNotification);
    oneDef.setToggleState(st.defaultMode == PlayMode::OneShot, juce::dontSendNotification);
    countBtn.setToggleState(st.countIn, juce::dontSendNotification);
    metroBtn.setToggleState(st.metronome, juce::dontSendNotification);
    recBtn.setToggleState(proc.isRecordArmed(), juce::dontSendNotification);
    int q = 1;
    if (st.recQuantize > 0.0f) q = st.recGrid == GridDiv::Eighth ? 4 : st.recGrid == GridDiv::SixteenthT ? 5 : (st.recQuantize < 0.99f ? 2 : 3);
    recQ.setSelectedId(q, juce::dontSendNotification);
    if (!swing.isMouseButtonDown()) swing.setValue(st.swing, juce::dontSendNotification);
    if (!master.isMouseButtonDown()) master.setValue(st.masterDb, juce::dontSendNotification);
}

void ControlStrip::timerCallback()
{
    const auto n = proc.nowPlaying();
    using Who = HPSamplerProcessor::NowPlaying::Who;
    const auto col = n.who == Who::Daw ? (proc.engine.hostRecording.load() ? theme::rec : theme::ok)
                   : n.who == Who::Main ? theme::accent : n.who == Who::Nothing ? theme::textDim : theme::wave;
    if (n.title != now.value || n.detail != now.sub || now.valueColour != col)
    {
        now.value = n.title; now.sub = n.detail; now.valueColour = col; now.subColour = col.withAlpha(0.85f);
        now.repaint();
    }
    playBtn.setToggleState(n.who == Who::Main, juce::dontSendNotification);
    playBtn.setEnabled(n.who != Who::Daw); // the DAW leads
    recBtn.setToggleState(proc.isRecordArmed() || (proc.engine.hostRecording.load()
                          && proc.project.state().settings.hostCapture != HostCapture::Off), juce::dontSendNotification);
    if (++captureTick >= 6)
    {
        captureTick = 0;
        const int cap = proc.captureCount();
        const auto t = cap > 0 ? "CAPTURE " + juce::String(cap) : juce::String("CAPTURE");
        if (captureBtn.getButtonText() != t) captureBtn.setButtonText(t);
        captureBtn.setToggleState(cap > 0, juce::dontSendNotification);
        captureBtn.setEnabled(cap > 0);
    }
    const double hb = proc.engine.hostBpm.load();
    if (std::abs(hb - lastHost) > 0.01) { lastHost = hb; refresh(); }
}

void ControlStrip::nowMenu()
{
    const auto& st = proc.project.state().settings;
    juce::PopupMenu m;
    m.addSectionHeader("While the DAW plays, you hear");
    m.addItem(1, "MAIN + DAW notes (a note that is in both sounds once)", true, st.hostPlays == HostPlays::Both);
    m.addItem(2, "Only MAIN (DAW notes are ignored unless recording)", true, st.hostPlays == HostPlays::Main);
    m.addItem(3, "Only DAW notes (MAIN is silent)", true, st.hostPlays == HostPlays::DawNotes);
    m.addSectionHeader("Write DAW notes and pads into MAIN");
    m.addItem(11, "When the DAW records", true, st.hostCapture == HostCapture::WhenRecording);
    m.addItem(12, "Always while the DAW plays", true, st.hostCapture == HostCapture::Always);
    m.addItem(10, "Only with REC / CAPTURE", true, st.hostCapture == HostCapture::Off);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&now), [this](int r)
    {
        if (r >= 10) proc.project.edit("DAW capture", [r](ProjectState& s) { s.settings.hostCapture = (HostCapture) (r - 10); });
        else if (r >= 1) proc.project.edit("DAW role", [r](ProjectState& s) { s.settings.hostPlays = (HostPlays) (r - 1); });
    });
}

void ControlStrip::bpmMenu()
{
    const auto& st = proc.project.state();
    juce::PopupMenu m;
    m.addItem(1, "Type BPM...");
    const double hb = proc.engine.hostBpm.load();
    m.addItem(2, hb > 20.0 ? "Follow DAW tempo (" + juce::String(hb, 1) + ")" : "Follow DAW tempo (no host tempo)", true, st.settings.followHostTempo);
    m.addItem(3, "x2");
    m.addItem(4, "/2");
    m.addSeparator();
    for (int i = 0; i < kNumSources; ++i)
    {
        const auto d = proc.project.sourceDataForSlot(i);
        const double b = d ? effectiveSourceBpm(st.sources[(size_t) i], d.get()) : 0.0;
        m.addItem(10 + i, "From SRC " + juce::String(i + 1) + (b > 20 ? " (" + juce::String(b, 1) + ")" : juce::String(" (no BPM)")), b > 20.0);
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&bpm), [this](int r)
    {
        auto set = [this](double b) { proc.project.edit("Project BPM", [b](ProjectState& s) { s.settings.bpm = juce::jlimit(40.0, 300.0, std::round(b * 10.0) / 10.0); s.settings.followHostTempo = false; }); };
        if (r == 1)
        {
            auto* w = new juce::AlertWindow("Project BPM", {}, juce::MessageBoxIconType::NoIcon, this);
            w->addTextEditor("v", juce::String(proc.project.state().settings.bpm, 1));
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([w, set](int res) { if (res == 1) { const double v = w->getTextEditorContents("v").getDoubleValue(); if (v > 20) set(v); } }), true);
        }
        else if (r == 2) proc.project.edit("Follow DAW", [](ProjectState& s) { s.settings.followHostTempo = !s.settings.followHostTempo; });
        else if (r == 3) set(proc.project.state().settings.bpm * 2.0);
        else if (r == 4) set(proc.project.state().settings.bpm / 2.0);
        else if (r >= 10) proc.useSourceAsAnchor(r - 10);
    });
}

void ControlStrip::keyMenu()
{
    juce::PopupMenu m, minor, major;
    for (int i = 0; i < 12; ++i)
    {
        minor.addItem(100 + i, MusicalKey { i, 0 }.label());
        major.addItem(200 + i, MusicalKey { i, 1 }.label());
    }
    m.addSubMenu("Minor", minor);
    m.addSubMenu("Major", major);
    m.addItem(1, "OFF (never transpose)");
    m.addSeparator();
    const auto& st = proc.project.state();
    for (int i = 0; i < kNumSources; ++i)
    {
        const auto d = proc.project.sourceDataForSlot(i);
        const auto k = d ? effectiveSourceKey(st.sources[(size_t) i], d.get()) : MusicalKey {};
        m.addItem(10 + i, "From SRC " + juce::String(i + 1) + " (" + (k.known() ? k.label() : juce::String("no key")) + ")", k.known());
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&key), [this](int r)
    {
        if (r == 0) return;
        MusicalKey k;
        if (r >= 100) k = { r % 100, r >= 200 ? 1 : 0 };
        else if (r >= 10)
        {
            const auto d = proc.project.sourceDataForSlot(r - 10);
            k = d ? effectiveSourceKey(proc.project.state().sources[(size_t) (r - 10)], d.get()) : MusicalKey {};
        }
        proc.project.edit("Project key", [k](ProjectState& s) { s.settings.key = k; });
    });
}

void ControlStrip::gridMenu()
{
    const auto& st = proc.project.state().settings;
    juce::PopupMenu m;
    for (int i = 0; i <= (int) GridDiv::Off; ++i) m.addItem(1 + i, gridName((GridDiv) i), true, (int) st.grid == i);
    m.addSeparator();
    m.addItem(100, "Snap", true, st.snap);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&grid), [this](int r)
    {
        if (r == 100) proc.project.edit("Snap", [](ProjectState& s) { s.settings.snap = !s.settings.snap; });
        else if (r >= 1) proc.project.edit("Grid", [r](ProjectState& s) { s.settings.grid = (GridDiv) (r - 1); });
    });
}

void ControlStrip::syncMenu()
{
    const auto cur = proc.project.state().settings.sync;
    juce::PopupMenu m;
    m.addItem(3, "STRETCH - follow project tempo and key, pitch independent", true, cur == SyncMode::Stretch);
    m.addItem(2, "REPITCH - speed changes pitch (MPC / vintage)", true, cur == SyncMode::Repitch);
    m.addItem(1, "OFF - play as recorded", true, cur == SyncMode::Off);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sync), [this](int r)
    {
        if (r > 0) proc.project.edit("Sync mode", [r](ProjectState& s) { s.settings.sync = (SyncMode) (r - 1); });
    });
}

void ControlStrip::paint(juce::Graphics& g)
{
    g.fillAll(theme::body);
}

void ControlStrip::resized()
{
    auto r = getLocalBounds().reduced(6, 3);
    const int chipW = juce::jlimit(84, 140, (r.getWidth() - 686 - 84) / 5);
    for (auto* c : { &now, &bpm, &key, &grid, &sync }) c->setBounds(r.removeFromLeft(chipW));
    r.removeFromLeft(6);
    auto col = [&](int w) { return r.removeFromLeft(w); };
    auto tr = col(250);
    auto trTop = tr.removeFromTop(tr.getHeight() / 2 + 4);
    playBtn.setBounds(trTop.removeFromLeft(62).reduced(2));
    stopBtn.setBounds(trTop.removeFromLeft(58).reduced(2));
    recBtn.setBounds(trTop.removeFromLeft(54).reduced(2));
    metroBtn.setBounds(trTop.removeFromLeft(66).reduced(2));
    captureBtn.setBounds(tr.removeFromLeft(92).reduced(2, 1));
    countBtn.setBounds(tr.removeFromLeft(58).reduced(2, 1));
    swingLabel.setBounds(tr.removeFromLeft(42));
    swing.setBounds(tr.reduced(2));

    r.removeFromLeft(4);
    auto cut = col(186);
    cutLabel.setBounds(cut.removeFromTop(14));
    auto cb = cut.reduced(0, 2);
    cutOff.setBounds(cb.removeFromLeft(50).reduced(2));
    cutSrc.setBounds(cb.removeFromLeft(76).reduced(2));
    cutAll.setBounds(cb.reduced(2));

    auto md = col(124);
    modeLabel.setBounds(md.removeFromTop(14));
    auto mb = md.reduced(0, 2);
    gateDef.setBounds(mb.removeFromLeft(58).reduced(2));
    oneDef.setBounds(mb.reduced(2));

    auto q = col(120);
    qLabel.setBounds(q.removeFromTop(14));
    recQ.setBounds(q.reduced(2, 6));
    master.setBounds(r.reduced(2, 8));
}
} // namespace hps
