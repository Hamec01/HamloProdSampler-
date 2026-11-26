#include "ui/PatternView.h"

namespace hps
{
namespace
{
constexpr int kAuditionPointer = Engine::kAuditionPointer | 5;
using Tool = UiState::Tool;
}

// =============================================================================================
// Tool icons
// =============================================================================================

void ToolButton::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(active ? theme::panelHi : theme::panel);
    g.fillRoundedRectangle(r, 4.0f);
    if (isMouseOver()) { g.setColour(juce::Colours::white.withAlpha(0.06f)); g.fillRoundedRectangle(r, 4.0f); }
    if (active) { g.setColour(theme::accent); g.fillRoundedRectangle(r.getX() + 4, r.getBottom() - 3, r.getWidth() - 8, 2.0f, 1.0f); }
    g.setColour(active ? theme::accent : theme::text.withAlpha(0.85f));
    const auto c = r.getCentre().translated(0, -1);
    const juce::Graphics::ScopedSaveState save(g);
    g.addTransform(juce::AffineTransform::translation(c.x, c.y)); // icons in a -8 .. 8 box
    juce::Path p;
    const juce::PathStrokeType st(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    switch (tool)
    {
        case Tool::Draw: // pencil
            p.startNewSubPath(-7, 7); p.lineTo(-6, 3); p.lineTo(3, -6); p.lineTo(6, -3); p.lineTo(-3, 6); p.closeSubPath();
            p.startNewSubPath(0.5f, -3.5f); p.lineTo(3.5f, -0.5f);
            g.strokePath(p, st); break;
        case Tool::Paint: // brush
            p.startNewSubPath(6, -7); p.lineTo(-1, 1); g.strokePath(p, st);
            p.clear(); p.startNewSubPath(-1, 0); p.quadraticTo(-6, 0, -7, 7); p.quadraticTo(0, 7, 1, 2); p.closeSubPath();
            g.fillPath(p); break;
        case Tool::StepPaint: // step blocks
            for (int i = 0; i < 4; ++i) g.fillRect(-7.5f + 4.0f * (float) i, i % 2 ? -1.0f : -5.0f, 3.0f, 9.0f);
            break;
        case Tool::Delete:
            g.drawEllipse(-6.5f, -6.5f, 13, 13, 1.6f); g.drawLine(-4.4f, -4.4f, 4.4f, 4.4f, 1.6f); break;
        case Tool::Mute: // speaker + cross
            p.startNewSubPath(-7, -2.5f); p.lineTo(-4, -2.5f); p.lineTo(0, -6); p.lineTo(0, 6); p.lineTo(-4, 2.5f); p.lineTo(-7, 2.5f); p.closeSubPath();
            g.fillPath(p); g.drawLine(2.5f, -3, 7.5f, 3, 1.6f); g.drawLine(7.5f, -3, 2.5f, 3, 1.6f); break;
        case Tool::Slice: // blade through a note
            g.drawRect(-7.5f, -3.0f, 6.0f, 6.0f, 1.4f); g.drawRect(1.5f, -3.0f, 6.0f, 6.0f, 1.4f); g.drawLine(1.5f, -7.5f, -1.5f, 7.5f, 1.6f); break;
        case Tool::Select:
        {
            const float d[2] = { 3.0f, 2.0f };
            for (auto l : { juce::Line<float>(-7, -6, 7, -6), { 7, -6, 7, 6 }, { 7, 6, -7, 6 }, { -7, 6, -7, -6 } }) g.drawDashedLine(l, d, 2, 1.5f);
            break;
        }
        case Tool::Zoom:
            g.drawEllipse(-7, -7, 10, 10, 1.6f); g.drawLine(2, 2, 7, 7, 2.2f); g.drawLine(-4.5f, -2, 0.5f, -2, 1.3f); g.drawLine(-2, -4.5f, -2, 0.5f, 1.3f); break;
        case Tool::Play:
            p.addTriangle(-5, -6.5f, -5, 6.5f, 6.5f, 0); g.fillPath(p); break;
    }
}

// =============================================================================================
// LaneCanvas: geometry (4 source lanes, overlapping notes stacked in sub-rows)
// =============================================================================================

LaneCanvas::LaneCanvas(PatternView& o) : owner(o)
{
    setWantsKeyboardFocus(false);
    for (int lane = 0; lane < kNumSources; ++lane)
        for (int k = 0; k < kLaneKnobs; ++k)
        {
            static constexpr MixKnob::Kind kinds[kLaneKnobs] = { MixKnob::Kind::Volume, MixKnob::Kind::Pan, MixKnob::Kind::LowCut, MixKnob::Kind::HighCut };
            auto knob = std::make_unique<MixKnob>(kinds[k]);
            auto* raw = knob.get();
            raw->onGestureStart = [this] { owner.proc.project.beginGesture("SRC mix"); };
            raw->onGestureEnd = [this] { owner.proc.project.endGesture(); };
            raw->onChange = [this, lane, k](float v)
            {
                static const char* names[kLaneKnobs] = { " volume", " pan", " low cut", " high cut" };
                owner.proc.project.edit("SRC " + juce::String(lane + 1) + names[k], [&](ProjectState& s)
                {
                    auto& src = s.sources[(size_t) lane];
                    if (k == 0) src.gainDb = v; else if (k == 1) src.pan = v; else if (k == 2) src.lowCutHz = v; else src.highCutHz = v;
                });
            };
            addAndMakeVisible(raw);
            laneKnobs[(size_t) lane][(size_t) k] = std::move(knob);
        }
}

void LaneCanvas::layoutKnobs()
{
    const auto rs = rows();
    const auto area = notesArea();
    const bool lanes = !owner.proc.ui.padRows;
    for (int lane = 0; lane < kNumSources; ++lane)
    {
        const bool has = lanes && lane < (int) rs.size();
        const auto r = has ? juce::Rectangle<int>(0, rs[(size_t) lane].y, getWidth(), rs[(size_t) lane].h) : juce::Rectangle<int>();
        const auto well = juce::Rectangle<int>(10, r.getY() + 31, kLabelW - 16, 36);
        const bool fits = has && r.getHeight() >= 70 && well.getY() >= area.getY() && well.getBottom() <= area.getBottom();
        auto row = well;
        for (int k = 0; k < kLaneKnobs; ++k)
        {
            auto& knob = *laneKnobs[(size_t) lane][(size_t) k];
            knob.setVisible(fits);
            knob.setBounds(row.removeFromLeft(row.getWidth() / (kLaneKnobs - k)).withSizeKeepingCentre(33, 36));
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
        k[2]->setValueQuietly(s.lowCutHz);
        k[3]->setValueQuietly(s.highCutHz);
        for (auto& knob : k) { knob->setAccent(s.colour); knob->setEnabled(!s.empty()); }
    }
    layoutKnobs();
}

juce::Rectangle<int> LaneCanvas::velocityArea() const
{
    return owner.proc.ui.showVelocity ? getLocalBounds().removeFromBottom(kVelH) : getLocalBounds().removeFromBottom(0);
}

juce::Rectangle<int> LaneCanvas::notesArea() const
{
    const bool guide = owner.proc.ui.showGuide && owner.proc.guide.present;
    return getLocalBounds().withTrimmedTop(kRulerH + (guide ? kGuideH : 0)).withTrimmedBottom(velocityArea().getHeight());
}

std::vector<LaneCanvas::Row> LaneCanvas::rows() const
{
    const auto& st = owner.proc.project.state();
    const auto& ui = owner.proc.ui;
    const auto area = notesArea();
    std::vector<Row> out;
    for (int src = 0; src < kNumSources; ++src)
    {
        if (!ui.padRows) { out.push_back({ src, -1, 0, 0 }); continue; }
        bool any = false;
        for (int p = 0; p < kNumPads; ++p)
            if (st.pads[(size_t) p].assigned && st.pads[(size_t) p].src == src) { out.push_back({ src, p, 0, 0 }); any = true; }
        if (!any && st.sources[(size_t) src].empty() == false) out.push_back({ src, -1, 0, 0 });
    }
    if (out.empty()) out.push_back({ 0, -1, 0, 0 });
    const double base = std::max(ui.padRows ? 20.0 : 24.0, std::min((double) area.getHeight() / (double) out.size(), ui.padRows ? 34.0 : 4000.0));
    const double h = base * ui.mainVZoom;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const int y0 = (int) std::floor(h * (double) i), y1 = (int) std::floor(h * (double) (i + 1));
        out[i].y = area.getY() + y0 - ui.mainScrollY;
        out[i].h = y1 - y0;
    }
    return out;
}

int LaneCanvas::contentHeight() const
{
    const auto rs = rows();
    return rs.back().y + rs.back().h - rs.front().y;
}

const LaneCanvas::Row* LaneCanvas::rowAt(const std::vector<Row>& rs, int y) const
{
    const auto area = notesArea();
    if (y < area.getY() || y >= area.getBottom()) return nullptr;
    for (const auto& r : rs) if (y >= r.y && y < r.y + r.h) return &r;
    return nullptr;
}

int LaneCanvas::padForRow(const Row& r) const { return r.pad >= 0 ? r.pad : owner.lanePad(r.src); }

double LaneCanvas::visibleTicks() const
{
    const auto& ui = owner.proc.ui;
    return ui.mainTicks > 0 ? std::min(ui.mainTicks, (double) owner.loopTicks()) : (double) owner.loopTicks();
}

double LaneCanvas::tickToX(double t) const
{
    return kLabelW + (t - owner.proc.ui.mainStart) / visibleTicks() * (getWidth() - kLabelW);
}

double LaneCanvas::xToTick(double x) const
{
    return owner.proc.ui.mainStart + (x - kLabelW) / std::max(1, getWidth() - kLabelW) * visibleTicks();
}

void LaneCanvas::clampView()
{
    auto& ui = owner.proc.ui;
    const double loop = (double) owner.loopTicks();
    if (ui.mainTicks >= loop) ui.mainTicks = 0.0;
    ui.mainStart = juce::jlimit(0.0, std::max(0.0, loop - visibleTicks()), ui.mainStart);
    ui.mainVZoom = juce::jlimit(1.0, 8.0, ui.mainVZoom);
    ui.mainScrollY = juce::jlimit(0, std::max(0, contentHeight() - notesArea().getHeight()), ui.mainScrollY);
    layoutKnobs();
    repaint();
}

void LaneCanvas::showTicks(double start, double len)
{
    owner.proc.ui.mainTicks = juce::jlimit((double) kPPQ / 2, (double) owner.loopTicks(), len);
    owner.proc.ui.mainStart = start;
    clampView();
}

void LaneCanvas::zoomTime(double factor, int anchorX)
{
    const double anchor = xToTick(anchorX);
    const double vt = juce::jlimit((double) kPPQ / 2, (double) owner.loopTicks(), visibleTicks() / factor);
    owner.proc.ui.mainStart = anchor - (anchorX - kLabelW) / (double) std::max(1, getWidth() - kLabelW) * vt;
    owner.proc.ui.mainTicks = vt;
    clampView();
}

void LaneCanvas::zoomRows(double factor, int anchorY)
{
    auto& ui = owner.proc.ui;
    const double rel = (anchorY - notesArea().getY() + ui.mainScrollY) / std::max(1.0, (double) contentHeight());
    ui.mainVZoom = juce::jlimit(1.0, 8.0, ui.mainVZoom * factor);
    ui.mainScrollY = (int) (rel * contentHeight()) - (anchorY - notesArea().getY());
    clampView();
}

void LaneCanvas::fitAll()
{
    auto& ui = owner.proc.ui;
    ui.mainTicks = 0.0; ui.mainStart = 0.0; ui.mainVZoom = 1.0; ui.mainScrollY = 0;
    clampView();
}

void LaneCanvas::fitSelection()
{
    int64_t lo = INT64_MAX, hi = 0;
    for (const auto& e : owner.proc.project.state().events)
        if (owner.isSelected(e.id)) { lo = std::min(lo, e.tick); hi = std::max(hi, e.tick + e.length); }
    if (hi <= lo) { fitAll(); return; }
    const double pad = (double) (hi - lo) * 0.1;
    showTicks((double) lo - pad, (double) (hi - lo) + 2 * pad);
}

void LaneCanvas::followPlayhead()
{
    auto& ui = owner.proc.ui;
    if (!ui.followPlayhead || ui.mainTicks <= 0 || isDragging()) return;
    const double t = owner.proc.engine.playheadTick.load(), vt = visibleTicks();
    if (t < ui.mainStart || t >= ui.mainStart + vt) { ui.mainStart = t; clampView(); }
}

void LaneCanvas::repaintPlayhead()
{
    const float x = owner.proc.engine.transportPlaying.load() ? (float) tickToX(owner.proc.engine.playheadTick.load()) : -100.0f;
    if (std::abs(x - lastPlayheadX) < 0.5f) return;
    if (lastPlayheadX > -50.0f) repaint((int) lastPlayheadX - 3, 0, 7, getHeight());
    if (x > -50.0f) repaint((int) x - 3, 0, 7, getHeight());
    repaint(0, 0, getWidth(), kNavH); // the playhead in the overview
    lastPlayheadX = x;
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
    const bool padRows = owner.proc.ui.padRows;
    std::vector<Placed> out;
    for (const auto& row : rows())
    {
        std::vector<int> idx;
        for (int i = 0; i < (int) st.events.size(); ++i)
        {
            const auto& e = st.events[(size_t) i];
            const auto& p = st.pads[(size_t) e.pad];
            if (p.assigned && (padRows ? e.pad == row.pad : p.src == row.src)) idx.push_back(i);
        }
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return st.events[(size_t) a].tick < st.events[(size_t) b].tick; });
        std::vector<double> subEnd;
        std::vector<int> sub(idx.size(), 0);
        if (!padRows) // a lane holds many pads: overlapping notes are stacked; a pad row never needs it
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
        const float top = (float) row.y + (padRows ? 1.0f : 3.0f);
        const float sh = ((float) row.h - (padRows ? 2.0f : 6.0f)) / (float) nsub;
        for (size_t k = 0; k < idx.size(); ++k)
        {
            const auto& e = st.events[(size_t) idx[k]];
            const float y = top + (float) (sub[k] % nsub) * sh + 1;
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

void LaneCanvas::drawNotes(juce::Graphics& g)
{
    const auto& st = owner.proc.project.state();
    for (const auto& p : layout())
    {
        if (p.trig.getX() > (float) getWidth() || std::max(p.tail.getRight(), p.trig.getRight()) < (float) kLabelW) continue;
        const auto& e = st.events[(size_t) p.index];
        const auto& pad = st.pads[(size_t) e.pad];
        const auto col = e.muted ? theme::textDim.darker(0.6f) : st.sources[(size_t) pad.src].colour;
        const bool sel = owner.isSelected(e.id);
        if (!e.muted)
        {
            g.setColour(col.withAlpha(0.16f)); // audible tail
            g.fillRoundedRectangle(p.tail, 3.0f);
        }
        g.setColour(e.muted ? col.withAlpha(0.3f) : sel ? col.brighter(0.3f) : col.withAlpha(0.35f + 0.6f * e.velocity));
        g.fillRoundedRectangle(p.trig, 3.0f);
        g.setColour(sel ? juce::Colours::white : col.brighter(0.4f));
        g.drawRoundedRectangle(p.trig, 3.0f, sel ? 2.0f : 1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.35f)); // resize grip on the right end
        g.fillRect(p.trig.getRight() - 3.0f, p.trig.getY() + 3.0f, 2.0f, p.trig.getHeight() - 6.0f);
        if (p.trig.getWidth() > 26 && p.trig.getHeight() > 12)
        {
            g.setColour(e.muted ? theme::textDim : theme::screen);
            g.setFont(theme::mono(10.5f, true));
            g.drawText(padLabel(e.pad), p.trig.reduced(3, 0).toNearestInt(), juce::Justification::centredLeft);
        }
        if (e.rawTick >= 0 && e.rawTick != e.tick) // quantized recording: where it was played
        {
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawVerticalLine((int) tickToX((double) e.rawTick), p.trig.getY(), p.trig.getY() + 4);
        }
    }
}

void LaneCanvas::drawVelocity(juce::Graphics& g)
{
    const auto va = velocityArea();
    if (va.isEmpty()) return;
    const auto& st = owner.proc.project.state();
    g.setColour(theme::body);
    g.fillRect(va);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawHorizontalLine(va.getY(), 0.0f, (float) getWidth());
    g.setColour(theme::bodyLight);
    g.fillRect(va.withWidth(kLabelW).withTrimmedTop(1));
    g.setColour(theme::textDim);
    g.setFont(theme::ui(10.5f, true));
    g.drawText("VELOCITY", va.withWidth(kLabelW).reduced(8, 4), juce::Justification::topLeft);
    const juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(va.withTrimmedLeft(kLabelW));
    const float base = (float) va.getBottom() - 4.0f, full = (float) va.getHeight() - 12.0f;
    for (int pass = 0; pass < 2; ++pass) // selected notes on top
        for (const auto& e : st.events)
        {
            const auto& pad = st.pads[(size_t) e.pad];
            const bool sel = owner.isSelected(e.id);
            if (!pad.assigned || sel != (pass == 1)) continue;
            const float x = (float) tickToX((double) e.tick), y = base - full * e.velocity;
            const auto col = e.muted ? theme::textDim.darker(0.4f) : st.sources[(size_t) pad.src].colour;
            g.setColour(sel ? juce::Colours::white : col);
            g.fillRect(x, y, 2.0f, base - y);
            g.fillEllipse(x - 2.5f, y - 3.0f, 7.0f, 7.0f);
            const float x1 = (float) tickToX((double) (e.tick + e.length));
            g.setColour(col.withAlpha(0.4f));
            g.drawLine(x + 3.0f, y, std::max(x + 6.0f, x1), y, 1.0f);
        }
}

// overview of the whole pattern (top strip): drag = move the visible part
void LaneCanvas::drawNav(juce::Graphics& g)
{
    const auto& st = owner.proc.project.state();
    const double loop = (double) owner.loopTicks();
    const auto r = juce::Rectangle<int>(kLabelW, 0, getWidth() - kLabelW, kNavH).toFloat();
    g.setColour(theme::screen);
    g.fillRect(0.0f, 0.0f, (float) getWidth(), (float) kNavH);
    auto xOf = [&](double t) { return r.getX() + (float) (t / loop) * r.getWidth(); };
    for (const auto& e : st.events)
    {
        const auto& pad = st.pads[(size_t) e.pad];
        if (!pad.assigned) continue;
        g.setColour((e.muted ? theme::textDim : st.sources[(size_t) pad.src].colour).withAlpha(0.9f));
        g.fillRect(xOf((double) e.tick), 1.0f + (float) pad.src * 3.0f, std::max(2.0f, (float) ((double) e.length / loop) * r.getWidth()), 2.0f);
    }
    const auto win = juce::Rectangle<float>(xOf(owner.proc.ui.mainStart), 0.5f, (float) (visibleTicks() / loop) * r.getWidth(), kNavH - 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.fillRect(win);
    g.setColour(theme::wave.withAlpha(0.8f));
    g.drawRect(win, 1.0f);
    if (owner.proc.engine.transportPlaying.load())
    {
        g.setColour(theme::ok);
        g.fillRect(xOf(owner.proc.engine.playheadTick.load()), 0.0f, 1.5f, (float) kNavH);
    }
}

void LaneCanvas::paint(juce::Graphics& g)
{
    const auto& st = owner.proc.project.state();
    g.fillAll(theme::screen);
    const int64_t loop = owner.loopTicks();
    const bool guide = owner.proc.ui.showGuide && owner.proc.guide.present;
    const auto area = notesArea();
    const bool padRows = owner.proc.ui.padRows;

    // rows + labels
    {
        const juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(area);
        int i = 0, prevSrc = -1;
        for (const auto& row : rows())
        {
            const auto r = juce::Rectangle<int>(0, row.y, getWidth(), row.h);
            const auto& s = st.sources[(size_t) row.src];
            g.setColour(i++ % 2 ? theme::screen : theme::screen.brighter(0.03f));
            g.fillRect(r.withTrimmedLeft(kLabelW));
            auto lab = r.withWidth(kLabelW);
            g.setColour(theme::bodyLight);
            g.fillRect(lab);
            g.setColour(s.colour);
            g.fillRect(lab.removeFromLeft(5));
            if (padRows && row.pad >= 0)
            {
                const bool sounding = owner.proc.engine.padVoices[(size_t) row.pad].load() > 0;
                const bool current = owner.proc.ui.selectedPad == row.pad;
                if (sounding || current) { g.setColour(s.colour.withAlpha(sounding ? 0.45f : 0.16f)); g.fillRect(lab); }
                auto txt = lab.reduced(6, 0);
                g.setColour(s.colour);
                g.setFont(theme::mono(12.0f, true));
                g.drawText(padLabel(row.pad), txt.removeFromLeft(34), juce::Justification::centredLeft);
                g.setColour(theme::text);
                g.setFont(theme::ui(11.0f));
                g.drawFittedText(st.pads[(size_t) row.pad].name, txt, juce::Justification::centredLeft, 1, 0.8f);
            }
            else
            {
                auto txt = lab.reduced(6, 4);
                g.setColour(s.colour);
                g.setFont(theme::mono(12.0f, true));
                g.drawText("SRC " + juce::String(row.src + 1) + (padRows ? "  no pads" : ""), txt.removeFromTop(14), juce::Justification::topLeft);
                if (laneHasNotes(row.src))
                    for (int which = 0; which < 2; ++which)
                    {
                        const auto ic = laneIcon(row, which).toFloat();
                        const bool hot = labelSrc == row.src && labelIcon == which;
                        g.setColour(hot ? s.colour.withAlpha(0.45f) : theme::screen.withAlpha(0.7f));
                        g.fillRoundedRectangle(ic, 3.0f);
                        g.setColour(s.colour.withAlpha(0.9f));
                        g.drawRoundedRectangle(ic.reduced(0.5f), 3.0f, 1.0f);
                        const auto in = ic.reduced(5.0f, 3.5f);
                        if (which == 0) // a small waveform: drag the lane as WAV
                        {
                            static constexpr float h[] = { 0.3f, 0.8f, 0.5f, 1.0f, 0.6f, 0.9f, 0.35f, 0.7f, 0.25f };
                            const int n = (int) (sizeof(h) / sizeof(h[0]));
                            for (int k = 0; k < n; ++k)
                            {
                                const float x = in.getX() + in.getWidth() * (k + 0.5f) / (float) n, a = in.getHeight() * 0.5f * h[k];
                                g.drawLine(x, in.getCentreY() - a, x, in.getCentreY() + a, 1.2f);
                            }
                        }
                        else // save: an arrow into a tray
                        {
                            const float cx = in.getCentreX();
                            g.drawLine(cx, in.getY(), cx, in.getBottom() - 3.0f, 1.4f);
                            juce::Path arrow;
                            arrow.addTriangle(cx - 3.5f, in.getBottom() - 5.5f, cx + 3.5f, in.getBottom() - 5.5f, cx, in.getBottom() - 1.5f);
                            g.fillPath(arrow);
                            g.drawLine(in.getX() + 1.0f, in.getBottom(), in.getRight() - 1.0f, in.getBottom(), 1.4f);
                        }
                    }
                g.setFont(theme::ui(11.0f));
                g.setColour(s.empty() ? theme::textDim : theme::text);
                if (!padRows)
                    g.drawFittedText(s.empty() ? "drop audio here" : s.name, txt.removeFromTop(13), juce::Justification::topLeft, 1, 0.8f);
                if (!padRows && r.getHeight() >= 70) // knob well behind VOL / PAN / FILTER
                {
                    g.setColour(theme::screen.withAlpha(0.55f));
                    g.fillRoundedRectangle(juce::Rectangle<float>(8.0f, (float) r.getY() + 30.0f, (float) kLabelW - 14.0f, 38.0f), 5.0f);
                }
                const int pad = owner.lanePad(row.src);
                if (!padRows && pad >= 0 && r.getHeight() >= 84)
                {
                    g.setColour(s.colour);
                    g.setFont(theme::mono(10.5f, true));
                    g.drawText("click places " + padLabel(pad), r.withWidth(kLabelW).reduced(11, 4).removeFromBottom(13), juce::Justification::bottomLeft);
                }
            }
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.drawHorizontalLine(r.getBottom() - 1, 0.0f, (float) getWidth());
            if (padRows && prevSrc >= 0 && prevSrc != row.src)
            {
                g.setColour(s.colour.withAlpha(0.5f));
                g.drawHorizontalLine(r.getY(), 0.0f, (float) getWidth());
            }
            prevSrc = row.src;
        }
    }

    // grid + notes
    const int sub = gridTicks(st.settings.grid);
    const double pxPerTick = (getWidth() - kLabelW) / visibleTicks();
    const auto top = (float) kRulerH, bottom = (float) getHeight();
    {
        const juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(area.withTrimmedLeft(kLabelW));
        for (int64_t b = 0; b < loop; b += kPPQ) // every second beat is a little lighter: easier to read when zoomed in
            if ((b / kPPQ) % 2 == 0)
            {
                const float x0 = (float) tickToX((double) b), x1 = (float) tickToX((double) (b + kPPQ));
                if (x1 < kLabelW || x0 > getWidth()) continue;
                g.setColour(juce::Colours::white.withAlpha(0.018f));
                g.fillRect(x0, top, x1 - x0, bottom - top);
            }
        const int step = sub > 0 && pxPerTick * sub >= 5 ? sub : (pxPerTick * kPPQ >= 5 ? kPPQ : kTicksPerBar);
        for (int64_t t = 0; t <= loop; t += step)
        {
            const float x = (float) tickToX((double) t);
            if (x < kLabelW - 1 || x > getWidth()) continue;
            const bool bar = t % kTicksPerBar == 0, beat = t % kPPQ == 0;
            g.setColour(juce::Colours::white.withAlpha(bar ? 0.28f : beat ? 0.14f : 0.06f));
            g.drawVerticalLine((int) x, top, bottom);
        }
        drawNotes(g);
    }
    drawVelocity(g);

    // overview + ruler + paste marker
    g.setColour(theme::bodyLight);
    g.fillRect(0, 0, getWidth(), kRulerH);
    drawNav(g);
    {
        const juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(juce::Rectangle<int>(kLabelW - 5, kNavH, getWidth() - kLabelW + 5, kRulerH - kNavH));
        for (int64_t t = 0; t < loop; t += kPPQ)
        {
            const float x = (float) tickToX((double) t);
            if (x < kLabelW - 1 || x > getWidth()) continue;
            const bool bar = t % kTicksPerBar == 0;
            g.setColour(bar ? theme::text : theme::textDim);
            g.drawVerticalLine((int) x, (float) kNavH + (bar ? 4.0f : 16.0f), (float) kRulerH);
            if (bar) { g.setFont(theme::mono(11.0f, true)); g.drawText(juce::String(t / kTicksPerBar + 1), (int) x + 3, kNavH + 2, 30, 14, juce::Justification::centredLeft); }
            else if (pxPerTick * kPPQ > 26) { g.setFont(theme::mono(9.5f)); g.drawText(juce::String((t % kTicksPerBar) / kPPQ + 1), (int) x + 2, kNavH + 12, 20, 12, juce::Justification::centredLeft); }
        }
        const float cx = (float) tickToX((double) owner.proc.ui.mainCursorTick);
        juce::Path p;
        p.addTriangle(cx - 5, (float) kRulerH - 8, cx + 5, (float) kRulerH - 8, cx, (float) kRulerH);
        g.setColour(theme::accent.withAlpha(0.85f));
        g.fillPath(p);
    }
    g.setColour(theme::textDim);
    g.setFont(theme::ui(11.0f));
    g.drawText(juce::String(st.settings.bpm, 1) + " BPM  " + st.settings.key.label(), juce::Rectangle<int>(0, kNavH, kLabelW, kRulerH - kNavH).reduced(6, 0), juce::Justification::centredLeft);

    // drum guide overlay (not a lane)
    if (guide)
    {
        auto gr = juce::Rectangle<int>(kLabelW, kRulerH, getWidth() - kLabelW, kGuideH);
        g.setColour(theme::body);
        g.fillRect(gr);
        g.setColour(theme::textDim);
        g.setFont(theme::ui(10.0f, true));
        g.drawText("DRUM GUIDE", juce::Rectangle<int>(0, kRulerH, kLabelW, kGuideH).reduced(6, 0), juce::Justification::centredLeft);
        const juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(juce::Rectangle<int>(kLabelW, kRulerH, getWidth() - kLabelW, area.getBottom() - kRulerH));
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

    // vertical scroll position
    if (const int ch = contentHeight(); ch > area.getHeight())
    {
        const float k = (float) area.getHeight() / (float) ch;
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.fillRoundedRectangle((float) getWidth() - 6.0f, (float) area.getY() + (float) owner.proc.ui.mainScrollY * k, 4.0f, (float) area.getHeight() * k, 2.0f);
    }

    if (owner.proc.engine.transportPlaying.load())
    {
        const float x = (float) tickToX(owner.proc.engine.playheadTick.load());
        const bool rec = owner.proc.isRecordArmed() || owner.proc.engine.hostRecording.load();
        g.setColour(rec ? theme::rec : owner.proc.engine.hostDriven.load() ? theme::ok : theme::accent);
        if (x >= kLabelW && x <= (float) getWidth()) g.fillRect(x - 1.0f, (float) kNavH, 2.0f, (float) getHeight());
    }
    if (dropX >= 0)
    {
        g.setColour(theme::accent);
        g.drawVerticalLine((int) tickToX((double) snapTick(xToTick(dropX), true)), (float) kRulerH, bottom);
    }
    if (drag == Drag::Marquee || drag == Drag::ZoomBox)
    {
        g.setColour((drag == Drag::ZoomBox ? theme::wave : juce::Colours::white).withAlpha(0.12f));
        g.fillRect(band);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.drawRect(band);
    }
    if (drag == Drag::Slice)
    {
        g.setColour(theme::rec);
        g.drawLine(juce::Line<float>(downPos.toFloat(), dragPos.toFloat()), 1.5f);
    }
    if (st.events.empty())
    {
        g.setColour(theme::textDim);
        g.setFont(theme::ui(14.0f));
        g.drawFittedText("Click a row to place its pad, right-click deletes, Ctrl+drag selects - or press REC and play",
                         area.withTrimmedLeft(kLabelW).reduced(20), juce::Justification::centred, 2);
    }
    if (const int beat = owner.proc.engine.countInBeat.load(); beat > 0) // 1-2-3-4 before recording
    {
        const auto c = area.withTrimmedLeft(kLabelW).withSizeKeepingCentre(280, 150).toFloat();
        g.setColour(theme::body.withAlpha(0.92f));
        g.fillRoundedRectangle(c, 12.0f);
        g.setColour(theme::rec);
        g.drawRoundedRectangle(c, 12.0f, 2.0f);
        for (int b = 1; b <= 4; ++b)
        {
            g.setColour(b == beat ? theme::rec : b < beat ? theme::text : theme::textDim.withAlpha(0.35f));
            g.setFont(theme::mono(b == beat ? 76.0f : 40.0f, true));
            g.drawText(juce::String(b), c.withTrimmedLeft(20.0f + (float) (b - 1) * 60.0f).withWidth(60.0f).toNearestInt(), juce::Justification::centred);
        }
    }
}

// =============================================================================================
// LaneCanvas: tools
// =============================================================================================

void LaneCanvas::mouseMove(const juce::MouseEvent& e)
{
    const auto tool = owner.proc.ui.tool;
    auto cursor = juce::MouseCursor::NormalCursor;
    if (e.y < kNavH && e.x > kLabelW) cursor = juce::MouseCursor::DraggingHandCursor;
    else if (velocityArea().contains(e.getPosition()) && e.x > kLabelW) cursor = juce::MouseCursor::UpDownResizeCursor;
    else if (e.x > kLabelW && notesArea().contains(e.getPosition()))
    {
        bool edge = false;
        const int ev = eventAt(e.getPosition(), edge);
        const bool edits = tool == Tool::Draw || tool == Tool::Select || tool == Tool::Paint;
        if (e.mods.isCtrlDown() || tool == Tool::Zoom || tool == Tool::Slice) cursor = juce::MouseCursor::CrosshairCursor;
        else if (edits && ev >= 0) cursor = edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
        else if (tool != Tool::Select) cursor = juce::MouseCursor::CrosshairCursor;
    }
    setMouseCursor(cursor);
}

void LaneCanvas::eraseAt(juce::Point<int> p)
{
    bool edge = false;
    const int ev = eventAt(p, edge);
    if (ev < 0) return;
    const auto id = owner.proc.project.state().events[(size_t) ev].id;
    owner.proc.project.edit("Delete notes", [&](ProjectState& s)
    {
        s.events.erase(std::remove_if(s.events.begin(), s.events.end(), [&](const PatternEvent& x) { return x.id == id; }), s.events.end());
    });
}

void LaneCanvas::muteAt(juce::Point<int> p)
{
    bool edge = false;
    const int ev = eventAt(p, edge);
    if (ev < 0 || owner.proc.project.state().events[(size_t) ev].muted == muteTo) return;
    const auto id = owner.proc.project.state().events[(size_t) ev].id;
    owner.proc.project.edit("Mute notes", [&](ProjectState& s) { for (auto& x : s.events) if (x.id == id) x.muted = muteTo; });
}

void LaneCanvas::addNote(int pad, int64_t tick, int64_t length)
{
    PatternEvent n;
    n.id = newId();
    n.pad = pad;
    n.tick = tick;
    n.length = std::max<int64_t>(kPPQ / 32, length);
    n.velocity = owner.lastVelocity;
    owner.proc.project.edit("Add notes", [&](ProjectState& s) { s.events.push_back(n); });
}

// Paint: notes of the current length one after another; Step paint: one per grid step (or erases them)
juce::Rectangle<int> LaneCanvas::laneIcon(const Row& r, int which) const
{
    return { kLabelW - 6 - 28 * (2 - which), r.y + 5, 25, 17 };
}

bool LaneCanvas::laneHasNotes(int src) const
{
    const auto& st = owner.proc.project.state();
    for (const auto& e : st.events)
        if (e.pad >= 0 && e.pad < kNumPads && st.pads[(size_t) e.pad].src == src) return true;
    return false;
}

void LaneCanvas::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.x >= kLabelW || e.y < kRulerH) return;
    const auto rs = rows();
    const Row* row = rowAt(rs, e.y);
    if (row == nullptr || row->pad >= 0) return;
    for (int which = 0; which < 2; ++which)
        if (laneHasNotes(row->src) && laneIcon(*row, which).contains(e.getPosition())) return;
    if (owner.onShowSource) owner.onShowSource(row->src);
}

void LaneCanvas::paintNotesTo(int x)
{
    const int64_t loop = owner.loopTicks(), g = owner.gridStep();
    const bool stepMode = drag == Drag::Step;
    const int64_t step = stepMode ? g : std::max<int64_t>(g, (owner.lastLength + g - 1) / g * g);
    const int64_t at = juce::jlimit<int64_t>(0, loop - 1, (int64_t) std::floor(xToTick(x) / (double) g) * g);
    auto& project = owner.proc.project;
    auto cell = [&](int64_t t)
    {
        if (t < 0 || t >= loop) return;
        auto inCell = [&](const PatternEvent& e) { return e.pad == paintPad && e.tick >= t && e.tick < t + step; };
        const bool any = std::any_of(project.state().events.begin(), project.state().events.end(), inCell);
        if (stepMode && stepErase)
        {
            if (any) project.edit("Step paint", [&](ProjectState& s) { s.events.erase(std::remove_if(s.events.begin(), s.events.end(), inCell), s.events.end()); });
        }
        else if (!any) addNote(paintPad, t, stepMode ? g : owner.lastLength);
    };
    if (paintLast < 0) { cell(at); paintLast = at; return; }
    while (at >= paintLast + step) { paintLast += step; cell(paintLast); }
    while (at <= paintLast - step) { paintLast -= step; cell(paintLast); }
}

void LaneCanvas::setVelocityAt(juce::Point<int> p)
{
    const auto va = velocityArea();
    const float base = (float) va.getBottom() - 4.0f, full = (float) va.getHeight() - 12.0f;
    const auto& st = owner.proc.project.state();
    auto velAt = [&](float y) { return juce::jlimit(0.05f, 1.0f, (base - y) / full); };
    // every note between the last and the current mouse position gets a value on the drawn line
    const int x0 = std::min(lastPos.x, p.x) - 4, x1 = std::max(lastPos.x, p.x) + 4;
    const bool onlySel = !owner.proc.ui.selectedEvents.empty();
    std::vector<std::pair<juce::String, float>> set;
    for (const auto& e : st.events)
    {
        if (!st.pads[(size_t) e.pad].assigned || (onlySel && !owner.isSelected(e.id))) continue;
        const float x = (float) tickToX((double) e.tick);
        if (x < (float) x0 || x > (float) x1) continue;
        float y = (float) p.y;
        if (p.x != lastPos.x)
            y = (float) lastPos.y + (float) (p.y - lastPos.y) * juce::jlimit(0.0f, 1.0f, (x - (float) lastPos.x) / (float) (p.x - lastPos.x));
        set.emplace_back(e.id, velAt(y));
    }
    lastPos = p;
    if (set.empty()) return;
    owner.proc.project.edit("Velocity", [&](ProjectState& s)
    {
        for (auto& e : s.events) for (const auto& [id, vel] : set) if (e.id == id) e.velocity = vel;
    });
    owner.lastVelocity = set.back().second;
}

void LaneCanvas::dragNav(int x)
{
    const double t = (x - kLabelW) / (double) std::max(1, getWidth() - kLabelW) * (double) owner.loopTicks();
    owner.proc.ui.mainStart = t - visibleTicks() / 2;
    clampView();
}

void LaneCanvas::mouseDown(const juce::MouseEvent& e)
{
    owner.grabKeyboardFocus();
    if (drag != Drag::None) return; // a second button during a drag
    auto& ui = owner.proc.ui;
    auto& project = owner.proc.project;
    const auto& st = project.state();
    downPos = dragPos = lastPos = e.getPosition();

    if (e.mods.isMiddleButtonDown())
    {
        drag = Drag::Pan;
        panStart = ui.mainStart;
        panScroll = ui.mainScrollY;
        return;
    }
    if (e.y < kNavH) { if (e.x >= kLabelW) { drag = Drag::Nav; dragNav(e.x); } return; }
    if (e.y < kRulerH) // ruler: paste position
    {
        if (e.x >= kLabelW) owner.proc.ui.mainCursorTick = juce::jlimit<int64_t>(0, owner.loopTicks() - 1, snapTick(xToTick(e.x), true));
        repaint();
        return;
    }
    if (velocityArea().contains(e.getPosition()))
    {
        if (e.x < kLabelW) return;
        project.beginGesture("Velocity");
        drag = Drag::Velocity;
        setVelocityAt(e.getPosition());
        return;
    }
    const auto rs = rows();
    const Row* row = rowAt(rs, e.y);
    if (row == nullptr) return;
    if (e.x < kLabelW) // label: pad row = listen + choose, lane = open that source
    {
        if (row->pad >= 0)
        {
            owner.laneLastPad[(size_t) row->src] = row->pad;
            audition(row->pad);
            owner.refresh();
        }
        else if (laneHasNotes(row->src))
        {
            for (int which = 0; which < 2; ++which)
                if (laneIcon(*row, which).contains(e.getPosition()))
                {
                    labelSrc = row->src; labelIcon = which; labelDragged = false;
                    repaint(0, row->y, kLabelW, row->h);
                    return;
                }
        }
        // a click on the name stays in MAIN (maintainer 2026-11-26); a double-click opens the source
        return;
    }
    const double mouseTick = xToTick(e.x);
    owner.proc.ui.mainCursorTick = juce::jlimit<int64_t>(0, owner.loopTicks() - 1, snapTick(mouseTick, true));
    bool edge = false;
    const int evIndex = eventAt(e.getPosition(), edge);

    if (ui.tool == Tool::Zoom && !e.mods.isCtrlDown())
    {
        if (e.mods.isRightButtonDown()) { zoomTime(0.5, e.x); return; }
        drag = Drag::ZoomBox;
        band = { e.x, e.y, 0, 0 };
        return;
    }
    // RMB: delete, keep the button down to sweep-delete (Delete tool: the same with LMB)
    if (e.mods.isRightButtonDown() || (ui.tool == Tool::Delete && !e.mods.isCtrlDown()))
    {
        project.beginGesture("Delete notes");
        drag = Drag::Erase;
        eraseAt(e.getPosition());
        return;
    }
    // Ctrl+drag (or SELECT tool on empty space): marquee
    if (e.mods.isCtrlDown() || (ui.tool == Tool::Select && evIndex < 0))
    {
        drag = Drag::Marquee;
        bandAdd = e.mods.isShiftDown();
        if (!bandAdd) ui.selectedEvents.clear();
        band = { e.x, e.y, 0, 0 };
        owner.refresh();
        return;
    }
    if (ui.tool == Tool::Mute)
    {
        if (evIndex >= 0) muteTo = !st.events[(size_t) evIndex].muted;
        else muteTo = true;
        project.beginGesture("Mute notes");
        drag = Drag::Mute;
        muteAt(e.getPosition());
        return;
    }
    if (ui.tool == Tool::Slice) { drag = Drag::Slice; return; }
    if (ui.tool == Tool::Play)
    {
        if (owner.proc.engine.hostDriven.load()) { owner.proc.setStatus("The DAW is playing and leads MAIN - stop it in the DAW"); return; }
        drag = Drag::Playback;
        owner.proc.playMain(owner.proc.ui.mainCursorTick);
        return;
    }

    const int pad = padForRow(*row);
    if (ui.tool == Tool::StepPaint || (ui.tool == Tool::Paint && evIndex < 0))
    {
        if (pad < 0) { owner.proc.setStatus("No pad uses SRC " + juce::String(row->src + 1) + " yet - assign a chop from SRC " + juce::String(row->src + 1) + " first"); return; }
        paintPad = evIndex >= 0 ? st.events[(size_t) evIndex].pad : pad;
        paintLast = -1;
        stepErase = evIndex >= 0;
        ui.selectedEvents.clear();
        ui.selectedPad = paintPad;
        project.beginGesture(ui.tool == Tool::StepPaint ? "Step paint" : "Paint notes");
        drag = ui.tool == Tool::StepPaint ? Drag::Step : Drag::Paint;
        if (!stepErase) audition(paintPad);
        paintNotesTo(e.x);
        return;
    }

    downRow = (int) (row - rs.data());
    lastPadShift = 0;
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
            if (owner.isSelected(x.id)) orig.push_back({ x.id, x.tick, x.length, x.pad });
        anchorTick = ev.tick;
        anchorLen = ev.length;
        grabOffset = (int64_t) mouseTick - ev.tick;
        drag = edge ? Drag::Resize : Drag::Move;
        if (!edge) audition(ev.pad);
        owner.refresh();
        return;
    }

