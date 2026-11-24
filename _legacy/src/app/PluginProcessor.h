#pragma once

#include <atomic>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "audio/VoiceEngine.h"
#include "hpbridge/FileSnapshotTransport.h"
#include "hpbridge/HamloLinkReader.h"
#include "model/SamplerModel.h"
#include "persistence/StateSerializer.h"

namespace hps
{
class HPSamplerAudioProcessor final : public juce::AudioProcessor,
                                      private juce::Timer
{
public:
    HPSamplerAudioProcessor();
    ~HPSamplerAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HPSAMPLER"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Project & Engine access (UI thread)
    SamplerProject&       getProject() noexcept { return project; }
    const SamplerProject& getProject() const noexcept { return project; }
    VoiceEngine&          getVoiceEngine() noexcept { return voiceEngine; }
    const VoiceEngine&    getVoiceEngine() const noexcept { return voiceEngine; }

    void publishProjectToAudio();
    bool loadSourceFile(int slotIndex, const juce::File& file, juce::String* errorOut = nullptr);

    // Bridge (HPBridgeCore + HamloLinkReader)
    hpbridge::FileSnapshotTransport& getFileTransport() noexcept { return fileTransport; }
    hpbridge::BridgeConnectionState  getBridgeState() const noexcept { return bridgeState; }
    juce::String                     getBridgePartnerName() const { return bridgePartnerName; }
    std::vector<hpbridge::BridgePeerInfo> discoverBridgePeers();
    bool linkToLiveHpdgSlot(const hpbridge::BridgePeerInfo& peer, juce::String* errorOut = nullptr);
    void disconnectBridge();
    bool exportSamplerSnapshot(const juce::File& file, hpbridge::ContextScope scope, int activeSrc, juce::String* errorOut = nullptr);
    bool importDrumGuideSnapshot(const juce::File& file, juce::String* errorOut = nullptr);

    // Result waveform cache
    const juce::AudioBuffer<float>& getResultWaveformCache() const noexcept { return resultCache; }
    uint64_t getResultCacheRevision() const noexcept { return resultCacheRevision; }
    void rebuildResultWaveformCache();

    // Change notification for editor
    std::function<void()> onProjectChanged;

private:
    void timerCallback() override;

    SamplerProject                  project;
    VoiceEngine                     voiceEngine;
    hpbridge::FileSnapshotTransport fileTransport;
    hpbridge::BridgeConnectionState bridgeState = hpbridge::BridgeConnectionState::Disconnected;
    juce::String                    bridgePartnerName;
    juce::File                      linkedHpdgSlotFile;
    double                          currentSampleRate = 44100.0;

    juce::AudioBuffer<float>        resultCache;
    uint64_t                        resultCacheRevision = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HPSamplerAudioProcessor)
};
} // namespace hps
