#include "analysis/ChopEngine.h"

#include <algorithm>
#include <numeric>

#include "analysis/SourceAnalysis.h"

namespace hps
{
namespace
{
float cosine(const std::array<float, 12>& a, const std::array<float, 12>& b)
{
    float ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < 12; ++i) { ab += a[(size_t) i] * b[(size_t) i]; aa += a[(size_t) i] * a[(size_t) i]; bb += b[(size_t) i] * b[(size_t) i]; }
    return (aa > 1e-9f && bb > 1e-9f) ? ab / std::sqrt(aa * bb) : 1.0f;
}

float rmsAt(const SourceAnalysis& a, int64_t frame)
{
    if (a.rms.empty()) return 0.0f;
    return a.rms[(size_t) juce::jlimit<int64_t>(0, (int64_t) a.rms.size() - 1, frame / a.featureHop)];
}

float meanRms(const SourceAnalysis& a, int64_t s, int64_t e)
{
    if (a.rms.empty() || e <= s) return 0.0f;
    const int64_t i0 = juce::jlimit<int64_t>(0, (int64_t) a.rms.size() - 1, s / a.featureHop);
    const int64_t i1 = juce::jlimit<int64_t>(i0 + 1, (int64_t) a.rms.size(), e / a.featureHop + 1);
    double sum = 0;
    for (int64_t i = i0; i < i1; ++i) sum += a.rms[(size_t) i];
    return (float) (sum / (double) (i1 - i0));
}

// Non-maximum suppression: keep higher scores, drop candidates overlapping a kept one by > 50 %.
std::vector<ChopCandidate> suppress(std::vector<ChopCandidate> c, int maxCount)
{
    std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    std::vector<ChopCandidate> kept;
    for (const auto& x : c)
    {
        bool clash = false;
        for (const auto& k : kept)
        {
            const int64_t ov = std::min(x.end, k.end) - std::max(x.start, k.start);
            const int64_t shorter = std::min(x.end - x.start, k.end - k.start);
            if (ov > 0 && shorter > 0 && (double) ov / (double) shorter > 0.5) { clash = true; break; }
        }
        if (!clash) kept.push_back(x);
        if ((int) kept.size() >= maxCount) break;
    }
    std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return kept;
}

int64_t preRoll(double sr) { return (int64_t) (0.002 * sr); } // keep the very start of the attack

std::vector<ChopCandidate> attackSpots(const SourceData& d, int64_t from, int64_t to, const SpotParams& p, const SourceGrid& g)
{
    const auto& a = d.analysis;
    const double sr = d.sampleRate;
    const float thr = juce::jmap(p.sensitivity, 0.55f, 0.08f);
    std::vector<int64_t> pts;
    std::vector<float> str;
    for (size_t i = 0; i < a.onsets.size(); ++i)
        if (a.onsets[i] >= from && a.onsets[i] < to && a.onsetStrength[i] >= thr) { pts.push_back(a.onsets[i]); str.push_back(a.onsetStrength[i]); }

    std::vector<ChopCandidate> out;
    const int64_t minLen = (int64_t) (p.minSeconds * sr), maxLen = (int64_t) (p.maxSeconds * sr);
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const int64_t s = std::max(from, pts[i] - preRoll(sr));
        int64_t e = to;
        for (size_t k = i + 1; k < pts.size(); ++k)
            if (pts[k] - s >= minLen) { e = pts[k] - preRoll(sr); break; }
        e = std::min(e, s + maxLen);
        if (e - s < minLen) continue;
        // novelty of the attack: louder after than before
        const float before = meanRms(a, std::max<int64_t>(0, s - (int64_t) (0.08 * sr)), s);
        const float after = meanRms(a, s, std::min(e, s + (int64_t) (0.08 * sr)));
        float score = str[i] * (0.6f + 0.4f * juce::jlimit(0.0f, 1.0f, (after - before) / std::max(0.05f, after)));
        if (g.valid())
        {
            const double beatPos = ((double) s - g.originFrame()) / (g.framesPerBeat() / 2.0); // eighths
            const double dist = std::abs(beatPos - std::round(beatPos));
            score *= (float) (0.8 + 0.2 * (1.0 - juce::jlimit(0.0, 1.0, dist * 4.0)));
        }
        out.push_back({ s, e, juce::jlimit(0.0f, 1.0f, score), "Attack" });
    }
    return out;
}

