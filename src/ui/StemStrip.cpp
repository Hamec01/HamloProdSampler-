#include "ui/StemStrip.h"

namespace hps
{
namespace
{
juce::String minutesText(double seconds)
{
    const int s = juce::roundToInt(seconds);
    if (s < 60) return juce::String(s) + " s";
    return juce::String(s / 60) + " min " + juce::String(s % 60).paddedLeft('0', 2) + " s";
}
} // namespace

StemStrip::StemStrip(HPSamplerProcessor& p) : proc(p)
{
    setRepaintsOnMouseActivity(false);
    startTimerHz(8);
}

float StemStrip::dbToPos(float db)
{
    if (db <= kStemMinDb) return 0.0f;
    if (db >= 0.0f) return 0.75f + 0.25f * juce::jmin(db, kStemMaxDb) / kStemMaxDb;
    return 0.75f * (1.0f - std::sqrt(-db / -kStemMinDb));
}

float StemStrip::posToDb(float pos)
{
    pos = juce::jlimit(0.0f, 1.0f, pos);
    if (pos <= 0.002f) return kStemMinDb;
    if (pos >= 0.75f) return (pos - 0.75f) / 0.25f * kStemMaxDb;
    const float t = 1.0f - pos / 0.75f;
    return kStemMinDb * t * t;
}

juce::Colour StemStrip::stemColour(int stem)
{
    switch (stem)
    {
        case kStemDrums: return juce::Colour(0xffffb347); // amber
        case kStemBass:  return juce::Colour(0xffb27cff); // violet
        case kStemVox:   return juce::Colour(0xff3fd48c); // green
        default:         return juce::Colour(0xff4ea4ff); // blue
    }
}

StemStrip::Mode StemStrip::mode() const
{
    using S = StemSeparator::State;
    if (proc.project.sourceDataForSlot(slot) == nullptr || proc.isSourceLoading(slot)) return Mode::NoSource;
    switch (proc.stems.state(slot))
    {
        case S::Separating: case S::Loading: return Mode::Working;
        case S::Ready: return Mode::Ready;
        default: break;
    }
    return proc.stemsCached(slot) ? Mode::Cached : Mode::Offer;
}

void StemStrip::refresh()
{
    const auto m = mode();
    juce::String tip;
    switch (m)
    {
        case Mode::NoSource: tip = "STEMS: load a source first"; break;
        case Mode::Offer:
            tip = "STEMS (optional): split this source into DRUMS / BASS / VOX / INST to rebalance it, like iZotope Music Rebalance. "
                  "Click to start - a heavy job, you will be asked first.";
            if (proc.stems.state(slot) == StemSeparator::State::Failed) tip = "Last split failed: " + proc.stems.error(slot) + "\n" + tip;
            break;
        case Mode::Cached: tip = "STEMS of this file are on disk already - click to load them (no new split)."; break;
        case Mode::Working: tip = "Splitting into stems in the background (HPStems). Click to cancel."; break;
        case Mode::Ready:
            tip = "Drag a cell up / down = level (Shift = fine), click = mute, double-click = 0 dB, right-click = solo / reset / delete. "
                  "The round button left = A/B with the original. Ctrl+Z undoes level changes.";
            break;
    }
    setTooltip(tip);
    lastMode = m;
    repaint();
}

void StemStrip::timerCallback()
{
    const auto m = mode();
    const float pr = proc.stems.progress(slot);
    if (m != lastMode || (m == Mode::Working && std::abs(pr - lastProgress) > 0.001f))
    {
        lastProgress = pr;
        refresh();
    }
}

juce::Rectangle<float> StemStrip::powerArea() const
{
    auto r = getLocalBounds().toFloat();
    return r.removeFromLeft(juce::jmin(22.0f, r.getHeight())).reduced(2.0f, 3.0f);
}

juce::Rectangle<float> StemStrip::cellArea(int stem) const
{
    auto r = getLocalBounds().toFloat().reduced(0.0f, 1.0f);
    r.removeFromLeft(juce::jmin(22.0f, r.getHeight() + 2.0f) + 3.0f);
    const float gap = 4.0f;
    const float w = juce::jmin(46.0f, (r.getWidth() - gap * (kNumStems - 1)) / (float) kNumStems);
    return { r.getX() + (float) stem * (w + gap), r.getY(), w, r.getHeight() };
}

int StemStrip::cellAt(juce::Point<float> p) const
{
    for (int k = 0; k < kNumStems; ++k)
        if (cellArea(k).expanded(2.0f, 0.0f).contains(p)) return k;
    return -1;
}

juce::String StemStrip::levelText(int stem) const
{
    const auto& src = proc.project.state().sources[(size_t) slot];
    if (src.stemMute[(size_t) stem]) return "MUTE";
    const float db = src.stemDb[(size_t) stem];
    if (db <= kStemMinDb) return "OFF";
    return (db > 0.05f ? "+" : "") + juce::String(db, std::abs(db) < 9.95f ? 1 : 0);
}

// --------------------------------------------------------------------------------------------

void StemStrip::drawIcon(juce::Graphics& g, int stem, juce::Rectangle<float> r, juce::Colour c) const
{
    const float s = juce::jmin(r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre(s, s);
    const float lw = juce::jmax(1.2f, s * 0.09f);
    g.setColour(c);
    juce::Path p;
    switch (stem)
    {
        case kStemDrums: // snare: shell + top + two sticks
        {
            auto body = r.reduced(s * 0.12f, s * 0.22f).withTrimmedTop(s * 0.12f);
            g.drawEllipse(body.withHeight(body.getHeight() * 0.38f), lw);
            g.drawLine(body.getX(), body.getY() + body.getHeight() * 0.19f, body.getX(), body.getBottom() - body.getHeight() * 0.12f, lw);
            g.drawLine(body.getRight(), body.getY() + body.getHeight() * 0.19f, body.getRight(), body.getBottom() - body.getHeight() * 0.12f, lw);
            p.addCentredArc(body.getCentreX(), body.getBottom() - body.getHeight() * 0.12f, body.getWidth() * 0.5f, body.getHeight() * 0.19f,
                            0.0f, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi * 3.0f, true);
            g.strokePath(p, juce::PathStrokeType(lw));
            g.drawLine(r.getX() + s * 0.2f, r.getY() + s * 0.08f, r.getCentreX() - s * 0.02f, r.getY() + s * 0.38f, lw);
            g.drawLine(r.getRight() - s * 0.2f, r.getY() + s * 0.08f, r.getCentreX() + s * 0.02f, r.getY() + s * 0.38f, lw);
            break;
        }
        case kStemBass: // bass guitar: body + long neck + head
        {
            const float cx = r.getX() + s * 0.36f, cy = r.getBottom() - s * 0.30f;
            g.fillEllipse(cx - s * 0.20f, cy - s * 0.14f, s * 0.30f, s * 0.30f);
            g.fillEllipse(cx - s * 0.06f, cy - s * 0.24f, s * 0.22f, s * 0.22f);
            g.drawLine(cx, cy, r.getRight() - s * 0.16f, r.getY() + s * 0.16f, lw * 1.2f);
            g.fillRoundedRectangle(r.getRight() - s * 0.24f, r.getY() + s * 0.04f, s * 0.14f, s * 0.14f, s * 0.03f);
            break;
        }
        case kStemVox: // microphone
        {
            auto head = juce::Rectangle<float>(s * 0.30f, s * 0.46f).withCentre({ r.getCentreX(), r.getY() + s * 0.32f });
            g.fillRoundedRectangle(head, s * 0.15f);
            p.addCentredArc(r.getCentreX(), r.getY() + s * 0.44f, s * 0.25f, s * 0.22f, 0.0f,
                            juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi * 3.0f, true);
            g.strokePath(p, juce::PathStrokeType(lw));
            g.drawLine(r.getCentreX(), r.getY() + s * 0.66f, r.getCentreX(), r.getBottom() - s * 0.12f, lw);
            g.drawLine(r.getCentreX() - s * 0.16f, r.getBottom() - s * 0.12f, r.getCentreX() + s * 0.16f, r.getBottom() - s * 0.12f, lw);
            break;
        }
        default: // INST: keys
        {
            auto k = r.reduced(s * 0.1f, s * 0.2f);
            g.drawRect(k, lw * 0.9f);
            for (int i = 1; i < 4; ++i) g.drawLine(k.getX() + k.getWidth() * i / 4.0f, k.getY(), k.getX() + k.getWidth() * i / 4.0f, k.getBottom(), lw * 0.7f);
            for (int i = 1; i < 4; ++i)
                g.fillRect(juce::Rectangle<float>(k.getWidth() * 0.13f, k.getHeight() * 0.56f)
                               .withCentre({ k.getX() + k.getWidth() * i / 4.0f, k.getY() + k.getHeight() * 0.28f }));
            break;
        }
    }
}

void StemStrip::paint(juce::Graphics& g)
{
    const auto m = mode();
    const auto& src = proc.project.state().sources[(size_t) slot];

    // A/B power
    {
        const auto pr = powerArea();
        const bool on = m == Mode::Ready && src.stemsOn;
        g.setColour(on ? theme::ok : theme::panelHi);
        g.drawEllipse(pr.reduced(1.0f), 1.5f);
        juce::Path p;
        const auto c = pr.getCentre();
        const float rad = pr.getWidth() * 0.26f;
        p.addCentredArc(c.x, c.y, rad, rad, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
        g.strokePath(p, juce::PathStrokeType(1.4f));
        g.drawLine(c.x, c.y - rad - 1.5f, c.x, c.y - 1.0f, 1.4f);
    }

    const bool live = m == Mode::Ready;
    const bool bypass = live && !src.stemsOn;
    for (int k = 0; k < kNumStems; ++k)
    {
        const auto r = cellArea(k);
        const auto col = stemColour(k);
        const bool muted = src.stemMute[(size_t) k] || src.stemDb[(size_t) k] <= kStemMinDb;
        theme::drawChip(g, r, live ? col : theme::panelHi, live && (hover == k || dragStem == k));
        auto inner = r.reduced(1.5f);
        if (live)
        {
            const float pos = dbToPos(src.stemDb[(size_t) k]);
            const float y = inner.getBottom() - pos * inner.getHeight();
            const auto lineCol = bypass ? theme::textDim : (muted ? theme::rec : col);
            if (!muted)
            {
                g.setColour(lineCol.withAlpha(bypass ? 0.08f : 0.18f));
                g.fillRect(inner.withTop(y));
            }
            // 0 dB tick
            const float y0 = inner.getBottom() - 0.75f * inner.getHeight();
            g.setColour(theme::textDim.withAlpha(0.35f));
            g.drawHorizontalLine(juce::roundToInt(y0), inner.getRight() - 4.0f, inner.getRight());
            g.drawHorizontalLine(juce::roundToInt(y0), inner.getX(), inner.getX() + 4.0f);
            g.setColour(lineCol);
            g.fillRect(juce::Rectangle<float>(inner.getX() + 1.0f, juce::jlimit(inner.getY(), inner.getBottom() - 2.0f, y - 1.0f),
                                              inner.getWidth() - 2.0f, 2.0f));
        }
        const auto iconCol = !live ? theme::textDim.withAlpha(m == Mode::NoSource ? 0.3f : 0.75f)
                                   : (muted || bypass ? theme::textDim.withAlpha(0.55f) : col.brighter(0.25f));
        if (live && (hover == k || dragStem == k))
        {
            g.setColour(theme::text);
            g.setFont(theme::mono(10.5f, true));
            g.drawText(levelText(k), inner.toNearestInt(), juce::Justification::centred);
        }
        else drawIcon(g, k, inner.reduced(inner.getWidth() * 0.16f, 2.0f), iconCol);
        if (live && src.stemMute[(size_t) k])
        {
            g.setColour(theme::rec.withAlpha(0.8f));
            g.drawLine(inner.getX() + 3.0f, inner.getBottom() - 3.0f, inner.getRight() - 3.0f, inner.getY() + 3.0f, 1.4f);
        }
    }

    // overlays
    const auto all = cellArea(0).getUnion(cellArea(kNumStems - 1));
    if (m == Mode::Working)
    {
        const float pr = proc.stems.state(slot) == StemSeparator::State::Loading ? 1.0f : juce::jlimit(0.0f, 1.0f, proc.stems.progress(slot));
        g.setColour(theme::screen.withAlpha(0.82f));
        g.fillRoundedRectangle(all, 4.0f);
        g.setColour(theme::accent.withAlpha(0.35f));
        g.fillRoundedRectangle(all.withWidth(all.getWidth() * pr), 4.0f);
        g.setColour(theme::accent);
        g.setFont(theme::ui(11.5f, true));
        const auto txt = proc.stems.state(slot) == StemSeparator::State::Loading ? juce::String("STEMS: loading...")
                                                                                 : "STEMS " + juce::String(juce::roundToInt(pr * 100.0f)) + "%   x";
        g.drawText(txt, all.toNearestInt(), juce::Justification::centred);
    }
    else if (m == Mode::Offer || m == Mode::Cached)
    {
        const bool failed = proc.stems.state(slot) == StemSeparator::State::Failed;
        if (hover >= 0 || failed)
        {
            g.setColour(theme::screen.withAlpha(0.78f));
            g.fillRoundedRectangle(all, 4.0f);
            g.setColour(failed ? theme::rec : theme::accent);
            g.setFont(theme::ui(11.5f, true));
            g.drawText(failed ? "STEMS FAILED - retry" : (m == Mode::Cached ? "LOAD STEMS" : "SPLIT STEMS..."), all.toNearestInt(), juce::Justification::centred);
        }
    }
}

// --------------------------------------------------------------------------------------------

void StemStrip::askToSplit()
{
    const auto exe = StemSeparator::helperExe();
    if (exe == juce::File())
    {
        auto opts = juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::InfoIcon).withTitle("Stem engine not installed")
            .withMessage("Splitting into stems needs the optional engine (HPStems.exe, onnxruntime.dll, htdemucs.onnx, ~190 MB).\n\n"
                         "Put those files into:\n" + StemSeparator::installHint().getFullPathName()
                         + "\n(or a folder \"HPStems\" next to the plug-in).")
            .withButton("OK").withAssociatedComponent(this);
        juce::AlertWindow::showAsync(opts, nullptr);
        return;
    }
    const auto d = proc.project.sourceDataForSlot(slot);
    if (d == nullptr) return;
    const double seconds = (double) d->numFrames / d->sampleRate;
    const double mb = seconds * d->sampleRate * 2.0 * 4.0 * kNumStems / (1024.0 * 1024.0);
    const auto name = proc.project.state().sources[(size_t) slot].name;
    const juce::String msg =
        "Split SRC " + juce::String(slot + 1) + " \"" + name + "\" (" + minutesText(seconds) + ") into DRUMS / BASS / VOX / INST?\n\n"
        "This is an optional, heavy step (HTDemucs neural network, in its own process):\n"
        "  -  about " + minutesText(proc.stemEstimateSeconds(slot)) + " of full CPU load. The DAW may stutter meanwhile -\n"
        "     better stop playback until it is done.\n"
        "  -  up to ~3 GB of RAM for the helper while it runs.\n"
        "  -  ~" + juce::String(juce::roundToInt(mb)) + " MB on disk (Documents\\HamloProd\\HPSAMPLER\\Stems), reused next time.\n\n"
        "The source file and your pads stay as they are; the stem mix is undoable and can be switched off (A/B).\n"
        "You can cancel at any time (click the progress).";
    auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::WarningIcon, "Split into stems", msg,
                                                             "Split", "Cancel", this);
    juce::AlertWindow::showAsync(opts, [this, s = slot, id = d->id, safe = juce::Component::SafePointer<StemStrip>(this)](int result)
    {
        if (result != 1 || safe == nullptr) return;
        if (proc.project.state().sources[(size_t) s].id != id) return; // replaced meanwhile
        proc.startStems(s);
        refresh();
    });
}

void StemStrip::setLevel(int stem, float db)
{
    const int s = slot;
    proc.project.edit("Stem level", [s, stem, db](ProjectState& st)
    {
        auto& src = st.sources[(size_t) s];
        src.stemDb[(size_t) stem] = juce::jlimit(kStemMinDb, kStemMaxDb, db);
        src.stemsOn = true;
    });
}

void StemStrip::showMenu(int stem)
{
    const auto& src = proc.project.state().sources[(size_t) slot];
    juce::PopupMenu m;
    if (stem >= 0)
    {
        m.addSectionHeader(stemLabel(stem) + "  " + levelText(stem) + " dB");
        m.addItem(1, "Solo " + stemLabel(stem));
        m.addItem(2, "Without " + stemLabel(stem) + " (others 0 dB)");
        m.addItem(3, "0 dB");
    }
    m.addItem(4, "Reset all stems to 0 dB");
    m.addItem(5, "A/B: hear the original", true, !src.stemsOn);
    m.addSeparator();
    m.addItem(6, "Split again (new run)...");
    m.addItem(7, "Delete these stems (frees disk + RAM)");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                    [this, stem, s = slot, safe = juce::Component::SafePointer<StemStrip>(this)](int r)
    {
        if (safe == nullptr || r == 0) return;
        auto setAll = [&](const juce::String& label, std::function<void(SourceSlot&)> fn)
        {
            proc.project.edit(label, [s, fn](ProjectState& st) { fn(st.sources[(size_t) s]); });
        };
        switch (r)
        {
            case 1: setAll("Solo stem", [stem](SourceSlot& x) { for (int k = 0; k < kNumStems; ++k) x.stemMute[(size_t) k] = k != stem;
                                                                  x.stemDb[(size_t) stem] = juce::jmax(0.0f, x.stemDb[(size_t) stem]); x.stemsOn = true; }); break;
            case 2: setAll("Remove stem", [stem](SourceSlot& x) { for (int k = 0; k < kNumStems; ++k) { x.stemMute[(size_t) k] = k == stem; x.stemDb[(size_t) k] = 0.0f; }
                                                                    x.stemsOn = true; }); break;
            case 3: setAll("Stem level", [stem](SourceSlot& x) { x.stemDb[(size_t) stem] = 0.0f; x.stemMute[(size_t) stem] = false; }); break;
            case 4: setAll("Reset stems", [](SourceSlot& x) { x.stemDb.fill(0.0f); x.stemMute.fill(false); x.stemsOn = true; }); break;
            case 5: setAll("Stems A/B", [](SourceSlot& x) { x.stemsOn = !x.stemsOn; }); break;
            case 6: askToSplit(); break;
            case 7:
            {
                auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::WarningIcon, "Delete stems",
                    "Delete the stems of this source from disk? The original is heard again; a new split takes the full time again.",
                    "Delete", "Cancel", this);
                juce::AlertWindow::showAsync(opts, [this, s, safe](int res) { if (res == 1 && safe != nullptr) { proc.deleteStems(s); refresh(); } });
                break;
            }
            default: break;
        }
    });
}

