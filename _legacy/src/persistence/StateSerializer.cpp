#include "persistence/StateSerializer.h"
#include "analysis/ClassicalAnalyzer.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace hps
{
bool StateSerializer::loadAudioFileIntoSource(const juce::File& file, int slotIndex, SourceAsset& outAsset, juce::String* errorOut)
{
    if (!file.existsAsFile())
    {
        if (errorOut != nullptr) *errorOut = "File not found: " + file.getFullPathName();
        return false;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr)
    {
        if (errorOut != nullptr) *errorOut = "Unsupported or corrupt audio file: " + file.getFileName();
        return false;
    }

    // Cap at 10 minutes to stay memory-safe
    const int64_t maxAllowed = static_cast<int64_t>(reader->sampleRate * 600.0);
    const int64_t framesToRead = std::min<int64_t>(reader->lengthInSamples, maxAllowed);
    if (framesToRead <= 0)
    {
        if (errorOut != nullptr) *errorOut = "Audio file is empty: " + file.getFileName();
        return false;
    }

    const int outCh = reader->numChannels >= 2 ? 2 : 1;
    auto buf = std::make_shared<juce::AudioBuffer<float>>(outCh, static_cast<int>(framesToRead));
    reader->read(buf.get(), 0, static_cast<int>(framesToRead), 0, true, outCh > 1);

    outAsset.visibleSlot = juce::jlimit(0, kNumVisibleSources - 1, slotIndex);
    if (outAsset.id.isEmpty()) outAsset.id = makeUuid();
    outAsset.name        = file.getFileNameWithoutExtension();
    outAsset.filePath    = file.getFullPathName();
    outAsset.contentHash = juce::String::toHexString(file.getSize()) + "-" + juce::String::toHexString(file.getLastModificationTime().toMilliseconds());
    outAsset.missingFile = false;
    outAsset.color       = SourceColor::defaultForSlot(outAsset.visibleSlot);
    outAsset.sampleRate  = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    outAsset.numFrames   = framesToRead;
    outAsset.audio       = buf;

    const auto hints = ClassicalAnalyzer::readFilenameAndAcidHints(file, reader->metadataValues);
    outAsset.analysis = ClassicalAnalyzer::analyze(*buf, outAsset.sampleRate, hints);
    return true;
}

juce::String StateSerializer::serializeToJson(const SamplerProject& project)
{
    const auto& st = project.state();
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schema", kStateSchemaV1);
    root->setProperty("projectId", st.projectId);
    root->setProperty("instanceId", st.instanceId);
    root->setProperty("projectBpm", st.projectBpm);
    root->setProperty("meterNumerator", st.meterNumerator);
    root->setProperty("meterDenominator", st.meterDenominator);
    root->setProperty("anchorSource", static_cast<int>(st.anchorSource));
    root->setProperty("bpmLocked", st.bpmLocked);
    root->setProperty("keyLocked", st.keyLocked);
    root->setProperty("hostSyncEnabled", st.hostSyncEnabled);
    root->setProperty("gridDivision", static_cast<int>(st.gridDivision));
    root->setProperty("globalSyncMode", static_cast<int>(st.globalSyncMode));
    root->setProperty("globalPlayback", static_cast<int>(st.globalPlayback));
    root->setProperty("globalOverlap", static_cast<int>(st.globalOverlap));
    root->setProperty("recordQuantizeOn", st.recordQuantizeOn);
    root->setProperty("recordQuantizeAmt", static_cast<double>(st.recordQuantizeAmt));
    root->setProperty("overdubMode", st.overdubMode);

    if (st.projectKey.tonicPitchClass.has_value())
    {
        auto ko = std::make_unique<juce::DynamicObject>();
        ko->setProperty("tonic", *st.projectKey.tonicPitchClass);
        ko->setProperty("mode", st.projectKey.mode);
        ko->setProperty("confidence", static_cast<double>(st.projectKey.confidence));
        ko->setProperty("origin", hpbridge::toString(st.projectKey.origin));
        root->setProperty("projectKey", juce::var(ko.release()));
    }

    // Sources (metadata + path + hash, NOT raw WAV blob)
    juce::Array<juce::var> srcs;
    for (int i = 0; i < kNumVisibleSources; ++i)
    {
        if (!st.sources[static_cast<size_t>(i)].has_value()) continue;
        const auto& s = *st.sources[static_cast<size_t>(i)];
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("id", s.id);
        o->setProperty("slot", i);
        o->setProperty("name", s.name);
        o->setProperty("filePath", s.filePath);
        o->setProperty("contentHash", s.contentHash);
        o->setProperty("sampleRate", s.sampleRate);
        o->setProperty("numFrames", static_cast<juce::int64>(s.numFrames));
        o->setProperty("colour", s.color.colour.toString());
        if (s.analysis.bpm.has_value()) o->setProperty("bpm", *s.analysis.bpm);
        srcs.add(juce::var(o.release()));
    }
    root->setProperty("sources", srcs);

    // Slices
    juce::Array<juce::var> slicesArr;
    for (const auto& sl : st.slices)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("id", sl.id);
        o->setProperty("sourceId", sl.sourceId);
        o->setProperty("name", sl.name);
        o->setProperty("startFrame", static_cast<juce::int64>(sl.startFrame));
        o->setProperty("endFrame", static_cast<juce::int64>(sl.endFrameExclusive));
        o->setProperty("fadeInMs", static_cast<double>(sl.fadeInMs));
        o->setProperty("fadeOutMs", static_cast<double>(sl.fadeOutMs));
        o->setProperty("pinned", sl.pinned);
        slicesArr.add(juce::var(o.release()));
    }
    root->setProperty("slices", slicesArr);

    // Pads
    juce::Array<juce::var> padsArr;
    for (int i = 0; i < kTotalPads; ++i)
    {
        const auto& p = st.pads[static_cast<size_t>(i)];
        if (!p.assignment.has_value()) continue;
        const auto& a = *p.assignment;
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("flatIndex", i);
        o->setProperty("padId", p.padId);
        o->setProperty("sliceId", a.sliceId);
        o->setProperty("sourceId", a.sourceId);
        o->setProperty("label", a.label);
        if (a.settings.playbackModeOverride.has_value())
            o->setProperty("playbackOverride", static_cast<int>(*a.settings.playbackModeOverride));
        if (a.settings.overlapOverride.has_value())
            o->setProperty("overlapOverride", static_cast<int>(*a.settings.overlapOverride));
        o->setProperty("retriggerMode", static_cast<int>(a.settings.retriggerMode));
        o->setProperty("chokeGroup", a.settings.chokeGroup);
        o->setProperty("loop", a.settings.loop);
        o->setProperty("reverse", a.settings.reverse);
        o->setProperty("pitchSemitones", static_cast<double>(a.settings.pitchSemitones));
        o->setProperty("fineCents", static_cast<double>(a.settings.fineCents));
        o->setProperty("syncMode", static_cast<int>(a.settings.syncMode));
        o->setProperty("attackMs", static_cast<double>(a.settings.attackMs));
        o->setProperty("releaseMs", static_cast<double>(a.settings.releaseMs));
        o->setProperty("fadeInMs", static_cast<double>(a.settings.fadeInMs));
        o->setProperty("fadeOutMs", static_cast<double>(a.settings.fadeOutMs));
        o->setProperty("gainDb", static_cast<double>(a.settings.gainDb));
        o->setProperty("pan", static_cast<double>(a.settings.pan));
        o->setProperty("filterEnabled", a.settings.filterEnabled);
        o->setProperty("filterType", a.settings.filterType);
        o->setProperty("filterCutoffHz", static_cast<double>(a.settings.filterCutoffHz));
        o->setProperty("filterResonance", static_cast<double>(a.settings.filterResonance));
        o->setProperty("midiNote", a.settings.midiNote);
        if (a.trimOverride.has_value())
        {
            o->setProperty("trimStart", static_cast<juce::int64>(a.trimOverride->startFrame));
            o->setProperty("trimEnd", static_cast<juce::int64>(a.trimOverride->endFrameExclusive));
        }
        padsArr.add(juce::var(o.release()));
    }
    root->setProperty("pads", padsArr);

    // Pattern
    auto pat = std::make_unique<juce::DynamicObject>();
    pat->setProperty("id", st.pattern.id);
    pat->setProperty("bars", st.pattern.bars);
    pat->setProperty("swingAmount", static_cast<double>(st.pattern.swingAmount));
    juce::Array<juce::var> evs;
    for (const auto& ev : st.pattern.events)
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty("id", ev.id);
        o->setProperty("padId", ev.padId);
        o->setProperty("startTick", static_cast<juce::int64>(ev.startTick));
        o->setProperty("rawStartTick", static_cast<juce::int64>(ev.rawStartTick));
        o->setProperty("durationTicks", static_cast<juce::int64>(ev.durationTicks));
        o->setProperty("velocity", static_cast<double>(ev.velocity));
        o->setProperty("microOffsetTicks", static_cast<int>(ev.microOffsetTicks));
        evs.add(juce::var(o.release()));
    }
    pat->setProperty("events", evs);
    root->setProperty("pattern", juce::var(pat.release()));

    // Master FX
    auto mfx = std::make_unique<juce::DynamicObject>();
    mfx->setProperty("warmTubeCompEnabled", st.masterFx.warmTubeCompEnabled);
    mfx->setProperty("driveDb", static_cast<double>(st.masterFx.driveDb));
    mfx->setProperty("compMix", static_cast<double>(st.masterFx.compMix));
    mfx->setProperty("limiterEnabled", st.masterFx.limiterEnabled);
    mfx->setProperty("masterGainDb", static_cast<double>(st.masterFx.masterGainDb));
    root->setProperty("masterFx", juce::var(mfx.release()));

    return juce::JSON::toString(juce::var(root.release()), false);
}

