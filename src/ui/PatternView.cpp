#include "ui/PatternView.h"

namespace hps
{
namespace
{
constexpr int kAuditionPointer = 5;
}

// =============================================================================================
// LaneCanvas: geometry (4 source lanes, overlapping notes stacked in sub-rows)
// =============================================================================================

LaneCanvas::LaneCanvas(PatternView& o) : owner(o)
{
    setWantsKeyboardFocus(false);
    for (int lane = 0; lane < kNumSources; ++lane)
        for (int k = 0; k < 3; ++k)
        {
            auto knob = std::make_unique<MixKnob>(k == 0 ? MixKnob::Kind::Volume : k == 1 ? MixKnob::Kind::Pan : MixKnob::Kind::Filter);
            auto* raw = knob.get();
            raw->onGestureStart = [this] { owner.proc.project.beginGesture("SRC mix"); };
            raw->onGestureEnd = [this] { owner.proc.project.endGesture(); };
            raw->onChange = [this, lane, k](float v)
            {
                owner.proc.project.edit("SRC " + juce::String(lane + 1) + (k == 0 ? " volume" : k == 1 ? " pan" : " filter"), [&](ProjectState& s)
                {
                    auto& src = s.sources[(size_t) lane];
                    if (k == 0) src.gainDb = v; else if (k == 1) src.pan = v; else src.filter = v;
                });
            };
            addAndMakeVisible(raw);
            laneKnobs[(size_t) lane][(size_t) k] = std::move(knob);
        }
}

void LaneCanvas::layoutKnobs()
{
    for (int lane = 0; lane < kNumSources; ++lane)
    {
        const auto r = laneRect(lane);
        const bool fits = r.getHeight() >= 70;
        auto row = juce::Rectangle<int>(10, r.getY() + 31, kLabelW - 16, 36);
        for (int k = 0; k < 3; ++k)
        {
            auto& knob = *laneKnobs[(size_t) lane][(size_t) k];
            knob.setVisible(fits);
            knob.setBounds(row.removeFromLeft(row.getWidth() / (3 - k)).withSizeKeepingCentre(38, 36));
        }
    }
}

void LaneCanvas::updateKnobs()
{
    const auto& st = owner.proc.project.state();
    for (int lane = 0; lane < kNumSources; ++lane)
    {
        const auto& s = st.sources[(size_t) lane];
        auto& k = laneKnobs[(size_t) lane];
        k[0]->setValueQuietly(s.gainDb);
        k[1]->setValueQuietly(s.pan);
        k[2]->setValueQuietly(s.filter);
        for (auto& knob : k) { knob->setAccent(s.colour); knob->setEnabled(!s.empty()); }
    }
    layoutKnobs();
}

juce::Rectangle<int> LaneCanvas::laneRect(int lane) const
{
    const bool guide = owner.proc.ui.showGuide && owner.proc.guide.present;
    auto area = getLocalBounds().withTrimmedTop(kRulerH + (guide ? kGuideH : 0));
    const int h = area.getHeight() / kNumSources;
    return { 0, area.getY() + lane * h, getWidth(), h };
}

int LaneCanvas::laneAt(int y) const
{
    for (int i = 0; i < kNumSources; ++i) if (y >= laneRect(i).getY() && y < laneRect(i).getBottom()) return i;
    return -1;
}

double LaneCanvas::tickToX(double t) const
{
    const double vt = viewTicks > 0 ? viewTicks : (double) owner.loopTicks();
    return kLabelW + (t - viewStart) / vt * (getWidth() - kLabelW);
}

double LaneCanvas::xToTick(double x) const
{
    const double vt = viewTicks > 0 ? viewTicks : (double) owner.loopTicks();
    return viewStart + (x - kLabelW) / std::max(1, getWidth() - kLabelW) * vt;
}

int64_t LaneCanvas::snapTick(double t, bool floorIt) const
{
    const auto& st = owner.proc.project.state();
    const int g = gridTicks(st.settings.grid);
    if (!st.settings.snap || g <= 0 || juce::ModifierKeys::currentModifiers.isAltDown()) return (int64_t) std::llround(t);
    return (int64_t) ((floorIt ? std::floor(t / g) : std::floor(t / g + 0.5)) * g);
}

std::vector<LaneCanvas::Placed> LaneCanvas::layout() const
{
    const auto& st = owner.proc.project.state();
    std::vector<Placed> out;
    for (int lane = 0; lane < kNumSources; ++lane)
    {
        std::vector<int> idx;
        for (int i = 0; i < (int) st.events.size(); ++i)
        {
            const auto& p = st.pads[(size_t) st.events[(size_t) i].pad];
            if (p.assigned && p.src == lane) idx.push_back(i);
        }
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return st.events[(size_t) a].tick < st.events[(size_t) b].tick; });
        std::vector<double> subEnd;
        std::vector<int> sub(idx.size(), 0);
        for (size_t k = 0; k < idx.size(); ++k)
        {
            const auto& e = st.events[(size_t) idx[k]];
            const double end = (double) e.tick + std::max<double>((double) e.length, owner.tailTicks(e.pad, e.length));
            int s = 0;
            while (s < (int) subEnd.size() && subEnd[(size_t) s] > (double) e.tick + 1) ++s;
            if (s == (int) subEnd.size()) subEnd.push_back(end); else subEnd[(size_t) s] = end;
            sub[k] = s;
        }
        const int nsub = juce::jlimit(1, 4, (int) subEnd.size());
        const auto lr = laneRect(lane).reduced(0, 3);
        const float sh = (float) lr.getHeight() / nsub;
        for (size_t k = 0; k < idx.size(); ++k)
        {
            const auto& e = st.events[(size_t) idx[k]];
            const float y = lr.getY() + (sub[k] % nsub) * sh + 1;
            const float x0 = (float) tickToX((double) e.tick);
            const float x1 = (float) tickToX((double) (e.tick + e.length));
            const float xt = (float) tickToX((double) e.tick + owner.tailTicks(e.pad, e.length));
            out.push_back({ idx[k], { x0, y, std::max(6.0f, x1 - x0), sh - 2 }, { x0, y, std::max(0.0f, xt - x0), sh - 2 } });
        }
    }
    return out;
}