void StemStrip::mouseDown(const juce::MouseEvent& e)
{
    dragStem = -1;
    dragged = false;
    const auto m = mode();
    const auto p = e.position;
    if (m == Mode::NoSource) return;
    if (m == Mode::Working)
    {
        auto opts = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Stems",
                                                                 "Cancel the stem split?", "Cancel split", "Keep going", this);
        juce::AlertWindow::showAsync(opts, [this, s = slot, safe = juce::Component::SafePointer<StemStrip>(this)](int r)
        {
            if (r == 1 && safe != nullptr) { proc.cancelStems(s); refresh(); }
        });
        return;
    }
    if (m == Mode::Offer) { if (e.mods.isPopupMenu()) return; askToSplit(); return; }
    if (m == Mode::Cached) { if (e.mods.isPopupMenu()) { showMenu(-1); return; } proc.loadCachedStems(slot); refresh(); return; }

    // Ready
    if (powerArea().expanded(2.0f).contains(p))
    {
        const int s = slot;
        proc.project.edit("Stems A/B", [s](ProjectState& st) { st.sources[(size_t) s].stemsOn = !st.sources[(size_t) s].stemsOn; });
        return;
    }
    const int k = cellAt(p);
    if (e.mods.isPopupMenu()) { showMenu(k); return; }
    if (k < 0) return;
    dragStem = k;
    dragStartPos = dbToPos(proc.project.state().sources[(size_t) slot].stemDb[(size_t) k]);
    repaint();
}

