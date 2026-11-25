#include "ui/SourceView.h"

namespace hps
{
namespace
{
constexpr int kRulerH = 24, kPadBandH = 18, kCandBandH = 20, kOverviewH = 16;

juce::String fmtTime(double seconds)
{
    const int m = (int) (seconds / 60.0);
    return juce::String(m) + ":" + juce::String(seconds - m * 60.0, 3).paddedLeft('0', 6);
}

juce::Colour specColour(uint8_t v)
{
    const float t = v / 255.0f;
    // dark blue -> cyan -> amber -> white
    if (t < 0.35f) return juce::Colour::fromFloatRGBA(0.02f, 0.04f + t * 0.5f, 0.08f + t * 0.9f, 1.0f);
    if (t < 0.65f) { const float u = (t - 0.35f) / 0.3f; return juce::Colour::fromFloatRGBA(0.1f + u * 0.8f, 0.22f + u * 0.45f, 0.4f - u * 0.25f, 1.0f); }
    const float u = (t - 0.65f) / 0.35f;
    return juce::Colour::fromFloatRGBA(0.9f + u * 0.1f, 0.67f + u * 0.33f, 0.15f + u * 0.85f, 1.0f);
}
} // namespace

// =============================================================================================
// WaveCanvas
// =============================================================================================

WaveCanvas::WaveCanvas(SourceView& o) : owner(o)
{
    setWantsKeyboardFocus(false);
}

juce::Rectangle<int> WaveCanvas::rulerArea() const { return getLocalBounds().removeFromTop(kRulerH); }
juce::Rectangle<int> WaveCanvas::padBand() const { return getLocalBounds().withTrimmedTop(kRulerH).removeFromTop(kPadBandH); }
juce::Rectangle<int> WaveCanvas::overviewArea() const { return getLocalBounds().removeFromBottom(kOverviewH); }
juce::Rectangle<int> WaveCanvas::candBand() const { return getLocalBounds().withTrimmedBottom(kOverviewH).removeFromBottom(kCandBandH); }
juce::Rectangle<int> WaveCanvas::waveArea() const
{
    return getLocalBounds().withTrimmedTop(kRulerH + kPadBandH).withTrimmedBottom(kOverviewH + kCandBandH);
}

double WaveCanvas::frameToX(double f) const
{
    const auto& v = owner.view();
    return (f - v.viewStart) / std::max(1.0, v.viewLen) * getWidth();
}

double WaveCanvas::xToFrame(double x) const
{
    const auto& v = owner.view();
    return v.viewStart + x / std::max(1, getWidth()) * v.viewLen;
}

int64_t WaveCanvas::snapped(double frame, bool force) const
{
    const auto d = owner.data();
    if (d == nullptr) return (int64_t) frame;
    auto mode = owner.proc.ui.snap;
    if (juce::ModifierKeys::currentModifiers.isAltDown() && !force) mode = SnapMode::Off; // Alt = free
    const int64_t maxDist = (int64_t) (owner.view().viewLen / std::max(1, getWidth()) * 14.0); // ~14 px magnet
    return ChopEngine::snap(*d, (int64_t) std::llround(frame), mode, owner.grid(), owner.proc.project.state().settings.grid,
                            std::max<int64_t>(maxDist, 16));
}

void WaveCanvas::drawGrid(juce::Graphics& g, juce::Rectangle<int> area, bool ruler)
{
    const auto d = owner.data();
    if (d == nullptr) return;
    const auto gr = owner.grid();
    const auto& v = owner.view();
    const double pxPerFrame = getWidth() / std::max(1.0, v.viewLen);
    if (!gr.valid())
    {
        if (!ruler) return;
        // seconds ruler
        const double pxPerSec = pxPerFrame * d->sampleRate;
        double step = 0.001;
        for (double s : { 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 30.0, 60.0 }) { step = s; if (s * pxPerSec >= 70) break; }
        const double t0 = std::floor(v.viewStart / d->sampleRate / step) * step;
        g.setFont(theme::mono(11.0f));
        for (double t = t0; t * d->sampleRate < v.viewStart + v.viewLen; t += step)
        {
            const float x = (float) frameToX(t * d->sampleRate);
            g.setColour(theme::textDim.withAlpha(0.5f));
            g.drawVerticalLine((int) x, (float) area.getBottom() - 6, (float) area.getBottom());
            g.setColour(theme::textDim);
            g.drawText(fmtTime(t), (int) x + 3, area.getY(), 80, area.getHeight() - 4, juce::Justification::centredLeft);
        }
        return;
    }
    const double fpb = gr.framesPerBeat();
    const double pxPerBeat = fpb * pxPerFrame;
    const int sub = gridTicks(owner.proc.project.state().settings.grid);
    const double subBeats = sub > 0 ? (double) sub / kPPQ : 1.0;
    const double first = std::floor((v.viewStart - gr.originFrame()) / fpb) - 1;
    const double last = std::ceil((v.viewStart + v.viewLen - gr.originFrame()) / fpb) + 1;
    const bool drawSub = sub > 0 && subBeats < 1.0 && pxPerBeat * subBeats >= 7.0;
    const bool drawBeats = pxPerBeat >= 5.0;
    const int barStep = pxPerBeat * 4 >= 40 ? 1 : (pxPerBeat * 16 >= 40 ? 4 : 16);
    g.setFont(theme::mono(11.0f, true));
    for (double b = first; b <= last; b += 1.0)
    {
        const int beat = (int) b;
        const bool isBar = ((beat % 4) + 4) % 4 == 0;
        const int bar = (int) std::floor(beat / 4.0) + 1;
        const float x = (float) frameToX(gr.originFrame() + b * fpb);
        if (isBar && (((bar - 1) % barStep) + barStep) % barStep == 0)
        {
            if (ruler)
            {
                g.setColour(theme::text.withAlpha(0.8f));
                g.drawVerticalLine((int) x, (float) area.getY() + 4, (float) area.getBottom());
                g.drawText(juce::String(bar), (int) x + 3, area.getY() + 1, 40, area.getHeight() - 6, juce::Justification::centredLeft);
            }
            else
            {
                g.setColour(juce::Colours::white.withAlpha(0.22f));
                g.drawVerticalLine((int) x, (float) area.getY(), (float) area.getBottom());
            }
        }
        else if (drawBeats && !isBar)
        {
            g.setColour(juce::Colours::white.withAlpha(ruler ? 0.4f : 0.09f));
            g.drawVerticalLine((int) x, ruler ? (float) area.getBottom() - 8 : (float) area.getY(), (float) area.getBottom());
        }
        if (drawSub)
            for (double s = subBeats; s < 0.999; s += subBeats)
            {
                const float sx = (float) frameToX(gr.originFrame() + (b + s) * fpb);
                g.setColour(juce::Colours::white.withAlpha(ruler ? 0.25f : 0.04f));
                g.drawVerticalLine((int) sx, ruler ? (float) area.getBottom() - 4 : (float) area.getY(), (float) area.getBottom());
            }
    }
    if (ruler)
    {
        // beat-1 flag (drag to fix the grid)
        const float ox = (float) frameToX(gr.originFrame());
        juce::Path flag;
        flag.addTriangle(ox, (float) area.getBottom(), ox, (float) area.getY() + 2, ox + 10, (float) area.getY() + 7);
        g.setColour(theme::accent);
        g.fillPath(flag);
    }
}

void WaveCanvas::drawWave(juce::Graphics& g, juce::Rectangle<int> area, const SourceData& d)
{
    const auto& v = owner.view();
    const double fpp = v.viewLen / std::max(1, getWidth());
    const float mid = (float) area.getCentreY(), half = area.getHeight() * 0.47f;
    g.setColour(theme::wave.withAlpha(0.9f));
    if (fpp >= d.peaks.framesPerPeak)
    {
        const auto& P = d.peaks;
        const int64_t count = (int64_t) P.mins.size();
        for (int x = 0; x < getWidth(); ++x)
        {
            const int64_t i0 = (int64_t) ((v.viewStart + x * fpp) / P.framesPerPeak);
            const int64_t i1 = (int64_t) ((v.viewStart + (x + 1) * fpp) / P.framesPerPeak);
            if (i1 < 0 || i0 >= count) continue;
            float mn = 1, mx = -1;
            for (int64_t i = std::max<int64_t>(0, i0); i <= std::min(count - 1, i1); ++i) { mn = std::min(mn, P.mins[(size_t) i]); mx = std::max(mx, P.maxs[(size_t) i]); }
            if (mn > mx) continue;
            g.drawVerticalLine(x, mid - mx * half, mid - mn * half + 1.0f);
        }
    }
    else
    {
        const auto& a = *d.audio;
        const int n = a.getNumSamples();
        if (fpp > 1.0)
        {
            for (int x = 0; x < getWidth(); ++x)
            {
                const int64_t s0 = (int64_t) (v.viewStart + x * fpp), s1 = (int64_t) (v.viewStart + (x + 1) * fpp);
                float mn = 1, mx = -1;
                for (int64_t s = std::max<int64_t>(0, s0); s <= std::min<int64_t>(n - 1, s1); ++s)
                {
                    const float m = 0.5f * (a.getSample(0, (int) s) + a.getSample(1, (int) s));
                    mn = std::min(mn, m); mx = std::max(mx, m);
                }
                if (mn <= mx) g.drawVerticalLine(x, mid - mx * half, mid - mn * half + 1.0f);
            }
        }
        else
        {
            juce::Path p;
            bool started = false;
            const int64_t s0 = std::max<int64_t>(0, (int64_t) v.viewStart - 1), s1 = std::min<int64_t>(n - 1, (int64_t) (v.viewStart + v.viewLen) + 1);
            for (int64_t s = s0; s <= s1; ++s)
            {
                const float m = 0.5f * (a.getSample(0, (int) s) + a.getSample(1, (int) s));
                const float x = (float) frameToX((double) s), y = mid - m * half;
                if (!started) { p.startNewSubPath(x, y); started = true; } else p.lineTo(x, y);
                if (1.0 / fpp > 6.0) g.fillEllipse(x - 2, y - 2, 4, 4);
            }
            g.strokePath(p, juce::PathStrokeType(1.3f));
        }
    }
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawHorizontalLine((int) mid, (float) area.getX(), (float) area.getRight());
}

void WaveCanvas::drawSpectrogram(juce::Graphics& g, juce::Rectangle<int> area, const SourceData& d)
{
    const auto& a = d.analysis;
    if (!a.ready || a.specCols <= 0) { g.setColour(theme::textDim); g.drawText("spectrogram after analysis...", area, juce::Justification::centred); return; }
    if (specFor != &d)
    {
        specImage = juce::Image(juce::Image::RGB, a.specCols, a.specBins, false);
        juce::Image::BitmapData bm(specImage, juce::Image::BitmapData::writeOnly);
        for (int c = 0; c < a.specCols; ++c)
            for (int b = 0; b < a.specBins; ++b)
                bm.setPixelColour(c, a.specBins - 1 - b, specColour(a.spectrogram[(size_t) c * (size_t) a.specBins + (size_t) b]));
        specFor = &d;
    }
    const auto& v = owner.view();
    const double c0 = v.viewStart / a.specHop, c1 = (v.viewStart + v.viewLen) / a.specHop;
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    g.drawImage(specImage, area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                (int) std::floor(c0), 0, juce::jmax(1, (int) std::ceil(c1 - c0)), a.specBins);
}

void WaveCanvas::paint(juce::Graphics& g)
{
    g.fillAll(theme::screen);
    const auto d = owner.data();
    auto& v = owner.view();
    const auto& st = owner.proc.project.state();
    const auto& slot = st.sources[(size_t) owner.getSlot()];
    const auto col = slot.colour;

    if (d == nullptr || d->audio == nullptr)
    {
        g.setColour(theme::textDim);
        g.setFont(theme::ui(18.0f, true));
        const bool loading = owner.proc.isSourceLoading(owner.getSlot());
        juce::String msg = loading ? "Loading..." : (slot.empty() ? "Drop an audio file here  (WAV / AIFF / FLAC / MP3)\nor press LOAD"
                                                                  : "File missing: " + slot.filePath + "\nPress LOAD to relink");
        g.drawFittedText(msg, getLocalBounds().reduced(20), juce::Justification::centred, 3);
        return;
    }
    if (v.viewLen <= 0.0) { v.viewStart = 0; v.viewLen = (double) d->numFrames; }

    auto wave = waveArea();
    // background grid
    drawGrid(g, wave, false);

    // pad regions tint
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = st.pads[(size_t) i];
        if (!p.assigned || p.src != owner.getSlot()) continue;
        const float x0 = (float) frameToX((double) p.start), x1 = (float) frameToX((double) p.end);
        if (x1 < 0 || x0 > getWidth()) continue;
        const bool sel = owner.proc.ui.selectedPad == i;
        g.setColour(col.withAlpha(sel ? 0.18f : 0.07f));
        g.fillRect(juce::Rectangle<float>(x0, (float) wave.getY(), std::max(1.0f, x1 - x0), (float) wave.getHeight()));
    }