std::vector<ChopCandidate> tonalSpots(const SourceData& d, int64_t from, int64_t to, const SpotParams& p)
{
    const auto& a = d.analysis;
    const double sr = d.sampleRate;
    if (a.chroma.size() < 24) return {};
    const int hop = a.specHop;
    const int c0 = (int) std::max<int64_t>(0, from / hop), c1 = (int) std::min<int64_t>((int64_t) a.chroma.size(), to / hop);
    const int W = juce::jmax(4, (int) (0.35 * sr / hop)); // ~350 ms context each side
    std::vector<float> nov((size_t) std::max(0, c1 - c0), 0.0f);
    for (int c = c0 + W; c < c1 - W; ++c)
    {
        std::array<float, 12> L {}, R {};
        float eL = 0, eR = 0;
        for (int k = 1; k <= W; ++k)
        {
            for (int pc = 0; pc < 12; ++pc) { L[(size_t) pc] += a.chroma[(size_t) (c - k)][(size_t) pc]; R[(size_t) pc] += a.chroma[(size_t) (c + k - 1)][(size_t) pc]; }
            eL += rmsAt(a, (int64_t) (c - k) * hop);
            eR += rmsAt(a, (int64_t) (c + k - 1) * hop);
        }
        const float energy = juce::jlimit(0.0f, 1.0f, std::min(eL, eR) / (float) W * 3.0f);
        nov[(size_t) (c - c0)] = (1.0f - cosine(L, R)) * energy;
    }
    // peaks of novelty = harmonic change boundaries
    std::vector<int64_t> bounds { from };
    const float mx = nov.empty() ? 0.0f : *std::max_element(nov.begin(), nov.end());
    if (mx < 1e-4f) return {};
    const float thr = mx * juce::jmap(p.sensitivity, 0.55f, 0.15f);
    for (int i = 1; i + 1 < (int) nov.size(); ++i)
    {
        if (nov[(size_t) i] < thr) continue;
        bool isMax = true;
        for (int k = -W / 2; k <= W / 2 && isMax; ++k)
            if (k != 0 && i + k >= 0 && i + k < (int) nov.size() && nov[(size_t) (i + k)] > nov[(size_t) i]) isMax = false;
        if (!isMax) continue;
        int64_t f = (int64_t) (c0 + i) * hop;
        const int64_t on = ChopEngine::nearestOnset(d, f, (int64_t) (0.08 * sr));
        bounds.push_back(on >= 0 ? on - preRoll(sr) : f);
    }
    bounds.push_back(to);
    std::sort(bounds.begin(), bounds.end());

    std::vector<ChopCandidate> out;
    const int64_t minLen = (int64_t) (p.minSeconds * sr * 2.0), maxLen = (int64_t) (p.maxSeconds * sr);
    for (size_t i = 0; i + 1 < bounds.size(); ++i)
    {
        const int64_t s = bounds[i], e = std::min(bounds[i + 1], s + maxLen);
        if (e - s < minLen) continue;
        const float energy = meanRms(a, s, e);
        if (energy < 0.05f) continue;
        const int ci = (int) juce::jlimit<int64_t>(0, (int64_t) nov.size() - 1, s / hop - c0);
        const float score = juce::jlimit(0.0f, 1.0f, 0.5f * (nov.empty() ? 0.0f : nov[(size_t) ci] / mx) + 0.5f * energy);
        out.push_back({ s, e, score, "Tonal" });
    }
    return out;
}