juce::Rectangle<float> LaneCanvas::rectOf(const juce::String& id) const
{
    const auto& ev = owner.proc.project.state().events;
    for (const auto& p : layout()) if (ev[(size_t) p.index].id == id) return p.trig;
    return {};
}

int LaneCanvas::eventAt(juce::Point<int> p, bool& onRightEdge) const
{
    const auto placed = layout();
    for (auto it = placed.rbegin(); it != placed.rend(); ++it)
        if (it->trig.expanded(2, 0).contains(p.toFloat()))
        {
            onRightEdge = p.x >= it->trig.getRight() - std::min(7.0f, it->trig.getWidth() * 0.35f);
            return it->index;
        }
    return -1;
}

void LaneCanvas::audition(int pad)
{
    stopAudition();
    auditionPad = pad;
    owner.proc.triggerPad(pad, owner.lastVelocity, kAuditionPointer);
}

void LaneCanvas::stopAudition()
{
    if (auditionPad >= 0) owner.proc.releasePad(auditionPad, kAuditionPointer);
    auditionPad = -1;
}

// =============================================================================================
// LaneCanvas: painting
// =============================================================================================

void LaneCanvas::paint(juce::Graphics& g)
{
    const auto& st = owner.proc.project.state();
    g.fillAll(theme::screen);
    const int64_t loop = owner.loopTicks();
    const bool guide = owner.proc.ui.showGuide && owner.proc.guide.present;

    // lanes + labels (the label shows which pad a click places)
    for (int lane = 0; lane < kNumSources; ++lane)
    {
        const auto r = laneRect(lane);
        const auto& s = st.sources[(size_t) lane];
        g.setColour(lane % 2 ? theme::screen : theme::screen.brighter(0.03f));
        g.fillRect(r.withTrimmedLeft(kLabelW));
        auto lab = r.withWidth(kLabelW);
        g.setColour(theme::bodyLight);
        g.fillRect(lab);
        g.setColour(s.colour);
        g.fillRect(lab.removeFromLeft(5));
        auto txt = lab.reduced(6, 4);
        g.setFont(theme::mono(12.0f, true));
        g.drawText("SRC " + juce::String(lane + 1), txt.removeFromTop(14), juce::Justification::topLeft);
        g.setFont(theme::ui(11.0f));
        g.setColour(s.empty() ? theme::textDim : theme::text);
        g.drawFittedText(s.empty() ? "drop audio here" : s.name, txt.removeFromTop(13), juce::Justification::topLeft, 1, 0.8f);
        if (r.getHeight() >= 70) // knob well behind VOL / PAN / FILTER
        {
            g.setColour(theme::screen.withAlpha(0.55f));
            g.fillRoundedRectangle(juce::Rectangle<float>(8.0f, (float) r.getY() + 30.0f, (float) kLabelW - 14.0f, 38.0f), 5.0f);
        }
        const int pad = owner.lanePad(lane);
        if (pad >= 0 && txt.getHeight() >= 12)
        {
            g.setColour(s.colour);
            g.setFont(theme::mono(10.5f, true));
            g.drawText("click places " + padLabel(pad), txt.removeFromBottom(14), juce::Justification::bottomLeft);
        }
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawHorizontalLine(r.getBottom() - 1, 0.0f, (float) getWidth());
    }

    // grid
    const int sub = gridTicks(st.settings.grid);
    const double pxPerTick = (getWidth() - kLabelW) / (viewTicks > 0 ? viewTicks : (double) loop);
    const auto top = (float) kRulerH, bottom = (float) getHeight();
    const int step = sub > 0 && pxPerTick * sub >= 5 ? sub : (pxPerTick * kPPQ >= 5 ? kPPQ : kTicksPerBar);
    for (int64_t t = 0; t <= loop; t += step)
    {
        const float x = (float) tickToX((double) t);
        if (x < kLabelW - 1 || x > getWidth()) continue;
        const bool bar = t % kTicksPerBar == 0, beat = t % kPPQ == 0;
        g.setColour(juce::Colours::white.withAlpha(bar ? 0.28f : beat ? 0.12f : 0.05f));
        g.drawVerticalLine((int) x, top, bottom);
    }

    // notes: audible tail (translucent) + trigger block
    for (const auto& p : layout())
    {
        const auto& e = st.events[(size_t) p.index];
        const auto& pad = st.pads[(size_t) e.pad];
        const auto col = st.sources[(size_t) pad.src].colour;
        const bool sel = owner.isSelected(e.id);
        g.setColour(col.withAlpha(0.16f));
        g.fillRoundedRectangle(p.tail, 3.0f);
        g.setColour(sel ? col.brighter(0.3f) : col.withAlpha(0.35f + 0.6f * e.velocity));
        g.fillRoundedRectangle(p.trig, 3.0f);
        g.setColour(sel ? juce::Colours::white : col.brighter(0.4f));
        g.drawRoundedRectangle(p.trig, 3.0f, sel ? 2.0f : 1.0f);
        // resize grip on the right end
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRect(p.trig.getRight() - 3.0f, p.trig.getY() + 3.0f, 2.0f, p.trig.getHeight() - 6.0f);
        if (p.trig.getWidth() > 26 && p.trig.getHeight() > 12)
        {
            g.setColour(theme::screen);
            g.setFont(theme::mono(10.5f, true));
            g.drawText(padLabel(e.pad), p.trig.reduced(3, 0).toNearestInt(), juce::Justification::centredLeft);
        }
        if (e.rawTick >= 0 && e.rawTick != e.tick) // quantized recording: where it was played
        {
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawVerticalLine((int) tickToX((double) e.rawTick), p.trig.getY(), p.trig.getY() + 4);
        }
    }

    // ruler + paste marker
    auto ruler = getLocalBounds().removeFromTop(kRulerH);
    g.setColour(theme::bodyLight);
    g.fillRect(ruler);
    for (int64_t t = 0; t < loop; t += kPPQ)
    {
        const float x = (float) tickToX((double) t);
        if (x < kLabelW - 1 || x > getWidth()) continue;
        const bool bar = t % kTicksPerBar == 0;
        g.setColour(bar ? theme::text : theme::textDim);
        g.drawVerticalLine((int) x, bar ? 4.0f : 16.0f, (float) kRulerH);
        if (bar) { g.setFont(theme::mono(11.0f, true)); g.drawText(juce::String(t / kTicksPerBar + 1), (int) x + 3, 2, 30, 14, juce::Justification::centredLeft); }
        else if (pxPerTick * kPPQ > 26) { g.setFont(theme::mono(9.5f)); g.drawText(juce::String((t % kTicksPerBar) / kPPQ + 1), (int) x + 2, 12, 20, 12, juce::Justification::centredLeft); }
    }
    {
        const float cx = (float) tickToX((double) cursorTick);
        juce::Path p;
        p.addTriangle(cx - 5, (float) kRulerH - 8, cx + 5, (float) kRulerH - 8, cx, (float) kRulerH);
        g.setColour(theme::accent.withAlpha(0.85f));
        g.fillPath(p);
    }
    g.setColour(theme::textDim);
    g.setFont(theme::ui(11.0f));
    g.drawText(juce::String(st.settings.bpm, 1) + " BPM  " + st.settings.key.label(), juce::Rectangle<int>(0, 0, kLabelW, kRulerH).reduced(6, 0), juce::Justification::centredLeft);

    // drum guide overlay (not a lane)
    if (guide)
    {
        auto gr = juce::Rectangle<int>(kLabelW, kRulerH, getWidth() - kLabelW, kGuideH);
        g.setColour(theme::body);
        g.fillRect(gr);
        g.setColour(theme::textDim);
        g.setFont(theme::ui(10.0f, true));
        g.drawText("DRUM GUIDE", juce::Rectangle<int>(0, kRulerH, kLabelW, kGuideH).reduced(6, 0), juce::Justification::centredLeft);
        const auto& G = owner.proc.guide;
        const int64_t gl = G.loopBars > 0 ? (int64_t) G.loopBars * kTicksPerBar : loop;
        for (int64_t base = 0; base < loop; base += std::max<int64_t>(kPPQ, gl))
            for (const auto& h : G.hits)
            {
                const int64_t t = base + h.tick;
                if (t >= loop) continue;
                const float x = (float) tickToX((double) t);
                const juce::Colour c = h.lane == hpbridge::DrumLaneClass::Kick ? juce::Colour(0xffff6b6b)
                                     : h.lane == hpbridge::DrumLaneClass::Snare ? juce::Colour(0xffffe066) : juce::Colour(0xff8ecae6);
                const float hh = h.lane == hpbridge::DrumLaneClass::Kick ? 1.0f : h.lane == hpbridge::DrumLaneClass::Snare ? 0.75f : 0.45f;
                g.setColour(c.withAlpha(0.5f + 0.5f * h.velocity));
                g.fillRect(x - 1.0f, (float) gr.getBottom() - kGuideH * hh, 2.5f, kGuideH * hh);
                g.setColour(c.withAlpha(0.06f));
                if (h.lane != hpbridge::DrumLaneClass::Hat) g.drawVerticalLine((int) x, (float) gr.getBottom(), bottom);
            }
    }

    if (owner.proc.engine.transportPlaying.load())
    {
        const float x = (float) tickToX(owner.proc.engine.playheadTick.load());
        g.setColour(owner.proc.isRecordArmed() ? theme::rec : theme::ok);
        g.fillRect(x - 1.0f, 0.0f, 2.0f, (float) getHeight());
    }
    if (dropX >= 0)
    {
        g.setColour(theme::accent);
        g.drawVerticalLine((int) tickToX((double) snapTick(xToTick(dropX), true)), (float) kRulerH, bottom);
    }
    if (drag == Drag::Marquee)
    {
        g.setColour(juce::Colours::white.withAlpha(0.1f));
        g.fillRect(band);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.drawRect(band);
    }
    if (st.events.empty())
    {
        g.setColour(theme::textDim);
        g.setFont(theme::ui(14.0f));
        g.drawFittedText("Click a lane to place its pad (as in FL Piano roll), right-click deletes, Ctrl+drag selects, or press REC and play",
                         getLocalBounds().withTrimmedLeft(kLabelW).withTrimmedTop(kRulerH).reduced(20), juce::Justification::centred, 2);
    }
}