    const int display = v.display;
    if (display == 0) drawWave(g, wave, *d);
    else if (display == 1) drawSpectrogram(g, wave, *d);
    else
    {
        auto top = wave.withHeight(wave.getHeight() / 2);
        drawSpectrogram(g, top, *d);
        drawWave(g, wave.withTrimmedTop(top.getHeight()), *d);
    }

    // onset ticks (small, at the bottom of the wave area)
    g.setColour(theme::accent.withAlpha(0.45f));
    for (size_t i = 0; i < d->analysis.onsets.size(); ++i)
    {
        const float x = (float) frameToX((double) d->analysis.onsets[i]);
        if (x < 0 || x > getWidth()) continue;
        const float h = 3.0f + 7.0f * d->analysis.onsetStrength[i];
        g.drawVerticalLine((int) x, (float) wave.getBottom() - h, (float) wave.getBottom());
    }

    // selection
    if (v.selStart >= 0 && v.selEnd > v.selStart)
    {
        const float x0 = (float) frameToX((double) v.selStart), x1 = (float) frameToX((double) v.selEnd);
        g.setColour(juce::Colours::white.withAlpha(0.13f));
        g.fillRect(juce::Rectangle<float>(x0, (float) wave.getY(), std::max(1.0f, x1 - x0), (float) wave.getHeight()));
        const bool padEdit = owner.editingPad() >= 0;
        if (padEdit)
        {
            g.setColour(col.withAlpha(0.9f));
            g.drawRect(juce::Rectangle<float>(x0, (float) wave.getY(), std::max(1.0f, x1 - x0), (float) wave.getHeight()), 2.0f);
        }
        g.setColour(juce::Colours::white);
        g.fillRect(juce::Rectangle<float>(x0 - 1, (float) wave.getY(), 2.0f, (float) wave.getHeight()));
        g.fillRect(juce::Rectangle<float>(x1 - 1, (float) wave.getY(), 2.0f, (float) wave.getHeight()));
        juce::Path h;
        h.addTriangle(x0, (float) wave.getY(), x0 + 9, (float) wave.getY(), x0, (float) wave.getY() + 9);
        h.addTriangle(x1, (float) wave.getY(), x1 - 9, (float) wave.getY(), x1, (float) wave.getY() + 9);
        h.addTriangle(x0, (float) wave.getBottom(), x0 + 9, (float) wave.getBottom(), x0, (float) wave.getBottom() - 9);
        h.addTriangle(x1, (float) wave.getBottom(), x1 - 9, (float) wave.getBottom(), x1, (float) wave.getBottom() - 9);
        g.fillPath(h);
        if (x1 - x0 > 120 && !padEdit)
        {
            g.setFont(theme::ui(11.0f, true));
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawText("drag onto a pad  /  Enter = next free pad", juce::Rectangle<float>(x0, (float) wave.getY() + 2, x1 - x0, 14.0f).toNearestInt(),
                       juce::Justification::centred);
        }
    }

    // ruler
    auto ruler = rulerArea();
    g.setColour(theme::bodyLight);
    g.fillRect(ruler);
    drawGrid(g, ruler, true);