std::vector<ChopCandidate> phraseSpots(const SourceData& d, int64_t from, int64_t to, const SpotParams& p)
{
    const auto& a = d.analysis;
    const double sr = d.sampleRate;
    if (a.rms.empty()) return {};
    const int hop = a.featureHop;
    const int i0 = (int) (from / hop), i1 = (int) std::min<int64_t>((int64_t) a.rms.size(), to / hop);
    if (i1 - i0 < 8) return {};
    // smoothed envelope (~60 ms)
    const int sm = juce::jmax(1, (int) (0.03 * sr / hop));
    std::vector<float> env((size_t) (i1 - i0));
    for (int i = i0; i < i1; ++i)
    {
        float s = 0; int n = 0;
        for (int k = -sm; k <= sm; ++k) if (i + k >= 0 && i + k < (int) a.rms.size()) { s += a.rms[(size_t) (i + k)]; ++n; }
        env[(size_t) (i - i0)] = s / (float) juce::jmax(1, n);
    }
    auto sorted = env;
    std::sort(sorted.begin(), sorted.end());
    const float floor = sorted[sorted.size() / 10], peak = sorted[sorted.size() * 95 / 100];
    const float silence = floor + (peak - floor) * juce::jmap(p.sensitivity, 0.30f, 0.10f);
    const int minGap = juce::jmax(2, (int) (0.12 * sr / hop));

    std::vector<ChopCandidate> out;
    int i = 0;
    const int n = (int) env.size();
    while (i < n)
    {
        while (i < n && env[(size_t) i] < silence) ++i;
        if (i >= n) break;
        const int s = i;
        int lastLoud = i, gap = 0;
        while (i < n)
        {
            if (env[(size_t) i] >= silence) { lastLoud = i; gap = 0; }
            else if (++gap >= minGap) break;
            ++i;
        }
        int64_t fs = (int64_t) (i0 + s) * hop, fe = (int64_t) (i0 + lastLoud + sm + 1) * hop;
        const int64_t on = ChopEngine::nearestOnset(d, fs, (int64_t) (0.1 * sr));
        if (on >= 0) fs = std::max(from, on - preRoll(sr));
        fe = std::min({ fe, to, fs + (int64_t) (p.maxSeconds * sr * 2.0) });
        const double len = (double) (fe - fs) / sr;
        if (len >= std::max(0.25, p.minSeconds * 2))
        {
            // phrases of 1..4 s score best; a clean (silent) lead-in helps
            const double lenScore = len < 1.0 ? len : (len <= 4.0 ? 1.0 : 4.0 / len);
            const float body = meanRms(d.analysis, fs, fe);
            out.push_back({ fs, fe, juce::jlimit(0.0f, 1.0f, (float) (0.6 * lenScore + 0.4 * body)), "Phrase" });
        }
    }
    return out;
}
} // namespace

int64_t ChopEngine::nearestOnset(const SourceData& data, int64_t frame, int64_t maxDistance)
{
    const auto& on = data.analysis.onsets;
    if (on.empty()) return -1;
    auto it = std::lower_bound(on.begin(), on.end(), frame);
    int64_t best = -1, bestD = maxDistance + 1;
    if (it != on.end() && *it - frame < bestD) { best = *it; bestD = *it - frame; }
    if (it != on.begin() && frame - *(it - 1) < bestD) { best = *(it - 1); }
    return best;
}

std::vector<ChopCandidate> ChopEngine::findSpots(const SourceData& data, int64_t from, int64_t to,
                                                 const SpotParams& p, const SourceGrid& grid)
{
    if (!data.analysis.ready) return {};
    from = juce::jlimit<int64_t>(0, data.numFrames, from);
    to = juce::jlimit<int64_t>(from, data.numFrames, to);
    std::vector<ChopCandidate> all;
    auto add = [&](std::vector<ChopCandidate> v, float w)
    {
        for (auto& c : v) { c.score *= w; all.push_back(c); }
    };
    switch (p.focus)
    {
        case SpotFocus::Attack: add(attackSpots(data, from, to, p, grid), 1.0f); break;
        case SpotFocus::Tonal:  add(tonalSpots(data, from, to, p), 1.0f); break;
        case SpotFocus::Phrase: add(phraseSpots(data, from, to, p), 1.0f); break;
        case SpotFocus::Hybrid:
            add(attackSpots(data, from, to, p, grid), data.analysis.drumLikelihood > 0.5f ? 1.0f : 0.8f);
            if (!data.analysis.atonal) add(tonalSpots(data, from, to, p), 0.9f);
            add(phraseSpots(data, from, to, p), 0.85f);
            break;
    }
    return suppress(std::move(all), juce::jlimit(1, kNumPads, p.maxSpots));
}

