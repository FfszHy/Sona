//
//  SonaTransportClient.h
//  Plug-in side of the Sona transport: owns the XPC connection to Sona Audio Service and the
//  current shared-memory mapping, and hands the realtime thread a lock-free view of it.
//
//  Threading:
//    - All control (connect, hello, register, retire) runs on the owner's serial queue.
//    - The realtime IO thread only calls acquire()/release() and the ring producers; it never
//      blocks, allocates or touches XPC. A missing service is a normal state: acquire() returns
//      null and the caller drops the block.
//    - Mapping retirement follows ARCHITECTURE.md: publish the new mapping, wait (off the
//      realtime thread) until the IO thread has confirmed it no longer references the old one,
//      then unmap.
//

#ifndef SonaTransportClient_h
#define SonaTransportClient_h

#include <atomic>
#include <cstdint>
#include <dispatch/dispatch.h>
#include <string>
#include <xpc/xpc.h>
#include "../Shared/SonaTransport.h"

class SonaTransportClient {
public:
    struct Mapping {
        void*      base = nullptr;
        size_t     size = 0;
        uint64_t   generation = 0;
        xpc_object_t shmem = nullptr;   // retained; keeps the region alive
    };

    // Held by the realtime thread for the duration of one IO operation.
    struct Guard {
        SonaTransportClient* owner = nullptr;
        Mapping* mapping = nullptr;
        ~Guard() { if (owner) owner->release(); }
        explicit operator bool() const { return mapping != nullptr; }
        SonaTransportClientRing* ring(uint32_t slot) const {
            return mapping ? SonaTransportRing(mapping->base, slot) : nullptr;
        }
    };

    struct ClientRecord {
        bool        used = false;
        uint32_t    instance = 0;
        uint32_t    clientID = 0;
        int32_t     pid = 0;
        std::string bundleID;
    };

    using MessageHandler = void (*)(xpc_object_t message);

    explicit SonaTransportClient(dispatch_queue_t controlQueue, const char* driverVersion);
    ~SonaTransportClient();

    // Control queue only.
    void setMessageHandler(MessageHandler handler) { mHandler = handler; }
    void start();
    void stop();
    void registerClient(uint32_t slot, uint32_t clientID, int32_t pid, const std::string& bundleID);
    void unregisterClient(uint32_t slot);
    void setFormat(double sampleRate);
    void setIORunning(bool running);
    // Forwards an arbitrary control message; consumes `message`. Dropped while disconnected
    // (the service re-pushes its snapshot after every handshake, so nothing is lost).
    void sendControl(xpc_object_t message) { send(message); }
    uint32_t formatGeneration() const { return mFormatGeneration.load(std::memory_order_relaxed); }
    uint32_t instanceForSlot(uint32_t slot) const { return mInstances[slot].load(std::memory_order_relaxed); }
    bool connected() const { return mConnected.load(std::memory_order_acquire); }
    uint64_t generation() const { return mGeneration.load(std::memory_order_acquire); }

    // Realtime thread only. Never blocks.
    Guard acquire() {
        mRealtimeBusy.store(1, std::memory_order_seq_cst);
        Mapping* m = mCurrent.load(std::memory_order_acquire);
        if (!m) { mRealtimeBusy.store(0, std::memory_order_release); return Guard{}; }
        return Guard{this, m};
    }

private:
    friend struct Guard;
    void release() { mRealtimeBusy.store(0, std::memory_order_release); }

    void connectLocked();
    void handleEvent(xpc_object_t event);
    void sendHello();
    void adoptMapping(xpc_object_t reply);
    void retireMapping(Mapping* old);
    void scheduleReconnect();
    void resendRegistrations();
    void send(xpc_object_t message);

    dispatch_queue_t     mQueue;              // control queue (owned by the driver)
    std::string          mDriverVersion;
    MessageHandler       mHandler = nullptr;
    xpc_connection_t     mConnection = nullptr;
    bool                 mStarted = false;
    bool                 mReconnectPending = false;
    uint64_t             mBackoffNanos = 0;
    uint32_t             mNextInstance = 1;
    double               mSampleRate = 48000.0;
    bool                 mIORunning = false;
    ClientRecord         mClients[kSonaTransportMaxClients];

    std::atomic<Mapping*> mCurrent{nullptr};
    std::atomic<uint32_t> mRealtimeBusy{0};
    std::atomic<bool>     mConnected{false};
    std::atomic<uint64_t> mGeneration{0};
    std::atomic<uint32_t> mFormatGeneration{1};
    std::atomic<uint32_t> mInstances[kSonaTransportMaxClients];
};

#endif /* SonaTransportClient_h */