    // pad band
    auto pb = padBand();
    g.setColour(theme::body);
    g.fillRect(pb);
    g.setFont(theme::mono(11.0f, true));
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& p = st.pads[(size_t) i];
        if (!p.assigned || p.src != owner.getSlot()) continue;
        const float x0 = (float) frameToX((double) p.start), x1 = (float) frameToX((double) p.end);
        if (x1 < 0 || x0 > getWidth()) continue;
        const bool sel = owner.proc.ui.selectedPad == i;
        auto r = juce::Rectangle<float>(x0, (float) pb.getY() + 2, std::max(3.0f, x1 - x0), (float) pb.getHeight() - 3);
        g.setColour(col.withAlpha(sel ? 0.95f : 0.6f));
        g.fillRoundedRectangle(r, 2.0f);
        g.setColour(theme::screen);
        if (r.getWidth() > 24) g.drawText(padLabel(i), r.toNearestInt().reduced(3, 0), juce::Justification::centredLeft);
    }

    // candidate band
    auto cb = candBand();
    g.setColour(theme::body);
    g.fillRect(cb);
    g.setFont(theme::mono(10.0f, true));
    for (int i = 0; i < (int) v.candidates.size(); ++i)
    {
        const auto& c = v.candidates[(size_t) i];
        const float x0 = (float) frameToX((double) c.start), x1 = (float) frameToX((double) c.end);
        if (x1 < 0 || x0 > getWidth()) continue;
        const bool sel = v.selectedCandidate == i;
        auto r = juce::Rectangle<float>(x0, (float) cb.getY() + 2, std::max(3.0f, x1 - x0 - 1), (float) cb.getHeight() - 4);
        g.setColour(theme::accent.withAlpha(sel ? 0.55f : 0.08f + 0.22f * c.score));
        g.fillRect(r);
        g.setColour(theme::accent.withAlpha(0.9f));
        const float dashes[] = { 3.0f, 2.0f };
        juce::Path outline; outline.addRectangle(r);
        juce::Path dashed; juce::PathStrokeType(1.0f).createDashedStroke(dashed, outline, dashes, 2);
        g.fillPath(dashed);
        g.setColour(theme::text);
        if (r.getWidth() > 16) g.drawText(juce::String(i + 1), r.toNearestInt(), juce::Justification::centred);
        // faint cut line on the waveform
        g.setColour(theme::accent.withAlpha(0.35f));
        g.drawVerticalLine((int) x0, (float) wave.getY(), (float) wave.getBottom());
    }

    // audition playhead
    if (owner.proc.engine.previewSlot.load() == owner.getSlot())
    {
        const double f = owner.proc.engine.previewFrame.load();
        if (f >= 0)
        {
            const float x = (float) frameToX(f);
            g.setColour(theme::ok);
            g.fillRect(juce::Rectangle<float>(x - 1, (float) ruler.getY(), 2.0f, (float) (candBand().getBottom() - ruler.getY())));
        }
    }
    if (owner.markFrame >= 0)
    {
        g.setColour(theme::rec);
        const float x = (float) frameToX((double) owner.markFrame);
        g.drawVerticalLine((int) x, (float) wave.getY(), (float) wave.getBottom());
    }

    // overview
    auto ov = overviewArea();
    g.setColour(theme::bodyLight);
    g.fillRect(ov);
    const auto& P = d->peaks;
    if (!P.maxs.empty())
    {
        g.setColour(theme::wave.withAlpha(0.4f));
        for (int x = 0; x < ov.getWidth(); ++x)
        {
            const size_t i = (size_t) ((double) x / ov.getWidth() * P.maxs.size());
            const float m = std::max(std::abs(P.mins[std::min(i, P.mins.size() - 1)]), P.maxs[std::min(i, P.maxs.size() - 1)]);
            g.drawVerticalLine(ov.getX() + x, ov.getCentreY() - m * ov.getHeight() * 0.45f, ov.getCentreY() + m * ov.getHeight() * 0.45f);
        }
    }
    const float vx0 = (float) (v.viewStart / d->numFrames * ov.getWidth()), vx1 = (float) ((v.viewStart + v.viewLen) / d->numFrames * ov.getWidth());
    g.setColour(juce::Colours::white.withAlpha(0.15f));
    g.fillRect(juce::Rectangle<float>(vx0, (float) ov.getY(), std::max(4.0f, vx1 - vx0), (float) ov.getHeight()));
    g.setColour(col);
    g.drawRect(juce::Rectangle<float>(vx0, (float) ov.getY(), std::max(4.0f, vx1 - vx0), (float) ov.getHeight()), 1.0f);
}

int WaveCanvas::candidateAt(juce::Point<int> p, int& edge) const
{
    if (!candBand().contains(p)) return -1;
    const auto& v = owner.view();
    for (int i = (int) v.candidates.size() - 1; i >= 0; --i)
    {
        const auto& c = v.candidates[(size_t) i];
        const double x0 = frameToX((double) c.start), x1 = frameToX((double) c.end);
        if (p.x >= x0 - 4 && p.x <= x1 + 4)
        {
            edge = std::abs(p.x - x0) <= 5 ? -1 : (std::abs(p.x - x1) <= 5 ? 1 : 0);
            return i;
        }
    }
    return -1;
}

int WaveCanvas::padRegionAt(juce::Point<int> p) const
{
    if (!padBand().contains(p)) return -1;
    const auto& st = owner.proc.project.state();
    for (int i = 0; i < kNumPads; ++i)
    {
        const auto& pad = st.pads[(size_t) i];
        if (!pad.assigned || pad.src != owner.getSlot()) continue;
        if (p.x >= frameToX((double) pad.start) && p.x <= frameToX((double) pad.end)) return i;
    }
    return -1;
}

void WaveCanvas::mouseMove(const juce::MouseEvent& e)
{
    const auto& v = owner.view();
    auto cursor = juce::MouseCursor::NormalCursor;
    int edge = 0;
    if (candidateAt(e.getPosition(), edge) >= 0 && edge != 0) cursor = juce::MouseCursor::LeftRightResizeCursor;
    else if (waveArea().contains(e.getPosition()) && v.selStart >= 0)
    {
        const double x0 = frameToX((double) v.selStart), x1 = frameToX((double) v.selEnd);
        if (std::abs(e.x - x0) <= 6 || std::abs(e.x - x1) <= 6) cursor = juce::MouseCursor::LeftRightResizeCursor;
        else if (e.x > x0 && e.x < x1) cursor = juce::MouseCursor::DraggingHandCursor;
        else cursor = juce::MouseCursor::IBeamCursor;
    }
    else if (waveArea().contains(e.getPosition())) cursor = juce::MouseCursor::IBeamCursor;
    else if (rulerArea().contains(e.getPosition()) && owner.grid().valid()
             && std::abs(e.x - frameToX(owner.grid().originFrame())) < 10) cursor = juce::MouseCursor::LeftRightResizeCursor;
    setMouseCursor(cursor);
}

void WaveCanvas::mouseDown(const juce::MouseEvent& e)
{
    owner.grabKeyboardFocus();
    const auto d = owner.data();
    if (d == nullptr) return;
    auto& v = owner.view();
    moved = false;
    drag = Drag::None;
    if (e.mods.isPopupMenu()) { showContextMenu(e); return; }

    const auto p = e.getPosition();
    if (overviewArea().contains(p))
    {
        drag = Drag::Overview;
        v.viewStart = (double) p.x / getWidth() * d->numFrames - v.viewLen * 0.5;
        owner.clampView();
        repaint();
        return;
    }
    if (rulerArea().contains(p))
    {
        const auto gr = owner.grid();
        if (gr.valid() && std::abs(p.x - frameToX(gr.originFrame())) < 10) { drag = Drag::Origin; return; }
        owner.auditionFrom(snapped(xToFrame(p.x)));
        return;
    }
    if (const int pad = padRegionAt(p); pad >= 0)
    {
        const auto& pr = owner.proc.project.state().pads[(size_t) pad];
        owner.proc.ui.selectedPad = pad;
        owner.proc.ui.editingPad = pad; // clicking a pad region edits that pad's range
        owner.setSelection(pr.start, pr.end, true);
        owner.refresh();
        return;
    }
    int edge = 0;
    if (const int c = candidateAt(p, edge); c >= 0)
    {
        if (owner.editingPad() >= 0) owner.endPadEdit();
        v.selectedCandidate = c;
        dragCandidate = c;
        if (edge == -1) drag = Drag::CandStart;
        else if (edge == 1) drag = Drag::CandEnd;
        else owner.setSelection(v.candidates[(size_t) c].start, v.candidates[(size_t) c].end, true);
        repaint();
        return;
    }
    if (candBand().contains(p)) return;

    const double f = xToFrame(p.x);
    if (v.selStart >= 0 && v.selEnd > v.selStart)
    {
        const double x0 = frameToX((double) v.selStart), x1 = frameToX((double) v.selEnd);
        if (std::abs(p.x - x0) <= 6) { drag = Drag::SelStart; return; }
        if (std::abs(p.x - x1) <= 6) { drag = Drag::SelEnd; return; }
        if (p.x > x0 && p.x < x1)
        {
            drag = e.mods.isShiftDown() ? Drag::MoveSel : Drag::ToPad;
            dragOffset = (int64_t) f - v.selStart;
            return;
        }
    }
    drag = Drag::NewSel;
    anchorFrame = snapped(f);
}