    // LMB on empty space: place the row's pad and keep dragging to move it
    if (pad < 0)
    {
        owner.proc.setStatus("No pad uses SRC " + juce::String(row->src + 1) + " yet - assign a chop from SRC " + juce::String(row->src + 1) + " first");
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
    owner.laneLastPad[(size_t) row->src] = pad;
    orig = { { n.id, n.tick, n.length, n.pad } };
    anchorTick = n.tick;
    anchorLen = n.length;
    grabOffset = (int64_t) mouseTick - n.tick;
    drag = Drag::Move;
    audition(pad);
    owner.refresh();
}

void LaneCanvas::mouseDrag(const juce::MouseEvent& e)
{
    if (labelIcon >= 0) // a lane's WAV / save icon: dragging takes the lane into the DAW
    {
        if (!labelDragged && e.getDistanceFromDragStart() > 6)
        {
            labelDragged = true;
            const int src = labelSrc;
            PatternView::startFileDrag(*this, [this, src] { return owner.laneDragFile(src); });
        }
        return;
    }
    auto& project = owner.proc.project;
    auto& ui = owner.proc.ui;
    const int64_t loop = owner.loopTicks();
    const auto prev = dragPos;
    dragPos = e.getPosition();
    // sweeps (erase / mute) also hit what a fast mouse jumps over
    auto sweep = [&](auto fn)
    {
        const int steps = juce::jlimit(1, 200, prev.getDistanceFrom(dragPos) / 4);
        for (int i = 1; i <= steps; ++i) fn(prev + (dragPos - prev) * ((float) i / (float) steps));
    };
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
        case Drag::ZoomBox:
            band = juce::Rectangle<int>(downPos, e.getPosition());
            repaint();
            return;
        case Drag::Slice: repaint(); return;
        case Drag::Playback: return;
        case Drag::Nav: dragNav(e.x); return;
        case Drag::Pan:
            ui.mainStart = panStart - (e.x - downPos.x) / (double) std::max(1, getWidth() - kLabelW) * visibleTicks();
            ui.mainScrollY = panScroll - (e.y - downPos.y);
            clampView();
            return;
        case Drag::Velocity: setVelocityAt(e.getPosition()); return;
        case Drag::Erase: sweep([this](juce::Point<int> p) { eraseAt(p); }); return;
        case Drag::Mute:  sweep([this](juce::Point<int> p) { muteAt(p); }); return;
        case Drag::Paint:
        case Drag::Step:  paintNotesTo(e.x); return;
        case Drag::Move:
        {
            int64_t delta = snapTick(xToTick(e.x) - (double) grabOffset) - anchorTick;
            int64_t lo = INT64_MAX, hi = 0;
            for (const auto& o : orig) { lo = std::min(lo, o.tick); hi = std::max(hi, o.tick); }
            if (!orig.empty()) delta = juce::jlimit(-lo, std::max<int64_t>(-lo, loop - 1 - hi), delta); // the group keeps its shape
            if (e.mods.isCtrlDown()) delta = 0; // vertical only
            // pad rows: up / down = another pad
            int shift = 0;
            const auto rs = rows();
            auto rowOf = [&](int pad) { for (int i = 0; i < (int) rs.size(); ++i) if (rs[(size_t) i].pad == pad) return i; return -1; };
            if (ui.padRows)
            {
                if (const Row* r = rowAt(rs, e.y)) shift = (int) (r - rs.data()) - downRow;
                else shift = lastPadShift;
                for (const auto& o : orig) // the whole group must land on pads
                {
                    const int ri = rowOf(o.pad) + shift;
                    if (ri < 0 || ri >= (int) rs.size() || rs[(size_t) ri].pad < 0) { shift = lastPadShift; break; }
                }
            }
            const bool padChanged = shift != lastPadShift;
            lastPadShift = shift;
            int newPad = -1;
            project.edit("Move notes", [&](ProjectState& s)
            {
                for (auto& x : s.events)
                    for (const auto& o : orig)
                        if (x.id == o.id)
                        {
                            const int64_t t = o.tick + delta;
                            if (t != x.tick) x.rawTick = -1;
                            x.tick = t;
                            if (ui.padRows)
                            {
                                x.pad = rs[(size_t) (rowOf(o.pad) + shift)].pad;
                                newPad = x.pad;
                            }
                        }
            });
            if (padChanged && newPad >= 0)
            {
                ui.selectedPad = newPad;
                audition(newPad);
                owner.refresh();
            }
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

void LaneCanvas::mouseUp(const juce::MouseEvent& e)
{
    if (labelIcon >= 0)
    {
        const int src = labelSrc;
        const bool dragged = labelDragged;
        labelSrc = labelIcon = -1;
        labelDragged = false;
        repaint(0, 0, kLabelW, getHeight());
        if (!dragged) owner.exportLaneWav(src); // a click: save as
        return;
    }
    auto& project = owner.proc.project;
    const auto was = drag;
    drag = Drag::None;
    switch (was)
    {
        case Drag::Move: case Drag::Resize: case Drag::Erase: case Drag::Paint:
        case Drag::Step: case Drag::Mute: case Drag::Velocity: project.endGesture(); break;
        case Drag::Playback: owner.proc.engine.setInternalPlaying(false); break;
        case Drag::ZoomBox:
            if (band.getWidth() < 6) zoomTime(2.0, e.x);
            else
            {
                const double t0 = xToTick(std::max(kLabelW, band.getX())), t1 = xToTick(band.getRight());
                if (band.getHeight() > 60) // a tall box also zooms the rows
                {
                    const double f = notesArea().getHeight() / (double) band.getHeight();
                    if (f > 1.15) zoomRows(f, band.getCentreY());
                }
                showTicks(t0, t1 - t0);
            }
            break;
        case Drag::Slice:
        {
            // a click cuts the note under it, a drag cuts every note the line crosses (at the snapped crossing)
            const auto placed = layout();
            const juce::Line<float> line(downPos.toFloat(), e.getPosition().toFloat());
            const bool click = line.getLength() < 4.0f;
            struct Cut { juce::String id; int64_t at; };
            std::vector<Cut> cuts;
            for (const auto& p : placed)
            {
                const auto& ev = project.state().events[(size_t) p.index];
                float x = (float) downPos.x;
                if (click) { if (!p.trig.contains(downPos.toFloat())) continue; }
                else
                {
                    const float cy = p.trig.getCentreY(), y0 = line.getStartY(), y1 = line.getEndY();
                    if (std::abs(y1 - y0) < 1.0f || (cy - y0) * (cy - y1) > 0.0f) continue;
                    x = line.getStartX() + (line.getEndX() - line.getStartX()) * (cy - y0) / (y1 - y0);
                    if (x <= p.trig.getX() || x >= p.trig.getRight()) continue;
                }
                const int64_t at = snapTick(xToTick(x));
                if (at > ev.tick && at < ev.tick + ev.length) cuts.push_back({ ev.id, at });
            }
            if (!cuts.empty())
                project.edit("Slice notes", [&](ProjectState& s)
                {
                    std::vector<PatternEvent> add;
                    for (auto& x : s.events)
                        for (const auto& c : cuts)
                            if (x.id == c.id)
                            {
                                auto right = x;
                                right.id = newId();
                                right.rawTick = -1;
                                right.tick = c.at;
                                right.length = x.tick + x.length - c.at;
                                x.length = c.at - x.tick;
                                add.push_back(right);
                                break;
                            }
                    s.events.insert(s.events.end(), add.begin(), add.end());
                });
            break;
        }
        default: break;
    }
    stopAudition();
    if (was == Drag::Marquee) owner.refresh();
    repaint();
}

void LaneCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto& ui = owner.proc.ui;
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
    const float d = std::abs(dy) > 0 ? dy : w.deltaX;
    if (e.mods.isCtrlDown() && e.mods.isShiftDown()) { zoomRows(std::pow(1.25, d * 4.0), e.y); return; }
    if (e.mods.isCtrlDown() || e.y < kRulerH) { zoomTime(std::pow(1.25, d * 4.0), std::max(kLabelW, e.x)); return; }
    const bool tall = contentHeight() > notesArea().getHeight();
    if (tall && !e.mods.isShiftDown() && std::abs(w.deltaX) < 0.001f) ui.mainScrollY -= (int) (dy * 120.0f);
    else ui.mainStart -= d * visibleTicks() * 0.25;
    clampView();
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
    const auto rs = rows();
    const auto* row = rowAt(rs, y);
    if (row != nullptr && !files.isEmpty()) owner.proc.loadSource(row->src, juce::File(files[0]));
}

// =============================================================================================
// PatternView
// =============================================================================================

PatternView::PatternView(HPSamplerProcessor& p) : proc(p)
{
    setWantsKeyboardFocus(true);
    addAndMakeVisible(lanes);
    for (auto* b : std::initializer_list<juce::TextButton*> { &patternTab, &resultTab, &helpBtn, &snapBtn, &rowsBtn, &velBtn, &followBtn, &zoomOutBtn, &zoomInBtn, &fitBtn, &rowOutBtn, &rowInBtn, &guideBtn, &quantBtn, &clearBtn, &wavBtn, &midiBtn, &renderBtn })
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
    static const std::pair<Tool, const char*> toolList[] = {
        { Tool::Draw, "Draw (P): click places a note, drag moves it, right end resizes, right-click deletes" },
        { Tool::Paint, "Paint (B): drag fills the row with notes of the current length" },
        { Tool::StepPaint, "Step paint (N): drag switches grid steps on; starting on a note switches them off" },
        { Tool::Delete, "Delete (D): click or drag over notes" },
        { Tool::Mute, "Mute (T): click or drag over notes; a muted note stays but does not sound" },
        { Tool::Slice, "Slice (C): click or drag across notes to cut them" },
        { Tool::Select, "Select (E): drag selects; Ctrl+drag selects with every tool" },
        { Tool::Zoom, "Zoom (Z): drag over a range, click = in, right-click = out" },
        { Tool::Play, "Playback (Y): hold to hear MAIN from there" } };
    for (const auto& [t, tip] : toolList)
    {
        auto* b = tools.add(new ToolButton(t));
        b->setTooltip(tip);
        b->setMouseClickGrabsKeyboardFocus(false);
        const Tool tool = t;
        b->onClick = [this, tool] { selectTool(tool); };
        addAndMakeVisible(b);
    }
    snapBtn.setTooltip("Magnet: the grid that notes stick to (Alt while dragging = free)");
    snapBtn.onClick = [this] { snapMenu(); };
    rowsBtn.setTooltip("One row per pad (like piano-roll keys) instead of the 4 source lanes");
    rowsBtn.onClick = [this] { proc.ui.padRows = !proc.ui.padRows; proc.ui.mainVZoom = 1.0; proc.ui.mainScrollY = 0; lanes.clampView(); refresh(); };
    velBtn.setTooltip("Velocity strip under the notes: drag to draw velocities");
    velBtn.onClick = [this] { proc.ui.showVelocity = !proc.ui.showVelocity; lanes.clampView(); refresh(); };
    followBtn.setTooltip("Scroll with the playhead while zoomed in");
    followBtn.onClick = [this] { proc.ui.followPlayhead = !proc.ui.followPlayhead; refresh(); };
    auto midX = [this] { return LaneCanvas::kLabelW + (lanes.getWidth() - LaneCanvas::kLabelW) / 2; };
    zoomOutBtn.setTooltip("Zoom out in time (Ctrl+wheel, PgDn)");
    zoomInBtn.setTooltip("Zoom in in time (Ctrl+wheel, PgUp)");
    zoomOutBtn.onClick = [this, midX] { lanes.zoomTime(1.0 / 1.5, midX()); };
    zoomInBtn.onClick = [this, midX] { lanes.zoomTime(1.5, midX()); };
    fitBtn.setTooltip("Show the whole pattern (F; Shift+F = the selection)");
    fitBtn.onClick = [this] { lanes.fitAll(); };
    rowOutBtn.setTooltip("Lower rows (Ctrl+Shift+wheel)");
    rowInBtn.setTooltip("Higher rows (Ctrl+Shift+wheel)");
    rowOutBtn.onClick = [this] { lanes.zoomRows(1.0 / 1.3, lanes.notesArea().getCentreY()); };
    rowInBtn.onClick = [this] { lanes.zoomRows(1.3, lanes.notesArea().getCentreY()); };
    zoomLabel.setText("ZOOM", juce::dontSendNotification);
    rowLabel.setText("ROWS", juce::dontSendNotification);
    for (auto* l : { &zoomLabel, &rowLabel })
    {
        l->setFont(theme::ui(10.0f, true));
        l->setColour(juce::Label::textColourId, theme::textDim);
        l->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(*l);
    }
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
    wavBtn.makeFile = [this] { return dragFile(false); };
    midiBtn.makeFile = [this] { return dragFile(true); };
    wavBtn.setTooltip("Drag into the DAW: MAIN as a WAV loop. Click = save as.");
    midiBtn.setTooltip("Drag into the DAW: the pad notes as MIDI (C1 = A01). Click = save as.");
    startTimerHz(30);
    refresh();
}

PatternView::~PatternView() { stopTimer(); }

void PatternView::showHelp()
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::NoIcon, "MAIN - editing as in FL Studio Piano roll",
        "Tools\n"
        "  P draw   B paint   N step paint   D delete   T mute   C slice   E select   Z zoom   Y playback\n\n"
        "Mouse (Draw)\n"
        "  Left click             place a note (its row shows which pad), keep dragging to move it\n"
        "  Left drag a note       move (PAD ROWS: up / down = another pad)    right end  resize    Shift+drag  copy    Alt  no snap\n"
        "  Click a note           'pick': its pad, length and velocity are used for the next notes\n"
        "  Right click            delete      Right drag  delete everything it touches\n"
        "  Ctrl+drag              select (any tool)    Ctrl+Shift+drag  add to the selection\n"
        "  Velocity strip         drag to draw velocities; Alt+wheel on a note changes one\n\n"
        "View\n"
        "  Ctrl+wheel  zoom time    Ctrl+Shift+wheel  row height    Shift+wheel  scroll time    middle drag  pan\n"
        "  Strip above the ruler: drag to move, wheel to zoom    PgUp / PgDn  zoom    F  fit    Shift+F  fit selection\n\n"
        "Keys\n"
        "  Ctrl+A  select all    Ctrl+D  deselect    Ctrl+B  duplicate after the selection\n"
        "  Ctrl+C / Ctrl+X / Ctrl+V  copy / cut / paste at the ruler marker    Delete  delete\n"
        "  Left / Right  move by the grid (Shift: a bar)    Up / Down  next / previous pad of the same source\n"
        "  Alt+Q / Ctrl+Q  quantize    Ctrl+U  chop into grid steps    Alt+M  mute / unmute    Ctrl+Z / Ctrl+Y  undo / redo\n"
        "  Space  play / stop everything in the plug-in", "OK", this);
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
    const int64_t at = proc.ui.mainCursorTick, loop = loopTicks();
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

void PatternView::chopSelected()
{
    if (proc.ui.selectedEvents.empty()) return;
    const int64_t g = gridStep();
    std::vector<juce::String> fresh;
    proc.project.edit("Chop notes", [&](ProjectState& s)
    {
        std::vector<PatternEvent> out;
        for (const auto& e : s.events)
        {
            if (!isSelected(e.id) || e.length <= g) { out.push_back(e); if (isSelected(e.id)) fresh.push_back(e.id); continue; }
            for (int64_t t = 0; t < e.length; t += g)
            {
                auto c = e;
                if (t > 0) { c.id = newId(); c.rawTick = -1; }
                c.tick = e.tick + t;
                c.length = std::min(g, e.length - t);
                out.push_back(c);
                fresh.push_back(c.id);
            }
        }
        s.events = out;
    });
    proc.ui.selectedEvents = fresh;
}

void PatternView::toggleMuteSelected()
{
    if (proc.ui.selectedEvents.empty()) return;
    bool anyOn = false;
    for (const auto& e : proc.project.state().events) anyOn = anyOn || (isSelected(e.id) && !e.muted);
    proc.project.edit("Mute notes", [&](ProjectState& s) { for (auto& e : s.events) if (isSelected(e.id)) e.muted = anyOn; });
}

void PatternView::selectTool(UiState::Tool t) { proc.ui.tool = t; refresh(); }

void PatternView::snapMenu()
{
    const auto& st = proc.project.state().settings;
    juce::PopupMenu m;
    for (int i = 0; i <= (int) GridDiv::Off; ++i) m.addItem(1 + i, gridName((GridDiv) i), true, (int) st.grid == i);
    m.addSeparator();
    m.addItem(100, "Snap on", true, st.snap);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&snapBtn), [this](int r)
    {
        if (r == 100) proc.project.edit("Snap", [](ProjectState& s) { s.settings.snap = !s.settings.snap; });
        else if (r >= 1) proc.project.edit("Grid", [r](ProjectState& s) { s.settings.grid = (GridDiv) (r - 1); s.settings.snap = true; });
    });
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
        if (is('Q')) { quantizeSelected(); return true; }
        if (is('U')) { chopSelected(); return true; }
        if (is('C')) { copySelected(false); return true; }
        if (is('X')) { copySelected(true); return true; }
        if (is('V')) { paste(); return true; }
        return false; // Ctrl+Z / Ctrl+Y go to the editor
    }
    if (m.isAltDown() && is('M')) { toggleMuteSelected(); return true; }
    if (code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey)
    {
        lanes.zoomTime(code == juce::KeyPress::pageUpKey ? 1.5 : 1.0 / 1.5, (int) lanes.tickToX(proc.ui.mainStart + lanes.visibleTicks() / 2));
        return true;
    }
    if (m.isAltDown() && is('Q')) { quantizeSelected(); return true; }
    if (!m.isCtrlDown() && !m.isAltDown())
    {
        const std::pair<char, Tool> keys[] = { { 'P', Tool::Draw }, { 'B', Tool::Paint }, { 'N', Tool::StepPaint }, { 'D', Tool::Delete },
            { 'T', Tool::Mute }, { 'C', Tool::Slice }, { 'E', Tool::Select }, { 'Z', Tool::Zoom }, { 'Y', Tool::Play } };
        for (const auto& [c, t] : keys) if (is(c)) { selectTool(t); return true; }
        if (is('F')) { if (m.isShiftDown()) lanes.fitSelection(); else lanes.fitAll(); return true; }
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
    const bool pattern = proc.ui.mainTab == 0;
    for (auto* b : tools) { b->active = b->tool == proc.ui.tool; b->setVisible(pattern); b->repaint(); }
    snapBtn.setButtonText("SNAP " + (st.settings.snap ? gridName(st.settings.grid) : juce::String("OFF")));
    snapBtn.setToggleState(st.settings.snap && st.settings.grid != GridDiv::Off, juce::dontSendNotification);
    rowsBtn.setToggleState(proc.ui.padRows, juce::dontSendNotification);
    velBtn.setToggleState(proc.ui.showVelocity, juce::dontSendNotification);
    followBtn.setToggleState(proc.ui.followPlayhead, juce::dontSendNotification);
    for (auto* b : { &snapBtn, &rowsBtn, &velBtn, &followBtn, &zoomOutBtn, &zoomInBtn, &fitBtn, &rowOutBtn, &rowInBtn }) b->setVisible(pattern);
    zoomLabel.setVisible(pattern); rowLabel.setVisible(pattern);
    guideBtn.setToggleState(proc.ui.showGuide && proc.guide.present, juce::dontSendNotification);
    guideBtn.setEnabled(proc.guide.present);
    lanes.setVisible(proc.ui.mainTab == 0);
    for (auto* b : { &helpBtn, &quantBtn, &clearBtn }) b->setVisible(proc.ui.mainTab == 0);
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
    lanes.clampView(); // pads / pattern length may have changed
    repaint();
}

