// HPSAMPLER audio-thread benchmark: Engine::process cost per block at small buffers.
// Usage: HPSAMPLER_EngineBench [seconds]  |  HPSAMPLER_EngineBench seconds load <workers> [low]
// Prints, for each block size: mean and worst block time against the block deadline (n / sampleRate).
#include <windows.h>
#undef min
#undef max
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>

#include "audio/Engine.h"
#include "render/PadRenderer.h"

using namespace hps;

namespace
{
constexpr double kSr = 48000.0;

std::unique_ptr<PadSound> noiseSound(int frames, uint32_t seed)
{
    auto s = std::make_unique<PadSound>();
    s->audio.setSize(2, frames);
    for (int c = 0; c < 2; ++c)
    {
        float* d = s->audio.getWritePointer(c);
        for (int i = 0; i < frames; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            d[i] = 0.3f * (float) (int32_t) seed / 2147483648.0f * std::exp(-3.0f * (float) i / (float) frames);
        }
    }
    s->sampleRate = kSr;
    s->fadeIn = 48; s->fadeOut = 192;
    return s;
}

EngineSnapshot benchSnapshot(bool filters)
{
    EngineSnapshot s;
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        auto& p = s.pads[(size_t) i];
        p.assigned = true;
        p.mode = (i % 4 == 3) ? PlayMode::Gate : PlayMode::OneShot;
        p.repeat = (i % 2) ? RepeatMode::Layer : RepeatMode::Retrigger;
        p.src = i % kNumSources;
        p.attackS = (i % 3 == 0) ? 0.002f : 0.0f;
        p.decayS = 0.2f; p.sustain = 0.7f;
        p.releaseS = 0.05f;
        if (filters) { p.hpHz = 80.0f + 10.0f * (float) i; p.lpHz = 12000.0f - 300.0f * (float) i; }
    }
    s.bpm = 140.0;
    s.loopTicks = (int64_t) kTicksPerBar * 4;
    s.metronome = true;
    // a busy MAIN: every 1/16 a few pads, 4 bars
    for (int64_t t = 0; t < s.loopTicks; t += kPPQ / 4)
        for (int k = 0; k < 3; ++k)
            s.events.push_back({ (int) ((t / (kPPQ / 4) + k * 5) % kPadsPerBank), t, kPPQ / 8, 0.8f, -1 });
    std::sort(s.events.begin(), s.events.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    return s;
}

void run(int block, double seconds, bool filters)
{
    Engine e;
    e.prepare(kSr, block);
    e.publishSnapshot(std::make_unique<EngineSnapshot>(benchSnapshot(filters)));
    for (int i = 0; i < kPadsPerBank; ++i) e.publishPadSound(i, noiseSound((int) (kSr * (0.3 + 0.2 * i)), 77u + (uint32_t) i));
    e.setInternalPlaying(true, true);

    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    HostTransport host;
    const int blocks = (int) (seconds * kSr / block);
    double total = 0.0, worst = 0.0;
    int over10 = 0, over50 = 0, maxVoices = 0;
    for (int b = 0; b < blocks; ++b)
    {
        // live MIDI on top of MAIN every ~100 ms
        midi.clear();
        if (b % std::max(1, (int) (0.1 * kSr / block)) == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, kBaseMidiNote + (b % kPadsPerBank), (juce::uint8) 100), block / 2);
        const auto t0 = std::chrono::high_resolution_clock::now();
        e.process(buf, midi, host);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        if (b < 8) continue; // warm-up
        total += us;
        worst = std::max(worst, us);
        int v = 0; for (int p = 0; p < kPadsPerBank; ++p) v += e.padVoices[(size_t) p].load();
        maxVoices = std::max(maxVoices, v);
        const double deadline = block / kSr * 1.0e6;
        if (us > 0.10 * deadline) ++over10;
        if (us > 0.50 * deadline) ++over50;
    }
    const double deadline = block / kSr * 1.0e6;
    const double mean = total / std::max(1, blocks - 8);
    std::cout << "block " << block << (filters ? " +filters" : "          ")
              << "  deadline " << juce::String(deadline, 1) << " us"
              << "  mean " << juce::String(mean, 2) << " us (" << juce::String(100.0 * mean / deadline, 2) << "%)"
              << "  worst " << juce::String(worst, 1) << " us (" << juce::String(100.0 * worst / deadline, 1) << "%)"
              << "  >10% " << over10 << "  >50% " << over50 << "  voices<=" << maxVoices << "\n";
}
} // namespace