void WaveCanvas::mouseDrag(const juce::MouseEvent& e)
{
    const auto d = owner.data();
    if (d == nullptr) return;
    auto& v = owner.view();
    if (e.getDistanceFromDragStart() > 3) moved = true;
    const double f = juce::jlimit(0.0, (double) d->numFrames, xToFrame(e.x));
    switch (drag)
    {
        case Drag::Overview:
            v.viewStart = (double) e.x / getWidth() * d->numFrames - v.viewLen * 0.5;
            owner.clampView();
            break;
        case Drag::Origin:
        {
            // free drag; snaps to a transient when close (Alt = free)
            int64_t fr = (int64_t) f;
            if (!e.mods.isAltDown())
            {
                const int64_t on = ChopEngine::nearestOnset(*d, fr, (int64_t) (v.viewLen / getWidth() * 10.0));
                if (on >= 0) fr = on;
            }
            const double origin = (double) fr / d->sampleRate;
            owner.proc.project.mutate([&](ProjectState& s) { s.sources[(size_t) owner.getSlot()].originOverride = origin; });
            break;
        }
        case Drag::NewSel:
            if (!moved) return;
            {
                const int64_t b = snapped(f);
                v.selStart = std::min(anchorFrame, b);
                v.selEnd = std::max(anchorFrame, b);
                v.selectedCandidate = -1;
            }
            break;
        case Drag::SelStart: v.selStart = juce::jlimit<int64_t>(0, v.selEnd - 1, snapped(f)); break;
        case Drag::SelEnd:   v.selEnd = juce::jlimit<int64_t>(v.selStart + 1, d->numFrames, snapped(f)); break;
        case Drag::MoveSel:
        {
            const int64_t len = v.selEnd - v.selStart;
            const int64_t s = juce::jlimit<int64_t>(0, d->numFrames - len, snapped(f - (double) dragOffset));
            v.selStart = s; v.selEnd = s + len;
            break;
        }
        case Drag::ToPad:
            if (moved)
            {
                if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this))
                {
                    const juce::String desc = "sel:" + juce::String(owner.getSlot()) + ":" + juce::String(v.selStart) + ":" + juce::String(v.selEnd);
                    juce::Image img(juce::Image::ARGB, 90, 26, true);
                    {
                        juce::Graphics ig(img);
                        ig.setColour(owner.proc.project.state().sources[(size_t) owner.getSlot()].colour.withAlpha(0.85f));
                        ig.fillRoundedRectangle(0, 0, 90, 26, 5);
                        ig.setColour(theme::screen);
                        ig.setFont(theme::ui(12.0f, true));
                        ig.drawText("-> PAD", 0, 0, 90, 26, juce::Justification::centred);
                    }
                    c->startDragging(desc, this, juce::ScaledImage(img), true);
                }
                drag = Drag::None;
            }
            return;
        case Drag::CandStart:
        case Drag::CandEnd:
            if (dragCandidate >= 0 && dragCandidate < (int) v.candidates.size())
            {
                auto& c = v.candidates[(size_t) dragCandidate];
                if (drag == Drag::CandStart) c.start = juce::jlimit<int64_t>(0, c.end - 1, snapped(f));
                else c.end = juce::jlimit<int64_t>(c.start + 1, d->numFrames, snapped(f));
                v.selStart = c.start; v.selEnd = c.end;
            }
            break;
        case Drag::None: return;
    }
    owner.selectionChanged();
    repaint();
}

void WaveCanvas::mouseUp(const juce::MouseEvent& e)
{
    const auto d = owner.data();
    if (d == nullptr) return;
    auto& v = owner.view();
    const auto was = drag;
    drag = Drag::None;
    switch (was)
    {
        case Drag::NewSel:
            if (!moved) // click = play from here
            {
                v.selStart = v.selEnd = -1;
                v.selectedCandidate = -1;
                owner.selectionChanged();
                owner.auditionFrom(snapped(xToFrame(e.x)));
            }
            else
            {
                if (owner.editingPad() >= 0) { owner.proc.ui.editingPad = -1; owner.refresh(); } // a new region leaves pad edit
                if (owner.proc.ui.autoAudition) owner.auditionSelection();
            }
            break;
        case Drag::SelStart: case Drag::SelEnd: case Drag::MoveSel:
            owner.commitPadEdit();
            if (owner.proc.ui.autoAudition) owner.auditionSelection();
            break;
        case Drag::CandStart: case Drag::CandEnd:
            if (owner.proc.ui.autoAudition) owner.auditionSelection();
            break;
        case Drag::ToPad:
            if (!moved) owner.auditionSelection(); // click inside the selection = listen to it
            break;
        default: break;
    }
    repaint();
}

void WaveCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto d = owner.data();
    if (d == nullptr) return;
    auto& v = owner.view();
    const float dy = w.deltaY != 0.0f ? w.deltaY : 0.0f;
    if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY))
    {
        const float delta = std::abs(w.deltaX) > 0 ? w.deltaX : dy;
        v.viewStart -= delta * v.viewLen * 0.25;
    }
    else
    {
        const double anchor = xToFrame(e.x);
        const double factor = std::pow(0.8, dy * 4.0);
        const double minLen = std::max(32.0, getWidth() * 0.05);
        v.viewLen = juce::jlimit(minLen, (double) d->numFrames, v.viewLen * factor);
        v.viewStart = anchor - (double) e.x / getWidth() * v.viewLen;
    }
    owner.clampView();
    repaint();
}

void WaveCanvas::showContextMenu(const juce::MouseEvent& e)
{
    const auto d = owner.data();
    auto& v = owner.view();
    juce::PopupMenu m;
    int edge = 0;
    const int cand = candidateAt(e.getPosition(), edge);
    const bool hasSel = v.selStart >= 0 && v.selEnd > v.selStart;
    m.addItem(1, "Assign selection to next free pad  (Enter)", hasSel);
    juce::PopupMenu padsMenu;
    const auto& st = owner.proc.project.state();
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        const int pad = owner.proc.ui.bank * kPadsPerBank + i;
        padsMenu.addItem(100 + pad, padLabel(pad) + (st.pads[(size_t) pad].assigned ? "  (replace " + st.pads[(size_t) pad].name + ")" : juce::String()), hasSel);
    }
    m.addSubMenu("Assign selection to pad", padsMenu, hasSel);
    m.addSeparator();
    m.addItem(2, "Zoom to selection  (Z)", hasSel);
    m.addItem(3, "Zoom out (whole file)");
    m.addItem(4, "Set beat 1 (bar start) here");
    if (cand >= 0) { m.addSeparator(); m.addItem(5, "Remove candidate " + juce::String(cand + 1)); }
    const double f = xToFrame(e.x);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this, f, cand, d](int r)
    {
        auto& vv = owner.view();
        if (r == 1) owner.quickAssign();
        else if (r >= 100) owner.proc.assignToPad(r - 100, owner.getSlot(), vv.selStart, vv.selEnd);
        else if (r == 2) owner.zoomToSelection();
        else if (r == 3 && d) { vv.viewStart = 0; vv.viewLen = (double) d->numFrames; }
        else if (r == 4 && d)
        {
            const int64_t on = ChopEngine::nearestOnset(*d, (int64_t) f, (int64_t) (vv.viewLen / getWidth() * 10.0));
            const double origin = (double) (on >= 0 ? on : (int64_t) f) / d->sampleRate;
            owner.proc.project.edit("Set beat 1", [&](ProjectState& s) { s.sources[(size_t) owner.getSlot()].originOverride = origin; });
        }
        else if (r == 5 && cand < (int) vv.candidates.size())
        {
            vv.candidates.erase(vv.candidates.begin() + cand);
            vv.selectedCandidate = -1;
        }
        owner.refresh();
    });
}

// =============================================================================================
// SourceView
// =============================================================================================

