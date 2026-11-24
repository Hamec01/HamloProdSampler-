#include "core/Project.h"

namespace hps
{
double effectiveSourceBpm(const SourceSlot& slot, const SourceData* data)
{
    if (slot.bpmOverride > 20.0) return slot.bpmOverride;
    if (data != nullptr && data->analysis.ready && data->analysis.bpm > 20.0
        && data->analysis.bpmConfidence >= kReliableBpmConfidence)
        return data->analysis.bpm;
    return 0.0;
}

double effectiveSourceOrigin(const SourceSlot& slot, const SourceData* data)
{
    if (slot.originOverride >= 0.0) return slot.originOverride;
    if (data != nullptr && data->analysis.ready) return data->analysis.originSeconds;
    return 0.0;
}

MusicalKey effectiveSourceKey(const SourceSlot& slot, const SourceData* data)
{
    if (slot.keyOverrideRoot == -1) return {};
    if (slot.keyOverrideRoot >= 0) return { slot.keyOverrideRoot, slot.keyOverrideMode };
    if (data != nullptr && data->analysis.ready && !data->analysis.atonal) return data->analysis.key;
    return {};
}

PlayMode effectivePlayMode(const Pad& pad, const ProjectSettings& s)
{
    return pad.s.playMode < 0 ? s.defaultMode : static_cast<PlayMode>(pad.s.playMode);
}

SyncMode effectiveSync(const Pad& pad, const ProjectSettings& s)
{
    return pad.s.sync < 0 ? s.sync : static_cast<SyncMode>(pad.s.sync);
}

juce::String newId()
{
    return juce::Uuid().toDashedString();
}

Project::Project()
{
    for (int i = 0; i < kNumSources; ++i)
        current.sources[(size_t) i].colour = sourceColour(i);
}

void Project::edit(const juce::String& label, const std::function<void(ProjectState&)>& fn)
{
    if (!inGesture || !gesturePushed)
    {
        undoStack.push_back({ current, inGesture ? gestureLabel : label });
        if (undoStack.size() > kMaxUndo) undoStack.erase(undoStack.begin());
        redoStack.clear();
        if (inGesture) gesturePushed = true;
    }
    fn(current);
    changed();
}

void Project::beginGesture(const juce::String& label)
{
    inGesture = true;
    gesturePushed = false;
    gestureLabel = label;
}

void Project::endGesture()
{
    inGesture = false;
    gesturePushed = false;
}

void Project::replaceState(const ProjectState& s, bool clearHistory)
{
    current = s;
    if (clearHistory) { undoStack.clear(); redoStack.clear(); }
    changed();
}

void Project::undo()
{
    if (undoStack.empty()) return;
    redoStack.push_back({ current, undoStack.back().label });
    current = undoStack.back().state;
    undoStack.pop_back();
    changed();
}

void Project::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back({ current, redoStack.back().label });
    current = redoStack.back().state;
    redoStack.pop_back();
    changed();
}

std::shared_ptr<const SourceData> Project::sourceData(const juce::String& id) const
{
    auto it = store.find(id);
    return it == store.end() ? nullptr : it->second;
}

std::shared_ptr<const SourceData> Project::sourceDataForSlot(int slot) const
{
    if (slot < 0 || slot >= kNumSources) return nullptr;
    const auto& s = current.sources[(size_t) slot];
    return s.empty() ? nullptr : sourceData(s.id);
}

void Project::setSourceData(std::shared_ptr<const SourceData> data)
{
    if (data == nullptr) return;
    store[data->id] = std::move(data);
    changed();
}

int Project::firstEmptyPad(int fromFlat) const
{
    for (int i = juce::jmax(0, fromFlat); i < kNumPads; ++i)
        if (!current.pads[(size_t) i].assigned) return i;
    return -1;
}

std::vector<int> Project::padsUsingSource(int slot) const
{
    std::vector<int> r;
    for (int i = 0; i < kNumPads; ++i)
        if (current.pads[(size_t) i].assigned && current.pads[(size_t) i].src == slot) r.push_back(i);
    return r;
}

void Project::changed()
{
    ++rev;
    if (onChanged) onChanged();
}
} // namespace hps
