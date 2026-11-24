#include "model/SamplerModel.h"

#include <algorithm>
#include <cmath>

namespace hps
{
int64_t gridDivisionToTicks(GridDivision div) noexcept
{
    switch (div)
    {
        case GridDivision::Quarter:          return kTicksPerQuarter;
        case GridDivision::Eighth:           return kTicksPerQuarter / 2;
        case GridDivision::Sixteenth:        return kTicksPerQuarter / 4;
        case GridDivision::ThirtySecond:     return kTicksPerQuarter / 8;
        case GridDivision::TripletEighth:    return kTicksPerQuarter / 3;
        case GridDivision::TripletSixteenth: return kTicksPerQuarter / 6;
        case GridDivision::Off:              return 0;
    }
    return kTicksPerQuarter / 4;
}

juce::String gridDivisionLabel(GridDivision div)
{
    switch (div)
    {
        case GridDivision::Quarter:          return "1/4";
        case GridDivision::Eighth:           return "1/8";
        case GridDivision::Sixteenth:        return "1/16";
        case GridDivision::ThirtySecond:     return "1/32";
        case GridDivision::TripletEighth:    return "1/8T";
        case GridDivision::TripletSixteenth: return "1/16T";
        case GridDivision::Off:              return "OFF";
    }
    return "1/16";
}

juce::String makeUuid()
{
    return juce::Uuid().toDashedString();
}

SamplerProject::SamplerProject()
{
    current.projectId  = makeUuid();
    current.instanceId = "hpsampler-" + makeUuid().substring(0, 8);
    current.pattern.id = makeUuid();

    for (int bank = 0; bank < kNumBanks; ++bank)
    {
        for (int pad = 0; pad < kPadsPerBank; ++pad)
        {
            const int idx = bank * kPadsPerBank + pad;
            auto& slot = current.pads[static_cast<size_t>(idx)];
            slot.padId           = "pad-" + juce::String(bank) + "-" + juce::String(pad);
            slot.bankIndex       = bank;
            slot.padIndex        = pad;
            slot.defaultMidiNote = kBaseMidiNote + pad;
        }
    }
}

void SamplerProject::pushUndoPoint(const juce::String&)
{
    undoStack.push_back(current);
    if (undoStack.size() > 64)
        undoStack.erase(undoStack.begin());
    redoStack.clear();
}

bool SamplerProject::undo()
{
    if (undoStack.empty()) return false;
    redoStack.push_back(current);
    current = std::move(undoStack.back());
    undoStack.pop_back();
    bumpRevision();
    return true;
}

bool SamplerProject::redo()
{
    if (redoStack.empty()) return false;
    undoStack.push_back(current);
    current = std::move(redoStack.back());
    redoStack.pop_back();
    bumpRevision();
    return true;
}

SourceAsset* SamplerProject::getSourceBySlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return nullptr;
    auto& opt = current.sources[static_cast<size_t>(slotIndex)];
    return opt.has_value() ? &(*opt) : nullptr;
}

const SourceAsset* SamplerProject::getSourceBySlot(int slotIndex) const
{
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return nullptr;
    const auto& opt = current.sources[static_cast<size_t>(slotIndex)];
    return opt.has_value() ? &(*opt) : nullptr;
}

SourceAsset* SamplerProject::getSourceById(const juce::String& sourceId)
{
    for (auto& opt : current.sources)
        if (opt.has_value() && opt->id == sourceId)
            return &(*opt);
    return nullptr;
}

const SourceAsset* SamplerProject::getSourceById(const juce::String& sourceId) const
{
    for (const auto& opt : current.sources)
        if (opt.has_value() && opt->id == sourceId)
            return &(*opt);
    return nullptr;
}

int SamplerProject::findSourceSlotById(const juce::String& sourceId) const
{
    for (int i = 0; i < kNumVisibleSources; ++i)
        if (current.sources[static_cast<size_t>(i)].has_value() && current.sources[static_cast<size_t>(i)]->id == sourceId)
            return i;
    return -1;
}

int SamplerProject::countDependentPadsForSource(const juce::String& sourceId) const
{
    int count = 0;
    for (const auto& pad : current.pads)
        if (pad.assignment.has_value() && pad.assignment->sourceId == sourceId)
            ++count;
    return count;
}