// Background load like the pad renderer's workers: stretch renders of a long source, over and over.
struct RenderLoad
{
    std::vector<std::thread> threads;
    std::atomic<bool> stop { false };
    std::atomic<int> renders { 0 };
    RenderLoad(int count, juce::Thread::Priority pr)
    {
        auto src = std::make_shared<juce::AudioBuffer<float>>(2, (int) (kSr * 12.0));
        for (int c = 0; c < 2; ++c)
        {
            uint32_t seed = 9u + (uint32_t) c;
            float* d = src->getWritePointer(c);
            for (int i = 0; i < src->getNumSamples(); ++i) { seed = seed * 1664525u + 1013904223u; d[i] = 0.2f * (float) (int32_t) seed / 2147483648.0f; }
        }
        for (int t = 0; t < count; ++t)
            threads.emplace_back([this, src, pr]
            {
                applyPriority(pr);
                PadRenderJob j;
                j.audio = src; j.sourceRate = j.targetRate = kSr;
                j.start = 0; j.end = src->getNumSamples();
                j.sync = SyncMode::Stretch; j.timeRatio = 1.13; j.transpose = 2.0f;
                while (!stop.load()) { PadRenderer::render(j); ++renders; }
            });
    }
    ~RenderLoad() { stop = true; for (auto& t : threads) t.join(); }
    static void applyPriority(juce::Thread::Priority pr)
    {
        SetThreadPriority(GetCurrentThread(), pr == juce::Thread::Priority::low ? THREAD_PRIORITY_BELOW_NORMAL
                                              : pr == juce::Thread::Priority::background ? THREAD_PRIORITY_IDLE : THREAD_PRIORITY_NORMAL);
    }
};

int main(int argc, char** argv)
{
    const double seconds = argc > 1 ? juce::String(argv[1]).getDoubleValue() : 20.0;
    const juce::String mode = argc > 2 ? juce::String(argv[2]) : juce::String();
    if (mode == "offline") // the MAIN render HPSAMPLER sends to HPDG: 16 bars at 90 BPM
    {
        auto snap = benchSnapshot(true);
        snap.bpm = 90.0;
        snap.loopTicks = (int64_t) kTicksPerBar * 16;
        std::vector<EngineEvent> ev;
        for (int rep = 0; rep < 4; ++rep) for (auto e : snap.events) { e.tick += rep * (int64_t) kTicksPerBar * 4; ev.push_back(e); }
        snap.events = ev;
        std::vector<std::unique_ptr<PadSound>> owned;
        std::array<const PadSound*, kNumPads> sounds {};
        for (int i = 0; i < kPadsPerBank; ++i) { owned.push_back(noiseSound((int) (kSr * (0.3 + 0.2 * i)), 5u + (uint32_t) i)); sounds[(size_t) i] = owned.back().get(); }
        juce::AudioBuffer<float> out;
        const auto t0 = std::chrono::high_resolution_clock::now();
        Engine::renderOffline(snap, sounds, kSr, out, 0.0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        std::cout << "offline MAIN render: " << juce::String(out.getNumSamples() / kSr, 1) << " s of audio in " << juce::String(ms, 0) << " ms\n";
        return 0;
    }
    if (mode == "load") // audio thread at a DAW's priority, render workers next to it
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const int workers = argc > 3 ? juce::String(argv[3]).getIntValue() : 6;
        const auto pr = argc > 4 && juce::String(argv[4]) == "low" ? juce::Thread::Priority::low : juce::Thread::Priority::normal;
        std::cout << "render load: " << workers << " workers, " << (pr == juce::Thread::Priority::low ? "below normal" : "normal") << " priority\n";
        RenderLoad load(workers, pr);
        for (const int block : { 32, 64, 128 })
            run(block, seconds, true);
        std::cout << "renders done meanwhile: " << load.renders.load() << "\n";
        return 0;
    }
    for (const bool f : { false, true })
        for (const int block : { 16, 32, 64, 128, 256, 512 })
            run(block, seconds, f);
    return 0;
}