void PatternView::timerCallback()
{
    // The whole lane canvas is redrawn only when its content moves (follow scroll, stop, tab). While the DAW plays,
    // only the playhead strips and the row labels of pads that start / stop sounding are: a full redraw of a big
    // window 30 times a second made FL crackle at small buffers even with the plug-in window in the background.
    uint64_t sounding = 0;
    for (int i = 0; i < kNumPads; ++i) if (proc.engine.padVoices[(size_t) i].load() > 0) sounding |= 1ull << i;
    const bool playing = proc.engine.transportPlaying.load();
    const double viewBefore = proc.ui.mainStart;
    if (playing) lanes.followPlayhead();
    const bool stopped = wasPlaying && !playing;
    wasPlaying = playing;
    if (stopped || proc.ui.mainStart != viewBefore || lastTab != proc.ui.mainTab)
    {
        lanes.repaint();
        lanes.lastPlayheadX = -100.0f;
        lastTab = proc.ui.mainTab;
    }
    else
    {
        if (playing) lanes.repaintPlayhead();
        if (sounding != lastSounding && proc.ui.padRows) lanes.repaint(0, 0, LaneCanvas::kLabelW, lanes.getHeight());
    }
    lastSounding = sounding;
}

void PatternView::paint(juce::Graphics& g)
{
    g.fillAll(theme::body);
    if (proc.ui.mainTab != 1) return;
    auto r = getLocalBounds().reduced(6).withTrimmedTop(64);
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
    helpBtn.setBounds(top.removeFromLeft(64).reduced(2));
    barsBox.setBounds(top.removeFromLeft(90).reduced(2));
    guideBtn.setBounds(top.removeFromLeft(98).reduced(2));
    midiBtn.setBounds(top.removeFromRight(96).reduced(2));
    wavBtn.setBounds(top.removeFromRight(94).reduced(2));
    clearBtn.setBounds(top.removeFromRight(60).reduced(2));
    quantBtn.setBounds(top.removeFromRight(110).reduced(2));
    renderBtn.setBounds(top.removeFromRight(80).reduced(2));
    info.setBounds(top.reduced(6, 0));
    auto bar = r.removeFromTop(32);
    for (auto* b : tools) b->setBounds(bar.removeFromLeft(33).reduced(1));
    bar.removeFromLeft(8);
    snapBtn.setBounds(bar.removeFromLeft(98).reduced(2));
    rowsBtn.setBounds(bar.removeFromLeft(92).reduced(2));
    velBtn.setBounds(bar.removeFromLeft(86).reduced(2));
    followBtn.setBounds(bar.removeFromLeft(76).reduced(2));
    fitBtn.setBounds(bar.removeFromRight(48).reduced(2));
    rowInBtn.setBounds(bar.removeFromRight(32).reduced(2));
    rowOutBtn.setBounds(bar.removeFromRight(32).reduced(2));
    rowLabel.setBounds(bar.removeFromRight(46));
    zoomInBtn.setBounds(bar.removeFromRight(32).reduced(2));
    zoomOutBtn.setBounds(bar.removeFromRight(32).reduced(2));
    zoomLabel.setBounds(bar.removeFromRight(50));
    lanes.setBounds(r.withTrimmedTop(2));
    lanes.clampView();
}

