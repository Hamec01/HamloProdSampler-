#include "hpbridge/BridgeValidator.h"

#include <cmath>

namespace hpbridge
{
namespace
{
bool isValidMeterDenominator(int d) noexcept
{
    return d == 2 || d == 4 || d == 8 || d == 16;
}

bool isFiniteInRange(double v, double lo, double hi) noexcept
{
    return std::isfinite(v) && v >= lo && v <= hi;
}

juce::Array<juce::var> toInt64Array(const std::vector<int64_t>& vec)
{
    juce::Array<juce::var> arr;
    arr.ensureStorageAllocated(static_cast<int>(vec.size()));
    for (auto v : vec)
        arr.add(static_cast<juce::int64>(v));
    return arr;
}

juce::Array<juce::var> toInt32Array(const std::vector<int32_t>& vec)
{
    juce::Array<juce::var> arr;
    arr.ensureStorageAllocated(static_cast<int>(vec.size()));
    for (auto v : vec)
        arr.add(static_cast<int>(v));
    return arr;
}

juce::Array<juce::var> toFloatArray(const std::vector<float>& vec)
{
    juce::Array<juce::var> arr;
    arr.ensureStorageAllocated(static_cast<int>(vec.size()));
    for (auto v : vec)
        arr.add(static_cast<double>(v));
    return arr;
}
} // namespace

juce::String toString(BridgeConnectionState state)
{
    switch (state)
    {
        case BridgeConnectionState::Disconnected:       return "Disconnected";
        case BridgeConnectionState::Available:          return "Available";
        case BridgeConnectionState::Connecting:         return "Connecting";
        case BridgeConnectionState::Connected:          return "Connected";
        case BridgeConnectionState::GuideLinkedReadOnly:return "Guide Linked (Read-Only)";
        case BridgeConnectionState::Stale:              return "Stale";
        case BridgeConnectionState::Incompatible:       return "Incompatible";
        case BridgeConnectionState::Error:              return "Error";
    }
    return "Disconnected";
}

juce::String toString(ContextOrigin origin)
{
    switch (origin)
    {
        case ContextOrigin::Analysis:     return "analysis";
        case ContextOrigin::UserOverride: return "user_override";
        case ContextOrigin::Unknown:      return "unknown";
    }
    return "unknown";
}

ContextOrigin contextOriginFromString(const juce::String& s)
{
    if (s == "analysis") return ContextOrigin::Analysis;
    if (s == "user_override") return ContextOrigin::UserOverride;
    return ContextOrigin::Unknown;
}

juce::String toString(ContextScope scope)
{
    return scope == ContextScope::ActiveSrc ? "active_src" : "main_mix";
}

ContextScope contextScopeFromString(const juce::String& s)
{
    return s == "active_src" ? ContextScope::ActiveSrc : ContextScope::MainMix;
}

juce::String toString(DrumLaneClass lane)
{
    switch (lane)
    {
        case DrumLaneClass::Kick:  return "kick";
        case DrumLaneClass::Snare: return "snare";
        case DrumLaneClass::Hat:   return "hat";
        case DrumLaneClass::Perc:  return "perc";
        case DrumLaneClass::Other: return "other";
    }
    return "other";
}

DrumLaneClass drumLaneClassFromString(const juce::String& s)
{
    if (s == "kick")  return DrumLaneClass::Kick;
    if (s == "snare") return DrumLaneClass::Snare;
    if (s == "hat")   return DrumLaneClass::Hat;
    if (s == "perc")  return DrumLaneClass::Perc;
    return DrumLaneClass::Other;
}

ValidationResult BridgeValidator::validate(const SamplerContextSnapshot& s)
{
    if (s.schema != kSamplerContextSchemaV1)
        return ValidationResult::failure("Incompatible schema: expected " + juce::String(kSamplerContextSchemaV1));
    if (s.revision < 1)
        return ValidationResult::failure("revision must be >= 1");
    if (s.projectId.trim().isEmpty() || s.samplerInstanceId.trim().isEmpty())
        return ValidationResult::failure("projectId and samplerInstanceId are required");
    if (s.timebase.ticksPerQuarter <= 0 || s.timebase.ticksPerQuarter > 15360)
        return ValidationResult::failure("ticksPerQuarter out of range");
    if (s.timebase.originTick < 0)
        return ValidationResult::failure("originTick must be >= 0");
    if (!isFiniteInRange(s.transport.bpm, 20.0, 300.0))
        return ValidationResult::failure("transport.bpm out of range [20, 300]");
    if (s.transport.meterNumerator < 1 || s.transport.meterNumerator > 16 || !isValidMeterDenominator(s.transport.meterDenominator))
        return ValidationResult::failure("transport.meter invalid");
    if (s.patternBars < 1 || s.patternBars > 64)
        return ValidationResult::failure("pattern.bars out of range [1, 64]");
    if (s.patternEvents.size() > kMaxEvents)
        return ValidationResult::failure("pattern.events exceeds max limit");

    if (s.key.tonicPitchClass.has_value() && (*s.key.tonicPitchClass < 0 || *s.key.tonicPitchClass > 11))
        return ValidationResult::failure("key.tonicPitchClass out of range [0, 11]");
    if (s.key.confidence.has_value() && !isFiniteInRange(*s.key.confidence, 0.0, 1.0))
        return ValidationResult::failure("key.confidence out of range [0, 1]");

    const int64_t ticksPerBar = static_cast<int64_t>(s.timebase.ticksPerQuarter) * s.transport.meterNumerator * 4 / s.transport.meterDenominator;
    const int64_t maxEndTick  = s.timebase.originTick + ticksPerBar * s.patternBars;

    for (const auto& ev : s.patternEvents)
    {
        if (ev.tick < s.timebase.originTick || ev.tick > maxEndTick)
            return ValidationResult::failure("pattern event tick out of range");
        if (ev.durationTicks < 0)
            return ValidationResult::failure("pattern event durationTicks must be >= 0");
        if (!isFiniteInRange(ev.velocity, 0.0, 1.0))
            return ValidationResult::failure("pattern event velocity out of range [0, 1]");
        if (ev.padId.isEmpty())
            return ValidationResult::failure("pattern event missing padId");
    }

    for (const auto& seg : s.harmony)
    {
        if (seg.startTick < s.timebase.originTick || seg.endTick <= seg.startTick)
            return ValidationResult::failure("harmony segment has invalid tick range");
        if (seg.confidence.has_value() && !isFiniteInRange(*seg.confidence, 0.0, 1.0))
            return ValidationResult::failure("harmony confidence out of range [0, 1]");
    }

    for (const auto& ph : s.phrases)
    {
        if (ph.startTick < s.timebase.originTick || ph.endTick <= ph.startTick)
            return ValidationResult::failure("phrase has invalid tick range");
    }

    if (s.featureVersion != 0 && s.featureVersion != 1)
        return ValidationResult::failure("Unsupported context feature version");
    const auto expectedSteps = (size_t) s.patternBars * s.transport.meterNumerator * 16 / s.transport.meterDenominator;
    if ((s.featureVersion == 0 && !s.features.empty()) || s.features.size() > 1024)
        return ValidationResult::failure("Context feature count out of range");
    if (s.featureVersion == 1 && (s.timebase.ticksPerQuarter % 4 != 0 || s.features.size() != expectedSteps))
        return ValidationResult::failure("Context feature grid does not match pattern");
    for (const auto& f : s.features)
    {
        for (float v : { f.energy, f.onset, f.low, f.mid, f.high, f.bassConfidence, f.confidence })
            if (!isFiniteInRange(v, 0, 1)) return ValidationResult::failure("Invalid context feature value");
        for (float v : f.chroma)
            if (!isFiniteInRange(v, 0, 1)) return ValidationResult::failure("Invalid context chroma");
        if (f.bassMidi < -1 || f.bassMidi > 127 || (f.bassMidi == -1 && f.bassConfidence != 0))
            return ValidationResult::failure("Invalid context bass note");
    }

    return ValidationResult::success();
}

ValidationResult BridgeValidator::validate(const DrumGuideSnapshot& g)
{
    if (g.schema != kDrumGuideSchemaV1)
        return ValidationResult::failure("Incompatible schema: expected " + juce::String(kDrumGuideSchemaV1));
    if (g.revision < 1)
        return ValidationResult::failure("revision must be >= 1");
    if (g.hpdgInstanceId.trim().isEmpty() || g.patternId.trim().isEmpty())
        return ValidationResult::failure("hpdgInstanceId and patternId are required");
    if (g.timebase.ticksPerQuarter <= 0 || g.timebase.ticksPerQuarter > 15360)
        return ValidationResult::failure("ticksPerQuarter out of range");
    if (g.timebase.originTick < 0)
        return ValidationResult::failure("originTick must be >= 0");
    if (!isFiniteInRange(g.transport.bpm, 20.0, 300.0))
        return ValidationResult::failure("transport.bpm out of range [20, 300]");
    if (g.transport.meterNumerator < 1 || g.transport.meterNumerator > 16 || !isValidMeterDenominator(g.transport.meterDenominator))
        return ValidationResult::failure("transport.meter invalid");
    if (g.transport.loopBars < 1 || g.transport.loopBars > 64)
        return ValidationResult::failure("transport.loopBars out of range [1, 64]");
    if (g.drumEvents.size() > kMaxEvents)
        return ValidationResult::failure("drumEvents exceeds max limit");

    const int64_t ticksPerBar = static_cast<int64_t>(g.timebase.ticksPerQuarter) * g.transport.meterNumerator * 4 / g.transport.meterDenominator;
    const int64_t maxEndTick  = g.timebase.originTick + ticksPerBar * g.transport.loopBars;

    for (const auto& ev : g.drumEvents)
    {
        if (ev.tick < g.timebase.originTick || ev.tick > maxEndTick)
            return ValidationResult::failure("drumEvent tick out of range");
        if (!isFiniteInRange(ev.velocity, 0.0, 1.0) || !isFiniteInRange(ev.accent, 0.0, 1.0))
            return ValidationResult::failure("drumEvent velocity/accent out of range [0, 1]");
    }

    for (const auto& ph : g.phrases)
    {
        if (ph.startTick < g.timebase.originTick || ph.endTick <= ph.startTick)
            return ValidationResult::failure("drum guide phrase has invalid tick range");
    }

    return ValidationResult::success();
}

juce::String BridgeValidator::toJson(const SamplerContextSnapshot& s)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schema", s.schema);
    root->setProperty("revision", static_cast<juce::int64>(s.revision));
    root->setProperty("projectId", s.projectId);
    root->setProperty("samplerInstanceId", s.samplerInstanceId);
    root->setProperty("scope", toString(s.scope));