std::vector<ChopCandidate> ChopEngine::beatSlicer(const SourceData& data, int64_t from, int count,
                                                  double beatsEach, const SourceGrid& grid)
{
    std::vector<ChopCandidate> out;
    if (!grid.valid() || count <= 0 || beatsEach <= 0.0) return out;
    const double step = grid.framesPerBeat() * beatsEach;
    // start on the grid position at / just before `from`
    const double rel = ((double) from - grid.originFrame()) / step;
    double pos = grid.originFrame() + std::floor(rel + 1e-6) * step;
    if (pos < 0) pos += step * std::ceil(-pos / step);
    for (int i = 0; i < count; ++i)
    {
        const int64_t s = (int64_t) std::llround(pos + i * step), e = (int64_t) std::llround(pos + (i + 1) * step);
        if (s >= data.numFrames) break;
        out.push_back({ s, std::min(e, data.numFrames), 1.0f, "Beat" });
    }
    return out;
}

std::vector<ChopCandidate> ChopEngine::equalRegions(int64_t from, int64_t to, int count)
{
    std::vector<ChopCandidate> out;
    if (count <= 0 || to <= from) return out;
    const double step = (double) (to - from) / count;
    for (int i = 0; i < count; ++i)
        out.push_back({ from + (int64_t) std::llround(i * step), from + (int64_t) std::llround((i + 1) * step), 1.0f, "Region" });
    return out;
}

std::vector<ChopCandidate> ChopEngine::threshold(const SourceData& data, int64_t from, int64_t to,
                                                 float sensitivity, double minSeconds)
{
    std::vector<int64_t> cuts;
    const float thr = juce::jmap(sensitivity, 0.6f, 0.06f);
    const int64_t minLen = (int64_t) (minSeconds * data.sampleRate);
    for (size_t i = 0; i < data.analysis.onsets.size(); ++i)
    {
        const int64_t o = data.analysis.onsets[i] - preRoll(data.sampleRate);
        if (o < from || o >= to || data.analysis.onsetStrength[i] < thr) continue;
        if (!cuts.empty() && o - cuts.back() < minLen) continue;
        cuts.push_back(o);
    }
    std::vector<ChopCandidate> out;
    for (size_t i = 0; i < cuts.size(); ++i)
    {
        const int64_t e = i + 1 < cuts.size() ? cuts[i + 1] : to;
        if (e - cuts[i] >= minLen / 2) out.push_back({ cuts[i], e, 1.0f, "Threshold" });
    }
    return out;
}

std::vector<ChopCandidate> ChopEngine::gridChop(const SourceData& data, int64_t from, int64_t to,
                                                const SourceGrid& grid, GridDiv div)
{
    std::vector<ChopCandidate> out;
    const int t = gridTicks(div);
    if (!grid.valid() || t <= 0 || to <= from) return out;
    const double step = grid.framesPerBeat() * (double) t / kPPQ;
    double pos = grid.originFrame() + std::ceil(((double) from - grid.originFrame()) / step - 1e-6) * step;
    std::vector<int64_t> cuts { from };
    for (; pos < (double) to - 1.0; pos += step)
        if ((int64_t) pos > from + 16) cuts.push_back((int64_t) std::llround(pos));
    cuts.push_back(to);
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
        if (cuts[i + 1] - cuts[i] > 32) out.push_back({ cuts[i], cuts[i + 1], 1.0f, "Grid" });
    juce::ignoreUnused(data);
    return out;
}

int64_t ChopEngine::snap(const SourceData& data, int64_t frame, SnapMode mode, const SourceGrid& grid,
                         GridDiv div, int64_t maxDistance)
{
    frame = juce::jlimit<int64_t>(0, data.numFrames, frame);
    switch (mode)
    {
        case SnapMode::Off: return frame;
        case SnapMode::ZeroCross:
            return data.audio ? SourceAnalyzer::nearestZeroCrossing(*data.audio, frame, (int64_t) (0.005 * data.sampleRate)) : frame;
        case SnapMode::Transient:
        {
            const int64_t o = nearestOnset(data, frame, maxDistance);
            return o >= 0 ? juce::jmax<int64_t>(0, o - preRoll(data.sampleRate)) : frame;
        }
        case SnapMode::Grid:
        {
            const int t = gridTicks(div);
            if (!grid.valid() || t <= 0) return frame;
            const double step = grid.framesPerBeat() * (double) t / kPPQ;
            const double snapped = grid.originFrame() + std::round(((double) frame - grid.originFrame()) / step) * step;
            if (std::abs(snapped - (double) frame) > (double) maxDistance) return frame;
            return juce::jlimit<int64_t>(0, data.numFrames, (int64_t) std::llround(snapped));
        }
    }
    return frame;
}
} // namespace hps