SourceView::SourceView(HPSamplerProcessor& p) : proc(p)
{
    setWantsKeyboardFocus(true);
    addAndMakeVisible(canvas);
    for (auto* b : { &loadBtn, &clearBtn, &anchorBtn, &bpmBtn, &keyBtn, &halfBtn, &doubleBtn, &waveBtn, &specBtn, &splitBtn,
                     &runBtn, &mapBtn, &acceptBtn, &clearCandBtn, &sMinus, &sPlus, &eMinus, &ePlus, &zoomSelBtn, &zoomAllBtn,
                     &loopBtn, &autoBtn, &assignBtn, &assignModeBtn })
    {
        addAndMakeVisible(*b);
        b->setWantsKeyboardFocus(false);
    }
    for (auto* l : { &title, &info, &candInfo, &selInfo, &amountLabel }) addAndMakeVisible(*l);
    title.setFont(theme::ui(15.0f, true));
    info.setFont(theme::ui(12.0f));
    info.setColour(juce::Label::textColourId, theme::textDim);
    selInfo.setFont(theme::mono(12.0f));
    candInfo.setFont(theme::ui(12.0f));
    candInfo.setColour(juce::Label::textColourId, theme::textDim);
    amountLabel.setFont(theme::ui(11.0f));
    amountLabel.setColour(juce::Label::textColourId, theme::textDim);
    amountLabel.setJustificationType(juce::Justification::centredRight);

    loadBtn.onClick = [this] { openFile(); };
    clearBtn.onClick = [this]
    {
        const auto pads = proc.project.padsUsingSource(slot);
        if (pads.empty()) { proc.clearSource(slot); return; }
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Clear SRC " + juce::String(slot + 1),
            juce::String((int) pads.size()) + " pad(s) use this source. Clearing removes them and their pattern events (Undo restores).",
            "Clear", "Cancel", this, juce::ModalCallbackFunction::create([this](int r) { if (r == 1) proc.clearSource(slot); }));
    };
    anchorBtn.setTooltip("Take this source's BPM and key as the project target. Other sources will follow it (SYNC).");
    anchorBtn.onClick = [this] { proc.useSourceAsAnchor(slot); };
    bpmBtn.setTooltip("Source tempo (detected / your value). Click to correct it.");
    bpmBtn.onClick = [this] { showBpmMenu(); };
    keyBtn.setTooltip("Source key (detected / your value). Click to correct it.");
    keyBtn.onClick = [this] { showKeyMenu(); };
    halfBtn.setTooltip("Source tempo is half of what was detected");
    doubleBtn.setTooltip("Source tempo is double of what was detected");
    halfBtn.onClick = [this]
    {
        const double b = effectiveSourceBpm(proc.project.state().sources[(size_t) slot], data().get());
        if (b > 20.0) proc.project.edit("Source BPM /2", [&](ProjectState& s) { s.sources[(size_t) slot].bpmOverride = b / 2.0; });
    };
    doubleBtn.onClick = [this]
    {
        const double b = effectiveSourceBpm(proc.project.state().sources[(size_t) slot], data().get());
        if (b > 20.0) proc.project.edit("Source BPM x2", [&](ProjectState& s) { s.sources[(size_t) slot].bpmOverride = b * 2.0; });
    };
    auto setDisplay = [this](int d) { view().display = d; updateHeader(); canvas.repaint(); };
    waveBtn.onClick = [setDisplay] { setDisplay(0); };
    specBtn.onClick = [setDisplay] { setDisplay(1); };
    splitBtn.onClick = [setDisplay] { setDisplay(2); };

    toolBox.addItem("FIND SPOTS", 1);
    toolBox.addItem("BEAT SLICER", 2);
    toolBox.addItem("EQUAL REGIONS", 3);
    toolBox.addItem("THRESHOLD CHOP", 4);
    toolBox.addItem("GRID CHOP", 5);
    toolBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(toolBox);
    addAndMakeVisible(focusBox);
    addAndMakeVisible(amount);
    amount.setSliderStyle(juce::Slider::LinearHorizontal);
    amount.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 20);
    toolBox.onChange = [this] { updateHeader(); };
    runBtn.onClick = [this] { runChopTool(); };
    mapBtn.setTooltip("Put all candidates on the next free pads (asks first, Undo-able). Occupied pads are never overwritten.");
    mapBtn.onClick = [this]
    {
        auto& v = view();
        if (v.candidates.empty()) return;
        int free = 0;
        for (const auto& p : proc.project.state().pads) if (!p.assigned) ++free;
        const int from = proc.ui.bank * kPadsPerBank;
        const int n = (int) v.candidates.size();
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Map candidates to pads",
            juce::String(n) + " candidate(s) -> free pads from " + padLabel(from) + " (" + juce::String(free) + " free). Occupied pads are skipped.",
            "Map", "Cancel", this, juce::ModalCallbackFunction::create([this, from](int r)
            {
                if (r != 1) return;
                const int done = proc.mapCandidatesToEmptyPads(slot, view().candidates, from);
                proc.setStatus(juce::String(done) + " chop(s) mapped to pads");
                view().candidates.clear();
                view().selectedCandidate = -1;
                refresh();
            }));
    };
    acceptBtn.setTooltip("Keep the candidates as slices of this source (markers) without using pads");
    acceptBtn.onClick = [this]
    {
        auto& v = view();
        if (v.candidates.empty()) return;
        proc.project.edit("Accept slices", [&](ProjectState& s)
        {
            for (const auto& c : v.candidates) s.slices.push_back({ newId(), slot, c.start, c.end, c.origin });
        });
        proc.setStatus(juce::String((int) v.candidates.size()) + " slice(s) accepted");
    };
    clearCandBtn.onClick = [this] { view().candidates.clear(); view().selectedCandidate = -1; refresh(); };

    sMinus.setTooltip("Start earlier (1 ms; Shift 10 ms; Alt 1 frame)");
    sPlus.setTooltip("Start later");
    eMinus.setTooltip("End earlier");
    ePlus.setTooltip("End later");
    auto step = []
    {
        const auto m = juce::ModifierKeys::currentModifiers;
        return m.isAltDown() ? -1 : (m.isShiftDown() ? 10 : 1);
    };
    sMinus.onClick = [this, step] { nudge(false, -step()); };
    sPlus.onClick = [this, step] { nudge(false, step()); };
    eMinus.onClick = [this, step] { nudge(true, -step()); };
    ePlus.onClick = [this, step] { nudge(true, step()); };
    snapBox.addItem("SNAP: TRANSIENT", 1);
    snapBox.addItem("SNAP: GRID", 2);
    snapBox.addItem("SNAP: ZERO-X", 3);
    snapBox.addItem("SNAP: OFF", 4);
    snapBox.setTooltip("Magnet for selection edges. Hold Alt while dragging to move freely.");
    addAndMakeVisible(snapBox);
    snapBox.onChange = [this]
    {
        const int id = snapBox.getSelectedId();
        proc.ui.snap = id == 1 ? SnapMode::Transient : id == 2 ? SnapMode::Grid : id == 3 ? SnapMode::ZeroCross : SnapMode::Off;
    };
    zoomSelBtn.onClick = [this] { zoomToSelection(); };
    zoomAllBtn.onClick = [this] { if (auto d = data()) { view().viewStart = 0; view().viewLen = (double) d->numFrames; canvas.repaint(); } };
    loopBtn.setClickingTogglesState(true);
    loopBtn.setTooltip("Loop the selection while listening");
    loopBtn.onClick = [this] { proc.ui.loopAudition = loopBtn.getToggleState(); if (proc.engine.previewFrame.load() >= 0) auditionSelection(); };
    autoBtn.setClickingTogglesState(true);
    autoBtn.setTooltip("Play the selection automatically whenever you change it");
    autoBtn.onClick = [this] { proc.ui.autoAudition = autoBtn.getToggleState(); };
    assignBtn.setTooltip("Put the selection on the next free pad of the current bank");
    assignBtn.onClick = [this] { quickAssign(); };
    assignModeBtn.setTooltip("Then click the pad that should get the selection (no sound)");
    assignModeBtn.onClick = [this]
    {
        auto& v = view();
        if (v.selStart < 0 || v.selEnd <= v.selStart) { proc.setStatus("Select a region first"); return; }
        proc.ui.padMode = UiState::PadMode::Assign;
        proc.setStatus("ASSIGN: click a pad to put the selection on it (Esc = cancel)");
    };
    for (auto* b : { &doneEditBtn, &playPadBtn }) { b->setWantsKeyboardFocus(false); addChildComponent(*b); }
    addChildComponent(editBanner);
    editBanner.setFont(theme::ui(13.0f, true));
    editBanner.setColour(juce::Label::backgroundColourId, theme::screen.withAlpha(0.85f));
    doneEditBtn.onClick = [this] { endPadEdit(); };
    playPadBtn.setTooltip("Play the pad as it sounds (speed, stretch, pitch, envelope)");
    playPadBtn.onClick = [this]
    {
        const int pad = editingPad();
        if (pad < 0) return;
        proc.triggerPad(pad, 0.9f, Engine::kAuditionPointer | 6);
        juce::Component::SafePointer<SourceView> safe(this);
        juce::Timer::callAfterDelay(500, [safe, pad] { if (safe) safe->proc.releasePad(pad, Engine::kAuditionPointer | 6); });
    };
    startTimerHz(30);
}

int SourceView::editingPad() const
{
    const int pad = proc.ui.editingPad;
    if (pad < 0 || pad >= kNumPads) return -1;
    const auto& p = proc.project.state().pads[(size_t) pad];
    return (p.assigned && p.src == slot) ? pad : -1;
}