    auto tb = std::make_unique<juce::DynamicObject>();
    tb->setProperty("ticksPerQuarter", s.timebase.ticksPerQuarter);
    tb->setProperty("originTick", static_cast<juce::int64>(s.timebase.originTick));
    root->setProperty("timebase", juce::var(tb.release()));

    auto tr = std::make_unique<juce::DynamicObject>();
    tr->setProperty("bpm", s.transport.bpm);
    juce::Array<juce::var> meter;
    meter.add(s.transport.meterNumerator);
    meter.add(s.transport.meterDenominator);
    tr->setProperty("meter", meter);
    tr->setProperty("hostSync", s.transport.hostSync);
    root->setProperty("transport", juce::var(tr.release()));

    auto keyObj = std::make_unique<juce::DynamicObject>();
    keyObj->setProperty("tonicPitchClass", s.key.tonicPitchClass.has_value() ? juce::var(*s.key.tonicPitchClass) : juce::var());
    keyObj->setProperty("mode", s.key.mode.isNotEmpty() ? juce::var(s.key.mode) : juce::var());
    keyObj->setProperty("confidence", s.key.confidence.has_value() ? juce::var(static_cast<double>(*s.key.confidence)) : juce::var());
    keyObj->setProperty("origin", toString(s.key.origin));
    root->setProperty("key", juce::var(keyObj.release()));

