#include "stems/Stems.h"

namespace hps
{
juce::String stemLabel(int stem)
{
    static const char* n[kNumStems] = { "DRUMS", "BASS", "VOX", "INST" };
    return stem >= 0 && stem < kNumStems ? n[stem] : "";
}

juce::String stemFileName(int stem)
{
    static const char* n[kNumStems] = { "drums", "bass", "vocals", "other" };
    return stem >= 0 && stem < kNumStems ? n[stem] : "";
}

std::shared_ptr<juce::AudioBuffer<float>> mixStems(const StemSet& set, const SourceSlot& slot, int64_t frames)
{
    auto out = std::make_shared<juce::AudioBuffer<float>>(2, (int) frames);
    out->clear();
    for (int k = 0; k < kNumStems; ++k)
    {
        const auto& b = set.stems[(size_t) k];
        if (b == nullptr || slot.stemMute[(size_t) k] || slot.stemDb[(size_t) k] <= kStemMinDb) continue;
        const float g = (float) dbToGain(slot.stemDb[(size_t) k]);
        const int n = (int) juce::jmin<int64_t>(frames, b->getNumSamples());
        for (int c = 0; c < 2; ++c)
            juce::FloatVectorOperations::addWithMultiply(out->getWritePointer(c), b->getReadPointer(juce::jmin(c, b->getNumChannels() - 1)), g, n);
    }
    return out;
}

// --------------------------------------------------------------------------------------------

juce::File StemSeparator::installHint()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HamloProd/HPSAMPLER/HPStems");
}

juce::File StemSeparator::helperExe()
{
    juce::Array<juce::File> dirs;
    const auto env = juce::SystemStats::getEnvironmentVariable("HPSTEMS_DIR", {});
    if (env.isNotEmpty()) dirs.add(juce::File(env));
    // the plug-in's own module (the .vst3 dll / standalone exe), not the host
    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    dirs.add(self.getSiblingFile("HPStems"));
    dirs.add(self.getParentDirectory().getParentDirectory().getChildFile("Resources/HPStems")); // VST3 bundle
    dirs.add(self.getParentDirectory());
    dirs.add(installHint());
    for (const auto& d : dirs)
    {
        const auto exe = d.getChildFile("HPStems.exe");
        if (exe.existsAsFile() && d.getChildFile("htdemucs.onnx").existsAsFile()) return exe;
    }
    return {};
}

juce::File StemSeparator::cacheDirFor(const SourceSlot& slot, const SourceData& data)
{
    const juce::File f(slot.filePath);
    juce::String key = slot.filePath.isNotEmpty() ? slot.filePath : data.id;
    if (f.existsAsFile()) key << "|" << f.getSize() << "|" << f.getLastModificationTime().toMilliseconds();
    key << "|" << data.numFrames << "|" << juce::String(data.sampleRate, 1) << "|htdemucs";
    const auto name = juce::File::createLegalFileName(slot.name.isNotEmpty() ? slot.name : "source").substring(0, 24).trim();
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("HamloProd/HPSAMPLER/Stems")
        .getChildFile(name + "_" + juce::String::toHexString(key.hashCode64()));
}

bool StemSeparator::hasCache(const juce::File& dir)
{
    if (!dir.getChildFile("stems.json").existsAsFile()) return false;
    for (int k = 0; k < kNumStems; ++k)
        if (!dir.getChildFile(stemFileName(k) + ".wav").existsAsFile()) return false;
    return true;
}

// --------------------------------------------------------------------------------------------

struct StemSeparator::Job : juce::Thread
{
    Job(std::shared_ptr<const SourceData> d, juce::File dir_, bool split_)
        : juce::Thread("HPSAMPLER stems"), data(std::move(d)), dir(std::move(dir_)), split(split_) {}

    ~Job() override { cancel(); stopThread(10000); }

    void cancel()
    {
        signalThreadShouldExit();
        const juce::ScopedLock sl(lock);
        if (child != nullptr) child->kill();
    }

    void run() override
    {
        if (split && !runHelper()) return;
        if (threadShouldExit()) return;
        loading = true;
        if (!loadStems()) return;
    }

