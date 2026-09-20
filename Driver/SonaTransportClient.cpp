//
//  SonaTransportClient.cpp
//

#include "SonaTransportClient.h"
#include <os/log.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef SONA_TRANSPORT_STDERR
#include <cstdio>
#define SONA_PUB "%s"
#define SonaTLog(fmt, ...) std::fprintf(stderr, "transport: " fmt "\n", ##__VA_ARGS__)
#else
#define SONA_PUB "%{public}s"
#define SonaTLog(fmt, ...) os_log(OS_LOG_DEFAULT, "SonaDriver transport: " fmt, ##__VA_ARGS__)
#endif

static constexpr uint64_t kBackoffMin = 250 * NSEC_PER_MSEC;
static constexpr uint64_t kBackoffMax = 8 * NSEC_PER_SEC;

SonaTransportClient::SonaTransportClient(dispatch_queue_t controlQueue, const char* driverVersion)
    : mQueue(controlQueue), mDriverVersion(driverVersion) {
    dispatch_retain(mQueue);
    for (auto& i : mInstances) i.store(0, std::memory_order_relaxed);
}

SonaTransportClient::~SonaTransportClient() {
    stop();
    dispatch_release(mQueue);
}

void SonaTransportClient::start() {
    if (mStarted) return;
    mStarted = true;
    mBackoffNanos = kBackoffMin;
    connectLocked();
}

void SonaTransportClient::stop() {
    mStarted = false;
    if (mConnection) {
        xpc_connection_cancel(mConnection);
        xpc_release(mConnection);
        mConnection = nullptr;
    }
    mConnected.store(false, std::memory_order_release);
    Mapping* old = mCurrent.exchange(nullptr, std::memory_order_acq_rel);
    if (old) retireMapping(old);
}

void SonaTransportClient::connectLocked() {
    if (!mStarted || mConnection) return;
    xpc_connection_t connection = xpc_connection_create_mach_service(kSonaTransportServiceName, mQueue, 0);
    if (!connection) { SonaTLog("cannot create connection"); scheduleReconnect(); return; }
    mConnection = connection;
    // A cancelled connection still delivers a final event; only the current connection matters.
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        if (connection == mConnection) handleEvent(event);
    });
    xpc_connection_resume(connection);
    sendHello();
}

void SonaTransportClient::handleEvent(xpc_object_t event) {
    if (xpc_get_type(event) == XPC_TYPE_DICTIONARY) { if (mHandler) mHandler(event); return; }
    if (xpc_get_type(event) != XPC_TYPE_ERROR) return;
    const char* description = xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);
    SonaTLog("connection error: " SONA_PUB, description ? description : "?");
    mConnected.store(false, std::memory_order_release);
    if (mConnection) { xpc_connection_cancel(mConnection); xpc_release(mConnection); mConnection = nullptr; }
    // The service is gone (crash or unload). Its region is no longer being drained; drop the
    // mapping so the IO thread stops writing into a dead ring, then try again with backoff.
    Mapping* old = mCurrent.exchange(nullptr, std::memory_order_acq_rel);
    if (old) retireMapping(old);
    scheduleReconnect();
}

void SonaTransportClient::scheduleReconnect() {
    if (!mStarted || mReconnectPending) return;
    mReconnectPending = true;
    SonaTLog("reconnect in %llu ms", mBackoffNanos / NSEC_PER_MSEC);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)mBackoffNanos), mQueue, ^{
        mReconnectPending = false;
        mBackoffNanos = std::min<uint64_t>(mBackoffNanos * 2, kBackoffMax);
        connectLocked();
    });
}

void SonaTransportClient::sendHello() {
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_Hello);
    xpc_dictionary_set_uint64(msg, kSonaMsg_ProtocolVersion, kSonaTransportProtocolVersion);
    xpc_dictionary_set_string(msg, kSonaMsg_DriverVersion, mDriverVersion.c_str());
    xpc_connection_t connection = mConnection;
    SonaTLog("sending hello");
    xpc_connection_send_message_with_reply(connection, msg, mQueue, ^(xpc_object_t reply) {
        SonaTLog("hello reply (%s)", connection == mConnection ? "current" : "stale");
        if (connection != mConnection) return;   // stale
        if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
            // Interrupted/invalid: the connection event handler tears down and reconnects.
            // Any other failure (e.g. reply timeout) must retry the handshake itself.
            const char* description = xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION);
            SonaTLog("hello failed: " SONA_PUB, description ? description : "?");
            if (reply != XPC_ERROR_CONNECTION_INTERRUPTED && reply != XPC_ERROR_CONNECTION_INVALID) {
                if (mConnection) { xpc_connection_cancel(mConnection); xpc_release(mConnection); mConnection = nullptr; }
                scheduleReconnect();
            }
            return;
        }
        adoptMapping(reply);
    });
    xpc_release(msg);
}