    juce::Array<juce::var> srcs;
    for (const auto& src : s.sources)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("sourceId", src.sourceId);
        o->setProperty("name", src.name);
        o->setProperty("originalBpm", src.originalBpm.has_value() ? juce::var(*src.originalBpm) : juce::var());
        srcs.add(juce::var(o.release()));
    }
    root->setProperty("sources", srcs);

    auto pat = std::make_unique<juce::DynamicObject>();
    pat->setProperty("bars", s.patternBars);
    juce::Array<juce::var> evs;
    for (const auto& ev : s.patternEvents)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("tick", static_cast<juce::int64>(ev.tick));
        o->setProperty("durationTicks", static_cast<juce::int64>(ev.durationTicks));
        o->setProperty("padId", ev.padId);
        o->setProperty("sliceId", ev.sliceId);
        o->setProperty("sourceId", ev.sourceId);
        o->setProperty("velocity", static_cast<double>(ev.velocity));
        if (ev.pitchSemitones.has_value())
            o->setProperty("pitchSemitones", *ev.pitchSemitones);
        evs.add(juce::var(o.release()));
    }
    pat->setProperty("events", evs);
    root->setProperty("pattern", juce::var(pat.release()));

    auto bg = std::make_unique<juce::DynamicObject>();
    bg->setProperty("beatTicks", toInt64Array(s.beatGrid.beatTicks));
    bg->setProperty("downbeatTicks", toInt64Array(s.beatGrid.downbeatTicks));
    root->setProperty("beatGrid", juce::var(bg.release()));

    auto gr = std::make_unique<juce::DynamicObject>();
    gr->setProperty("subdivision", s.groove.subdivision);
    gr->setProperty("swingAmount", static_cast<double>(s.groove.swingAmount));
    gr->setProperty("microOffsetTicks", toInt32Array(s.groove.microOffsetTicks));
    root->setProperty("groove", juce::var(gr.release()));

    juce::Array<juce::var> accs;
    for (const auto& a : s.accents)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("tick", static_cast<juce::int64>(a.tick));
        o->setProperty("strength", static_cast<double>(a.strength));
        o->setProperty("band", a.band);
        accs.add(juce::var(o.release()));
    }
    root->setProperty("accents", accs);

    juce::Array<juce::var> harm;
    for (const auto& h : s.harmony)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("startTick", static_cast<juce::int64>(h.startTick));
        o->setProperty("endTick", static_cast<juce::int64>(h.endTick));
        o->setProperty("chord", h.chord);
        o->setProperty("rootPitchClass", h.rootPitchClass.has_value() ? juce::var(*h.rootPitchClass) : juce::var());
        o->setProperty("confidence", h.confidence.has_value() ? juce::var(static_cast<double>(*h.confidence)) : juce::var());
        o->setProperty("origin", toString(h.origin));
        harm.add(juce::var(o.release()));
    }
    root->setProperty("harmony", harm);

    juce::Array<juce::var> phs;
    for (const auto& p : s.phrases)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("startTick", static_cast<juce::int64>(p.startTick));
        o->setProperty("endTick", static_cast<juce::int64>(p.endTick));
        o->setProperty("energy", static_cast<double>(p.energy));
        phs.add(juce::var(o.release()));
    }
    root->setProperty("phrases", phs);

    auto le = std::make_unique<juce::DynamicObject>();
    le->setProperty("gridStepTicks", static_cast<juce::int64>(s.lowEnd.gridStepTicks));
    le->setProperty("occupancy", toFloatArray(s.lowEnd.occupancy));
    root->setProperty("lowEnd", juce::var(le.release()));

    auto an = std::make_unique<juce::DynamicObject>();
    an->setProperty("algorithmVersion", s.algorithmVersion);
    an->setProperty("unknownPolicy", s.unknownPolicy);
    root->setProperty("analysis", juce::var(an.release()));
    root->setProperty("audioReference", s.audioReference.has_value() ? juce::var(*s.audioReference) : juce::var());
    if (s.featureVersion != 0)
    {
        auto features = std::make_unique<juce::DynamicObject>();
        features->setProperty("version", s.featureVersion);
        features->setProperty("method", "source_projection");
        juce::Array<juce::var> rows;
        for (const auto& f : s.features)
        {
            juce::Array<juce::var> row;
            // Quantisation bounds both payload size and the numerical round-trip error.
            auto add = [&](float v) { row.add(std::round((double) v * 10000.0) / 10000.0); };
            for (float v : { f.energy, f.onset, f.low, f.mid, f.high }) add(v);
            for (float v : f.chroma) add(v);
            row.add(f.bassMidi);
            add(f.bassConfidence); add(f.confidence);
            rows.add(row);
        }
        features->setProperty("steps", rows);
        root->setProperty("features", juce::var(features.release()));
    }
    if (!s.bassNotes.empty())
    {
        // [startTick, lengthTicks, midi, velocity, confidence]; older peers ignore the field
        juce::Array<juce::var> notes;
        for (const auto& n : s.bassNotes)
        {
            juce::Array<juce::var> row;
            row.add((juce::int64) n.startTick); row.add((juce::int64) n.lengthTicks); row.add(n.midi);
            row.add(std::round((double) n.velocity * 1000.0) / 1000.0);
            row.add(std::round((double) n.confidence * 1000.0) / 1000.0);
            notes.add(row);
        }
        root->setProperty("bassNotes", notes);
    }

    return juce::JSON::toString(juce::var(root.release()), s.featureVersion != 0);
}