void SourceView::commitPadEdit()
{
    const int pad = editingPad();
    if (pad < 0) return;
    const auto& v = view();
    const auto& p = proc.project.state().pads[(size_t) pad];
    if (v.selStart < 0 || v.selEnd <= v.selStart || (v.selStart == p.start && v.selEnd == p.end)) return;
    const int64_t s = v.selStart, e = v.selEnd;
    proc.project.edit("Trim " + padLabel(pad), [&](ProjectState& st) { st.pads[(size_t) pad].start = s; st.pads[(size_t) pad].end = e; });
    proc.setStatus(padLabel(pad) + " trimmed: " + juce::String((double) (e - s) / (data() ? data()->sampleRate : 44100.0), 3) + " s");
}

void SourceView::endPadEdit()
{
    if (proc.ui.editingPad < 0) return;
    proc.ui.editingPad = -1;
    refresh();
}

SourceView::~SourceView() { stopTimer(); }

void SourceView::setSlot(int s)
{
    slot = juce::jlimit(0, kNumSources - 1, s);
    markFrame = -1;
    refresh();
}

void SourceView::refresh()
{
    if (const int pad = editingPad(); pad >= 0)
    {
        const auto& p = proc.project.state().pads[(size_t) pad];
        view().selStart = p.start;
        view().selEnd = p.end;
    }
    else if (proc.ui.editingPad >= 0 && proc.ui.editingPad < kNumPads
             && !proc.project.state().pads[(size_t) proc.ui.editingPad].assigned)
        proc.ui.editingPad = -1;
    const bool editing = editingPad() >= 0;
    doneEditBtn.setVisible(editing);
    playPadBtn.setVisible(editing);
    editBanner.setVisible(editing);
    if (editing)
    {
        const int pad = editingPad();
        editBanner.setText("  EDITING " + padLabel(pad) + "  " + proc.project.state().pads[(size_t) pad].name
                           + "  -  drag the white edges, the pad follows (Undo-able)", juce::dontSendNotification);
        editBanner.setColour(juce::Label::textColourId, proc.project.state().sources[(size_t) slot].colour);
    }
    clampView();
    updateHeader();
    canvas.repaint();
}

void SourceView::clampView()
{
    const auto d = data();
    if (d == nullptr) return;
    auto& v = view();
    const double n = (double) d->numFrames;
    if (v.viewLen <= 0.0 || v.viewLen > n) v.viewLen = n;
    v.viewStart = juce::jlimit(0.0, std::max(0.0, n - v.viewLen), v.viewStart);
}

void SourceView::updateHeader()
{
    const auto& st = proc.project.state();
    const auto& s = st.sources[(size_t) slot];
    const auto d = data();
    title.setText("SRC " + juce::String(slot + 1) + "  " + (s.empty() ? juce::String("(empty)") : s.name), juce::dontSendNotification);
    title.setColour(juce::Label::textColourId, s.colour);

    const double bpm = effectiveSourceBpm(s, d.get());
    juce::String bt = "SRC BPM  ";
    if (d == nullptr || !d->analysis.ready) bt << (proc.isSourceLoading(slot) ? "..." : "--");
    else if (bpm > 20.0) bt << juce::String(bpm, 1) << (s.bpmOverride > 20.0 ? "  (yours)" : "  " + juce::String(juce::roundToInt(d->analysis.bpmConfidence * 100)) + "%");
    else if (d->analysis.bpm > 20.0) bt << "?  (~" << juce::String(d->analysis.bpm, 1) << ")";
    else bt << "free";
    bpmBtn.setButtonText(bt);
    const auto key = effectiveSourceKey(s, d.get());
    juce::String kt = "SRC KEY  ";
    if (d == nullptr || !d->analysis.ready) kt << (proc.isSourceLoading(slot) ? "..." : "--");
    else if (key.known()) kt << key.label() << (s.keyOverrideRoot >= 0 ? "  (yours)" : "  " + juce::String(juce::roundToInt(d->analysis.keyConfidence * 100)) + "%");
    else kt << "none";
    keyBtn.setButtonText(kt);

    juce::String inf;
    if (d != nullptr)
    {
        inf << juce::String((double) d->numFrames / d->sampleRate, 2) << " s  " << juce::String(d->sampleRate / 1000.0, 1) << " kHz";
        if (st.settings.anchorSrc == slot) inf << "   |  PROJECT ANCHOR";
        else if (bpm > 20.0)
        {
            const auto sync = st.settings.sync;
            if (sync == SyncMode::Stretch) inf << "   |  pads stretch " << juce::String(bpm, 1) << " -> " << juce::String(st.settings.bpm, 1) << " BPM";
            else if (sync == SyncMode::Repitch) inf << "   |  pads repitch to " << juce::String(st.settings.bpm, 1) << " BPM";
            if (key.known() && st.settings.key.known() && sync == SyncMode::Stretch)
            {
                const int k = keyTransposeSemitones(key, st.settings.key);
                inf << ", key " << (k > 0 ? "+" : "") << juce::String(k) << " st";
            }
        }
        else if (d->analysis.ready) inf << "   |  no reliable BPM: set it (click SRC BPM) or drag the beat-1 flag";
        if (d->analysis.labelInfo.isNotEmpty()) inf << "   |  " << d->analysis.labelInfo;
    }
    info.setText(inf, juce::dontSendNotification);
    anchorBtn.setToggleState(st.settings.anchorSrc == slot, juce::dontSendNotification);
    for (auto* b : { &waveBtn, &specBtn, &splitBtn }) b->setToggleState(false, juce::dontSendNotification);
    (view().display == 0 ? waveBtn : view().display == 1 ? specBtn : splitBtn).setToggleState(true, juce::dontSendNotification);
    const bool have = d != nullptr;
    for (auto* b : { &clearBtn, &anchorBtn, &bpmBtn, &keyBtn, &halfBtn, &doubleBtn, &runBtn, &mapBtn, &acceptBtn, &clearCandBtn,
                     &sMinus, &sPlus, &eMinus, &ePlus, &zoomSelBtn, &zoomAllBtn, &assignBtn, &assignModeBtn })
        b->setEnabled(have || b == &clearBtn ? !s.empty() : false);

    // chop tool parameters
    const int tool = toolBox.getSelectedId();
    focusBox.clear(juce::dontSendNotification);
    focusBox.setVisible(tool == 1 || tool == 2 || tool == 5);
    if (tool == 1)
    {
        focusBox.addItem("HYBRID", 1); focusBox.addItem("ATTACK", 2); focusBox.addItem("TONAL", 3); focusBox.addItem("PHRASE", 4);
        focusBox.setSelectedId(1, juce::dontSendNotification);
        amount.setRange(2, 32, 1);
        amountLabel.setText("max", juce::dontSendNotification);
        runBtn.setButtonText("FIND");
    }
    else if (tool == 2 || tool == 5)
    {
        if (tool == 2) { focusBox.addItem("1/4 beat", 1); focusBox.addItem("1/2 beat", 2); focusBox.addItem("1 beat", 3); focusBox.addItem("2 beats", 4); focusBox.addItem("1 bar", 5); focusBox.setSelectedId(3, juce::dontSendNotification); }
        else { focusBox.addItem("1 bar", 1); focusBox.addItem("1/2", 2); focusBox.addItem("1/4", 3); focusBox.addItem("1/8", 4); focusBox.addItem("1/16", 5); focusBox.setSelectedId(3, juce::dontSendNotification); }
        amount.setRange(1, 32, 1);
        amountLabel.setText(tool == 2 ? "slices" : "", juce::dontSendNotification);
        amount.setVisible(tool == 2);
        runBtn.setButtonText("SLICE");
    }
    else
    {
        amount.setRange(tool == 3 ? 2 : 0, tool == 3 ? 32 : 100, 1);

        amountLabel.setText(tool == 3 ? "parts" : "sens", juce::dontSendNotification);
        runBtn.setButtonText("CHOP");
    }
    if (tool != lastTool) // sensible default per tool
    {
        lastTool = tool;
        amount.setValue(tool == 1 ? 12 : tool == 2 ? 8 : tool == 3 ? 8 : 50, juce::dontSendNotification);
    }
    if (tool != 5) amount.setVisible(true);
    if (tool == 5) amount.setVisible(false);

    const auto& v = view();
    candInfo.setText(v.candidates.empty() ? juce::String("range: selection, or the whole file")
                                          : juce::String((int) v.candidates.size()) + " candidates - click to hear, drag edges, right-click to drop",
                     juce::dontSendNotification);
    mapBtn.setButtonText("-> PADS (" + juce::String((int) v.candidates.size()) + ")");
    selectionChanged();
    snapBox.setSelectedId(proc.ui.snap == SnapMode::Transient ? 1 : proc.ui.snap == SnapMode::Grid ? 2 : proc.ui.snap == SnapMode::ZeroCross ? 3 : 4,
                          juce::dontSendNotification);
    loopBtn.setToggleState(proc.ui.loopAudition, juce::dontSendNotification);
    autoBtn.setToggleState(proc.ui.autoAudition, juce::dontSendNotification);
    resized();
}