void SamplerProject::setSourceInSlot(int slotIndex, SourceAsset asset)
{
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return;
    pushUndoPoint("Load Source");
    asset.visibleSlot = slotIndex;
    if (asset.id.isEmpty()) asset.id = makeUuid();
    asset.color = SourceColor::defaultForSlot(slotIndex);
    current.sources[static_cast<size_t>(slotIndex)] = std::move(asset);
    candidates[static_cast<size_t>(slotIndex)].clear();
    bumpRevision();
}

void SamplerProject::clearSourceSlot(int slotIndex, bool clearDependentPads)
{
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return;
    auto& opt = current.sources[static_cast<size_t>(slotIndex)];
    if (!opt.has_value()) return;

    pushUndoPoint("Clear Source");
    const juce::String removedId = opt->id;
    opt.reset();
    candidates[static_cast<size_t>(slotIndex)].clear();

    if (clearDependentPads)
    {
        for (auto& pad : current.pads)
            if (pad.assignment.has_value() && pad.assignment->sourceId == removedId)
                pad.assignment.reset();
        current.slices.erase(
            std::remove_if(current.slices.begin(), current.slices.end(),
                           [&](const SliceDefinition& s) { return s.sourceId == removedId; }),
            current.slices.end());
    }
    bumpRevision();
}

void SamplerProject::setProjectAnchorFromSource(int slotIndex)
{
    const auto* src = getSourceBySlot(slotIndex);
    if (src == nullptr) return;
    pushUndoPoint("Set Project Anchor");
    if (src->analysis.bpm.has_value() && *src->analysis.bpm >= 20.0 && *src->analysis.bpm <= 300.0)
        current.projectBpm = *src->analysis.bpm;
    if (src->analysis.key.tonicPitchClass.has_value())
        current.projectKey = src->analysis.key;

    switch (slotIndex)
    {
        case 0: current.anchorSource = AnchorSource::Src1; break;
        case 1: current.anchorSource = AnchorSource::Src2; break;
        case 2: current.anchorSource = AnchorSource::Src3; break;
        case 3: current.anchorSource = AnchorSource::Src4; break;
        default: break;
    }
    current.bpmLocked = true;
    current.keyLocked = src->analysis.key.tonicPitchClass.has_value();
    bumpRevision();
}

void SamplerProject::clearProjectAnchor()
{
    pushUndoPoint("Clear Project Anchor");
    current.anchorSource = AnchorSource::Manual;
    current.bpmLocked    = false;
    current.keyLocked    = false;
    bumpRevision();
}

void SamplerProject::replaceCandidatesForSlot(int slotIndex, std::vector<SliceCandidate> newCandidates)
{
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return;
    auto& slotCands = candidates[static_cast<size_t>(slotIndex)];

    // Preserve pinned candidates! Rescan NEVER destroys pinned candidates or accepted slices/pads.
    std::vector<SliceCandidate> merged;
    for (const auto& c : slotCands)
        if (c.pinned)
            merged.push_back(c);

    for (auto& nc : newCandidates)
    {
        const bool overlapsPinned = std::any_of(merged.begin(), merged.end(), [&](const SliceCandidate& p) {
            return std::abs(p.startFrame - nc.startFrame) < 1024;
        });
        if (!overlapsPinned)
            merged.push_back(std::move(nc));
    }

    std::sort(merged.begin(), merged.end(), [](const SliceCandidate& a, const SliceCandidate& b) {
        return a.startFrame < b.startFrame;
    });
    slotCands = std::move(merged);
}

const SliceDefinition* SamplerProject::getSliceById(const juce::String& sliceId) const
{
    for (const auto& s : current.slices)
        if (s.id == sliceId) return &s;
    return nullptr;
}

SliceDefinition* SamplerProject::getSliceById(const juce::String& sliceId)
{
    for (auto& s : current.slices)
        if (s.id == sliceId) return &s;
    return nullptr;
}

