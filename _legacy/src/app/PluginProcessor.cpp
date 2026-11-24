#include "app/PluginProcessor.h"
#include "app/PluginEditor.h"

namespace hps
{
HPSamplerAudioProcessor::HPSamplerAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    publishProjectToAudio();
    startTimerHz(30); // Drain recorded hits & poll live HPDG guide off the audio thread
}

HPSamplerAudioProcessor::~HPSamplerAudioProcessor()
{
    stopTimer();
}

void HPSamplerAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate > 8000.0 ? sampleRate : 44100.0;
    voiceEngine.prepare(currentSampleRate, samplesPerBlock);
    publishProjectToAudio();
}

void HPSamplerAudioProcessor::releaseResources()
{
    voiceEngine.reset();
}

bool HPSamplerAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void HPSamplerAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    bool hostPlaying = false;
    double hostBpm = 0.0;
    double hostPpq = -1.0;

    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            hostPlaying = pos->getIsPlaying();
            if (const auto b = pos->getBpm()) hostBpm = *b;
            if (const auto p = pos->getPpqPosition()) hostPpq = *p;
        }
    }

    voiceEngine.processBlock(buffer, midiMessages, hostPlaying, hostBpm, hostPpq);
}

juce::AudioProcessorEditor* HPSamplerAudioProcessor::createEditor()
{
    return new HPSamplerAudioProcessorEditor(*this);
}

void HPSamplerAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const auto json = StateSerializer::serializeToJson(project);
    destData.replaceAll(json.toRawUTF8(), json.getNumBytesAsUTF8());
}

void HPSamplerAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0) return;
    const auto json = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    const auto res = StateSerializer::deserializeFromJson(json, project, true);
    if (res.ok)
    {
        publishProjectToAudio();
        if (onProjectChanged) onProjectChanged();
    }
}

void HPSamplerAudioProcessor::publishProjectToAudio()
{
    voiceEngine.publishSnapshot(VoiceEngine::buildSnapshot(project));
}

bool HPSamplerAudioProcessor::loadSourceFile(int slotIndex, const juce::File& file, juce::String* errorOut)
{
    SourceAsset asset;
    if (!StateSerializer::loadAudioFileIntoSource(file, slotIndex, asset, errorOut))
        return false;

    project.setSourceInSlot(slotIndex, std::move(asset));
    publishProjectToAudio();
    if (onProjectChanged) onProjectChanged();
    return true;
}

std::vector<hpbridge::BridgePeerInfo> HPSamplerAudioProcessor::discoverBridgePeers()
{
    auto peers = hpbridge::HamloLinkReader::discoverLiveHpdgInstances();
    auto filePeers = fileTransport.discoverPeers();
    peers.insert(peers.end(), filePeers.begin(), filePeers.end());
    return peers;
}

bool HPSamplerAudioProcessor::linkToLiveHpdgSlot(const hpbridge::BridgePeerInfo& peer, juce::String* errorOut)
{
    hpbridge::DrumGuideSnapshot guide;
    const auto res = hpbridge::HamloLinkReader::readDrumGuideFromSlot(peer.backingFile, guide);
    if (!res.ok)
    {
        bridgeState = hpbridge::BridgeConnectionState::Stale;
        if (errorOut != nullptr) *errorOut = res.error;
        return false;
    }

    linkedHpdgSlotFile = peer.backingFile;
    bridgePartnerName  = peer.displayName;
    bridgeState        = hpbridge::BridgeConnectionState::GuideLinkedReadOnly;
    project.setExternalDrumGuide(std::move(guide));
    if (onProjectChanged) onProjectChanged();
    return true;
}

void HPSamplerAudioProcessor::disconnectBridge()
{
    linkedHpdgSlotFile = {};
    bridgePartnerName.clear();
    bridgeState = hpbridge::BridgeConnectionState::Disconnected;
    if (onProjectChanged) onProjectChanged();
}

bool HPSamplerAudioProcessor::exportSamplerSnapshot(const juce::File& file, hpbridge::ContextScope scope, int activeSrc, juce::String* errorOut)
{
    const auto snap = project.buildSamplerContextSnapshot(scope, activeSrc);
    const auto res = fileTransport.publishSamplerContext(snap, file);
    if (!res.ok && errorOut != nullptr) *errorOut = res.error;
    return res.ok;
}

bool HPSamplerAudioProcessor::importDrumGuideSnapshot(const juce::File& file, juce::String* errorOut)
{
    hpbridge::DrumGuideSnapshot guide;
    const auto res = fileTransport.loadDrumGuide(file, guide);
    if (!res.ok)
    {
        if (errorOut != nullptr) *errorOut = res.error;
        return false;
    }
    project.setExternalDrumGuide(std::move(guide));
    bridgePartnerName = file.getFileName();
    if (bridgeState == hpbridge::BridgeConnectionState::Disconnected)
        bridgeState = hpbridge::BridgeConnectionState::Available;
    if (onProjectChanged) onProjectChanged();
    return true;
}

void HPSamplerAudioProcessor::rebuildResultWaveformCache()
{
    resultCache = VoiceEngine::renderPatternOffline(project, currentSampleRate);
    resultCacheRevision = project.state().revision;
}

void HPSamplerAudioProcessor::timerCallback()
{
    // 1. Drain live recorded pad hits into Pattern (with optional non-destructive quantize)
    bool recordedAny = false;
    RecordedPadHit hit;
    while (voiceEngine.popRecordedHit(hit))
    {
        const auto& pad = project.getPad(hit.flatPadIndex);
        PatternEvent ev;
        ev.id            = makeUuid();
        ev.padId         = pad.padId;
        ev.rawStartTick  = hit.startTick;
        ev.startTick     = hit.startTick;
        ev.durationTicks = hit.durationTicks;
        ev.velocity      = hit.velocity;
        project.applyRecordQuantizeToEvent(ev);

        // Deduplicate loop-wrap identical hits within 16 ticks on the same pad
        bool dup = false;
        for (const auto& existing : project.state().pattern.events)
        {
            if (existing.padId == ev.padId && std::abs(existing.rawStartTick - ev.rawStartTick) < 16)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
        {
            project.addPatternEvent(std::move(ev), !recordedAny);
            recordedAny = true;
        }
    }
    if (recordedAny)
    {
        publishProjectToAudio();
        if (onProjectChanged) onProjectChanged();
    }

    // 2. Poll live read-only HPDG HamloLink slot if linked
    if (bridgeState == hpbridge::BridgeConnectionState::GuideLinkedReadOnly && linkedHpdgSlotFile.existsAsFile())
    {
        hpbridge::DrumGuideSnapshot guide;
        const auto res = hpbridge::HamloLinkReader::readDrumGuideFromSlot(linkedHpdgSlotFile, guide);
        if (res.ok)
        {
            const auto& prev = project.getExternalDrumGuide();
            if (!prev.has_value() || prev->revision != guide.revision || prev->drumEvents.size() != guide.drumEvents.size())
            {
                project.setExternalDrumGuide(std::move(guide));
                if (onProjectChanged) onProjectChanged();
            }
        }
        else
        {
            bridgeState = hpbridge::BridgeConnectionState::Stale;
            if (onProjectChanged) onProjectChanged();
        }
    }
}
} // namespace hps

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new hps::HPSamplerAudioProcessor();
}
