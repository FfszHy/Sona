//
//  SonaTransport.h
//  Wire contract between the Sona AudioServerPlugIn and Sona Audio Service.
//
//  Control plane: XPC dictionaries over the launchd Mach service kSonaTransportServiceName.
//  Data plane:    one shared-memory region per connection generation, laid out below.
//
//  Both sides compile this header with the same toolchain. Every field has an explicit width and
//  the layout is checked with static_asserts; nothing in the region is a pointer. The region is
//  validated on map (magic, protocol version, capacities, total size) and rejected otherwise.
//
//  Realtime rules for the producer (plug-in IO thread): no allocation, no locks, no syscalls.
//  A full ring or a missing mapping drops the block and bumps a counter; it never waits.
//

#ifndef SonaTransport_h
#define SonaTransport_h

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// MARK: - Control plane

#define kSonaTransportServiceName       "com.sona.audio-service"          // plug-in host only (code-signing checked)
#define kSonaTransportStatusServiceName "com.sona.audio-service.status"   // read-only status for tools, any caller
#define kSonaTransportProtocolVersion   2u

// Message keys. Every message carries kSonaMsg_Op.
#define kSonaMsg_Op                     "op"
#define kSonaMsg_ProtocolVersion        "protocolVersion"
#define kSonaMsg_DriverVersion          "driverVersion"
#define kSonaMsg_OK                     "ok"
#define kSonaMsg_Error                  "error"
#define kSonaMsg_Generation             "generation"
#define kSonaMsg_SharedMemory           "shm"          // xpc_shmem_t, hello reply only
#define kSonaMsg_Slot                   "slot"
#define kSonaMsg_Instance               "instance"
#define kSonaMsg_ClientID               "clientID"
#define kSonaMsg_PID                    "pid"
#define kSonaMsg_BundleID               "bundleID"
#define kSonaMsg_SampleRate             "sampleRate"
#define kSonaMsg_FormatGeneration       "formatGeneration"
#define kSonaMsg_Running                "running"
#define kSonaMsg_Status                 "status"

// Ops sent by the plug-in. The service replies only to hello; everything else is fire-and-forget
// and idempotent, so a lost message after a reconnect is repaired by the re-registration pass.
// The service accepts hello (and every op below) only from Apple's plug-in host process, checked
// by libxpc against the code signature behind each message. Anyone else is disconnected before
// the message is seen; the caller's hello ends in XPC_ERROR_CONNECTION_INTERRUPTED. A hello from
// a new host connection replaces the current session: the old connection is cancelled, so its
// owner sees XPC_ERROR_CONNECTION_INVALID, drops its mapping and reconnects.
#define kSonaOp_Hello                   "hello"        // -> reply {ok, generation, shm}
#define kSonaOp_ClientAdd               "clientAdd"    // {slot, instance, clientID, pid, bundleID}
#define kSonaOp_ClientRemove            "clientRemove" // {slot, instance}
#define kSonaOp_Format                  "format"       // {sampleRate, formatGeneration}
#define kSonaOp_IO                      "io"           // {running}
// Op sent by tools over kSonaTransportStatusServiceName; read-only, answered for any caller.
#define kSonaOp_Status                  "status"       // -> reply {status: dictionary}

// Control plane forwarded from the plug-in's property facade (plug-in -> service).
#define kSonaOp_Config                  "config"       // {plist: binary CFPropertyList data (config dictionary)}
#define kSonaOp_OutputLevel             "outputLevel"  // {uid, volume?, mute?}
#define kSonaOp_Master                  "master"       // {volume?, mute?} plug-in -> service: request
                                                       // service -> plug-in: actual {volume, mute}
// Pushed by the service to the plug-in whenever they change (service -> plug-in).
#define kSonaOp_StatusPush              "statusPush"   // {plist}: kSonaProperty_Status dictionary
#define kSonaOp_ClientsPush             "clientsPush"  // {plist}: kSonaProperty_Clients array
#define kSonaOp_ConfigPush              "configPush"   // {plist}: config the service holds (restored or accepted)
#define kSonaMsg_Plist                  "plist"
#define kSonaMsg_UID                    "uid"
#define kSonaMsg_Volume                 "volume"
#define kSonaMsg_Mute                   "mute"

// MARK: - Shared memory layout