SliceDefinition SamplerProject::createSlice(const juce::String& sourceId,
                                            int64_t startFrame,
                                            int64_t endFrameExclusive,
                                            const juce::String& name)
{
    const auto* src = getSourceById(sourceId);
    const int64_t maxFrames = src != nullptr ? std::max<int64_t>(1, src->numFrames) : std::max<int64_t>(1, endFrameExclusive);
    startFrame        = juce::jlimit<int64_t>(0, maxFrames - 1, startFrame);
    endFrameExclusive = juce::jlimit<int64_t>(startFrame + 1, maxFrames, endFrameExclusive);

    pushUndoPoint("Create Slice");
    SliceDefinition def;
    def.id                = makeUuid();
    def.sourceId          = sourceId;
    def.startFrame        = startFrame;
    def.endFrameExclusive = endFrameExclusive;
    def.name              = name.isNotEmpty() ? name : ("Slice " + juce::String(current.slices.size() + 1));
    current.slices.push_back(def);
    bumpRevision();
    return def;
}

SliceDefinition SamplerProject::acceptCandidate(int slotIndex, const juce::String& candidateId)
{
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return {};
    const auto& slotCands = candidates[static_cast<size_t>(slotIndex)];
    for (const auto& c : slotCands)
    {
        if (c.id == candidateId)
        {
            auto def = createSlice(c.sourceId, c.startFrame, c.endFrameExclusive, c.originLabel);
            def.pinned = c.pinned;
            if (auto* stored = getSliceById(def.id))
                stored->pinned = c.pinned;
            return def;
        }
    }
    return {};
}

std::vector<SliceDefinition> SamplerProject::acceptAllCandidates(int slotIndex)
{
    std::vector<SliceDefinition> accepted;
    if (slotIndex < 0 || slotIndex >= kNumVisibleSources) return accepted;
    const auto& slotCands = candidates[static_cast<size_t>(slotIndex)];
    if (slotCands.empty()) return accepted;

    pushUndoPoint("Accept All Candidates");
    for (const auto& c : slotCands)
    {
        const auto* src = getSourceById(c.sourceId);
        if (src == nullptr || src->numFrames <= 0) continue;
        SliceDefinition def;
        def.id                = makeUuid();
        def.sourceId          = c.sourceId;
        def.startFrame        = juce::jlimit<int64_t>(0, src->numFrames - 1, c.startFrame);
        def.endFrameExclusive = juce::jlimit<int64_t>(def.startFrame + 1, src->numFrames, c.endFrameExclusive);
        def.name              = c.originLabel.isNotEmpty() ? c.originLabel : ("Spot " + juce::String(accepted.size() + 1));
        def.pinned            = c.pinned;
        current.slices.push_back(def);
        accepted.push_back(def);
    }
    bumpRevision();
    return accepted;
}

PadSlot& SamplerProject::getPad(int flatIndex)
{
    return current.pads[static_cast<size_t>(juce::jlimit(0, kTotalPads - 1, flatIndex))];
}

const PadSlot& SamplerProject::getPad(int flatIndex) const
{
    return current.pads[static_cast<size_t>(juce::jlimit(0, kTotalPads - 1, flatIndex))];
}

PadSlot* SamplerProject::getPadById(const juce::String& padId)
{
    for (auto& p : current.pads)
        if (p.padId == padId) return &p;
    return nullptr;
}

const PadSlot* SamplerProject::getPadById(const juce::String& padId) const
{
    for (const auto& p : current.pads)
        if (p.padId == padId) return &p;
    return nullptr;
}

int SamplerProject::findPadFlatIndexById(const juce::String& padId) const
{
    for (int i = 0; i < kTotalPads; ++i)
        if (current.pads[static_cast<size_t>(i)].padId == padId)
            return i;
    return -1;
}

int SamplerProject::findPadFlatIndexByMidiNote(int bankIndex, int midiNote) const
{
    bankIndex = juce::jlimit(0, kNumBanks - 1, bankIndex);
    // First check active bank custom mappings, then default mappings
    for (int p = 0; p < kPadsPerBank; ++p)
    {
        const int flat = bankIndex * kPadsPerBank + p;
        const auto& slot = current.pads[static_cast<size_t>(flat)];
        const int note = slot.assignment.has_value() ? slot.assignment->settings.midiNote : slot.defaultMidiNote;
        if (note == midiNote) return flat;
    }
    return -1;
}