void SourceView::selectionChanged()
{
    const auto d = data();
    const auto& v = view();
    if (d == nullptr || v.selStart < 0 || v.selEnd <= v.selStart)
    {
        selInfo.setText(d ? "Drag on the wave to select. Click = play from there. Wheel = zoom." : "", juce::dontSendNotification);
        return;
    }
    const double sr = d->sampleRate;
    juce::String t = "IN " + fmtTime(v.selStart / sr) + "  OUT " + fmtTime(v.selEnd / sr) + "  LEN " + juce::String((v.selEnd - v.selStart) / sr, 3) + " s";
    const auto g = grid();
    if (g.valid()) t << "  =  " << juce::String((v.selEnd - v.selStart) / g.framesPerBeat(), 2) << " beats";
    selInfo.setText(t, juce::dontSendNotification);
}

void SourceView::paint(juce::Graphics& g)
{
    g.fillAll(theme::body);
    const auto col = proc.project.state().sources[(size_t) slot].colour;
    g.setColour(col);
    g.fillRect(0, 0, 4, 32);
}

void SourceView::resized()
{
    auto r = getLocalBounds().reduced(6, 4);
    auto top = r.removeFromTop(30);
    title.setBounds(top.removeFromLeft(220).withTrimmedLeft(4));
    bpmBtn.setBounds(top.removeFromLeft(170).reduced(2));
    halfBtn.setBounds(top.removeFromLeft(30).reduced(2));
    doubleBtn.setBounds(top.removeFromLeft(30).reduced(2));
    keyBtn.setBounds(top.removeFromLeft(150).reduced(2));
    anchorBtn.setBounds(top.removeFromLeft(190).reduced(2));
    clearBtn.setBounds(top.removeFromRight(62).reduced(2));
    loadBtn.setBounds(top.removeFromRight(62).reduced(2));
    top.removeFromRight(8);
    splitBtn.setBounds(top.removeFromRight(54).reduced(2));
    specBtn.setBounds(top.removeFromRight(50).reduced(2));
    waveBtn.setBounds(top.removeFromRight(54).reduced(2));
    info.setBounds(r.removeFromTop(18).withTrimmedLeft(4));

    auto sel = r.removeFromBottom(30);
    auto chop = r.removeFromBottom(30);
    r.removeFromBottom(2);
    canvas.setBounds(r);
    {
        auto b = r.withTrimmedTop(24 + 18).removeFromTop(26).reduced(4, 1);
        doneEditBtn.setBounds(b.removeFromRight(70));
        playPadBtn.setBounds(b.removeFromRight(90).withTrimmedRight(4));
        editBanner.setBounds(b.removeFromLeft(juce::jmin(560, b.getWidth())));
    }

    toolBox.setBounds(chop.removeFromLeft(150).reduced(2));
    if (focusBox.isVisible()) focusBox.setBounds(chop.removeFromLeft(100).reduced(2));
    if (amount.isVisible())
    {
        amountLabel.setBounds(chop.removeFromLeft(42));
        amount.setBounds(chop.removeFromLeft(150).reduced(2));
    }
    runBtn.setBounds(chop.removeFromLeft(64).reduced(2));
    clearCandBtn.setBounds(chop.removeFromRight(60).reduced(2));
    acceptBtn.setBounds(chop.removeFromRight(70).reduced(2));
    mapBtn.setBounds(chop.removeFromRight(110).reduced(2));
    candInfo.setBounds(chop.reduced(6, 0));

    sMinus.setBounds(sel.removeFromLeft(24).reduced(1));
    sPlus.setBounds(sel.removeFromLeft(24).reduced(1));
    selInfo.setBounds(sel.removeFromLeft(juce::jmin(470, sel.getWidth() / 2)).reduced(4, 0));
    eMinus.setBounds(sel.removeFromLeft(24).reduced(1));
    ePlus.setBounds(sel.removeFromLeft(24).reduced(1));
    sel.removeFromLeft(6);
    assignModeBtn.setBounds(sel.removeFromRight(100).reduced(2));
    assignBtn.setBounds(sel.removeFromRight(170).reduced(2));
    snapBox.setBounds(sel.removeFromLeft(140).reduced(2));
    zoomSelBtn.setBounds(sel.removeFromLeft(76).reduced(2));
    zoomAllBtn.setBounds(sel.removeFromLeft(40).reduced(2));
    loopBtn.setBounds(sel.removeFromLeft(52).reduced(2));
    autoBtn.setBounds(sel.removeFromLeft(84).reduced(2));
}

void SourceView::timerCallback()
{
    if (proc.engine.previewSlot.load() == slot && (proc.engine.previewFrame.load() >= 0 || markFrame >= 0)) canvas.repaint();
    if (wasLoading[(size_t) slot] != proc.isSourceLoading(slot)) { wasLoading[(size_t) slot] = proc.isSourceLoading(slot); refresh(); }
}

void SourceView::auditionSelection()
{
    const auto& v = view();
    if (v.selStart < 0 || v.selEnd <= v.selStart) return;
    proc.engine.previewRange(slot, v.selStart, v.selEnd, proc.ui.loopAudition);
}

void SourceView::auditionFrom(int64_t frame)
{
    const auto d = data();
    if (d == nullptr) return;
    markFrame = -1;
    proc.engine.previewRange(slot, frame, d->numFrames, false);
}

void SourceView::stopAudition()
{
    proc.engine.stopPreview();
    markFrame = -1;
}

void SourceView::quickAssign()
{
    const auto& v = view();
    if (v.selStart < 0 || v.selEnd <= v.selStart) { proc.setStatus("Select a region first (drag on the waveform)"); return; }
    proc.assignToNextEmptyPad(slot, v.selStart, v.selEnd);
}

void SourceView::zoomToSelection()
{
    auto& v = view();
    if (v.selStart < 0 || v.selEnd <= v.selStart) return;
    const double len = (double) (v.selEnd - v.selStart);
    v.viewLen = len * 1.25;
    v.viewStart = (double) v.selStart - len * 0.125;
    clampView();
    canvas.repaint();
}

void SourceView::setSelection(int64_t s, int64_t e, bool audition)
{
    auto& v = view();
    v.selStart = s;
    v.selEnd = e;
    selectionChanged();
    canvas.repaint();
    if (audition) auditionSelection();
}

void SourceView::nudge(bool end, int64_t ms)
{
    const auto d = data();
    auto& v = view();
    if (d == nullptr || v.selStart < 0) return;
    const int64_t frames = juce::ModifierKeys::currentModifiers.isAltDown() ? (ms < 0 ? -1 : 1) : (int64_t) std::llround(ms * 0.001 * d->sampleRate);
    if (end) v.selEnd = juce::jlimit<int64_t>(v.selStart + 1, d->numFrames, v.selEnd + frames);
    else v.selStart = juce::jlimit<int64_t>(0, v.selEnd - 1, v.selStart + frames);
    if (v.selectedCandidate >= 0 && v.selectedCandidate < (int) v.candidates.size())
    {
        v.candidates[(size_t) v.selectedCandidate].start = v.selStart;
        v.candidates[(size_t) v.selectedCandidate].end = v.selEnd;
    }
    selectionChanged();
    commitPadEdit();
    canvas.repaint();
    // listen around the edge that moved
    if (end) proc.engine.previewRange(slot, std::max<int64_t>(v.selStart, v.selEnd - (int64_t) (0.4 * d->sampleRate)), v.selEnd, false);
    else proc.engine.previewRange(slot, v.selStart, std::min<int64_t>(v.selEnd, v.selStart + (int64_t) (0.4 * d->sampleRate)), false);
}