juce::String BridgeValidator::toJson(const DrumGuideSnapshot& g)
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schema", g.schema);
    root->setProperty("revision", static_cast<juce::int64>(g.revision));
    root->setProperty("hpdgInstanceId", g.hpdgInstanceId);
    root->setProperty("patternId", g.patternId);

    auto tb = std::make_unique<juce::DynamicObject>();
    tb->setProperty("ticksPerQuarter", g.timebase.ticksPerQuarter);
    tb->setProperty("originTick", static_cast<juce::int64>(g.timebase.originTick));
    root->setProperty("timebase", juce::var(tb.release()));

    auto tr = std::make_unique<juce::DynamicObject>();
    tr->setProperty("bpm", g.transport.bpm);
    juce::Array<juce::var> meter;
    meter.add(g.transport.meterNumerator);
    meter.add(g.transport.meterDenominator);
    tr->setProperty("meter", meter);
    tr->setProperty("loopBars", g.transport.loopBars);
    root->setProperty("transport", juce::var(tr.release()));

    auto bg = std::make_unique<juce::DynamicObject>();
    bg->setProperty("beatTicks", toInt64Array(g.beatGrid.beatTicks));
    bg->setProperty("downbeatTicks", toInt64Array(g.beatGrid.downbeatTicks));
    root->setProperty("beatGrid", juce::var(bg.release()));

    juce::Array<juce::var> evs;
    for (const auto& ev : g.drumEvents)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("tick", static_cast<juce::int64>(ev.tick));
        o->setProperty("laneClass", toString(ev.laneClass));
        o->setProperty("velocity", static_cast<double>(ev.velocity));
        o->setProperty("accent", static_cast<double>(ev.accent));
        evs.add(juce::var(o.release()));
    }
    root->setProperty("drumEvents", evs);

    auto gr = std::make_unique<juce::DynamicObject>();
    gr->setProperty("swingAmount", static_cast<double>(g.groove.swingAmount));
    gr->setProperty("subdivision", g.groove.subdivision);
    gr->setProperty("microOffsetTicks", toInt32Array(g.groove.microOffsetTicks));
    root->setProperty("groove", juce::var(gr.release()));

    juce::Array<juce::var> phs;
    for (const auto& p : g.phrases)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("startTick", static_cast<juce::int64>(p.startTick));
        o->setProperty("endTick", static_cast<juce::int64>(p.endTick));
        phs.add(juce::var(o.release()));
    }
    root->setProperty("phrases", phs);
    root->setProperty("audioReference", g.audioReference.has_value() ? juce::var(*g.audioReference) : juce::var());

    return juce::JSON::toString(juce::var(root.release()), false);
}