    bool fail(const juce::String& e) { error = e; return false; }

    bool runHelper()
    {
        const auto exe = StemSeparator::helperExe();
        if (exe == juce::File()) return fail("stem engine not installed (" + StemSeparator::installHint().getFullPathName() + ")");
        if (!dir.createDirectory()) return fail("cannot create " + dir.getFullPathName());
        dir.getChildFile("stems.json").deleteFile();

        // the decoded source, so the stems line up with it frame for frame
        const auto input = dir.getChildFile("input.wav");
        {
            input.deleteFile();
            std::unique_ptr<juce::OutputStream> os(input.createOutputStream().release());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w(os ? wav.createWriterFor(os.get(), data->sampleRate, 2, 32, {}, 0) : nullptr);
            if (w == nullptr) return fail("cannot write " + input.getFullPathName());
            os.release();
            if (!w->writeFromAudioSampleBuffer(*data->audio, 0, data->audio->getNumSamples())) return fail("disk full?");
        }
        if (threadShouldExit()) { input.deleteFile(); return false; }

        juce::StringArray args { exe.getFullPathName(), "--in", input.getFullPathName(), "--out", dir.getFullPathName(),
                                 "--threads", juce::String(juce::jmax(1, juce::SystemStats::getNumPhysicalCpus() - 1)) };
        {
            const juce::ScopedLock sl(lock);
            child = std::make_unique<juce::ChildProcess>();
            if (!child->start(args, juce::ChildProcess::wantStdOut)) { child.reset(); input.deleteFile(); return fail("cannot start HPStems.exe"); }
        }
        juce::String pendingText, lastError;
        bool done = false;
        char buf[1]; // JUCE's read waits until the buffer is full: one byte keeps the progress live
        for (;;)
        {
            const int n = child->readProcessOutput(buf, (int) sizeof(buf));
            if (n <= 0) { if (!child->isRunning()) break; juce::Thread::sleep(20); continue; }
            pendingText += juce::String::fromUTF8(buf, n);
            for (int nl; (nl = pendingText.indexOfChar('\n')) >= 0;)
            {
                const auto line = pendingText.substring(0, nl).trim();
                pendingText = pendingText.substring(nl + 1);
                if (line.startsWith("HPSTEMS PROGRESS ")) progress = line.fromLastOccurrenceOf(" ", false, false).getFloatValue();
                else if (line == "HPSTEMS DONE") done = true;
                else if (line.startsWith("HPSTEMS ERROR ")) lastError = line.fromFirstOccurrenceOf("ERROR ", false, false);
            }
            if (threadShouldExit()) break;
        }
        {
            const juce::ScopedLock sl(lock);
            if (threadShouldExit() && child->isRunning()) child->kill();
            child.reset();
        }
        input.deleteFile();
        if (threadShouldExit()) return false;
        if (!done || !StemSeparator::hasCache(dir)) return fail(lastError.isNotEmpty() ? lastError : juce::String("HPStems stopped without a result"));
        return true;
    }

    bool loadStems()
    {
        auto set = std::make_shared<StemSet>();
        set->sourceId = data->id;
        set->sampleRate = data->sampleRate;
        juce::WavAudioFormat wav;
        for (int k = 0; k < kNumStems; ++k)
        {
            const auto f = dir.getChildFile(stemFileName(k) + ".wav");
            std::unique_ptr<juce::AudioFormatReader> r(wav.createReaderFor(f.createInputStream().release(), true));
            if (r == nullptr) return fail("cannot read " + f.getFullPathName());
            if (std::abs(r->sampleRate - data->sampleRate) > 0.5) return fail("stem cache does not match the source (sample rate)");
            auto b = std::make_shared<juce::AudioBuffer<float>>(2, (int) data->numFrames);
            b->clear();
            r->read(b.get(), 0, (int) juce::jmin<int64_t>(data->numFrames, r->lengthInSamples), 0, true, true);
            if (r->numChannels == 1) b->copyFrom(1, 0, *b, 0, 0, b->getNumSamples());
            set->stems[(size_t) k] = b;
            if (threadShouldExit()) return false;
        }
        result = set;
        return true;
    }