void SourceView::runChopTool()
{
    const auto d = data();
    if (d == nullptr) return;
    auto& v = view();
    const bool hasSel = v.selStart >= 0 && v.selEnd > v.selStart;
    const int64_t from = hasSel ? v.selStart : 0, to = hasSel ? v.selEnd : d->numFrames;
    const auto g = grid();
    const int tool = toolBox.getSelectedId();
    std::vector<ChopCandidate> c;
    if (tool == 1)
    {
        if (!d->analysis.ready) { proc.setStatus("Analysis is still running..."); return; }
        SpotParams p;
        const int f = focusBox.getSelectedId();
        p.focus = f == 2 ? SpotFocus::Attack : f == 3 ? SpotFocus::Tonal : f == 4 ? SpotFocus::Phrase : SpotFocus::Hybrid;
        p.maxSpots = (int) amount.getValue();
        c = ChopEngine::findSpots(*d, from, to, p, g);
        if (c.empty()) proc.setStatus("No confident spots found - try another focus or select by hand");
    }
    else if (tool == 2)
    {
        if (!g.valid()) { proc.setStatus("BEAT SLICER needs the source BPM: set it on SRC BPM (or use EQUAL REGIONS)"); return; }
        static const double beats[] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
        c = ChopEngine::beatSlicer(*d, hasSel ? v.selStart : (int64_t) v.viewStart, (int) amount.getValue(), beats[juce::jlimit(0, 4, focusBox.getSelectedId() - 1)], g);
    }
    else if (tool == 3) c = ChopEngine::equalRegions(from, to, (int) amount.getValue());
    else if (tool == 4)
    {
        if (!d->analysis.ready) { proc.setStatus("Analysis is still running..."); return; }
        c = ChopEngine::threshold(*d, from, to, (float) amount.getValue() / 100.0f, 0.06);
    }
    else
    {
        if (!g.valid()) { proc.setStatus("GRID CHOP needs the source BPM: set it on SRC BPM"); return; }
        static const GridDiv divs[] = { GridDiv::Bar, GridDiv::Half, GridDiv::Quarter, GridDiv::Eighth, GridDiv::Sixteenth };
        c = ChopEngine::gridChop(*d, from, to, g, divs[juce::jlimit(0, 4, focusBox.getSelectedId() - 1)]);
    }
    if (c.size() > (size_t) kNumPads) c.resize((size_t) kNumPads);
    v.candidates = std::move(c);
    v.selectedCandidate = -1;
    if (!v.candidates.empty()) proc.setStatus(juce::String((int) v.candidates.size()) + " candidates. Nothing is changed until you press -> PADS or ACCEPT.");
    refresh();
}

void SourceView::showBpmMenu()
{
    const auto d = data();
    if (d == nullptr) return;
    const auto& s = proc.project.state().sources[(size_t) slot];
    juce::PopupMenu m;
    m.addItem(1, "Type source BPM...");
    if (d->analysis.bpm > 20.0)
    {
        m.addItem(2, "Use detected " + juce::String(d->analysis.bpm, 2) + " (" + juce::String(juce::roundToInt(d->analysis.bpmConfidence * 100)) + "% sure)");
        m.addItem(3, "Detected x2 = " + juce::String(d->analysis.bpm * 2.0, 2));
        m.addItem(4, "Detected /2 = " + juce::String(d->analysis.bpm / 2.0, 2));
    }
    m.addItem(5, "From selection: it is 1 bar", view().selEnd > view().selStart);
    m.addItem(6, "From selection: it is 2 bars", view().selEnd > view().selStart);
    m.addItem(7, "From selection: it is 4 beats... (custom)", view().selEnd > view().selStart);
    m.addSeparator();
    m.addItem(8, "Reset to analysis", s.bpmOverride > 0 || s.originOverride >= 0);
    m.addItem(9, "Set beat 1 to the selection start", view().selStart >= 0);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&bpmBtn), [this, d](int r)
    {
        auto setBpm = [this](double b, double origin = -2.0)
        {
            proc.project.edit("Source BPM", [&](ProjectState& st)
            {
                st.sources[(size_t) slot].bpmOverride = juce::jlimit(20.0, 400.0, b);
                if (origin > -1.5) st.sources[(size_t) slot].originOverride = origin;
            });
        };
        const auto& v = view();
        const double selSec = (double) (v.selEnd - v.selStart) / d->sampleRate;
        const double selStartSec = (double) v.selStart / d->sampleRate;
        if (r == 1 || r == 7)
        {
            auto* w = new juce::AlertWindow(r == 1 ? "Source BPM" : "Selection length", r == 1 ? "Tempo of this recording:" : "How many beats is the selection?", juce::MessageBoxIconType::NoIcon, this);
            w->addTextEditor("v", r == 1 ? juce::String(effectiveSourceBpm(proc.project.state().sources[(size_t) slot], d.get()) > 0 ? effectiveSourceBpm(proc.project.state().sources[(size_t) slot], d.get()) : 120.0, 2) : "4");
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, r, setBpm, selSec, selStartSec](int res)
            {
                if (res == 1)
                {
                    const double val = w->getTextEditorContents("v").getDoubleValue();
                    if (r == 1 && val > 20.0) setBpm(val);
                    if (r == 7 && val > 0.0 && selSec > 0.05) setBpm(60.0 * val / selSec, selStartSec);
                }
            }), true);
        }
        else if (r == 2) setBpm(d->analysis.bpm);
        else if (r == 3) setBpm(d->analysis.bpm * 2.0);
        else if (r == 4) setBpm(d->analysis.bpm / 2.0);
        else if (r == 5 && selSec > 0.05) setBpm(240.0 / selSec, selStartSec);
        else if (r == 6 && selSec > 0.05) setBpm(480.0 / selSec, selStartSec);
        else if (r == 8) proc.project.edit("Reset source BPM", [&](ProjectState& st) { st.sources[(size_t) slot].bpmOverride = 0; st.sources[(size_t) slot].originOverride = -1; });
        else if (r == 9) proc.project.edit("Set beat 1", [&](ProjectState& st) { st.sources[(size_t) slot].originOverride = selStartSec; });
    });
}

void SourceView::showKeyMenu()
{
    juce::PopupMenu m, minor, major;
    for (int i = 0; i < 12; ++i)
    {
        minor.addItem(100 + i, MusicalKey { i, 0 }.label());
        major.addItem(200 + i, MusicalKey { i, 1 }.label());
    }
    m.addSubMenu("Minor", minor);
    m.addSubMenu("Major", major);
    m.addItem(2, "No key (drums / noise / speech) - never transpose");
    m.addSeparator();
    m.addItem(1, "Reset to analysis");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&keyBtn), [this](int r)
    {
        if (r == 0) return;
        proc.project.edit("Source key", [&](ProjectState& st)
        {
            auto& s = st.sources[(size_t) slot];
            if (r == 1) s.keyOverrideRoot = -2;
            else if (r == 2) s.keyOverrideRoot = -1;
            else { s.keyOverrideRoot = r % 100; s.keyOverrideMode = r >= 200 ? 1 : 0; }
        });
    });
}

void SourceView::openFile()
{
    chooser = std::make_unique<juce::FileChooser>("Load audio into SRC " + juce::String(slot + 1), juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f.existsAsFile()) proc.loadSource(slot, f);
    });
}

void SourceView::filesDropped(const juce::StringArray& files, int, int)
{
    if (!files.isEmpty()) proc.loadSource(slot, juce::File(files[0]));
}

void SourceView::playFromKeyboard()
{
    const auto& v = view();
    if (v.selStart >= 0 && v.selEnd > v.selStart) auditionSelection();
    else if (data()) auditionFrom((int64_t) v.viewStart);
}

bool SourceView::keyPressed(const juce::KeyPress& k)
{
    auto& v = view();
    const auto d = data();
    // Space goes to the editor (one play / stop switch for the whole plug-in)
    if (k == juce::KeyPress::returnKey) { quickAssign(); return true; }
    if (k == juce::KeyPress::escapeKey) { stopAudition(); proc.ui.padMode = UiState::PadMode::Perform; endPadEdit(); return true; }
    if (k.getTextCharacter() == 'z' || k.getTextCharacter() == 'Z') { zoomToSelection(); return true; }
    if ((k.getTextCharacter() == 'm' || k.getTextCharacter() == 'M') && d)
    {
        // quick chop while listening: each M closes a slice from the previous mark
        const double pf = proc.engine.previewFrame.load();
        if (pf < 0) { proc.setStatus("M marks slices while audio is playing (click the wave to play)"); return true; }
        const int64_t now = ChopEngine::snap(*d, (int64_t) pf, SnapMode::Transient, grid(), GridDiv::Sixteenth, (int64_t) (0.03 * d->sampleRate));
        if (markFrame >= 0 && now > markFrame + 64)
        {
            v.candidates.push_back({ markFrame, now, 1.0f, "Mark" });
            proc.setStatus("Marked slice " + juce::String((int) v.candidates.size()) + "  (-> PADS to map them)");
            updateHeader();
        }
        markFrame = now;
        canvas.repaint();
        return true;
    }
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
    {
        if (v.selectedCandidate >= 0 && v.selectedCandidate < (int) v.candidates.size())
        {
            v.candidates.erase(v.candidates.begin() + v.selectedCandidate);
            v.selectedCandidate = -1;
            refresh();
        }
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey)
    {
        const int dir = k.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1;
        const int ms = k.getModifiers().isShiftDown() ? 10 : 1;
        nudge(k.getModifiers().isCtrlDown(), dir * ms);
        return true;
    }
    return false;
}
} // namespace hps