ValidationResult BridgeValidator::fromJson(const juce::String& jsonText, SamplerContextSnapshot& out)
{
    if (jsonText.getNumBytesAsUTF8() > kMaxPayloadBytes)
        return ValidationResult::failure("Payload exceeds maximum allowed size (512 KB)");

    juce::var parsed;
    const auto parseRes = juce::JSON::parse(jsonText, parsed);
    if (parseRes.failed() || !parsed.isObject())
        return ValidationResult::failure("Invalid JSON: " + parseRes.getErrorMessage());

    SamplerContextSnapshot s;
    s.schema            = parsed.getProperty("schema", "").toString();
    s.revision          = static_cast<int64_t>(parsed.getProperty("revision", 0));
    s.projectId         = parsed.getProperty("projectId", "").toString();
    s.samplerInstanceId = parsed.getProperty("samplerInstanceId", "").toString();
    s.scope             = contextScopeFromString(parsed.getProperty("scope", "main_mix").toString());

    if (const auto tb = parsed.getProperty("timebase", {}); tb.isObject())
    {
        s.timebase.ticksPerQuarter = static_cast<int>(tb.getProperty("ticksPerQuarter", kDefaultTicksPerQuarter));
        s.timebase.originTick      = static_cast<int64_t>(tb.getProperty("originTick", 0));
    }

    if (const auto tr = parsed.getProperty("transport", {}); tr.isObject())
    {
        s.transport.bpm      = static_cast<double>(tr.getProperty("bpm", 0.0));
        s.transport.hostSync = static_cast<bool>(tr.getProperty("hostSync", true));
        if (const auto* meter = tr.getProperty("meter", {}).getArray(); meter != nullptr && meter->size() == 2)
        {
            s.transport.meterNumerator   = static_cast<int>((*meter)[0]);
            s.transport.meterDenominator = static_cast<int>((*meter)[1]);
        }
    }

    if (const auto key = parsed.getProperty("key", {}); key.isObject())
    {
        const auto tonic = key.getProperty("tonicPitchClass", {});
        if (!tonic.isVoid()) s.key.tonicPitchClass = static_cast<int>(tonic);
        const auto mode = key.getProperty("mode", {});
        if (!mode.isVoid()) s.key.mode = mode.toString();
        const auto conf = key.getProperty("confidence", {});
        if (!conf.isVoid()) s.key.confidence = static_cast<float>(static_cast<double>(conf));
        s.key.origin = contextOriginFromString(key.getProperty("origin", "unknown").toString());
    }

    if (const auto* srcs = parsed.getProperty("sources", {}).getArray())
    {
        for (const auto& item : *srcs)
        {
            if (!item.isObject()) continue;
            SourceSummary sum;
            sum.sourceId = item.getProperty("sourceId", "").toString();
            sum.name     = item.getProperty("name", "").toString();
            const auto bpm = item.getProperty("originalBpm", {});
            if (!bpm.isVoid()) sum.originalBpm = static_cast<double>(bpm);
            s.sources.push_back(std::move(sum));
        }
    }

    if (const auto pat = parsed.getProperty("pattern", {}); pat.isObject())
    {
        s.patternBars = static_cast<int>(pat.getProperty("bars", 2));
        if (const auto* evs = pat.getProperty("events", {}).getArray())
        {
            for (const auto& item : *evs)
            {
                if (!item.isObject()) continue;
                PatternEventDto ev;
                ev.tick          = static_cast<int64_t>(item.getProperty("tick", 0));
                ev.durationTicks = static_cast<int64_t>(item.getProperty("durationTicks", 0));
                ev.padId         = item.getProperty("padId", "").toString();
                ev.sliceId       = item.getProperty("sliceId", "").toString();
                ev.sourceId      = item.getProperty("sourceId", "").toString();
                ev.velocity      = static_cast<float>(static_cast<double>(item.getProperty("velocity", 1.0)));
                const auto p = item.getProperty("pitchSemitones", {});
                if (!p.isVoid()) ev.pitchSemitones = static_cast<int>(p);
                s.patternEvents.push_back(std::move(ev));
            }
        }
    }

    if (const auto bg = parsed.getProperty("beatGrid", {}); bg.isObject())
    {
        if (const auto* bt = bg.getProperty("beatTicks", {}).getArray())
            for (const auto& v : *bt) s.beatGrid.beatTicks.push_back(static_cast<int64_t>(v));
        if (const auto* dt = bg.getProperty("downbeatTicks", {}).getArray())
            for (const auto& v : *dt) s.beatGrid.downbeatTicks.push_back(static_cast<int64_t>(v));
    }

    if (const auto gr = parsed.getProperty("groove", {}); gr.isObject())
    {
        s.groove.subdivision = gr.getProperty("subdivision", "1/16").toString();
        s.groove.swingAmount = static_cast<float>(static_cast<double>(gr.getProperty("swingAmount", 0.0)));
        if (const auto* mo = gr.getProperty("microOffsetTicks", {}).getArray())
            for (const auto& v : *mo) s.groove.microOffsetTicks.push_back(static_cast<int32_t>(static_cast<int>(v)));
    }

    if (const auto* accs = parsed.getProperty("accents", {}).getArray())
    {
        for (const auto& item : *accs)
        {
            if (!item.isObject()) continue;
            s.accents.push_back({
                static_cast<int64_t>(item.getProperty("tick", 0)),
                static_cast<float>(static_cast<double>(item.getProperty("strength", 0.0))),
                item.getProperty("band", "mid").toString()
            });
        }
    }

    if (const auto* harm = parsed.getProperty("harmony", {}).getArray())
    {
        for (const auto& item : *harm)
        {
            if (!item.isObject()) continue;
            HarmonySegmentDto h;
            h.startTick = static_cast<int64_t>(item.getProperty("startTick", 0));
            h.endTick   = static_cast<int64_t>(item.getProperty("endTick", 0));
            h.chord     = item.getProperty("chord", "").toString();
            const auto r = item.getProperty("rootPitchClass", {});
            if (!r.isVoid()) h.rootPitchClass = static_cast<int>(r);
            const auto c = item.getProperty("confidence", {});
            if (!c.isVoid()) h.confidence = static_cast<float>(static_cast<double>(c));
            h.origin = contextOriginFromString(item.getProperty("origin", "unknown").toString());
            s.harmony.push_back(std::move(h));
        }
    }

    if (const auto* phs = parsed.getProperty("phrases", {}).getArray())
    {
        for (const auto& item : *phs)
        {
            if (!item.isObject()) continue;
            s.phrases.push_back({
                static_cast<int64_t>(item.getProperty("startTick", 0)),
                static_cast<int64_t>(item.getProperty("endTick", 0)),
                static_cast<float>(static_cast<double>(item.getProperty("energy", 0.5)))
            });
        }
    }

    if (const auto le = parsed.getProperty("lowEnd", {}); le.isObject())
    {
        s.lowEnd.gridStepTicks = static_cast<int64_t>(le.getProperty("gridStepTicks", 240));
        if (const auto* occ = le.getProperty("occupancy", {}).getArray())
            for (const auto& v : *occ) s.lowEnd.occupancy.push_back(static_cast<float>(static_cast<double>(v)));
    }

    if (const auto an = parsed.getProperty("analysis", {}); an.isObject())
    {
        s.algorithmVersion = static_cast<int>(an.getProperty("algorithmVersion", 1));
        s.unknownPolicy    = an.getProperty("unknownPolicy", "null").toString();
    }
    if (const auto ref = parsed.getProperty("audioReference", {}); ref.isString() && ref.toString().isNotEmpty())
        s.audioReference = ref.toString();
    if (parsed.hasProperty("features"))
    {
        const auto f = parsed["features"];
        s.featureVersion = (int) f.getProperty("version", 0);
        const auto* rows = f.getProperty("steps", {}).getArray();
        if (!f.isObject() || s.featureVersion != 1 || f.getProperty("method", "").toString() != "source_projection"
            || rows == nullptr || rows->size() > 1024)
            return ValidationResult::failure("Invalid context feature capability");
        for (const auto& value : *rows)
        {
            const auto* row = value.getArray();
            if (row == nullptr || row->size() != 20)
                return ValidationResult::failure("Invalid context feature row");
            for (const auto& cell : *row)
                if (!(cell.isInt() || cell.isInt64() || cell.isDouble()))
                    return ValidationResult::failure("Context feature cells must be numbers");
            ContextStep step;
            step.energy = (float) (double) (*row)[0]; step.onset = (float) (double) (*row)[1];
            step.low = (float) (double) (*row)[2]; step.mid = (float) (double) (*row)[3]; step.high = (float) (double) (*row)[4];
            for (int pc = 0; pc < 12; ++pc) step.chroma[(size_t) pc] = (float) (double) (*row)[5 + pc];
            const double bass = (double) (*row)[17];
            if (!isFiniteInRange(bass, -1, 127) || bass != std::floor(bass))
                return ValidationResult::failure("Invalid context bass pitch");
            step.bassMidi = (int) bass;
            step.bassConfidence = (float) (double) (*row)[18]; step.confidence = (float) (double) (*row)[19];
            s.features.push_back(step);
        }
    }
    if (const auto* notes = parsed.getProperty("bassNotes", {}).getArray())
    {
        if (notes->size() > 4096) return ValidationResult::failure("Too many bass notes");
        for (const auto& value : *notes)
        {
            const auto* row = value.getArray();
            if (row == nullptr || row->size() != 5) return ValidationResult::failure("Invalid bass note");
            for (const auto& cell : *row)
                if (!(cell.isInt() || cell.isInt64() || cell.isDouble())) return ValidationResult::failure("Bass note cells must be numbers");
            BassNoteDto n;
            n.startTick = (int64_t) (*row)[0]; n.lengthTicks = (int64_t) (*row)[1];
            const double midi = (double) (*row)[2];
            if (!isFiniteInRange(midi, 0, 127) || midi != std::floor(midi) || n.startTick < 0 || n.lengthTicks <= 0)
                return ValidationResult::failure("Invalid bass note");
            n.midi = (int) midi;
            n.velocity = juce::jlimit(0.0f, 1.0f, (float) (double) (*row)[3]);
            n.confidence = juce::jlimit(0.0f, 1.0f, (float) (double) (*row)[4]);
            s.bassNotes.push_back(n);
        }
    }

    const auto check = validate(s);
    if (!check.ok) return check;
    out = std::move(s);
    return ValidationResult::success();
}