enum : uint32_t {
    kSonaTransportMagic          = 0x534F4E41u,   // 'SONA'
    kSonaTransportMaxClients     = 64,
    kSonaTransportChannels       = 2,
    kSonaTransportRingFrames     = 1u << 14,      // 16384 frames per client (~0.34 s @ 48 kHz)
    kSonaTransportBlockCapacity  = 1u << 8,       // 256 block descriptors per client
    kSonaTransportMaxBlockFrames = 4096,          // one IO cycle; larger cycles are split by the producer
};

enum : uint32_t {
    kSonaBlockFlag_TimestampValid = 1u << 0,      // sampleTime/hostTime come from the host output time
    kSonaBlockFlag_Discontinuity  = 1u << 1,      // producer dropped data before this block
};

// One IO-cycle worth of PCM for one client. 40 bytes, 8-byte aligned.
struct SonaTransportBlock {
    double   sampleTime;        // output sample time of the first frame (virtual device timeline)
    uint64_t hostTime;          // host time paired with sampleTime
    uint64_t cycle;             // AudioServerPlugInIOCycleInfo.mIOCycleCounter
    uint32_t frames;            // 1..kSonaTransportMaxBlockFrames
    uint32_t formatGeneration;  // bumps whenever the virtual device format changes
    uint32_t instance;          // client instance this block belongs to
    uint32_t flags;             // kSonaBlockFlag_*
};
static_assert(sizeof(SonaTransportBlock) == 40, "block layout");

// One SPSC ring per client slot. Producer fields and consumer fields live on separate cache
// lines. Indices are monotonically increasing frame/block counts; capacity is a power of two.
struct SonaTransportClientRing {
    alignas(64) std::atomic<uint64_t> blockWrite;
    std::atomic<uint64_t> frameWrite;
    std::atomic<uint64_t> droppedBlocks;    // producer statistic
    std::atomic<uint64_t> droppedFrames;    // producer statistic
    std::atomic<uint32_t> dropPending;      // producer only: next block carries Discontinuity
    alignas(64) std::atomic<uint64_t> blockRead;
    std::atomic<uint64_t> frameRead;
    alignas(64) SonaTransportBlock blocks[kSonaTransportBlockCapacity];
    alignas(64) float samples[kSonaTransportRingFrames * kSonaTransportChannels];
};
#if defined(__cpp_lib_atomic_is_always_lock_free)
static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared atomics must be lock-free");
#endif
static_assert(offsetof(SonaTransportClientRing, blockRead) == 64, "consumer line");
static_assert(offsetof(SonaTransportClientRing, blocks) == 128, "blocks offset");
static_assert(sizeof(SonaTransportClientRing) % 64 == 0, "ring stride");

struct SonaTransportHeader {
    uint32_t magic;
    uint32_t protocolVersion;
    uint64_t generation;
    uint32_t clientSlots;
    uint32_t ringFrames;
    uint32_t blockCapacity;
    uint32_t channels;
    uint64_t ringStride;         // bytes between consecutive client rings
    uint64_t ringsOffset;        // bytes from region start to ring 0
    uint64_t totalSize;          // bytes; must equal the mapped size
};
static_assert(sizeof(SonaTransportHeader) == 56, "header layout");

// Clock anchor published by the service's master output IOProc so the virtual device timeline
// can follow the physical clock. Seqlock: single writer (service), single reader (plug-in IO
// thread), bounded retries. driverLocked flows the other way: the plug-in reports whether its
// timeline is currently rate-locked, which lets the master sink copy frames 1:1.
struct SonaTransportClock {
    std::atomic<uint64_t> seq;
    std::atomic<uint64_t> hostTime;
    std::atomic<uint64_t> sampleTimeBits;   // double bit pattern
    std::atomic<uint64_t> rateBits;         // double bit pattern
    std::atomic<uint64_t> generation;       // 0 = no anchor
    alignas(64) std::atomic<uint32_t> driverLocked;
};
enum : uint64_t { kSonaTransportClockOffset = 128 };
static_assert(kSonaTransportClockOffset + sizeof(SonaTransportClock) <= 4096, "clock fits header page");

struct SonaClockAnchor {
    uint64_t hostTime = 0;
    double   sampleTime = 0;
    double   rate = 0;
    uint64_t generation = 0;   // Changes whenever the timeline is unrelated to the previous one.
    bool valid() const { return generation != 0 && rate > 0 && hostTime != 0; }
};