    std::shared_ptr<const SourceData> data;
    juce::File dir;
    bool split;
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> loading { false };
    juce::CriticalSection lock;
    std::unique_ptr<juce::ChildProcess> child;
    juce::String error;
    std::shared_ptr<const StemSet> result;
};

// --------------------------------------------------------------------------------------------

StemSeparator::StemSeparator() = default;

StemSeparator::~StemSeparator()
{
    *alive = false;
    for (auto& j : jobs) j.reset();
}

void StemSeparator::separate(int slot, std::shared_ptr<const SourceData> data, const juce::File& dir) { start(slot, std::move(data), dir, true); }
void StemSeparator::load(int slot, std::shared_ptr<const SourceData> data, const juce::File& dir) { start(slot, std::move(data), dir, false); }

void StemSeparator::start(int slot, std::shared_ptr<const SourceData> data, const juce::File& dir, bool split)
{
    if (slot < 0 || slot >= kNumSources || data == nullptr || data->audio == nullptr) return;
    jobs[(size_t) slot].reset();
    ready[(size_t) slot] = nullptr;
    errors[(size_t) slot] = {};
    ids[(size_t) slot] = data->id;
    states[(size_t) slot] = split ? State::Separating : State::Loading;
    auto job = std::make_unique<Job>(std::move(data), dir, split);
    auto* raw = job.get();
    jobs[(size_t) slot] = std::move(job);

    // a watcher on the message thread: the job reports by finishing
    struct Finish : juce::Timer
    {
        StemSeparator* owner; int slot; Job* job; std::shared_ptr<std::atomic<bool>> alive;
        void timerCallback() override
        {
            if (!*alive) { delete this; return; }
            auto& slotJob = owner->jobs[(size_t) slot];
            if (slotJob.get() != job) { delete this; return; } // replaced / cancelled
            if (job->loading && owner->states[(size_t) slot] == State::Separating)
            {
                owner->states[(size_t) slot] = State::Loading;
                if (owner->onChanged) owner->onChanged(slot);
            }
            if (job->isThreadRunning()) { if (owner->onChanged && owner->states[(size_t) slot] == State::Separating) owner->onChanged(slot); return; }
            if (job->result != nullptr) { owner->ready[(size_t) slot] = job->result; owner->states[(size_t) slot] = State::Ready; }
            else { owner->errors[(size_t) slot] = job->error.isNotEmpty() ? job->error : juce::String("cancelled");
                   owner->states[(size_t) slot] = job->error.isNotEmpty() ? State::Failed : State::None; }
            slotJob.reset();
            if (owner->onChanged) owner->onChanged(slot);
            delete this;
        }
    };
    auto* f = new Finish();
    f->owner = this; f->slot = slot; f->job = raw; f->alive = alive;
    f->startTimer(250);
    raw->startThread(juce::Thread::Priority::low);
    if (onChanged) onChanged(slot);
}

void StemSeparator::cancel(int slot)
{
    if (slot < 0 || slot >= kNumSources) return;
    if (auto& j = jobs[(size_t) slot]) { j->cancel(); j.reset(); }
    if (states[(size_t) slot] == State::Separating || states[(size_t) slot] == State::Loading)
        states[(size_t) slot] = State::None;
    if (onChanged) onChanged(slot);
}

void StemSeparator::forget(int slot)
{
    if (slot < 0 || slot >= kNumSources) return;
    if (auto& j = jobs[(size_t) slot]) { j->cancel(); j.reset(); }
    ready[(size_t) slot] = nullptr;
    states[(size_t) slot] = State::None;
    errors[(size_t) slot] = {};
    ids[(size_t) slot] = {};
}

StemSeparator::State StemSeparator::state(int slot) const { return states[(size_t) slot]; }
float StemSeparator::progress(int slot) const { auto& j = jobs[(size_t) slot]; return j ? j->progress.load() : 0.0f; }
juce::String StemSeparator::error(int slot) const { return errors[(size_t) slot]; }
juce::String StemSeparator::sourceIdOf(int slot) const { return ids[(size_t) slot]; }
std::shared_ptr<const StemSet> StemSeparator::stems(int slot) const { return ready[(size_t) slot]; }
} // namespace hps