ValidationResult BridgeValidator::fromJson(const juce::String& jsonText, DrumGuideSnapshot& out)
{
    if (jsonText.getNumBytesAsUTF8() > kMaxPayloadBytes)
        return ValidationResult::failure("Payload exceeds maximum allowed size (512 KB)");

    juce::var parsed;
    const auto parseRes = juce::JSON::parse(jsonText, parsed);
    if (parseRes.failed() || !parsed.isObject())
        return ValidationResult::failure("Invalid JSON: " + parseRes.getErrorMessage());

    DrumGuideSnapshot g;
    g.schema         = parsed.getProperty("schema", "").toString();
    g.revision       = static_cast<int64_t>(parsed.getProperty("revision", 0));
    g.hpdgInstanceId = parsed.getProperty("hpdgInstanceId", "").toString();
    g.patternId      = parsed.getProperty("patternId", "").toString();

    if (const auto tb = parsed.getProperty("timebase", {}); tb.isObject())
    {
        g.timebase.ticksPerQuarter = static_cast<int>(tb.getProperty("ticksPerQuarter", kDefaultTicksPerQuarter));
        g.timebase.originTick      = static_cast<int64_t>(tb.getProperty("originTick", 0));
    }

    if (const auto tr = parsed.getProperty("transport", {}); tr.isObject())
    {
        g.transport.bpm      = static_cast<double>(tr.getProperty("bpm", 0.0));
        g.transport.loopBars = static_cast<int>(tr.getProperty("loopBars", 2));
        if (const auto* meter = tr.getProperty("meter", {}).getArray(); meter != nullptr && meter->size() == 2)
        {
            g.transport.meterNumerator   = static_cast<int>((*meter)[0]);
            g.transport.meterDenominator = static_cast<int>((*meter)[1]);
        }
    }

    if (const auto bg = parsed.getProperty("beatGrid", {}); bg.isObject())
    {
        if (const auto* bt = bg.getProperty("beatTicks", {}).getArray())
            for (const auto& v : *bt) g.beatGrid.beatTicks.push_back(static_cast<int64_t>(v));
        if (const auto* dt = bg.getProperty("downbeatTicks", {}).getArray())
            for (const auto& v : *dt) g.beatGrid.downbeatTicks.push_back(static_cast<int64_t>(v));
    }

    if (const auto* evs = parsed.getProperty("drumEvents", {}).getArray())
    {
        for (const auto& item : *evs)
        {
            if (!item.isObject()) continue;
            g.drumEvents.push_back({
                static_cast<int64_t>(item.getProperty("tick", 0)),
                drumLaneClassFromString(item.getProperty("laneClass", "other").toString()),
                static_cast<float>(static_cast<double>(item.getProperty("velocity", 0.8))),
                static_cast<float>(static_cast<double>(item.getProperty("accent", 0.8)))
            });
        }
    }

    if (const auto gr = parsed.getProperty("groove", {}); gr.isObject())
    {
        g.groove.subdivision = gr.getProperty("subdivision", "1/16").toString();
        g.groove.swingAmount = static_cast<float>(static_cast<double>(gr.getProperty("swingAmount", 0.0)));
        if (const auto* mo = gr.getProperty("microOffsetTicks", {}).getArray())
            for (const auto& v : *mo) g.groove.microOffsetTicks.push_back(static_cast<int32_t>(static_cast<int>(v)));
    }

    if (const auto* phs = parsed.getProperty("phrases", {}).getArray())
    {
        for (const auto& item : *phs)
        {
            if (!item.isObject()) continue;
            g.phrases.push_back({
                static_cast<int64_t>(item.getProperty("startTick", 0)),
                static_cast<int64_t>(item.getProperty("endTick", 0)),
                0.5f
            });
        }
    }

    const auto ref = parsed.getProperty("audioReference", {});
    if (!ref.isVoid()) g.audioReference = ref.toString();

    const auto check = validate(g);
    if (!check.ok) return check;
    out = std::move(g);
    return ValidationResult::success();
}
} // namespace hpbridge