void SonaTransportClient::adoptMapping(xpc_object_t reply) {
    if (!xpc_dictionary_get_bool(reply, kSonaMsg_OK)) {
        const char* error = xpc_dictionary_get_string(reply, kSonaMsg_Error);
        SonaTLog("hello rejected: " SONA_PUB, error ? error : "?");
        return;   // incompatible service: stay disconnected until it is replaced
    }
    xpc_object_t shmem = xpc_dictionary_get_value(reply, kSonaMsg_SharedMemory);
    uint64_t generation = xpc_dictionary_get_uint64(reply, kSonaMsg_Generation);
    if (!shmem || xpc_get_type(shmem) != XPC_TYPE_SHMEM || generation == 0) {
        SonaTLog("hello reply without a usable region");
        return;
    }
    void* base = nullptr;
    size_t size = xpc_shmem_map(shmem, &base);
    if (!base || !SonaTransportValidateRegion(base, size, generation)) {
        SonaTLog("rejected region (generation %llu, %zu bytes)", generation, size);
        if (base) munmap(base, size);
        return;
    }
    // Pre-touch every page off the realtime thread so the first IO cycle never faults.
    volatile uint8_t sink = 0;
    for (size_t offset = 0; offset < size; offset += 4096) sink += static_cast<uint8_t*>(base)[offset];
    (void)sink;

    Mapping* fresh = new Mapping;
    fresh->base = base;
    fresh->size = size;
    fresh->generation = generation;
    fresh->shmem = xpc_retain(shmem);
    mGeneration.store(generation, std::memory_order_release);
    Mapping* old = mCurrent.exchange(fresh, std::memory_order_acq_rel);
    if (old) retireMapping(old);
    mBackoffNanos = kBackoffMin;
    mConnected.store(true, std::memory_order_release);
    SonaTLog("connected, generation %llu", generation);
    resendRegistrations();
}

// Control queue. The IO thread holds a mapping only for one IO operation; wait for that to end.
void SonaTransportClient::retireMapping(Mapping* old) {
    for (int spins = 0; mRealtimeBusy.load(std::memory_order_acquire) != 0; ++spins) {
        usleep(spins < 100 ? 100 : 1000);
        if (spins > 5000) { SonaTLog("realtime thread never released mapping; leaking"); return; }
    }
    munmap(old->base, old->size);
    xpc_release(old->shmem);
    delete old;
}

void SonaTransportClient::send(xpc_object_t message) {
    if (mConnection && mConnected.load(std::memory_order_acquire)) xpc_connection_send_message(mConnection, message);
    xpc_release(message);
}

void SonaTransportClient::resendRegistrations() {
    {
        xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_Format);
        xpc_dictionary_set_double(msg, kSonaMsg_SampleRate, mSampleRate);
        xpc_dictionary_set_uint64(msg, kSonaMsg_FormatGeneration, mFormatGeneration.load());
        send(msg);
    }
    for (uint32_t slot = 0; slot < kSonaTransportMaxClients; ++slot) {
        const ClientRecord& c = mClients[slot];
        if (!c.used) continue;
        xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_ClientAdd);
        xpc_dictionary_set_uint64(msg, kSonaMsg_Slot, slot);
        xpc_dictionary_set_uint64(msg, kSonaMsg_Instance, c.instance);
        xpc_dictionary_set_uint64(msg, kSonaMsg_ClientID, c.clientID);
        xpc_dictionary_set_int64(msg, kSonaMsg_PID, c.pid);
        xpc_dictionary_set_string(msg, kSonaMsg_BundleID, c.bundleID.c_str());
        send(msg);
    }
    {
        xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_IO);
        xpc_dictionary_set_bool(msg, kSonaMsg_Running, mIORunning);
        send(msg);
    }
}

void SonaTransportClient::registerClient(uint32_t slot, uint32_t clientID, int32_t pid, const std::string& bundleID) {
    if (slot >= kSonaTransportMaxClients) return;
    ClientRecord& c = mClients[slot];
    c.used = true;
    c.instance = mNextInstance++;
    if (mNextInstance == 0) mNextInstance = 1;
    c.clientID = clientID;
    c.pid = pid;
    c.bundleID = bundleID;
    mInstances[slot].store(c.instance, std::memory_order_release);
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_ClientAdd);
    xpc_dictionary_set_uint64(msg, kSonaMsg_Slot, slot);
    xpc_dictionary_set_uint64(msg, kSonaMsg_Instance, c.instance);
    xpc_dictionary_set_uint64(msg, kSonaMsg_ClientID, clientID);
    xpc_dictionary_set_int64(msg, kSonaMsg_PID, pid);
    xpc_dictionary_set_string(msg, kSonaMsg_BundleID, bundleID.c_str());
    send(msg);
}

void SonaTransportClient::unregisterClient(uint32_t slot) {
    if (slot >= kSonaTransportMaxClients || !mClients[slot].used) return;
    uint32_t instance = mClients[slot].instance;
    mClients[slot] = ClientRecord{};
    mInstances[slot].store(0, std::memory_order_release);
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_ClientRemove);
    xpc_dictionary_set_uint64(msg, kSonaMsg_Slot, slot);
    xpc_dictionary_set_uint64(msg, kSonaMsg_Instance, instance);
    send(msg);
}

void SonaTransportClient::setFormat(double sampleRate) {
    if (sampleRate == mSampleRate) return;
    mSampleRate = sampleRate;
    uint32_t generation = mFormatGeneration.fetch_add(1) + 1;
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_Format);
    xpc_dictionary_set_double(msg, kSonaMsg_SampleRate, sampleRate);
    xpc_dictionary_set_uint64(msg, kSonaMsg_FormatGeneration, generation);
    send(msg);
}

void SonaTransportClient::setIORunning(bool running) {
    if (running == mIORunning) return;
    mIORunning = running;
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_IO);
    xpc_dictionary_set_bool(msg, kSonaMsg_Running, running);
    send(msg);
}