juce::File PatternView::laneDragFile(int src)
{
    juce::String err;
    const auto f = proc.writeDragFile(false, err, src);
    proc.setStatus(f == juce::File() ? err : "Drop it into the DAW: " + f.getFileName());
    return f;
}

void PatternView::exportLaneWav(int src)
{
    const auto name = "HPSAMPLER_SRC" + juce::String(src + 1) + ".wav";
    chooser = std::make_unique<juce::FileChooser>("Export SRC " + juce::String(src + 1) + " of MAIN as WAV",
                                                  juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(name), "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, src](const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("wav")) f = f.withFileExtension("wav");
        juce::String err;
        proc.setStatus(proc.exportWav(f, err, 2.0, src) ? "Exported " + f.getFullPathName() : "Export failed: " + err);
    });
}

juce::File PatternView::dragFile(bool midi)
{
    juce::String err;
    const auto f = proc.writeDragFile(midi, err);
    proc.setStatus(f == juce::File() ? err : "Drop it into the DAW: " + f.getFileName());
    return f;
}

void PatternView::startFileDrag(juce::Component& from, const std::function<juce::File()>& makeFile)
{
    const auto f = makeFile ? makeFile() : juce::File();
    if (f.existsAsFile())
        juce::DragAndDropContainer::performExternalDragDropOfFiles({ f.getFullPathName() }, false, &from);
}

void PatternView::mouseDrag(const juce::MouseEvent& e)
{
    if (resultDragged || proc.ui.mainTab != 1 || e.getDistanceFromDragStart() < 8 || proc.resultAudio == nullptr) return;
    resultDragged = true;
    startFileDrag(*this, [this] { return dragFile(false); });
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