// =============================================================================================
// LaneCanvas: FL Piano-roll logic
// =============================================================================================

void LaneCanvas::mouseMove(const juce::MouseEvent& e)
{
    bool edge = false;
    const int ev = e.x > kLabelW && e.y > kRulerH ? eventAt(e.getPosition(), edge) : -1;
    setMouseCursor(ev >= 0 ? (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor)
                           : (e.x > kLabelW && laneAt(e.y) >= 0 && owner.proc.ui.drawTool && !e.mods.isCtrlDown()
                                  ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor));
}

void LaneCanvas::mouseDown(const juce::MouseEvent& e)
{
    owner.grabKeyboardFocus();
    auto& ui = owner.proc.ui;
    auto& project = owner.proc.project;
    const auto& st = project.state();
    downPos = e.getPosition();
    drag = Drag::None;

    if (e.y < kRulerH) // ruler: paste position
    {
        if (e.x >= kLabelW) cursorTick = juce::jlimit<int64_t>(0, owner.loopTicks() - 1, snapTick(xToTick(e.x), true));
        repaint();
        return;
    }
    if (e.x < kLabelW) // lane label: open that source
    {
        const int lane = laneAt(e.y);
        if (lane >= 0 && owner.onShowSource) owner.onShowSource(lane);
        return;
    }
    const int lane = laneAt(e.y);
    if (lane < 0) return;
    const double mouseTick = xToTick(e.x);
    cursorTick = juce::jlimit<int64_t>(0, owner.loopTicks() - 1, snapTick(mouseTick, true));
    bool edge = false;
    const int evIndex = eventAt(e.getPosition(), edge);

    // RMB: delete, keep the button down to sweep-delete
    if (e.mods.isRightButtonDown())
    {
        project.beginGesture("Delete notes");
        drag = Drag::Erase;
        if (evIndex >= 0)
        {
            const auto id = st.events[(size_t) evIndex].id;
            project.edit("Delete notes", [&](ProjectState& s) { s.events.erase(std::remove_if(s.events.begin(), s.events.end(), [&](const PatternEvent& x) { return x.id == id; }), s.events.end()); });
        }
        return;
    }

    // Ctrl+drag (or SELECT tool on empty space): marquee
    if (e.mods.isCtrlDown() || (!ui.drawTool && evIndex < 0))
    {
        drag = Drag::Marquee;
        bandAdd = e.mods.isShiftDown();
        if (!bandAdd) ui.selectedEvents.clear();
        band = { e.x, e.y, 0, 0 };
        owner.refresh();
        return;
    }

    if (evIndex >= 0)
    {
        auto ev = st.events[(size_t) evIndex];
        if (!owner.isSelected(ev.id)) ui.selectedEvents = { ev.id };
        // FL "pick": the clicked note's pad, length and velocity are used for the next placed notes
        owner.lastLength = ev.length;
        owner.lastVelocity = ev.velocity;
        owner.laneLastPad[(size_t) st.pads[(size_t) ev.pad].src] = ev.pad;
        ui.selectedPad = ev.pad;
        project.beginGesture(edge ? "Resize notes" : (e.mods.isShiftDown() ? "Copy notes" : "Move notes"));
        if (e.mods.isShiftDown() && !edge) // Shift+drag = copy: the copies move, the originals stay
        {
            std::vector<juce::String> fresh;
            juce::String grabbed;
            project.edit("Copy notes", [&](ProjectState& s)
            {
                std::vector<PatternEvent> add;
                for (const auto& x : s.events)
                    if (owner.isSelected(x.id))
                    {
                        auto c = x;
                        c.id = newId();
                        c.rawTick = -1;
                        if (x.id == ev.id) grabbed = c.id;
                        add.push_back(c);
                        fresh.push_back(c.id);
                    }
                s.events.insert(s.events.end(), add.begin(), add.end());
            });
            ui.selectedEvents = fresh;
            ev.id = grabbed;
        }
        orig.clear();
        for (const auto& x : project.state().events)
            if (owner.isSelected(x.id)) orig.push_back({ x.id, x.tick, x.length });
        anchorTick = ev.tick;
        anchorLen = ev.length;
        grabOffset = (int64_t) mouseTick - ev.tick;
        drag = edge ? Drag::Resize : Drag::Move;
        if (!edge) audition(ev.pad);
        owner.refresh();
        return;
    }

    // LMB on empty lane space: place the lane's pad and keep dragging to move it
    const int pad = owner.lanePad(lane);
    if (pad < 0)
    {
        owner.proc.setStatus("No pad uses SRC " + juce::String(lane + 1) + " yet - assign a chop from SRC " + juce::String(lane + 1) + " first");
        return;
    }
    PatternEvent n;
    n.id = newId();
    n.pad = pad;
    n.tick = juce::jlimit<int64_t>(0, owner.loopTicks() - 1, snapTick(mouseTick, true));
    n.length = std::max<int64_t>(kPPQ / 32, owner.lastLength);
    n.velocity = owner.lastVelocity;
    project.beginGesture("Add note");
    project.edit("Add note", [&](ProjectState& s) { s.events.push_back(n); });
    ui.selectedEvents = { n.id };
    ui.selectedPad = pad;
    orig = { { n.id, n.tick, n.length } };
    anchorTick = n.tick;
    anchorLen = n.length;
    grabOffset = (int64_t) mouseTick - n.tick;
    drag = Drag::Move;
    audition(pad);
    owner.refresh();
}