inline uint64_t SonaDoubleBits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
inline double SonaBitsDouble(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

inline void SonaClockPublish(SonaTransportClock* c, const SonaClockAnchor& a) {
    uint64_t s = c->seq.load(std::memory_order_relaxed);
    c->seq.store(s + 1, std::memory_order_release);
    c->hostTime.store(a.hostTime, std::memory_order_relaxed);
    c->sampleTimeBits.store(SonaDoubleBits(a.sampleTime), std::memory_order_relaxed);
    c->rateBits.store(SonaDoubleBits(a.rate), std::memory_order_relaxed);
    c->generation.store(a.generation, std::memory_order_relaxed);
    c->seq.store(s + 2, std::memory_order_release);
}
inline void SonaClockClear(SonaTransportClock* c) { SonaClockPublish(c, SonaClockAnchor{}); }

inline SonaClockAnchor SonaClockRead(const SonaTransportClock* c) {
    SonaClockAnchor a;
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint64_t before = c->seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        a.hostTime = c->hostTime.load(std::memory_order_relaxed);
        a.sampleTime = SonaBitsDouble(c->sampleTimeBits.load(std::memory_order_relaxed));
        a.rate = SonaBitsDouble(c->rateBits.load(std::memory_order_relaxed));
        a.generation = c->generation.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (c->seq.load(std::memory_order_relaxed) == before) return a;
    }
    return SonaClockAnchor{};
}

inline SonaTransportClock* SonaTransportClockCell(void* region) {
    return reinterpret_cast<SonaTransportClock*>(static_cast<uint8_t*>(region) + kSonaTransportClockOffset);
}

enum : uint64_t {
    kSonaTransportRingsOffset = 4096,
    kSonaTransportRegionSize  = kSonaTransportRingsOffset +
                                (uint64_t)kSonaTransportMaxClients * sizeof(SonaTransportClientRing),
};

// MARK: - Region helpers (both sides)

inline void SonaTransportInitRegion(void* region, uint64_t generation) {
    memset(region, 0, kSonaTransportRegionSize);   // also pre-touches every page
    auto* h = static_cast<SonaTransportHeader*>(region);
    h->magic = kSonaTransportMagic;
    h->protocolVersion = kSonaTransportProtocolVersion;
    h->generation = generation;
    h->clientSlots = kSonaTransportMaxClients;
    h->ringFrames = kSonaTransportRingFrames;
    h->blockCapacity = kSonaTransportBlockCapacity;
    h->channels = kSonaTransportChannels;
    h->ringStride = sizeof(SonaTransportClientRing);
    h->ringsOffset = kSonaTransportRingsOffset;
    h->totalSize = kSonaTransportRegionSize;
}

// Rejects anything that does not match this build's layout. Never trust the other side.
inline bool SonaTransportValidateRegion(const void* region, size_t mappedSize, uint64_t expectedGeneration) {
    if (!region || mappedSize < sizeof(SonaTransportHeader)) return false;
    SonaTransportHeader h;
    memcpy(&h, region, sizeof(h));
    return h.magic == kSonaTransportMagic &&
           h.protocolVersion == kSonaTransportProtocolVersion &&
           h.generation == expectedGeneration &&
           h.clientSlots == kSonaTransportMaxClients &&
           h.ringFrames == kSonaTransportRingFrames &&
           h.blockCapacity == kSonaTransportBlockCapacity &&
           h.channels == kSonaTransportChannels &&
           h.ringStride == sizeof(SonaTransportClientRing) &&
           h.ringsOffset == kSonaTransportRingsOffset &&
           h.totalSize == kSonaTransportRegionSize &&
           mappedSize >= kSonaTransportRegionSize;
}

inline SonaTransportClientRing* SonaTransportRing(void* region, uint32_t slot) {
    return reinterpret_cast<SonaTransportClientRing*>(
        static_cast<uint8_t*>(region) + kSonaTransportRingsOffset + (size_t)slot * sizeof(SonaTransportClientRing));
}

// MARK: - Producer (realtime, one thread per ring)