bool SamplerProject::assignSliceToPad(const juce::String& sliceId, int flatPadIndex, bool allowOverwrite)
{
    if (flatPadIndex < 0 || flatPadIndex >= kTotalPads) return false;
    const auto* slice = getSliceById(sliceId);
    if (slice == nullptr) return false;

    auto& pad = current.pads[static_cast<size_t>(flatPadIndex)];
    if (pad.assignment.has_value() && !allowOverwrite)
        return false;

    pushUndoPoint("Assign Slice to Pad");
    PadAssignment assign;
    assign.sliceId           = slice->id;
    assign.sourceId          = slice->sourceId;
    assign.label             = slice->name;
    assign.settings.midiNote = pad.defaultMidiNote;
    assign.settings.fadeInMs = slice->fadeInMs;
    assign.settings.fadeOutMs = slice->fadeOutMs;
    pad.assignment           = std::move(assign);
    bumpRevision();
    return true;
}

int SamplerProject::autoMapSlicesToEmptyPads(const std::vector<juce::String>& sliceIds,
                                             int startBank,
                                             int startPadInBank,
                                             bool overwriteOccupied)
{
    if (sliceIds.empty()) return 0;
    pushUndoPoint("Auto Map Slices to Pads");
    int mapped = 0;
    int cursor = juce::jlimit(0, kTotalPads - 1, startBank * kPadsPerBank + startPadInBank);

    for (const auto& sid : sliceIds)
    {
        const auto* slice = getSliceById(sid);
        if (slice == nullptr) continue;

        while (cursor < kTotalPads && !overwriteOccupied && current.pads[static_cast<size_t>(cursor)].assignment.has_value())
            ++cursor;
        if (cursor >= kTotalPads) break;

        auto& pad = current.pads[static_cast<size_t>(cursor)];
        PadAssignment assign;
        assign.sliceId            = slice->id;
        assign.sourceId           = slice->sourceId;
        assign.label              = slice->name;
        assign.settings.midiNote  = pad.defaultMidiNote;
        assign.settings.fadeInMs  = slice->fadeInMs;
        assign.settings.fadeOutMs = slice->fadeOutMs;
        pad.assignment            = std::move(assign);
        ++mapped;
        ++cursor;
    }
    if (mapped > 0) bumpRevision();
    return mapped;
}

void SamplerProject::clearPad(int flatPadIndex)
{
    if (flatPadIndex < 0 || flatPadIndex >= kTotalPads) return;
    if (!current.pads[static_cast<size_t>(flatPadIndex)].assignment.has_value()) return;
    pushUndoPoint("Clear Pad");
    current.pads[static_cast<size_t>(flatPadIndex)].assignment.reset();
    bumpRevision();
}

void SamplerProject::copyPad(int fromFlatIndex, int toFlatIndex)
{
    if (fromFlatIndex < 0 || fromFlatIndex >= kTotalPads || toFlatIndex < 0 || toFlatIndex >= kTotalPads || fromFlatIndex == toFlatIndex)
        return;
    const auto& src = current.pads[static_cast<size_t>(fromFlatIndex)];
    if (!src.assignment.has_value()) return;
    pushUndoPoint("Copy Pad");
    auto copy = *src.assignment;
    copy.settings.midiNote = current.pads[static_cast<size_t>(toFlatIndex)].defaultMidiNote;
    current.pads[static_cast<size_t>(toFlatIndex)].assignment = std::move(copy);
    bumpRevision();
}

int SamplerProject::getPadSourceSlot(int flatPadIndex) const
{
    if (flatPadIndex < 0 || flatPadIndex >= kTotalPads) return -1;
    const auto& pad = current.pads[static_cast<size_t>(flatPadIndex)];
    if (!pad.assignment.has_value()) return -1;
    return findSourceSlotById(pad.assignment->sourceId);
}

SourceColor SamplerProject::getPadColor(int flatPadIndex) const
{
    const int slot = getPadSourceSlot(flatPadIndex);
    if (slot >= 0)
        if (const auto* src = getSourceBySlot(slot))
            return src->color;
    return { juce::Colour(0xff3a3f47), "--" };
}

