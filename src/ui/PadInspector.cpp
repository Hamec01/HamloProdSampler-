#include "ui/PadInspector.h"

namespace hps
{
PadInspector::PadInspector(HPSamplerProcessor& p) : proc(p)
{
    addAndMakeVisible(header);
    header.setFont(theme::mono(15.0f, true));
    addAndMakeVisible(closeBtn);
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(viewport);
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);

    auto addRow = [this](const juce::String& section, const juce::String& name, juce::Component& c, int h = 26)
    {
        Row r;
        r.section = section;
        if (name.isNotEmpty())
        {
            r.label = std::make_unique<juce::Label>(name, name);
            r.label->setFont(theme::ui(11.5f));
            r.label->setColour(juce::Label::textColourId, theme::textDim);
            content.addAndMakeVisible(*r.label);
        }
        r.control = &c;
        r.height = h;
        content.addAndMakeVisible(c);
        rows.push_back(std::move(r));
    };

    // SLICE
    sliceInfo.setFont(theme::mono(11.5f));
    addRow("SLICE", "", sliceInfo, 34);
    for (auto* b : { &startM, &startP, &endM, &endP, &locateBtn, &playBtn }) b->setWantsKeyboardFocus(false);
    startM.setTooltip("Move the start 1 ms earlier (Shift 10 ms, Alt 1 frame)");
    auto nudge = [this](bool end, int dir)
    {
        const int pad = proc.ui.selectedPad;
        const auto& pd = proc.project.state().pads[(size_t) pad];
        const auto d = proc.project.sourceDataForSlot(pd.src);
        if (!pd.assigned || d == nullptr) return;
        const auto m = juce::ModifierKeys::currentModifiers;
        const int64_t f = m.isAltDown() ? dir : (int64_t) std::llround(dir * (m.isShiftDown() ? 0.010 : 0.001) * d->sampleRate);
        proc.project.edit("Trim " + padLabel(pad), [&](ProjectState& s)
        {
            auto& x = s.pads[(size_t) pad];
            if (end) x.end = juce::jlimit<int64_t>(x.start + 16, d->numFrames, x.end + f);
            else x.start = juce::jlimit<int64_t>(0, x.end - 16, x.start + f);
        });
    };
    startM.onClick = [nudge] { nudge(false, -1); };
    startP.onClick = [nudge] { nudge(false, 1); };
    endM.onClick = [nudge] { nudge(true, -1); };
    endP.onClick = [nudge] { nudge(true, 1); };
    struct Pair : juce::Component
    {
        juce::Component *a, *b, *c, *d;
        Pair(juce::Component* x, juce::Component* y, juce::Component* z, juce::Component* w) : a(x), b(y), c(z), d(w)
        {
            for (auto* q : { a, b, c, d }) if (q) addAndMakeVisible(q);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            const int n = d ? 4 : (c ? 3 : 2);
            const int w = r.getWidth() / n;
            for (auto* q : { a, b, c, d }) if (q) q->setBounds(r.removeFromLeft(w).reduced(2, 1));
        }
    };
    auto* trimRow = extras.add(new Pair(&startM, &startP, &endM, &endP));
    auto* locRow = extras.add(new Pair(&locateBtn, &playBtn, nullptr, nullptr));
    content.addAndMakeVisible(trimRow);
    content.addAndMakeVisible(locRow);
    rows.push_back({ "SLICE", nullptr, trimRow, 28 });
    rows.push_back({ "SLICE", nullptr, locRow, 28 });
    locateBtn.onClick = [this] { if (onLocate) onLocate(proc.ui.selectedPad); };
    playBtn.onClick = [this]
    {
        const int pad = proc.ui.selectedPad;
        proc.triggerPad(pad, 0.9f, Engine::kAuditionPointer | 7);
        juce::Component::SafePointer<PadInspector> safe(this);
        juce::Timer::callAfterDelay(400, [safe, pad] { if (safe) safe->proc.releasePad(pad, Engine::kAuditionPointer | 7); });
    };
    // PITCH & TIME (speed first: the most used control)
    addSlider("PITCH & TIME", "Speed", 0.25, 4.0, 0.01, " x", [](const PadSettings& s) { return s.speed; }, [](PadSettings& s, float v) { s.speed = v; }, 1.0);
    auto setSpeed = [this](float v) { edit("Speed", [v](PadSettings& s) { s.speed = juce::jlimit(0.125f, 8.0f, v); }); };
    for (auto* b : { &half, &normal, &dbl }) b->setWantsKeyboardFocus(false);
    half.setTooltip("Half speed (STRETCH / OFF keep the pitch, REPITCH goes an octave down)");
    dbl.setTooltip("Double speed");
    half.onClick = [this, setSpeed] { setSpeed(proc.project.state().pads[(size_t) proc.ui.selectedPad].s.speed * 0.5f); };
    normal.onClick = [setSpeed] { setSpeed(1.0f); };
    dbl.onClick = [this, setSpeed] { setSpeed(proc.project.state().pads[(size_t) proc.ui.selectedPad].s.speed * 2.0f); };
    auto* speedRow = extras.add(new Pair(&half, &normal, &dbl, &fitBox));
    content.addAndMakeVisible(speedRow);
    rows.push_back({ "PITCH & TIME", nullptr, speedRow, 28 });
    fitBox.setTextWhenNothingSelected("FIT TO...");
    fitBox.setTooltip("Set the speed so the pad lasts exactly this long at the project tempo");
    const juce::StringArray fits { "1/4 beat", "1/2 beat", "1 beat", "2 beats", "1 bar", "2 bars", "4 bars" };
    for (int i = 0; i < fits.size(); ++i) fitBox.addItem(fits[i], i + 1);
    fitBox.onChange = [this, setSpeed]
    {
        const int id = fitBox.getSelectedId();
        if (id <= 0 || updating) return;
        static const double beats[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
        const auto info = proc.padSyncInfo(proc.ui.selectedPad);
        const double target = beats[id - 1] * 60.0 / proc.project.state().settings.bpm;
        const double base = info.outputSeconds * info.userSpeed; // length at speed 1
        if (base > 0.0 && target > 0.0) setSpeed((float) (base / target));
        fitBox.setSelectedId(0, juce::dontSendNotification);
    };
    addCombo("PITCH & TIME", "Sync", { "Project default", "Off (as recorded)", "Repitch (vintage)", "Stretch (independent)" },
             [](const PadSettings& s) { return s.sync + 2; }, [](PadSettings& s, int id) { s.sync = id - 2; });
    addSlider("PITCH & TIME", "Pitch", -24, 24, 1, " st", [](const PadSettings& s) { return s.pitch; }, [](PadSettings& s, float v) { s.pitch = v; });
    addSlider("PITCH & TIME", "Fine", -100, 100, 1, " ct", [](const PadSettings& s) { return s.fine; }, [](PadSettings& s, float v) { s.fine = v; });
    addRow("PITCH & TIME", "", followKey, 24);
    followKey.onClick = [this] { if (!updating) { const bool v = followKey.getToggleState(); edit("Follow key", [v](PadSettings& s) { s.followKey = v; }); } };
    syncInfo.setFont(theme::ui(11.5f));
    syncInfo.setJustificationType(juce::Justification::topLeft);
    addRow("PITCH & TIME", "", syncInfo, 80);
    addRow("PITCH & TIME", "", resetPitch, 26);
    resetPitch.onClick = [this] { edit("Reset pitch", [](PadSettings& s) { s.speed = 1; s.pitch = 0; s.fine = 0; s.sync = -1; s.followKey = true; }); };

    // PLAY
    addCombo("PLAY", "Play mode", { "Project default", "Gate", "One shot", "Toggle" },
             [](const PadSettings& s) { return s.playMode + 2; }, [](PadSettings& s, int id) { s.playMode = id - 2; });
    addCombo("PLAY", "Repeat hits", { "Retrigger", "Layer" },
             [](const PadSettings& s) { return (int) s.repeat + 1; }, [](PadSettings& s, int id) { s.repeat = (RepeatMode) (id - 1); });
    juce::StringArray cut { "Off" };
    for (int i = 1; i <= kMaxChokeGroups; ++i) cut.add("Group " + juce::String(i));
    addCombo("PLAY", "Cut group", cut, [](const PadSettings& s) { return s.chokeGroup + 1; }, [](PadSettings& s, int id) { s.chokeGroup = id - 1; });
    addRow("PLAY", "", loop, 24);
    loop.onClick = [this] { if (!updating) { const bool v = loop.getToggleState(); edit("Loop", [v](PadSettings& s) { s.loop = v; }); } };
    addRow("PLAY", "", reverse, 24);
    reverse.onClick = [this] { if (!updating) { const bool v = reverse.getToggleState(); edit("Reverse", [v](PadSettings& s) { s.reverse = v; }); } };

    // ENVELOPE
    addSlider("ENVELOPE", "Fade in", 0, 200, 0.1, " ms", [](const PadSettings& s) { return s.fadeInMs; }, [](PadSettings& s, float v) { s.fadeInMs = v; }, 10);
    addSlider("ENVELOPE", "Fade out", 0, 1000, 0.1, " ms", [](const PadSettings& s) { return s.fadeOutMs; }, [](PadSettings& s, float v) { s.fadeOutMs = v; }, 20);
    addSlider("ENVELOPE", "Attack", 0, 2000, 0.1, " ms", [](const PadSettings& s) { return s.attackMs; }, [](PadSettings& s, float v) { s.attackMs = v; }, 30);
    addSlider("ENVELOPE", "Decay", 0, 4000, 1, " ms", [](const PadSettings& s) { return s.decayMs; }, [](PadSettings& s, float v) { s.decayMs = v; }, 200);
    addSlider("ENVELOPE", "Sustain", 0, 1, 0.01, "", [](const PadSettings& s) { return s.sustain; }, [](PadSettings& s, float v) { s.sustain = v; });
    addSlider("ENVELOPE", "Release", 1, 4000, 1, " ms", [](const PadSettings& s) { return s.releaseMs; }, [](PadSettings& s, float v) { s.releaseMs = v; }, 150);
    addRow("ENVELOPE", "", resetEnv, 26);
    resetEnv.onClick = [this] { edit("Reset envelope", [](PadSettings& s) { const PadSettings d; s.attackMs = d.attackMs; s.decayMs = d.decayMs; s.sustain = d.sustain; s.releaseMs = d.releaseMs; }); };

    // MIX
    addSlider("MIX", "Gain", -36, 12, 0.1, " dB", [](const PadSettings& s) { return s.gainDb; }, [](PadSettings& s, float v) { s.gainDb = v; });
    addSlider("MIX", "Pan", -1, 1, 0.01, "", [](const PadSettings& s) { return s.pan; }, [](PadSettings& s, float v) { s.pan = v; });
    addSlider("MIX", "High-pass", 20, 2000, 1, " Hz", [](const PadSettings& s) { return s.hpHz; }, [](PadSettings& s, float v) { s.hpHz = v; }, 200);
    addSlider("MIX", "Low-pass", 200, 20000, 1, " Hz", [](const PadSettings& s) { return s.lpHz; }, [](PadSettings& s, float v) { s.lpHz = v; }, 3000);
    addRow("MIX", "", resetMix, 26);
    resetMix.onClick = [this] { edit("Reset mix", [](PadSettings& s) { const PadSettings d; s.gainDb = d.gainDb; s.pan = d.pan; s.hpHz = d.hpHz; s.lpHz = d.lpHz; }); };
    refresh();
}

juce::Slider& PadInspector::addSlider(const juce::String& section, const juce::String& name, double lo, double hi, double step,
                                      const juce::String& suffix, std::function<float(const PadSettings&)> get,
                                      std::function<void(PadSettings&, float)> set, double skewMid)
{
    auto* s = sliders.add(new juce::Slider());
    s->setSliderStyle(juce::Slider::LinearHorizontal);
    s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
    s->setRange(lo, hi, step);
    if (skewMid > 0.0) s->setSkewFactorFromMidPoint(skewMid);
    s->setTextValueSuffix(suffix);
    s->setDoubleClickReturnValue(true, (double) get(PadSettings {})); // reset on the slider, not on a pad
    s->onDragStart = [this, name] { proc.project.beginGesture(name); };
    s->onDragEnd = [this] { proc.project.endGesture(); };
    s->onValueChange = [this, s, set, name]
    {
        if (updating) return;
        const float v = (float) s->getValue();
        edit(name, [set, v](PadSettings& ps) { set(ps, v); });
    };
    Row r;
    r.section = section;
    r.label = std::make_unique<juce::Label>(name, name);
    r.label->setFont(theme::ui(11.5f));
    r.label->setColour(juce::Label::textColourId, theme::textDim);
    content.addAndMakeVisible(*r.label);
    r.control = s;
    content.addAndMakeVisible(s);
    rows.push_back(std::move(r));
    updaters.push_back([this, s, get]
    {
        const auto& p = proc.project.state().pads[(size_t) proc.ui.selectedPad];
        if (!s->isMouseButtonDown()) s->setValue(get(p.s), juce::dontSendNotification);
    });
    return *s;
}

juce::ComboBox& PadInspector::addCombo(const juce::String& section, const juce::String& name, const juce::StringArray& items,
                                       std::function<int(const PadSettings&)> get, std::function<void(PadSettings&, int)> set)
{
    auto* c = combos.add(new juce::ComboBox());
    for (int i = 0; i < items.size(); ++i) c->addItem(items[i], i + 1);
    c->onChange = [this, c, set, name]
    {
        if (updating) return;
        const int id = c->getSelectedId();
        edit(name, [set, id](PadSettings& ps) { set(ps, id); });
    };
    Row r;
    r.section = section;
    r.label = std::make_unique<juce::Label>(name, name);
    r.label->setFont(theme::ui(11.5f));
    r.label->setColour(juce::Label::textColourId, theme::textDim);
    content.addAndMakeVisible(*r.label);
    r.control = c;
    content.addAndMakeVisible(c);
    rows.push_back(std::move(r));
    updaters.push_back([this, c, get]
    {
        const auto& p = proc.project.state().pads[(size_t) proc.ui.selectedPad];
        c->setSelectedId(get(p.s), juce::dontSendNotification);
    });
    return *c;
}

void PadInspector::edit(const juce::String& label, const std::function<void(PadSettings&)>& fn)
{
    const int pad = proc.ui.selectedPad;
    if (!proc.project.state().pads[(size_t) pad].assigned) return;
    proc.project.edit(label + " " + padLabel(pad), [&](ProjectState& s) { fn(s.pads[(size_t) pad].s); });
}

void PadInspector::refresh()
{
    updating = true;
    const auto& st = proc.project.state();
    const int pad = proc.ui.selectedPad;
    const auto& p = st.pads[(size_t) pad];
    header.setText("PAD EDIT  " + padLabel(pad) + (p.assigned ? "  " + p.name : juce::String("  (empty)")), juce::dontSendNotification);
    header.setColour(juce::Label::textColourId, p.assigned ? st.sources[(size_t) p.src].colour : theme::textDim);
    for (auto& u : updaters) u();
    reverse.setToggleState(p.s.reverse, juce::dontSendNotification);
    loop.setToggleState(p.s.loop, juce::dontSendNotification);
    followKey.setToggleState(p.s.followKey, juce::dontSendNotification);
    content.setEnabled(p.assigned);

    if (p.assigned)
    {
        const auto d = proc.project.sourceDataForSlot(p.src);
        const double sr = d ? d->sampleRate : 44100.0;
        sliceInfo.setText("SRC " + juce::String(p.src + 1) + "  IN " + juce::String(p.start) + "  OUT " + juce::String(p.end)
                          + "\n" + juce::String((p.end - p.start) / sr, 3) + " s of source", juce::dontSendNotification);
        const auto info = proc.padSyncInfo(pad);
        juce::String t;
        if (info.sync == SyncMode::Off) t = "Plays as recorded" + juce::String(info.totalTranspose != 0.0f ? ", pitch " + juce::String(info.totalTranspose, 2) + " st (time kept)" : "");
        else if (!info.tempoKnown) t = "Source BPM unknown: no time stretch (set SRC BPM to sync).";
        else
        {
            t << juce::String(info.sourceBpm, 1) << " -> " << juce::String(st.settings.bpm, 1) << " BPM";
            t << (info.sync == SyncMode::Repitch ? "  (repitch " : "  (time x") << juce::String(info.timeRatio, 3) << ")";
        }
        if (std::abs(info.userSpeed - 1.0) > 1e-3) t << "\nSpeed x" << juce::String(info.userSpeed, 2) << (info.sync == SyncMode::Repitch ? " (pitch moves)" : " (pitch kept)");
        t << "\nLength " << juce::String(info.outputSeconds, 3) << " s = " << juce::String(info.outputSeconds * st.settings.bpm / 60.0, 2) << " beats";
        if (info.sync == SyncMode::Stretch)
        {
            if (info.keyFollowActive) t << "\nKey follow: " << (info.keyShift >= 0 ? "+" : "") << juce::String(info.keyShift) << " st";
            else if (p.s.followKey) t << "\nKey follow: off (source or project key unknown)";
            if (std::abs(info.totalTranspose) > 0.001f) t << "   total " << juce::String(info.totalTranspose, 2) << " st";
        }
        else if (info.sync == SyncMode::Repitch) t << "\nPitch moves with speed: " << juce::String(info.totalTranspose, 2) << " st";
        if (info.extreme) t << "\n! extreme ratio - artifacts likely, try REPITCH or reslice";
        syncInfo.setText(t, juce::dontSendNotification);
        syncInfo.setColour(juce::Label::textColourId, info.extreme ? theme::warn : theme::text);
    }
    else
    {
        sliceInfo.setText("Assign a selection to this pad first", juce::dontSendNotification);
        syncInfo.setText({}, juce::dontSendNotification);
    }
    updating = false;
}

void PadInspector::paint(juce::Graphics& g)
{
    g.fillAll(theme::bodyLight);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawVerticalLine(0, 0, (float) getHeight());
}

void PadInspector::resized()
{
    auto r = getLocalBounds().reduced(8, 6);
    auto top = r.removeFromTop(28);
    closeBtn.setBounds(top.removeFromRight(28).reduced(2));
    header.setBounds(top);
    viewport.setBounds(r);
    const int w = r.getWidth() - viewport.getScrollBarThickness() - 2;
    int y = 0;
    juce::String lastSection;
    content.headers.clear();
    for (auto& row : rows)
    {
        if (row.section != lastSection)
        {
            y += lastSection.isEmpty() ? 4 : 14;
            lastSection = row.section;
            content.headers.push_back({ y, row.section });
            y += 20;
        }
        if (row.label)
        {
            row.label->setBounds(0, y, 78, row.height);
            row.control->setBounds(80, y, w - 80, row.height);
        }
        else row.control->setBounds(0, y, w, row.height);
        y += row.height + 3;
    }
    content.setSize(w, y + 10);
    content.repaint();
}
} // namespace hps