void LaneCanvas::mouseDrag(const juce::MouseEvent& e)
{
    auto& project = owner.proc.project;
    auto& ui = owner.proc.ui;
    const int64_t loop = owner.loopTicks();
    switch (drag)
    {
        case Drag::Marquee:
            band = juce::Rectangle<int>(downPos, e.getPosition());
            if (!bandAdd) ui.selectedEvents.clear();
            for (const auto& p : layout())
            {
                const auto& id = project.state().events[(size_t) p.index].id;
                if (band.toFloat().intersects(p.trig) && !owner.isSelected(id)) ui.selectedEvents.push_back(id);
            }
            repaint();
            return;
        case Drag::Erase:
        {
            bool edge = false;
            const int ev = eventAt(e.getPosition(), edge);
            if (ev >= 0)
            {
                const auto id = project.state().events[(size_t) ev].id;
                project.edit("Delete notes", [&](ProjectState& s) { s.events.erase(std::remove_if(s.events.begin(), s.events.end(), [&](const PatternEvent& x) { return x.id == id; }), s.events.end()); });
            }
            return;
        }
        case Drag::Move:
        {
            const int64_t delta = snapTick(xToTick(e.x) - (double) grabOffset) - anchorTick;
            project.edit("Move notes", [&](ProjectState& s)
            {
                for (auto& x : s.events)
                    for (const auto& o : orig)
                        if (x.id == o.id)
                        {
                            const int64_t t = juce::jlimit<int64_t>(0, loop - 1, o.tick + delta);
                            if (t != x.tick) x.rawTick = -1;
                            x.tick = t;
                        }
            });
            return;
        }
        case Drag::Resize:
        {
            const auto& st = project.state();
            const int g = gridTicks(st.settings.grid);
            const int64_t minLen = (st.settings.snap && g > 0 && !e.mods.isAltDown()) ? g : kPPQ / 32;
            const int64_t newLen = std::max(minLen, snapTick(xToTick(e.x)) - anchorTick);
            const int64_t delta = newLen - anchorLen;
            project.edit("Resize notes", [&](ProjectState& s)
            {
                for (auto& x : s.events)
                    for (const auto& o : orig)
                        if (x.id == o.id) x.length = juce::jlimit<int64_t>(minLen, loop, o.length + delta);
            });
            owner.lastLength = newLen; // FL: the new length is used for the next notes
            return;
        }
        case Drag::None: return;
    }
}