int64_t SamplerProject::getPatternLengthTicks() const noexcept
{
    const int64_t ticksPerBar = static_cast<int64_t>(kTicksPerQuarter) * current.meterNumerator * 4 / std::max(1, current.meterDenominator);
    return ticksPerBar * std::max(1, current.pattern.bars);
}

void SamplerProject::applyRecordQuantizeToEvent(PatternEvent& ev) const
{
    const int64_t step = gridDivisionToTicks(current.gridDivision);
    if (!current.recordQuantizeOn || step <= 0)
    {
        ev.startTick = ev.rawStartTick;
        return;
    }
    const double nearest = std::round(static_cast<double>(ev.rawStartTick) / static_cast<double>(step)) * static_cast<double>(step);
    const double blended = static_cast<double>(ev.rawStartTick) + (nearest - static_cast<double>(ev.rawStartTick)) * juce::jlimit(0.0f, 1.0f, current.recordQuantizeAmt);
    const int64_t len = getPatternLengthTicks();
    ev.startTick = juce::jlimit<int64_t>(0, std::max<int64_t>(0, len - 1), static_cast<int64_t>(std::llround(blended)));
}

void SamplerProject::reapplyRecordQuantizeAll()
{
    pushUndoPoint("Update Quantize");
    for (auto& ev : current.pattern.events)
        applyRecordQuantizeToEvent(ev);
    bumpRevision();
}

void SamplerProject::addPatternEvent(PatternEvent ev, bool recordUndo)
{
    if (ev.id.isEmpty()) ev.id = makeUuid();
    const int64_t len = getPatternLengthTicks();
    ev.rawStartTick = juce::jlimit<int64_t>(0, std::max<int64_t>(0, len - 1), ev.rawStartTick);
    ev.startTick    = juce::jlimit<int64_t>(0, std::max<int64_t>(0, len - 1), ev.startTick);
    if (recordUndo) pushUndoPoint("Add Pattern Event");
    current.pattern.events.push_back(std::move(ev));
    std::sort(current.pattern.events.begin(), current.pattern.events.end(),
              [](const PatternEvent& a, const PatternEvent& b) { return a.startTick < b.startTick; });
    bumpRevision();
}

void SamplerProject::removePatternEvent(const juce::String& eventId)
{
    const auto oldSize = current.pattern.events.size();
    auto it = std::remove_if(current.pattern.events.begin(), current.pattern.events.end(),
                             [&](const PatternEvent& e) { return e.id == eventId; });
    if (it == current.pattern.events.end()) return;
    pushUndoPoint("Remove Pattern Event");
    // re-find because pushUndoPoint copied current
    current.pattern.events.erase(
        std::remove_if(current.pattern.events.begin(), current.pattern.events.end(),
                       [&](const PatternEvent& e) { return e.id == eventId; }),
        current.pattern.events.end());
    if (current.pattern.events.size() != oldSize) bumpRevision();
}

void SamplerProject::clearPattern()
{
    if (current.pattern.events.empty()) return;
    pushUndoPoint("Clear Pattern");
    current.pattern.events.clear();
    bumpRevision();
}

int SamplerProject::getEventSourceLane(const PatternEvent& ev) const
{
    const int flat = findPadFlatIndexById(ev.padId);
    return flat >= 0 ? getPadSourceSlot(flat) : -1;
}

void SamplerProject::setExternalDrumGuide(std::optional<hpbridge::DrumGuideSnapshot> guide)
{
    if (drumGuideFrozen && externalDrumGuide.has_value())
        return;
    externalDrumGuide = std::move(guide);
}

