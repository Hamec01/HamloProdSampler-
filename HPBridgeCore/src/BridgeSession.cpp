#include "hpbridge/BridgeSession.h"

#include <atomic>
#include <cstring>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace hpbridge
{
namespace
{
constexpr char kMagic[8] { 'H', 'P', 'B', 'R', 'I', 'D', 'G', 'E' };
constexpr juce::int64 kDeadSlotMs = 60 * 60 * 1000;

std::atomic<std::uint32_t>& counterAt(char* base) noexcept
{
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t) && std::atomic<std::uint32_t>::is_always_lock_free);
    return *reinterpret_cast<std::atomic<std::uint32_t>*>(base + offsetof(SlotHeader, sequence));
}

void copyString(char* dst, std::size_t cap, const juce::String& s)
{
    std::memset(dst, 0, cap);
    const auto utf8 = s.toUTF8();
    std::memcpy(dst, utf8.getAddress(), std::min(cap - 1, std::strlen(utf8.getAddress())));
}

juce::String readString(const char* src, std::size_t cap)
{
    return juce::String::fromUTF8(src, (int) strnlen(src, cap));
}

std::uint64_t randomId()
{
    juce::Random r;
    r.setSeedRandomly();
    std::uint64_t id = 0;
    while (id == 0) id = ((std::uint64_t) (std::uint32_t) r.nextInt() << 32) ^ (std::uint64_t) (std::uint32_t) r.nextInt()
                         ^ (std::uint64_t) juce::Time::getHighResolutionTicks();
    return id;
}
} // namespace

juce::File bridgeDirectory()
{
   #if JUCE_WINDOWS
    const auto base = juce::File::getSpecialLocation(juce::File::windowsLocalAppData);
   #else
    const auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
   #endif
    return base.getChildFile("HamloProd").getChildFile("Bridge");
}

juce::String peerTypeName(PeerType t)
{
    return t == PeerType::Hpdg ? "HPDG" : t == PeerType::Sampler ? "HPSAMPLER" : "?";
}

juce::File bridgeSlotFile(PeerType type, std::uint64_t id)
{
    return bridgeDirectory().getChildFile(peerTypeName(type).toLowerCase() + "-"
                                          + juce::String::toHexString((juce::int64) id).paddedLeft('0', 16) + ".slot");
}

juce::String BridgePeer::label() const
{
    return (name.isNotEmpty() ? name : peerTypeName(type)) + "  #" + juce::String::toHexString((juce::int64) id).getLastCharacters(4).toUpperCase();
}

// ---------------------------------------------------------------------------------------------------------------
// mapping (Windows: full sharing so readers open while the owner writes, and the owner can delete it)