void LaneCanvas::mouseUp(const juce::MouseEvent&)
{
    if (drag == Drag::Move || drag == Drag::Resize || drag == Drag::Erase) owner.proc.project.endGesture();
    stopAudition();
    const bool wasMarquee = drag == Drag::Marquee;
    drag = Drag::None;
    if (wasMarquee) owner.refresh();
    repaint();
}

void LaneCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const double loop = (double) owner.loopTicks();
    double vt = viewTicks > 0 ? viewTicks : loop;
    const float dy = w.deltaY;
    if (e.mods.isAltDown()) // velocity of the note under the mouse (or of the whole selection)
    {
        bool edge = false;
        const int ev = eventAt(e.getPosition(), edge);
        if (ev < 0) return;
        const auto id = owner.proc.project.state().events[(size_t) ev].id;
        const bool sel = owner.isSelected(id);
        owner.proc.project.edit("Velocity", [&](ProjectState& s)
        {
            for (auto& x : s.events)
                if (x.id == id || (sel && owner.isSelected(x.id))) x.velocity = juce::jlimit(0.05f, 1.0f, x.velocity + (dy > 0 ? 0.05f : -0.05f));
        });
        return;
    }
    if (e.mods.isCtrlDown())
    {
        const double anchor = xToTick(e.x);
        vt = juce::jlimit((double) kPPQ / 2, loop, vt * std::pow(0.8, dy * 4.0));
        viewStart = anchor - (e.x - kLabelW) / (double) std::max(1, getWidth() - kLabelW) * vt;
    }
    else viewStart -= (std::abs(w.deltaX) > 0 ? w.deltaX : dy) * vt * 0.25;
    viewTicks = vt >= loop ? 0.0 : vt;
    viewStart = juce::jlimit(0.0, std::max(0.0, loop - vt), viewStart);
    repaint();
}

void LaneCanvas::itemDropped(const SourceDetails& d)
{
    dropX = -1;
    const int pad = d.description.toString().fromFirstOccurrenceOf("pad:", false, false).getIntValue();
    if (pad < 0 || pad >= kNumPads || !owner.proc.project.state().pads[(size_t) pad].assigned) return;
    PatternEvent ev;
    ev.id = newId();
    ev.pad = pad;
    ev.tick = juce::jlimit<int64_t>(0, owner.loopTicks() - 1, snapTick(xToTick(d.localPosition.x), true));
    ev.length = owner.lastLength;
    ev.velocity = owner.lastVelocity;
    owner.proc.project.edit("Place " + padLabel(pad), [&](ProjectState& s) { s.events.push_back(ev); });
    owner.proc.ui.selectedEvents = { ev.id };
    owner.laneLastPad[(size_t) owner.proc.project.state().pads[(size_t) pad].src] = pad;
}

void LaneCanvas::filesDropped(const juce::StringArray& files, int, int y)
{
    const int lane = laneAt(y);
    if (lane >= 0 && !files.isEmpty()) owner.proc.loadSource(lane, juce::File(files[0]));
}

// =============================================================================================
// PatternView
// =============================================================================================