// Writes one block. Returns false and records a drop when either the sample ring or the
// descriptor ring lacks space. The next successful block carries kSonaBlockFlag_Discontinuity.
inline bool SonaTransportRingWrite(SonaTransportClientRing* ring, const float* interleaved,
                                   SonaTransportBlock block) {
    if (block.frames == 0 || block.frames > kSonaTransportMaxBlockFrames) return false;
    const uint64_t bw = ring->blockWrite.load(std::memory_order_relaxed);
    const uint64_t br = ring->blockRead.load(std::memory_order_acquire);
    const uint64_t fw = ring->frameWrite.load(std::memory_order_relaxed);
    const uint64_t fr = ring->frameRead.load(std::memory_order_acquire);
    if (bw - br >= kSonaTransportBlockCapacity || fw - fr + block.frames > kSonaTransportRingFrames) {
        ring->droppedBlocks.fetch_add(1, std::memory_order_relaxed);
        ring->droppedFrames.fetch_add(block.frames, std::memory_order_relaxed);
        ring->dropPending.store(1, std::memory_order_relaxed);
        return false;
    }
    if (ring->dropPending.load(std::memory_order_relaxed)) {
        block.flags |= kSonaBlockFlag_Discontinuity;
        ring->dropPending.store(0, std::memory_order_relaxed);
    }
    const uint32_t mask = kSonaTransportRingFrames - 1;
    const uint32_t start = (uint32_t)(fw & mask);
    const uint32_t firstFrames = std::min<uint32_t>(block.frames, kSonaTransportRingFrames - start);
    memcpy(&ring->samples[(size_t)start * kSonaTransportChannels], interleaved,
           (size_t)firstFrames * kSonaTransportChannels * sizeof(float));
    if (firstFrames < block.frames) {
        memcpy(&ring->samples[0], interleaved + (size_t)firstFrames * kSonaTransportChannels,
               (size_t)(block.frames - firstFrames) * kSonaTransportChannels * sizeof(float));
    }
    ring->blocks[bw & (kSonaTransportBlockCapacity - 1)] = block;
    ring->frameWrite.store(fw + block.frames, std::memory_order_release);
    ring->blockWrite.store(bw + 1, std::memory_order_release);
    return true;
}

// MARK: - Consumer (one thread per ring)

struct SonaTransportReadBlock {
    SonaTransportBlock desc;
    const float* first;      // first contiguous run
    uint32_t firstFrames;
    const float* second;     // wrap-around run, may be null
    uint32_t secondFrames;
};

// Peeks the next block without consuming it. Returns false if the ring is empty.
inline bool SonaTransportRingPeek(SonaTransportClientRing* ring, SonaTransportReadBlock& out) {
    const uint64_t br = ring->blockRead.load(std::memory_order_relaxed);
    const uint64_t bw = ring->blockWrite.load(std::memory_order_acquire);
    if (bw == br) return false;
    out.desc = ring->blocks[br & (kSonaTransportBlockCapacity - 1)];
    if (out.desc.frames == 0 || out.desc.frames > kSonaTransportMaxBlockFrames) {
        // Corrupt descriptor: consume nothing, report as empty; caller should resync.
        return false;
    }
    const uint64_t fr = ring->frameRead.load(std::memory_order_relaxed);
    const uint32_t mask = kSonaTransportRingFrames - 1;
    const uint32_t start = (uint32_t)(fr & mask);
    out.firstFrames = std::min<uint32_t>(out.desc.frames, kSonaTransportRingFrames - start);
    out.first = &ring->samples[(size_t)start * kSonaTransportChannels];
    out.secondFrames = out.desc.frames - out.firstFrames;
    out.second = out.secondFrames ? &ring->samples[0] : nullptr;
    return true;
}

// Consumes the block previously returned by SonaTransportRingPeek.
inline void SonaTransportRingConsume(SonaTransportClientRing* ring, const SonaTransportReadBlock& block) {
    ring->frameRead.store(ring->frameRead.load(std::memory_order_relaxed) + block.desc.frames, std::memory_order_release);
    ring->blockRead.store(ring->blockRead.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

// Consumer-side resync: discard everything currently queued. Only the consumer moves read indices,
// and only from its own thread: a flush issued from another thread races with a Peek/Consume pair
// and can push blockRead past blockWrite, after which the producer sees a full ring forever.
inline void SonaTransportRingFlush(SonaTransportClientRing* ring) {
    ring->frameRead.store(ring->frameWrite.load(std::memory_order_acquire), std::memory_order_release);
    ring->blockRead.store(ring->blockWrite.load(std::memory_order_acquire), std::memory_order_release);
}

#endif /* SonaTransport_h */