struct BridgeSlot::Native
{
   #if JUCE_WINDOWS
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    void* view = nullptr;
    ~Native()
    {
        if (view != nullptr) UnmapViewOfFile(view);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
   #else
    std::unique_ptr<juce::MemoryMappedFile> mapped;
   #endif
};

namespace
{
std::unique_ptr<BridgeSlot::Native> mapFile(const juce::File& f, bool writer, char*& base)
{
    auto n = std::make_unique<BridgeSlot::Native>();
   #if JUCE_WINDOWS
    n->file = CreateFileW(f.getFullPathName().toWideCharPointer(), writer ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, writer ? OPEN_ALWAYS : OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (n->file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size {};
    if (!writer && (!GetFileSizeEx(n->file, &size) || size.QuadPart < (LONGLONG) kSlotBytes)) return {};
    n->mapping = CreateFileMappingW(n->file, nullptr, writer ? PAGE_READWRITE : PAGE_READONLY, 0, (DWORD) kSlotBytes, nullptr);
    if (n->mapping == nullptr) return {};
    n->view = MapViewOfFile(n->mapping, writer ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, kSlotBytes);
    if (n->view == nullptr) return {};
    base = static_cast<char*>(n->view);
   #else
    if (!writer && f.getSize() < (juce::int64) kSlotBytes) return {};
    if (writer && f.getSize() < (juce::int64) kSlotBytes)
    {
        juce::MemoryBlock zeros(kSlotBytes, true);
        f.replaceWithData(zeros.getData(), zeros.getSize());
    }
    n->mapped = std::make_unique<juce::MemoryMappedFile>(f, juce::Range<juce::int64>(0, (juce::int64) kSlotBytes),
                                                          writer ? juce::MemoryMappedFile::readWrite : juce::MemoryMappedFile::readOnly, false);
    if (n->mapped->getData() == nullptr) return {};
    base = static_cast<char*>(n->mapped->getData());
   #endif
    return n;
}
} // namespace

std::unique_ptr<BridgeSlot> BridgeSlot::openForWriting(const juce::File& f)
{
    f.getParentDirectory().createDirectory();
    std::unique_ptr<BridgeSlot> s(new BridgeSlot());
    s->native = mapFile(f, true, s->base);
    if (s->native == nullptr) return {};
    return s;
}

std::unique_ptr<BridgeSlot> BridgeSlot::openForReading(const juce::File& f)
{
    if (!f.existsAsFile()) return {};
    std::unique_ptr<BridgeSlot> s(new BridgeSlot());
    s->native = mapFile(f, false, s->base);
    if (s->native == nullptr) return {};
    return s;
}

BridgeSlot::~BridgeSlot() = default;

void BridgeSlot::write(const SlotHeader& h, const char* payloadData, std::size_t bytes) noexcept
{
    auto& seq = counterAt(base);
    const auto start = seq.load(std::memory_order_relaxed) | 1u; // odd = writing
    seq.store(start, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    SlotHeader copy = h;
    copy.sequence = start;
    std::memcpy(base, &copy, sizeof(SlotHeader));
    if (payloadData != nullptr)
        std::memcpy(base + sizeof(SlotHeader), payloadData, std::min(bytes, kSlotPayloadBytes));
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(start + 1, std::memory_order_release);
}

bool BridgeSlot::readHeader(SlotHeader& out) const noexcept
{
    auto& seq = counterAt(base);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto before = seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) continue;
        std::memcpy(&out, base, sizeof(SlotHeader));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == before)
            return std::memcmp(out.magic, kMagic, sizeof(kMagic)) == 0;
    }
    return false;
}

bool BridgeSlot::readPayload(SlotHeader& h, juce::String& out) const
{
    auto& seq = counterAt(base);
    juce::HeapBlock<char> tmp(kSlotPayloadBytes);
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto before = seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) { juce::Thread::yield(); continue; }
        std::memcpy(&h, base, sizeof(SlotHeader));
        const auto n = std::min<std::size_t>(h.payloadBytes, kSlotPayloadBytes);
        std::memcpy(tmp.get(), base + sizeof(SlotHeader), n);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_acquire) == before)
        {
            if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return false;
            out = juce::String::fromUTF8(tmp.get(), (int) n);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------

BridgeSession::BridgeSession(PeerType self, juce::String displayName)
    : selfType(self), otherType(self == PeerType::Hpdg ? PeerType::Sampler : PeerType::Hpdg), selfId(randomId())
{
    std::memset(&header, 0, sizeof(header));
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.versionMajor = kSlotVersionMajor;
    header.versionMinor = kSlotVersionMinor;
    header.instanceId = selfId;
    header.peerType = (std::uint32_t) self;
    copyString(header.name, sizeof(header.name), displayName);
    own = BridgeSlot::openForWriting(bridgeSlotFile(selfType, selfId));
    if (own == nullptr) setState(BridgeConnectionState::Error, "cannot create the bridge slot in " + bridgeDirectory().getFullPathName());
    tick(juce::Time::currentTimeMillis());
    startTimer(200);
}

BridgeSession::~BridgeSession()
{
    stopTimer();
    if (own != nullptr)
    {
        header.pairTarget = 0;
        header.heartbeatMs = 0; // closed
        own->write(header, nullptr, 0);
        own.reset();
    }
    readers.clear();
    bridgeSlotFile(selfType, selfId).deleteFile();
}

void BridgeSession::setDisplayName(const juce::String& n)
{
    copyString(header.name, sizeof(header.name), n);
    headerDirty = true;
}

void BridgeSession::setBpm(float bpm)
{
    if (std::abs(header.bpm - bpm) > 0.001f) { header.bpm = bpm; headerDirty = true; }
}

BridgeSlot* BridgeSession::readerFor(const juce::File& f)
{
    const auto key = f.getFullPathName();
    auto it = readers.find(key);
    if (it != readers.end() && it->second != nullptr) return it->second.get();
    auto r = BridgeSlot::openForReading(f);
    if (r == nullptr) return nullptr;
    auto* raw = r.get();
    readers[key] = std::move(r);
    return raw;
}

std::vector<BridgePeer> BridgeSession::discover()
{
    std::vector<BridgePeer> out;
    const auto now = juce::Time::currentTimeMillis();
    const auto prefix = peerTypeName(otherType).toLowerCase() + "-";
    for (const auto& f : bridgeDirectory().findChildFiles(juce::File::findFiles, false, prefix + "*.slot"))
    {
        auto* r = readerFor(f);
        SlotHeader h {};
        if (r == nullptr || !r->readHeader(h)) continue;
        BridgePeer p;
        p.id = h.instanceId;
        p.type = (PeerType) h.peerType;
        p.name = readString(h.name, sizeof(h.name));
        p.bpm = h.bpm;
        p.alive = h.heartbeatMs > 0 && now - h.heartbeatMs <= kBridgeAliveMs;
        p.busy = h.pairTarget != 0 && h.pairTarget != selfId;
        p.incompatible = h.versionMajor != kSlotVersionMajor;
        if (p.alive && p.type == otherType) out.push_back(p);
    }
    return out;
}

void BridgeSession::connectTo(std::uint64_t peerId)
{
    if (peerId == 0 || peerId == selfId) return;
    header.pairTarget = peerId;
    partner = {};
    partner.id = peerId;
    partner.type = otherType;
    partnerAck = 0;
    received = 0;
    headerDirty = true;
    setState(BridgeConnectionState::Connecting);
    tick(juce::Time::currentTimeMillis());
}

void BridgeSession::disconnect()
{
    header.pairTarget = 0;
    header.ackRevision = 0;
    headerDirty = true;
    partner = {};
    const bool was = state != BridgeConnectionState::Disconnected;
    setState(BridgeConnectionState::Disconnected);
    if (!was && onStateChanged) onStateChanged();
    writeOwn(juce::Time::currentTimeMillis(), false);
}

std::int64_t BridgeSession::publish(const juce::String& schema, const juce::String& json)
{
    const auto utf8 = json.toUTF8();
    const auto n = std::strlen(utf8.getAddress());
    if (n > kSlotPayloadBytes) { lastError = "payload too large (" + juce::String((juce::int64) n) + " bytes)"; return header.payloadRevision; }
    payload.replaceAll(utf8.getAddress(), n);
    header.payloadBytes = (std::uint32_t) n;
    copyString(header.schema, sizeof(header.schema), schema);
    ++header.payloadRevision;
    payloadDirty = true;
    headerDirty = true;
    writeOwn(juce::Time::currentTimeMillis(), true);
    return header.payloadRevision;
}

void BridgeSession::writeOwn(juce::int64 now, bool withPayload)
{
    if (own == nullptr) return;
    header.heartbeatMs = now;
    own->write(header, withPayload ? static_cast<const char*>(payload.getData()) : nullptr, withPayload ? payload.getSize() : 0);
    lastWriteMs = now;
    headerDirty = false;
    if (withPayload) payloadDirty = false;
}

void BridgeSession::setState(BridgeConnectionState s, const juce::String& error)
{
    if (error.isNotEmpty()) lastError = error;
    if (s == state) return;
    state = s;
    stateSinceMs = juce::Time::currentTimeMillis();
    if (onStateChanged) onStateChanged();
}

void BridgeSession::tick(juce::int64 now)
{
    if (own == nullptr) return;

    // housekeeping: forget readers of vanished files, delete slots of crashed instances
    if (now - lastCleanupMs > 5000)
    {
        lastCleanupMs = now;
        for (auto it = readers.begin(); it != readers.end();)
            it = juce::File(it->first).existsAsFile() ? std::next(it) : readers.erase(it);
        for (const auto& f : bridgeDirectory().findChildFiles(juce::File::findFiles, false, "*.slot"))
            if (now - f.getLastModificationTime().toMilliseconds() > kDeadSlotMs && f != bridgeSlotFile(selfType, selfId))
            {
                auto* r = readerFor(f);
                SlotHeader h {};
                if (r == nullptr || !r->readHeader(h) || h.heartbeatMs <= 0 || now - h.heartbeatMs > kDeadSlotMs)
                {
                    readers.erase(f.getFullPathName());
                    f.deleteFile();
                }
            }
    }

    // a free instance accepts an incoming Connect
    if (header.pairTarget == 0 && acceptIncoming)
    {
        for (const auto& f : bridgeDirectory().findChildFiles(juce::File::findFiles, false, peerTypeName(otherType).toLowerCase() + "-*.slot"))
        {
            auto* r = readerFor(f);
            SlotHeader h {};
            if (r == nullptr || !r->readHeader(h)) continue;
            const bool alive = h.heartbeatMs > 0 && now - h.heartbeatMs <= kBridgeAliveMs;
            if (alive && h.pairTarget == selfId && h.peerType == (std::uint32_t) otherType && h.versionMajor == kSlotVersionMajor)
            {
                header.pairTarget = h.instanceId;
                partner = {};
                partner.id = h.instanceId;
                partner.type = otherType;
                partnerAck = received = 0;
                headerDirty = true;
                setState(BridgeConnectionState::Connecting);
                break;
            }
        }
    }

    if (header.pairTarget != 0)
    {
        auto* r = readerFor(bridgeSlotFile(otherType, header.pairTarget));
        SlotHeader h {};
        const bool ok = r != nullptr && r->readHeader(h);
        if (!ok)
        {
            if (state == BridgeConnectionState::Connected) setState(BridgeConnectionState::Stale, "partner slot unreadable");
            if (now - stateSinceMs > kBridgeGiveUpMs) { lastError = "partner is gone"; disconnect(); }
        }
        else if (h.versionMajor != kSlotVersionMajor)
        {
            setState(BridgeConnectionState::Incompatible, "bridge protocol " + juce::String(h.versionMajor) + ".x vs " + juce::String(kSlotVersionMajor) + ".x - update both plug-ins");
        }
        else
        {
            partner.name = readString(h.name, sizeof(h.name));
            partner.bpm = h.bpm;
            const bool alive = h.heartbeatMs > 0 && now - h.heartbeatMs <= kBridgeAliveMs;
            partner.alive = alive;
            if (!alive)
            {
                if (state == BridgeConnectionState::Connected) setState(BridgeConnectionState::Stale, "partner stopped responding");
                if (h.heartbeatMs <= 0 || now - stateSinceMs > kBridgeGiveUpMs) { lastError = "partner closed"; disconnect(); }
            }
            else if (h.pairTarget == selfId)
            {
                setState(BridgeConnectionState::Connected);
                partnerAck = h.ackRevision;
                if (h.payloadRevision > 0 && h.payloadRevision != received)
                {
                    SlotHeader ph {};
                    juce::String json;
                    if (r->readPayload(ph, json) && ph.payloadRevision > received)
                    {
                        const auto schema = readString(ph.schema, sizeof(ph.schema));
                        const bool accepted = onIncoming ? onIncoming(schema, json, ph.payloadRevision) : false;
                        received = ph.payloadRevision; // a rejected revision is not re-read (its error is shown)
                        if (accepted) { header.ackRevision = ph.payloadRevision; headerDirty = true; }
                    }
                }
            }
            else if (state == BridgeConnectionState::Connected || state == BridgeConnectionState::Stale)
            {
                lastError = "partner disconnected";
                disconnect();
            }
            else if (h.pairTarget != 0)
            {
                lastError = partner.name + " is connected to another instance";
                disconnect();
            }
            else if (now - stateSinceMs > kBridgeGiveUpMs)
            {
                lastError = "no answer from " + partner.name;
                disconnect();
            }
        }
    }

    if (headerDirty || payloadDirty || now - lastWriteMs >= kBridgeHeartbeatMs)
        writeOwn(now, payloadDirty);
}

juce::String BridgeSession::getStatusText() const
{
    const auto who = partner.name.isNotEmpty() ? partner.name : peerTypeName(otherType);
    switch (state)
    {
        case BridgeConnectionState::Connected:    return "Connected: " + who;
        case BridgeConnectionState::Connecting:   return "Connecting to " + who + "...";
        case BridgeConnectionState::Stale:        return "Stale: " + who + " (" + lastError + ")";
        case BridgeConnectionState::Incompatible: return "Incompatible: " + lastError;
        case BridgeConnectionState::Error:        return "Error: " + lastError;
        default: break;
    }
    return lastError.isNotEmpty() ? "Disconnected (" + lastError + ")" : "Disconnected";
}
} // namespace hpbridge