PatternView::PatternView(HPSamplerProcessor& p) : proc(p)
{
    setWantsKeyboardFocus(true);
    addAndMakeVisible(lanes);
    for (auto* b : { &patternTab, &resultTab, &drawBtn, &selectBtn, &helpBtn, &guideBtn, &quantBtn, &clearBtn, &wavBtn, &midiBtn, &renderBtn })
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    addAndMakeVisible(barsBox);
    addAndMakeVisible(info);
    info.setFont(theme::ui(12.0f));
    info.setColour(juce::Label::textColourId, theme::textDim);
    for (int b : { 1, 2, 4, 8, 16 }) barsBox.addItem(juce::String(b) + (b == 1 ? " BAR" : " BARS"), b);
    barsBox.onChange = [this]
    {
        const int b = barsBox.getSelectedId();
        if (b > 0 && b != proc.project.state().settings.patternBars)
            proc.project.edit("Pattern length", [b](ProjectState& s) { s.settings.patternBars = b; });
    };
    patternTab.onClick = [this] { proc.ui.mainTab = 0; refresh(); };
    resultTab.onClick = [this] { proc.ui.mainTab = 1; proc.renderResult(); refresh(); };
    renderBtn.onClick = [this] { proc.renderResult(); repaint(); };
    drawBtn.setTooltip("Draw (FL: P): click places notes");
    selectBtn.setTooltip("Select (FL: E): drag selects; Ctrl+drag selects in Draw too");
    drawBtn.onClick = [this] { proc.ui.drawTool = true; refresh(); };
    selectBtn.onClick = [this] { proc.ui.drawTool = false; refresh(); };
    helpBtn.onClick = [this] { showHelp(); };
    guideBtn.setTooltip("Show the HPDG drum guide (kick / snare / hats) above the lanes");
    guideBtn.onClick = [this] { proc.ui.showGuide = !proc.ui.showGuide; refresh(); };
    quantBtn.setTooltip("Re-apply REC QUANTIZE (strength / grid in the control strip) to the recorded timing. Strength 0 = back to what you played.");
    quantBtn.onClick = [this] { proc.requantizeRecorded(); };
    clearBtn.onClick = [this]
    {
        if (proc.project.state().events.empty()) return;
        proc.project.edit("Clear pattern", [](ProjectState& s) { s.events.clear(); });
        proc.setStatus("Pattern cleared (Undo restores it)");
    };
    wavBtn.onClick = [this] { exportWav(); };
    midiBtn.onClick = [this] { exportMidi(); };
    startTimerHz(30);
    refresh();
}

PatternView::~PatternView() { stopTimer(); }

void PatternView::showHelp()
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::NoIcon, "MAIN - editing as in FL Studio Piano roll",
        "Mouse\n"
        "  Left click on a lane   place the lane's pad (its label shows which), keep dragging to move it\n"
        "  Left drag a note       move        Drag its right end  resize        Shift+drag  copy        Alt  no snap\n"
        "  Click a note           'pick': its pad, length and velocity are used for the next notes\n"
        "  Right click            delete      Right drag  delete everything it touches\n"
        "  Ctrl+drag              select      Ctrl+Shift+drag  add to the selection\n"
        "  Wheel  scroll    Ctrl+wheel  zoom    Alt+wheel on a note  velocity    Click the ruler  paste position\n\n"
        "Keys\n"
        "  Ctrl+A  select all    Ctrl+D  deselect    Ctrl+B  duplicate after the selection\n"
        "  Ctrl+C / Ctrl+X / Ctrl+V  copy / cut / paste at the ruler marker    Delete  delete\n"
        "  Left / Right  move by the grid (Shift: a bar)    Up / Down  next / previous pad of the same source\n"
        "  Alt+Q  quantize    P / E  draw / select tool    Ctrl+Z / Ctrl+Y  undo / redo    Space  play / stop", "OK", this);
}

int PatternView::lanePad(int lane) const
{
    const auto& st = proc.project.state();
    auto fits = [&](int pad) { return pad >= 0 && pad < kNumPads && st.pads[(size_t) pad].assigned && st.pads[(size_t) pad].src == lane; };
    if (fits(laneLastPad[(size_t) lane])) return laneLastPad[(size_t) lane];   // last picked / placed on this lane
    if (fits(proc.ui.selectedPad)) return proc.ui.selectedPad;                  // the pad you just played / selected
    for (int i = 0; i < kNumPads; ++i) if (fits(i)) return i;
    return -1;
}

double PatternView::tailTicks(int pad, int64_t length) const
{
    const auto& st = proc.project.state();
    const auto& p = st.pads[(size_t) pad];
    const double secs = proc.padSoundSeconds(pad);
    const double soundTicks = secs * st.settings.bpm / 60.0 * kPPQ;
    if (!p.assigned) return (double) length;
    if (effectivePlayMode(p, st.settings) == PlayMode::Gate)
    {
        const double rel = p.s.releaseMs * 0.001 * st.settings.bpm / 60.0 * kPPQ;
        return p.s.loop ? (double) length + rel : std::min((double) length + rel, soundTicks);
    }
    return soundTicks;
}

bool PatternView::isSelected(const juce::String& id) const
{
    const auto& s = proc.ui.selectedEvents;
    return std::find(s.begin(), s.end(), id) != s.end();
}

int64_t PatternView::gridStep() const
{
    const int g = gridTicks(proc.project.state().settings.grid);
    return g > 0 ? g : kPPQ / 4;
}

void PatternView::deleteSelected()
{
    auto& sel = proc.ui.selectedEvents;
    if (sel.empty()) return;
    proc.project.edit("Delete notes", [&](ProjectState& s)
    {
        s.events.erase(std::remove_if(s.events.begin(), s.events.end(), [&](const PatternEvent& e) { return isSelected(e.id); }), s.events.end());
    });
    sel.clear();
}

