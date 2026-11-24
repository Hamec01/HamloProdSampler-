#pragma once

// HP Bridge live transport (P1). ONE connection between one HPSAMPLER and one HPDG instance, started from either
// side ("Connect to HPDG" / "Connect to HPSAMPLER"), two message directions on it:
//   HPSAMPLER -> HPDG : hp.sample-context.v1 (the MAIN context)
//   HPDG -> HPSAMPLER : hp.drum-guide.v1     (the current drum pattern as a guide)
//
// Transport: like HPDG Link (HamloLink), each instance owns a small memory-mapped slot file
// %LOCALAPPDATA%\HamloProd\Bridge\<hpdg|hpsampler>-<id>.slot (same OS user only, no network, works when the host
// runs the plug-ins in different processes). The writer uses a seqlock; readers copy and retry. Nothing here is
// ever called on an audio thread: the session is a message-thread timer.
//
// Handshake: A sets pairTarget = B (Connecting). B sees an alive slot whose pairTarget is B, and - if B is free -
// sets its own pairTarget = A. Both see the mutual pairing + fresh heartbeats = Connected (shown on both sides).
// Disconnect = pairTarget 0; a partner that stops beating becomes Stale, then Disconnected.
// Payloads carry a revision; the receiver writes the revision it accepted (ackRevision) back into its own slot.
//
// THIS FILE IS SHARED: identical copies live in HPSAMPLER (HPBridgeCore) and HPDG (Source/Services/HPBridge).
// Change both together and bump kSlotVersionMinor (a new major = an incompatible layout).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "BridgeTypes.h"

namespace hpbridge
{
enum class PeerType : std::uint32_t { None = 0, Hpdg = 1, Sampler = 2 };

constexpr std::uint16_t kSlotVersionMajor = 1;
constexpr std::uint16_t kSlotVersionMinor = 0;
constexpr std::size_t kSlotPayloadBytes = 480 * 1024;
constexpr juce::int64 kBridgeHeartbeatMs = 500;
constexpr juce::int64 kBridgeAliveMs = 3000;      // no heartbeat for this long = Stale
constexpr juce::int64 kBridgeGiveUpMs = 10000;    // Stale / unanswered request for this long = Disconnected

#pragma pack(push, 1)
struct SlotHeader
{
    char magic[8];               // "HPBRIDGE"
    std::uint16_t versionMajor;
    std::uint16_t versionMinor;
    std::uint32_t sequence;      // seqlock (odd while writing)
    std::int64_t heartbeatMs;    // 0 = closed
    std::uint64_t instanceId;
    std::uint32_t peerType;      // PeerType
    char name[64];               // UTF-8 display name
    float bpm;
    std::uint64_t pairTarget;    // the instance this one is (or wants to be) paired with, 0 = none
    std::int64_t payloadRevision;// revision of the payload below, 0 = none
    std::int64_t ackRevision;    // the partner payload revision this instance accepted
    char schema[32];             // schema id of the payload
    std::uint32_t payloadBytes;
    std::uint32_t reserved;
};
#pragma pack(pop)

constexpr std::size_t kSlotBytes = sizeof(SlotHeader) + kSlotPayloadBytes;

juce::File bridgeDirectory();
juce::File bridgeSlotFile(PeerType type, std::uint64_t instanceId);
juce::String peerTypeName(PeerType);

// One mapped slot file.
class BridgeSlot
{
public:
    static std::unique_ptr<BridgeSlot> openForWriting(const juce::File&);
    static std::unique_ptr<BridgeSlot> openForReading(const juce::File&);
    ~BridgeSlot();

    // Writer: header (sequence taken from the slot) and, if `payload` is not null, the payload.
    void write(const SlotHeader& header, const char* payload, std::size_t bytes) noexcept;
    // Reader: a consistent header copy; false if not a valid / readable slot or the writer stayed busy.
    bool readHeader(SlotHeader& out) const noexcept;
    // Reader: header + payload consistent with each other.
    bool readPayload(SlotHeader& header, juce::String& payload) const;

    struct Native;

private:
    BridgeSlot() = default;
    std::unique_ptr<Native> native;
    char* base = nullptr;
};

struct BridgePeer
{
    std::uint64_t id = 0;
    PeerType type = PeerType::None;
    juce::String name;
    float bpm = 0.0f;
    bool alive = false;
    bool busy = false;           // paired with someone else
    bool incompatible = false;
    juce::String label() const;  // "name (id)" for menus
};

// The connection state machine of one plug-in instance. Message thread only.
class BridgeSession : private juce::Timer
{
public:
    BridgeSession(PeerType self, juce::String displayName);
    ~BridgeSession() override; // closes and deletes the own slot (the partner sees Disconnected)

    // ---- configuration
    void setDisplayName(const juce::String&);
    void setBpm(float bpm);
    bool acceptIncoming = true;  // a free instance accepts a Connect from the other side

    // ---- discovery / pairing
    std::vector<BridgePeer> discover();     // alive instances of the opposite type
    void connectTo(std::uint64_t peerId);
    void disconnect();

    // ---- payloads
    // Publish a new payload (revision = previous + 1). Kept and re-read by the partner after a reconnect.
    std::int64_t publish(const juce::String& schema, const juce::String& json);
    // Called with the partner's new payload; return true when accepted (validated) -> acknowledged.
    std::function<bool(const juce::String& schema, const juce::String& json, std::int64_t revision)> onIncoming;
    std::function<void()> onStateChanged;

    // ---- status
    BridgeConnectionState getState() const noexcept { return state; }
    const BridgePeer& getPartner() const noexcept { return partner; }
    juce::String getStatusText() const;
    std::uint64_t getInstanceId() const noexcept { return selfId; }
    std::int64_t getPublishedRevision() const noexcept { return header.payloadRevision; }
    std::int64_t getPartnerAckRevision() const noexcept { return partnerAck; }
    std::int64_t getReceivedRevision() const noexcept { return received; }
    juce::String getLastError() const { return lastError; }

    void tick(juce::int64 nowMs); // the timer's work; public for tests

private:
    void timerCallback() override { tick(juce::Time::currentTimeMillis()); }
    void writeOwn(juce::int64 nowMs, bool withPayload);
    void setState(BridgeConnectionState s, const juce::String& error = {});
    BridgeSlot* readerFor(const juce::File&);

    PeerType selfType;
    PeerType otherType;
    std::uint64_t selfId = 0;
    SlotHeader header {};
    juce::MemoryBlock payload;
    std::unique_ptr<BridgeSlot> own;
    std::map<juce::String, std::unique_ptr<BridgeSlot>> readers;
    BridgeConnectionState state = BridgeConnectionState::Disconnected;
    BridgePeer partner;
    std::int64_t partnerAck = 0, received = 0;
    juce::int64 lastWriteMs = 0, stateSinceMs = 0, lastCleanupMs = 0;
    bool headerDirty = true, payloadDirty = false;
    juce::String lastError;
};
} // namespace hpbridge