LoadProjectResult StateSerializer::deserializeFromJson(const juce::String& jsonText,
                                                       SamplerProject& project,
                                                       bool reloadAudioFiles)
{
    LoadProjectResult res;
    juce::var parsed;
    const auto err = juce::JSON::parse(jsonText, parsed);
    if (err.failed() || !parsed.isObject())
        return { false, "Invalid project state JSON: " + err.getErrorMessage(), {} };

    if (parsed.getProperty("schema", "").toString() != kStateSchemaV1)
        return { false, "Unsupported project state schema", {} };

    SamplerProject fresh;
    auto& st = fresh.mutableState();
    st.projectId         = parsed.getProperty("projectId", st.projectId).toString();
    st.instanceId        = parsed.getProperty("instanceId", st.instanceId).toString();
    st.projectBpm        = juce::jlimit(20.0, 300.0, static_cast<double>(parsed.getProperty("projectBpm", 120.0)));
    st.meterNumerator    = juce::jlimit(1, 16, static_cast<int>(parsed.getProperty("meterNumerator", 4)));
    st.meterDenominator  = static_cast<int>(parsed.getProperty("meterDenominator", 4));
    st.anchorSource      = static_cast<AnchorSource>(static_cast<int>(parsed.getProperty("anchorSource", 0)));
    st.bpmLocked         = static_cast<bool>(parsed.getProperty("bpmLocked", false));
    st.keyLocked         = static_cast<bool>(parsed.getProperty("keyLocked", false));
    st.hostSyncEnabled   = static_cast<bool>(parsed.getProperty("hostSyncEnabled", true));
    st.gridDivision      = static_cast<GridDivision>(static_cast<int>(parsed.getProperty("gridDivision", 2)));
    st.globalSyncMode    = static_cast<SyncMode>(static_cast<int>(parsed.getProperty("globalSyncMode", 0)));
    st.globalPlayback    = static_cast<PlaybackMode>(static_cast<int>(parsed.getProperty("globalPlayback", 1)));
    st.globalOverlap     = static_cast<OverlapPolicy>(static_cast<int>(parsed.getProperty("globalOverlap", 1)));
    st.recordQuantizeOn  = static_cast<bool>(parsed.getProperty("recordQuantizeOn", false));
    st.recordQuantizeAmt = static_cast<float>(static_cast<double>(parsed.getProperty("recordQuantizeAmt", 1.0)));
    st.overdubMode       = static_cast<bool>(parsed.getProperty("overdubMode", true));

    if (const auto ko = parsed.getProperty("projectKey", {}); ko.isObject())
    {
        st.projectKey.tonicPitchClass = static_cast<int>(ko.getProperty("tonic", 0));
        st.projectKey.mode            = ko.getProperty("mode", "minor").toString();
        st.projectKey.confidence      = static_cast<float>(static_cast<double>(ko.getProperty("confidence", 0.0)));
        st.projectKey.origin          = hpbridge::contextOriginFromString(ko.getProperty("origin", "unknown").toString());
    }

    if (const auto* srcs = parsed.getProperty("sources", {}).getArray())
    {
        for (const auto& item : *srcs)
        {
            if (!item.isObject()) continue;
            const int slot = juce::jlimit(0, kNumVisibleSources - 1, static_cast<int>(item.getProperty("slot", 0)));
            SourceAsset asset;
            asset.id          = item.getProperty("id", makeUuid()).toString();
            asset.visibleSlot = slot;
            asset.name        = item.getProperty("name", "Source").toString();
            asset.filePath    = item.getProperty("filePath", "").toString();
            asset.contentHash = item.getProperty("contentHash", "").toString();
            asset.sampleRate  = static_cast<double>(item.getProperty("sampleRate", 44100.0));
            asset.numFrames   = static_cast<int64_t>(item.getProperty("numFrames", 0));
            asset.color       = SourceColor::defaultForSlot(slot);

            if (reloadAudioFiles && asset.filePath.isNotEmpty())
            {
                juce::File f(asset.filePath);
                if (!loadAudioFileIntoSource(f, slot, asset))
                {
                    asset.missingFile = true;
                    res.missingSourceSlots.push_back(slot);
                }
            }
            else if (asset.filePath.isNotEmpty() && !juce::File(asset.filePath).existsAsFile())
            {
                asset.missingFile = true;
                res.missingSourceSlots.push_back(slot);
            }

            st.sources[static_cast<size_t>(slot)] = std::move(asset);
        }
    }

    if (const auto* slicesArr = parsed.getProperty("slices", {}).getArray())
    {
        for (const auto& item : *slicesArr)
        {
            if (!item.isObject()) continue;
            SliceDefinition sl;
            sl.id                = item.getProperty("id", makeUuid()).toString();
            sl.sourceId          = item.getProperty("sourceId", "").toString();
            sl.name              = item.getProperty("name", "Slice").toString();
            sl.startFrame        = static_cast<int64_t>(item.getProperty("startFrame", 0));
            sl.endFrameExclusive = static_cast<int64_t>(item.getProperty("endFrame", 1));
            sl.fadeInMs          = static_cast<float>(static_cast<double>(item.getProperty("fadeInMs", 2.0)));
            sl.fadeOutMs         = static_cast<float>(static_cast<double>(item.getProperty("fadeOutMs", 5.0)));
            sl.pinned            = static_cast<bool>(item.getProperty("pinned", false));
            st.slices.push_back(std::move(sl));
        }
    }

    if (const auto* padsArr = parsed.getProperty("pads", {}).getArray())
    {
        for (const auto& item : *padsArr)
        {
            if (!item.isObject()) continue;
            const int flat = static_cast<int>(item.getProperty("flatIndex", -1));
            if (flat < 0 || flat >= kTotalPads) continue;

            PadAssignment a;
            a.sliceId  = item.getProperty("sliceId", "").toString();
            a.sourceId = item.getProperty("sourceId", "").toString();
            a.label    = item.getProperty("label", "").toString();
            if (!item.getProperty("playbackOverride", {}).isVoid())
                a.settings.playbackModeOverride = static_cast<PlaybackMode>(static_cast<int>(item.getProperty("playbackOverride", 1)));
            if (!item.getProperty("overlapOverride", {}).isVoid())
                a.settings.overlapOverride = static_cast<OverlapPolicy>(static_cast<int>(item.getProperty("overlapOverride", 1)));
            a.settings.retriggerMode   = static_cast<RetriggerMode>(static_cast<int>(item.getProperty("retriggerMode", 0)));
            a.settings.chokeGroup      = static_cast<int>(item.getProperty("chokeGroup", 0));
            a.settings.loop            = static_cast<bool>(item.getProperty("loop", false));
            a.settings.reverse         = static_cast<bool>(item.getProperty("reverse", false));
            a.settings.pitchSemitones  = static_cast<float>(static_cast<double>(item.getProperty("pitchSemitones", 0.0)));
            a.settings.fineCents       = static_cast<float>(static_cast<double>(item.getProperty("fineCents", 0.0)));
            a.settings.syncMode        = static_cast<SyncMode>(static_cast<int>(item.getProperty("syncMode", 0)));
            a.settings.attackMs        = static_cast<float>(static_cast<double>(item.getProperty("attackMs", 1.0)));
            a.settings.releaseMs       = static_cast<float>(static_cast<double>(item.getProperty("releaseMs", 25.0)));
            a.settings.fadeInMs        = static_cast<float>(static_cast<double>(item.getProperty("fadeInMs", 2.0)));
            a.settings.fadeOutMs       = static_cast<float>(static_cast<double>(item.getProperty("fadeOutMs", 5.0)));
            a.settings.gainDb          = static_cast<float>(static_cast<double>(item.getProperty("gainDb", 0.0)));
            a.settings.pan             = static_cast<float>(static_cast<double>(item.getProperty("pan", 0.0)));
            a.settings.filterEnabled   = static_cast<bool>(item.getProperty("filterEnabled", false));
            a.settings.filterType      = static_cast<int>(item.getProperty("filterType", 0));
            a.settings.filterCutoffHz  = static_cast<float>(static_cast<double>(item.getProperty("filterCutoffHz", 12000.0)));
            a.settings.filterResonance = static_cast<float>(static_cast<double>(item.getProperty("filterResonance", 0.707)));
            a.settings.midiNote        = static_cast<int>(item.getProperty("midiNote", kBaseMidiNote));
            if (!item.getProperty("trimStart", {}).isVoid() && !item.getProperty("trimEnd", {}).isVoid())
                a.trimOverride = LocalTrimOverride { static_cast<int64_t>(item.getProperty("trimStart", 0)),
                                                     static_cast<int64_t>(item.getProperty("trimEnd", 1)) };

            st.pads[static_cast<size_t>(flat)].assignment = std::move(a);
        }
    }

    if (const auto pat = parsed.getProperty("pattern", {}); pat.isObject())
    {
        st.pattern.id          = pat.getProperty("id", st.pattern.id).toString();
        st.pattern.bars        = juce::jlimit(1, 16, static_cast<int>(pat.getProperty("bars", 2)));
        st.pattern.swingAmount = static_cast<float>(static_cast<double>(pat.getProperty("swingAmount", 0.0)));
        if (const auto* evs = pat.getProperty("events", {}).getArray())
        {
            for (const auto& item : *evs)
            {
                if (!item.isObject()) continue;
                PatternEvent ev;
                ev.id               = item.getProperty("id", makeUuid()).toString();
                ev.padId            = item.getProperty("padId", "").toString();
                ev.startTick        = static_cast<int64_t>(item.getProperty("startTick", 0));
                ev.rawStartTick     = static_cast<int64_t>(item.getProperty("rawStartTick", ev.startTick));
                ev.durationTicks    = static_cast<int64_t>(item.getProperty("durationTicks", 480));
                ev.velocity         = static_cast<float>(static_cast<double>(item.getProperty("velocity", 0.9)));
                ev.microOffsetTicks = static_cast<int32_t>(static_cast<int>(item.getProperty("microOffsetTicks", 0)));
                st.pattern.events.push_back(std::move(ev));
            }
        }
    }

    if (const auto mfx = parsed.getProperty("masterFx", {}); mfx.isObject())
    {
        st.masterFx.warmTubeCompEnabled = static_cast<bool>(mfx.getProperty("warmTubeCompEnabled", false));
        st.masterFx.driveDb             = static_cast<float>(static_cast<double>(mfx.getProperty("driveDb", 2.0)));
        st.masterFx.compMix             = static_cast<float>(static_cast<double>(mfx.getProperty("compMix", 0.5)));
        st.masterFx.limiterEnabled      = static_cast<bool>(mfx.getProperty("limiterEnabled", true));
        st.masterFx.masterGainDb        = static_cast<float>(static_cast<double>(mfx.getProperty("masterGainDb", 0.0)));
    }

    project = std::move(fresh);
    project.bumpRevision();
    return res;
}
} // namespace hps