void PatternView::duplicateSelected()
{
    auto& sel = proc.ui.selectedEvents;
    if (sel.empty()) return;
    const auto& st = proc.project.state();
    int64_t lo = INT64_MAX, hi = 0;
    for (const auto& e : st.events)
        if (isSelected(e.id)) { lo = std::min(lo, e.tick); hi = std::max(hi, e.tick + e.length); }
    const int64_t g = gridStep();
    const int64_t span = std::max<int64_t>(g, ((hi - lo + g - 1) / g) * g);
    const int64_t loop = loopTicks();
    std::vector<juce::String> fresh;
    proc.project.edit("Duplicate notes", [&](ProjectState& s)
    {
        std::vector<PatternEvent> add;
        for (const auto& e : s.events)
            if (isSelected(e.id) && e.tick + span < loop)
            {
                auto c = e;
                c.id = newId();
                c.tick += span;
                c.rawTick = -1;
                add.push_back(c);
                fresh.push_back(c.id);
            }
        s.events.insert(s.events.end(), add.begin(), add.end());
    });
    if (!fresh.empty()) sel = fresh; // the copies stay selected: Ctrl+B again repeats the pattern
}

void PatternView::copySelected(bool cut)
{
    clipboard.clear();
    for (const auto& e : proc.project.state().events)
        if (isSelected(e.id)) clipboard.push_back(e);
    if (cut) deleteSelected();
    proc.setStatus(juce::String((int) clipboard.size()) + " note(s) " + (cut ? "cut" : "copied") + " - Ctrl+V pastes at the ruler marker");
}

void PatternView::paste()
{
    if (clipboard.empty()) return;
    int64_t lo = INT64_MAX;
    for (const auto& e : clipboard) lo = std::min(lo, e.tick);
    const int64_t at = lanes.cursorTick, loop = loopTicks();
    std::vector<juce::String> fresh;
    proc.project.edit("Paste notes", [&](ProjectState& s)
    {
        for (const auto& e : clipboard)
        {
            if (!s.pads[(size_t) e.pad].assigned) continue;
            auto c = e;
            c.id = newId();
            c.tick = at + (e.tick - lo);
            c.rawTick = -1;
            if (c.tick >= loop) continue;
            s.events.push_back(c);
            fresh.push_back(c.id);
        }
    });
    proc.ui.selectedEvents = fresh;
}

void PatternView::nudgeSelected(int64_t ticks, int padStep)
{
    if (proc.ui.selectedEvents.empty()) return;
    const int64_t loop = loopTicks();
    proc.project.edit("Move notes", [&](ProjectState& s)
    {
        for (auto& e : s.events)
        {
            if (!isSelected(e.id)) continue;
            e.tick = juce::jlimit<int64_t>(0, loop - 1, e.tick + ticks);
            if (ticks != 0) e.rawTick = -1;
            if (padStep != 0)
            {
                std::vector<int> same; // pads of the same source: the lane never changes
                for (int i = 0; i < kNumPads; ++i)
                    if (s.pads[(size_t) i].assigned && s.pads[(size_t) i].src == s.pads[(size_t) e.pad].src) same.push_back(i);
                auto it = std::find(same.begin(), same.end(), e.pad);
                if (it != same.end())
                    e.pad = same[(size_t) juce::jlimit(0, (int) same.size() - 1, (int) (it - same.begin()) + padStep)];
            }
        }
    });
}

void PatternView::quantizeSelected()
{
    const int64_t g = gridStep(), loop = loopTicks();
    const bool all = proc.ui.selectedEvents.empty();
    proc.project.edit("Quantize", [&](ProjectState& s)
    {
        for (auto& e : s.events)
        {
            if (!all && !isSelected(e.id)) continue;
            if (e.rawTick < 0) e.rawTick = e.tick; // keeps the played / placed timing for un-quantize
            e.tick = ((e.tick + g / 2) / g * g) % loop;
        }
    });
    proc.setStatus(all ? "All notes quantized to the grid" : "Selection quantized to the grid");
}

bool PatternView::keyPressed(const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    const int code = k.getKeyCode();
    auto is = [&](char c) { return code == c || code == c + 32; };
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelected(); return true; }
    if (m.isCtrlDown() && !m.isAltDown())
    {
        if (is('A')) { proc.ui.selectedEvents.clear(); for (const auto& e : proc.project.state().events) proc.ui.selectedEvents.push_back(e.id); refresh(); return true; }
        if (is('D')) { proc.ui.selectedEvents.clear(); refresh(); return true; }
        if (is('B')) { duplicateSelected(); return true; }
        if (is('C')) { copySelected(false); return true; }
        if (is('X')) { copySelected(true); return true; }
        if (is('V')) { paste(); return true; }
        return false; // Ctrl+Z / Ctrl+Y go to the editor
    }
    if (m.isAltDown() && is('Q')) { quantizeSelected(); return true; }
    if (!m.isCtrlDown() && !m.isAltDown())
    {
        if (is('P')) { proc.ui.drawTool = true; refresh(); return true; }   // FL: Draw
        if (is('E')) { proc.ui.drawTool = false; refresh(); return true; }  // FL: Select
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const int64_t step = m.isShiftDown() ? (int64_t) kTicksPerBar : gridStep();
        nudgeSelected(code == juce::KeyPress::leftKey ? -step : step, 0);
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        nudgeSelected(0, code == juce::KeyPress::upKey ? 1 : -1);
        return true;
    }
    return false;
}