hpbridge::SamplerContextSnapshot SamplerProject::buildSamplerContextSnapshot(hpbridge::ContextScope scope, int activeSourceSlot) const
{
    hpbridge::SamplerContextSnapshot snap;
    snap.schema            = hpbridge::kSamplerContextSchemaV1;
    snap.revision          = static_cast<int64_t>(std::max<uint64_t>(1, current.revision));
    snap.projectId         = current.projectId;
    snap.samplerInstanceId = current.instanceId;
    snap.scope             = scope;

    snap.timebase.ticksPerQuarter = kTicksPerQuarter;
    snap.timebase.originTick      = 0;

    snap.transport.bpm              = juce::jlimit(20.0, 300.0, current.projectBpm);
    snap.transport.meterNumerator   = current.meterNumerator;
    snap.transport.meterDenominator = current.meterDenominator;
    snap.transport.hostSync         = current.hostSyncEnabled;
    snap.transport.loopBars         = current.pattern.bars;

    snap.key.tonicPitchClass = current.projectKey.tonicPitchClass;
    snap.key.mode            = current.projectKey.mode;
    if (current.projectKey.tonicPitchClass.has_value())
        snap.key.confidence = current.projectKey.confidence;
    snap.key.origin          = current.projectKey.origin;

    for (int i = 0; i < kNumVisibleSources; ++i)
    {
        if (scope == hpbridge::ContextScope::ActiveSrc && i != activeSourceSlot)
            continue;
        if (const auto* src = getSourceBySlot(i))
            snap.sources.push_back({ src->id, src->name, src->analysis.bpm });
    }

    snap.patternBars = current.pattern.bars;
    const int64_t ticksPerBar = static_cast<int64_t>(kTicksPerQuarter) * current.meterNumerator * 4 / std::max(1, current.meterDenominator);
    const int64_t totalTicks  = ticksPerBar * snap.patternBars;

    for (int bar = 0; bar < snap.patternBars; ++bar)
    {
        snap.beatGrid.downbeatTicks.push_back(bar * ticksPerBar);
        for (int b = 0; b < current.meterNumerator; ++b)
            snap.beatGrid.beatTicks.push_back(bar * ticksPerBar + b * kTicksPerQuarter);
    }

    snap.groove.subdivision = gridDivisionLabel(current.gridDivision);
    snap.groove.swingAmount = current.pattern.swingAmount;

    for (const auto& ev : current.pattern.events)
    {
        const auto* pad = getPadById(ev.padId);
        if (pad == nullptr || !pad->assignment.has_value()) continue;
        if (scope == hpbridge::ContextScope::ActiveSrc)
        {
            const int lane = findSourceSlotById(pad->assignment->sourceId);
            if (lane != activeSourceSlot) continue;
        }

        hpbridge::PatternEventDto dto;
        dto.tick          = juce::jlimit<int64_t>(0, totalTicks, ev.startTick);
        dto.durationTicks = std::max<int64_t>(0, ev.durationTicks);
        dto.padId         = ev.padId;
        dto.sliceId       = pad->assignment->sliceId;
        dto.sourceId      = pad->assignment->sourceId;
        dto.velocity      = juce::jlimit(0.0f, 1.0f, ev.velocity);
        const int totalPitch = static_cast<int>(std::llround(pad->assignment->settings.pitchSemitones))
                               + ev.pitchOverrideSemitones.value_or(0);
        if (totalPitch != 0) dto.pitchSemitones = totalPitch;
        snap.patternEvents.push_back(std::move(dto));

        // Accent hint derived from actual pattern trigger
        snap.accents.push_back({ dto.tick, dto.velocity, "mid" });
    }

    if (current.projectKey.tonicPitchClass.has_value())
    {
        hpbridge::HarmonySegmentDto h;
        h.startTick      = 0;
        h.endTick        = totalTicks;
        h.chord          = current.projectKey.label();
        h.rootPitchClass = current.projectKey.tonicPitchClass;
        h.confidence     = current.projectKey.confidence;
        h.origin         = current.projectKey.origin;
        snap.harmony.push_back(std::move(h));
    }

    snap.phrases.push_back({ 0, totalTicks, 0.5f });

    // Low-end occupancy grid (1/16th steps)
    snap.lowEnd.gridStepTicks = kTicksPerQuarter / 4;
    const int numSteps = static_cast<int>(totalTicks / snap.lowEnd.gridStepTicks);
    snap.lowEnd.occupancy.assign(static_cast<size_t>(std::max(1, numSteps)), 0.0f);
    for (const auto& ev : snap.patternEvents)
    {
        const int step = juce::jlimit(0, numSteps - 1, static_cast<int>(ev.tick / snap.lowEnd.gridStepTicks));
        snap.lowEnd.occupancy[static_cast<size_t>(step)] = std::min(1.0f, snap.lowEnd.occupancy[static_cast<size_t>(step)] + 0.5f * ev.velocity);
    }

    return snap;
}
} // namespace hps