void StemStrip::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStem < 0) return;
    const int dy = e.getDistanceFromDragStartY();
    if (!dragged && std::abs(dy) < 3) return;
    if (!dragged)
    {
        dragged = true;
        proc.project.beginGesture("Stem level");
        const int s = slot, k = dragStem;
        proc.project.edit("Stem level", [s, k](ProjectState& st) { st.sources[(size_t) s].stemMute[(size_t) k] = false; });
    }
    const float range = e.mods.isShiftDown() ? 600.0f : 150.0f; // pixels for the whole fader
    setLevel(dragStem, posToDb(dragStartPos - (float) dy / range));
    repaint();
}

void StemStrip::mouseUp(const juce::MouseEvent& e)
{
    if (dragStem >= 0)
    {
        if (dragged) proc.project.endGesture();
        else if (e.getNumberOfClicks() == 1 && cellAt(e.position) == dragStem)
        {
            const int s = slot, k = dragStem;
            proc.project.edit("Stem mute", [s, k](ProjectState& st)
            {
                auto& x = st.sources[(size_t) s];
                x.stemMute[(size_t) k] = !x.stemMute[(size_t) k];
                if (!x.stemMute[(size_t) k] && x.stemDb[(size_t) k] <= kStemMinDb) x.stemDb[(size_t) k] = 0.0f;
                x.stemsOn = true;
            });
        }
    }
    dragStem = -1;
    dragged = false;
    repaint();
}

void StemStrip::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (mode() != Mode::Ready) return;
    const int k = cellAt(e.position);
    if (k < 0) return;
    const int s = slot;
    proc.project.edit("Stem level", [s, k](ProjectState& st)
    {
        auto& x = st.sources[(size_t) s];
        x.stemDb[(size_t) k] = 0.0f;
        x.stemMute[(size_t) k] = false;
        x.stemsOn = true;
    });
}

void StemStrip::mouseMove(const juce::MouseEvent& e)
{
    const int k = cellAt(e.position);
    if (k != hover) { hover = k; repaint(); }
}

void StemStrip::mouseExit(const juce::MouseEvent&)
{
    if (hover != -1) { hover = -1; repaint(); }
}
} // namespace hps