void PatternView::refresh()
{
    const auto& st = proc.project.state();
    barsBox.setSelectedId(st.settings.patternBars, juce::dontSendNotification);
    patternTab.setToggleState(proc.ui.mainTab == 0, juce::dontSendNotification);
    resultTab.setToggleState(proc.ui.mainTab == 1, juce::dontSendNotification);
    drawBtn.setToggleState(proc.ui.drawTool, juce::dontSendNotification);
    selectBtn.setToggleState(!proc.ui.drawTool, juce::dontSendNotification);
    guideBtn.setToggleState(proc.ui.showGuide && proc.guide.present, juce::dontSendNotification);
    guideBtn.setEnabled(proc.guide.present);
    lanes.setVisible(proc.ui.mainTab == 0);
    for (auto* b : { &drawBtn, &selectBtn, &helpBtn, &quantBtn, &clearBtn }) b->setVisible(proc.ui.mainTab == 0);
    renderBtn.setVisible(proc.ui.mainTab == 1);
    // drop ids of notes that no longer exist (undo / clear)
    auto& sel = proc.ui.selectedEvents;
    sel.erase(std::remove_if(sel.begin(), sel.end(), [&](const juce::String& id)
    {
        return std::none_of(st.events.begin(), st.events.end(), [&](const PatternEvent& e) { return e.id == id; });
    }), sel.end());
    juce::String t;
    t << (int) st.events.size() << " notes";
    if (!sel.empty()) t << ", " << (int) sel.size() << " selected";
    t << "   new: " << padLabel(proc.ui.selectedPad) << ", " << juce::String((double) lastLength / kPPQ, 2) << " beat";
    if (proc.guide.present) t << "  |  guide: " << proc.guide.origin << " " << juce::String(proc.guide.bpm, 1) << " BPM";
    info.setText(t, juce::dontSendNotification);
    lanes.updateKnobs();
    lanes.repaint();
    repaint();
}

void PatternView::timerCallback()
{
    bool sounding = false;
    for (const auto& v : proc.engine.padVoices) if (v.load() > 0) { sounding = true; break; }
    if (proc.engine.transportPlaying.load() || sounding || lastTab != proc.ui.mainTab) { lanes.repaint(); lastTab = proc.ui.mainTab; }
}

void PatternView::paint(juce::Graphics& g)
{
    g.fillAll(theme::body);
    if (proc.ui.mainTab != 1) return;
    auto r = getLocalBounds().reduced(6).withTrimmedTop(32);
    g.setColour(theme::screen);
    g.fillRect(r);
    const auto& st = proc.project.state();
    if (proc.resultAudio == nullptr || st.events.empty())
    {
        g.setColour(theme::textDim);
        g.setFont(theme::ui(15.0f));
        g.drawText(st.events.empty() ? "The pattern is empty - nothing to render" : "Press RENDER", r, juce::Justification::centred);
        return;
    }
    const auto& a = *proc.resultAudio;
    const int n = a.getNumSamples();
    const float mid = (float) r.getCentreY(), half = r.getHeight() * 0.45f;
    const double bars = st.settings.patternBars;
    for (int b = 0; b <= (int) bars; ++b)
    {
        const float x = r.getX() + (float) (b / bars) * r.getWidth();
        g.setColour(juce::Colours::white.withAlpha(0.2f));
        g.drawVerticalLine((int) x, (float) r.getY(), (float) r.getBottom());
    }
    g.setColour(theme::wave);
    for (int x = 0; x < r.getWidth(); ++x)
    {
        const int s0 = (int) ((double) x / r.getWidth() * n), s1 = (int) ((double) (x + 1) / r.getWidth() * n);
        float mn = 0, mx = 0;
        for (int s = s0; s < std::min(n, s1); ++s)
        {
            const float v = 0.5f * (a.getSample(0, s) + a.getSample(1, s));
            mn = std::min(mn, v); mx = std::max(mx, v);
        }
        g.drawVerticalLine(r.getX() + x, mid - mx * half, mid - mn * half + 1);
    }
    if (proc.resultRevision != proc.project.revision())
    {
        g.setColour(theme::warn);
        g.setFont(theme::ui(12.0f));
        g.drawText("pattern changed - press RENDER", r.reduced(8), juce::Justification::topRight);
    }
}

void PatternView::resized()
{
    auto r = getLocalBounds().reduced(6, 4);
    auto top = r.removeFromTop(30);
    patternTab.setBounds(top.removeFromLeft(84).reduced(2));
    resultTab.setBounds(top.removeFromLeft(76).reduced(2));
    top.removeFromLeft(10);
    drawBtn.setBounds(top.removeFromLeft(58).reduced(2));
    selectBtn.setBounds(top.removeFromLeft(64).reduced(2));
    helpBtn.setBounds(top.removeFromLeft(64).reduced(2));
    barsBox.setBounds(top.removeFromLeft(90).reduced(2));
    guideBtn.setBounds(top.removeFromLeft(98).reduced(2));
    midiBtn.setBounds(top.removeFromRight(96).reduced(2));
    wavBtn.setBounds(top.removeFromRight(94).reduced(2));
    clearBtn.setBounds(top.removeFromRight(60).reduced(2));
    quantBtn.setBounds(top.removeFromRight(110).reduced(2));
    renderBtn.setBounds(top.removeFromRight(80).reduced(2));
    info.setBounds(top.reduced(6, 0));
    lanes.setBounds(r.withTrimmedTop(2));
}

void PatternView::exportWav()
{
    chooser = std::make_unique<juce::FileChooser>("Export MAIN as WAV", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("HPSAMPLER_main.wav"), "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("wav")) f = f.withFileExtension("wav");
        juce::String err;
        proc.setStatus(proc.exportWav(f, err) ? "Exported " + f.getFullPathName() : "Export failed: " + err);
    });
}

void PatternView::exportMidi()
{
    chooser = std::make_unique<juce::FileChooser>("Export pad triggers as MIDI", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("HPSAMPLER_pattern.mid"), "*.mid");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("mid")) f = f.withFileExtension("mid");
        juce::String err;
        proc.setStatus(proc.exportMidi(f, err) ? "Exported " + f.getFullPathName() : "Export failed: " + err);
    });
}
} // namespace hps
