//
//  main.cpp — Sona Audio Service
//
//  Owns the launchd Mach services, hands the plug-in a shared-memory region per connection
//  generation, and hosts the audio engine (SonaEngine.cpp): the only place in Sona that is
//  allowed to call the Core Audio client HAL.
//
//  Two listeners:
//    kSonaTransportServiceName        the driver. Every peer must satisfy a code-signing
//                                     requirement naming Apple's plug-in host; libxpc drops
//                                     anything else before it reaches HandleMessage.
//    kSonaTransportStatusServiceName  read-only status for tools, answered for any caller.
//

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <libproc.h>
#include <os/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <xpc/xpc.h>
#include <atomic>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../Shared/SonaProtocol.h"
#include "../Shared/SonaTransport.h"
#include "SonaEngine.h"

os_log_t gSonaServiceLog = nullptr;
#define SLog(fmt, ...) os_log(gSonaServiceLog ? gSonaServiceLog : OS_LOG_DEFAULT, "SonaService: " fmt, ##__VA_ARGS__)

namespace {

// The processes Apple hosts AudioServerPlugIns in: coreaudiod itself (macOS 14/15) and the
// per-driver helper of later releases. Anything else is not the driver, whatever it sends.
// SONA_PEER_REQUIREMENT replaces this for a test service bootstrapped in a user domain, where
// the loopback tests (signed as com.sona.test.loopback) stand in for the host.
const char* const kHostRequirement =
    "anchor apple and ("
    "identifier \"com.apple.audio.coreaudiod\" or "
    "identifier \"com.apple.audio.Core-Audio-Driver-Service\" or "
    "identifier \"com.apple.audio.Core-Audio-Driver-Service.helper\")";

struct Session {
    xpc_connection_t peer = nullptr;
    uint64_t         generation = 0;
    xpc_object_t     shmem = nullptr;
    void*            region = nullptr;
    size_t           regionSize = 0;
    SonaEngineRetireTicket ticket;   // set once the engine has unpublished the region
};

dispatch_queue_t      gQueue;                // all state below is touched only on this queue
Session*              gSession = nullptr;
std::vector<Session*> gRetired;              // unpublished, still referenced by a reader
uint64_t              gNextGeneration = 1;
bool                  gRetirePollScheduled = false;
constexpr size_t      kMaxRetiredSessions = 8;   // ~9 MB each; beyond this the service is broken anyway
constexpr int64_t     kRetirePollInterval = 50 * NSEC_PER_MSEC;

void ReleasePeer(Session* s) {
    if (s->peer) { xpc_release(s->peer); s->peer = nullptr; }
}

void FreeSession(Session* s) {
    if (s->region) munmap(s->region, s->regionSize);
    if (s->shmem) xpc_release(s->shmem);
    ReleasePeer(s);
    delete s;
}

void PollRetired();
void ScheduleRetirePoll() {
    if (gRetirePollScheduled || gRetired.empty()) return;
    gRetirePollScheduled = true;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRetirePollInterval), gQueue, ^{ gRetirePollScheduled = false; PollRetired(); });
}

void PollRetired() {
    for (auto it = gRetired.begin(); it != gRetired.end();) {
        Session* s = *it;
        if (!SonaEngineRegionRetired(s->ticket)) { ++it; continue; }
        SLog("region generation %llu retired late; unmapping", s->generation);
        FreeSession(s);
        it = gRetired.erase(it);
    }
    ScheduleRetirePoll();
}

// Unpublishes the session's region from the engine. The mapping is released only once every
// reader has provably left it; a reader still inside keeps the mapping alive (never unmapped
// on a timeout) and the session waits in gRetired.
void RetireSession(Session* s) {
    if (!s) return;
    ReleasePeer(s);
    if (!s->region) { FreeSession(s); return; }
    s->ticket = SonaEngineDetachRegion();
    if (SonaEngineRegionRetired(s->ticket)) { FreeSession(s); return; }
    SLog("region generation %llu still referenced by a reader; unmap deferred", s->generation);
    gRetired.push_back(s);
    ScheduleRetirePoll();
}

Session* CreateSession(xpc_connection_t peer) {
    Session* s = new Session;
    s->peer = (xpc_connection_t)xpc_retain(peer);
    s->generation = gNextGeneration++;
    void* region = mmap(nullptr, kSonaTransportRegionSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (region == MAP_FAILED) { SLog("mmap failed"); FreeSession(s); return nullptr; }
    SonaTransportInitRegion(region, s->generation);
    s->region = region;
    s->regionSize = kSonaTransportRegionSize;
    s->shmem = xpc_shmem_create(region, kSonaTransportRegionSize);
    if (!s->shmem) { SLog("xpc_shmem_create failed"); FreeSession(s); return nullptr; }
    return s;
}

// MARK: - Plist <-> XPC

xpc_object_t PlistData(CFPropertyListRef plist) {
    CFDataRef data = CFPropertyListCreateData(nullptr, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    if (!data) return nullptr;
    xpc_object_t x = xpc_data_create(CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
    CFRelease(data);
    return x;
}

CFPropertyListRef PlistFromMessage(xpc_object_t message) {
    size_t length = 0;
    const void* bytes = xpc_dictionary_get_data(message, kSonaMsg_Plist, &length);
    if (!bytes || length == 0 || length > (1u << 20)) return nullptr;
    CFDataRef data = CFDataCreate(nullptr, (const UInt8*)bytes, (CFIndex)length);
    CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    return plist;
}

void PushPlist(const char* op, CFPropertyListRef plist) {
    if (!gSession || !plist) return;
    xpc_object_t data = PlistData(plist);
    if (!data) return;
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, op);
    xpc_dictionary_set_value(msg, kSonaMsg_Plist, data);
    xpc_connection_send_message(gSession->peer, msg);
    xpc_release(data);
    xpc_release(msg);
}

void PushStatus(void*, CFDictionaryRef status) { PushPlist(kSonaOp_StatusPush, status); }
void PushClients(void*, CFArrayRef clients) { PushPlist(kSonaOp_ClientsPush, clients); }
void PushConfig(void*, CFDictionaryRef config) { PushPlist(kSonaOp_ConfigPush, config); }
void PushMaster(void*, float volume, bool mute) {
    if (!gSession) return;
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_Master);
    xpc_dictionary_set_double(msg, kSonaMsg_Volume, volume);
    xpc_dictionary_set_bool(msg, kSonaMsg_Mute, mute);
    xpc_connection_send_message(gSession->peer, msg);
    xpc_release(msg);
}

// Everything the plug-in facade caches, sent right after a handshake.
void PushSnapshot() {
    CFDictionaryRef status = SonaEngineCopyStatus(); PushStatus(nullptr, status); CFRelease(status);
    CFArrayRef clients = SonaEngineCopyClients(); PushClients(nullptr, clients); CFRelease(clients);
    if (CFDictionaryRef config = SonaEngineCopyConfig()) { PushConfig(nullptr, config); CFRelease(config); }
    float volume; bool mute; SonaEngineMasterLevel(volume, mute); PushMaster(nullptr, volume, mute);
}

xpc_object_t BuildToolStatus() {
    xpc_object_t status = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(status, "version", kSonaDriverVersion);
    xpc_dictionary_set_bool(status, "connected", gSession != nullptr);
    if (gSession) xpc_dictionary_set_uint64(status, "generation", gSession->generation);
    xpc_dictionary_set_uint64(status, "retiredRegions", gRetired.size());
    CFDictionaryRef engine = SonaEngineCopyStatus();
    if (xpc_object_t data = PlistData(engine)) { xpc_dictionary_set_value(status, "enginePlist", data); xpc_release(data); }
    CFRelease(engine);
    CFArrayRef clients = SonaEngineCopyClients();
    if (xpc_object_t data = PlistData(clients)) { xpc_dictionary_set_value(status, "clientsPlist", data); xpc_release(data); }
    CFRelease(clients);
    if (gSession) {
        xpc_object_t rings = xpc_array_create(nullptr, 0);
        for (uint32_t slot = 0; slot < kSonaTransportMaxClients; ++slot) {
            SonaTransportClientRing* ring = SonaTransportRing(gSession->region, slot);
            uint64_t written = ring->blockWrite.load(std::memory_order_relaxed);
            if (written == 0) continue;
            xpc_object_t d = xpc_dictionary_create(nullptr, nullptr, 0);
            xpc_dictionary_set_uint64(d, "slot", slot);
            xpc_dictionary_set_uint64(d, "blocks", written);
            xpc_dictionary_set_uint64(d, "queued", written - ring->blockRead.load(std::memory_order_relaxed));
            xpc_dictionary_set_uint64(d, "droppedBlocks", ring->droppedBlocks.load(std::memory_order_relaxed));
            xpc_array_append_value(rings, d);
            xpc_release(d);
        }
        xpc_dictionary_set_value(status, "rings", rings);
        xpc_release(rings);
    }
    return status;
}

void Reply(xpc_object_t request, xpc_object_t reply) {
    xpc_connection_t peer = xpc_dictionary_get_remote_connection(request);
    if (peer) xpc_connection_send_message(peer, reply);
    xpc_release(reply);
}

void RejectHello(xpc_object_t request, xpc_object_t reply, const char* error) {
    xpc_dictionary_set_bool(reply, kSonaMsg_OK, false);
    xpc_dictionary_set_string(reply, kSonaMsg_Error, error);
    Reply(request, reply);
}

// A hello from the (verified) plug-in host opens a new session generation. The previous
// session ends: its region is unpublished and its connection, if it is a different one, is
// cancelled, so that host instance sees XPC_ERROR_CONNECTION_INVALID, drops its mapping and
// reconnects instead of writing into a ring nobody drains.
void HandleHello(xpc_connection_t peer, xpc_object_t request) {
    xpc_object_t reply = xpc_dictionary_create_reply(request);
    if (!reply) return;
    uint64_t version = xpc_dictionary_get_uint64(request, kSonaMsg_ProtocolVersion);
    if (version != kSonaTransportProtocolVersion) {
        SLog("rejecting driver with protocol %llu (want %u)", version, kSonaTransportProtocolVersion);
        RejectHello(request, reply, "protocol version mismatch");
        return;
    }
    if (gRetired.size() >= kMaxRetiredSessions) {
        SLog("rejecting hello: %zu retired regions are still referenced by a stuck reader", gRetired.size());
        RejectHello(request, reply, "retired regions over budget");
        return;
    }
    Session* fresh = CreateSession(peer);
    if (!fresh) { RejectHello(request, reply, "cannot allocate region"); return; }
    Session* old = gSession;
    gSession = nullptr;
    if (old) {
        SLog("session generation %llu replaced by a new handshake", old->generation);
        if (old->peer != peer) xpc_connection_cancel(old->peer);
        RetireSession(old);
    }
    gSession = fresh;
    SonaEngineAttachRegion(fresh->region, fresh->generation);
    const char* driverVersion = xpc_dictionary_get_string(request, kSonaMsg_DriverVersion);
    SLog("session generation %llu for driver %{public}s (pid %d)", fresh->generation,
         driverVersion ? driverVersion : "?", xpc_connection_get_pid(peer));
    xpc_dictionary_set_bool(reply, kSonaMsg_OK, true);
    xpc_dictionary_set_uint64(reply, kSonaMsg_Generation, fresh->generation);
    xpc_dictionary_set_value(reply, kSonaMsg_SharedMemory, fresh->shmem);
    Reply(request, reply);
    PushSnapshot();
}

void HandleMessage(xpc_connection_t peer, xpc_object_t request) {
    const char* op = xpc_dictionary_get_string(request, kSonaMsg_Op);
    if (!op) return;
    if (strcmp(op, kSonaOp_Hello) == 0) { HandleHello(peer, request); return; }
    if (!gSession || gSession->peer != peer) return;   // only the current session may configure
    if (strcmp(op, kSonaOp_ClientAdd) == 0) {
        uint64_t slot = xpc_dictionary_get_uint64(request, kSonaMsg_Slot);
        const char* bundle = xpc_dictionary_get_string(request, kSonaMsg_BundleID);
        SonaEngineClientAdd((uint32_t)slot, (uint32_t)xpc_dictionary_get_uint64(request, kSonaMsg_Instance),
                            (uint32_t)xpc_dictionary_get_uint64(request, kSonaMsg_ClientID),
                            (int32_t)xpc_dictionary_get_int64(request, kSonaMsg_PID), bundle ? bundle : "");
    } else if (strcmp(op, kSonaOp_ClientRemove) == 0) {
        SonaEngineClientRemove((uint32_t)xpc_dictionary_get_uint64(request, kSonaMsg_Slot),
                               (uint32_t)xpc_dictionary_get_uint64(request, kSonaMsg_Instance));
    } else if (strcmp(op, kSonaOp_Format) == 0) {
        SonaEngineSetFormat(xpc_dictionary_get_double(request, kSonaMsg_SampleRate),
                            (uint32_t)xpc_dictionary_get_uint64(request, kSonaMsg_FormatGeneration));
    } else if (strcmp(op, kSonaOp_IO) == 0) {
        SonaEngineSetIORunning(xpc_dictionary_get_bool(request, kSonaMsg_Running));
    } else if (strcmp(op, kSonaOp_Config) == 0) {
        CFPropertyListRef plist = PlistFromMessage(request);
        if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) SonaEngineApplyConfig((CFDictionaryRef)plist);
        if (plist) CFRelease(plist);
    } else if (strcmp(op, kSonaOp_OutputLevel) == 0) {
        const char* uid = xpc_dictionary_get_string(request, kSonaMsg_UID);
        xpc_object_t v = xpc_dictionary_get_value(request, kSonaMsg_Volume);
        xpc_object_t m = xpc_dictionary_get_value(request, kSonaMsg_Mute);
        if (uid) SonaEngineSetOutputLevel(uid, v != nullptr, v ? (float)xpc_double_get_value(v) : 1.0f,
                                          m != nullptr, m ? xpc_bool_get_value(m) : false);
    } else if (strcmp(op, kSonaOp_Master) == 0) {
        if (xpc_object_t v = xpc_dictionary_get_value(request, kSonaMsg_Volume)) SonaEngineSetMasterVolume((float)xpc_double_get_value(v));
        if (xpc_object_t m = xpc_dictionary_get_value(request, kSonaMsg_Mute)) SonaEngineSetMasterMute(xpc_bool_get_value(m));
    }
}

std::string PeerPath(xpc_connection_t peer) {
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    return proc_pidpath(xpc_connection_get_pid(peer), path, sizeof(path)) > 0 ? path : "?";
}

void HandleDriverPeer(xpc_connection_t peer) {
    const char* requirement = getenv("SONA_PEER_REQUIREMENT");
    if (!requirement || !*requirement) requirement = kHostRequirement;
    // Checked by libxpc against the audit token of every message: a peer that does not satisfy
    // it never reaches HandleMessage, and its hello is answered with
    // XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT. The pid is logged so a rejected host can be
    // identified after an OS update renames the plug-in host.
    int err = xpc_connection_set_peer_code_signing_requirement(peer, requirement);
    if (err != 0) {
        SLog("cannot apply the peer code-signing requirement (%d); refusing pid %d", err, xpc_connection_get_pid(peer));
        xpc_connection_cancel(peer);
        return;
    }
    SLog("driver listener: connection from pid %d (%{public}s)", xpc_connection_get_pid(peer), PeerPath(peer).c_str());
    xpc_connection_set_target_queue(peer, gQueue);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        xpc_type_t type = xpc_get_type(event);
        if (type == XPC_TYPE_DICTIONARY) { HandleMessage(peer, event); return; }
        if (type != XPC_TYPE_ERROR) return;
        if (event == XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT) {
            SLog("rejected pid %d (%{public}s): not the plug-in host", xpc_connection_get_pid(peer), PeerPath(peer).c_str());
            return;
        }
        if (gSession && gSession->peer == peer) {
            SLog("plug-in session generation %llu ended", gSession->generation);
            Session* old = gSession;
            gSession = nullptr;
            RetireSession(old);
        }
    });
    xpc_connection_resume(peer);
}

void HandleStatusPeer(xpc_connection_t peer) {
    xpc_connection_set_target_queue(peer, gQueue);
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_DICTIONARY) return;
        const char* op = xpc_dictionary_get_string(event, kSonaMsg_Op);
        if (!op || strcmp(op, kSonaOp_Status) != 0) return;
        xpc_object_t reply = xpc_dictionary_create_reply(event);
        if (!reply) return;
        xpc_object_t status = BuildToolStatus();
        xpc_dictionary_set_value(reply, kSonaMsg_Status, status);
        xpc_release(status);
        Reply(event, reply);
    });
    xpc_connection_resume(peer);
}

xpc_connection_t Listen(const char* name, void (*handlePeer)(xpc_connection_t)) {
    xpc_connection_t listener = xpc_connection_create_mach_service(name, gQueue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) { SLog("cannot create listener for %{public}s", name); return nullptr; }
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) == XPC_TYPE_CONNECTION) handlePeer((xpc_connection_t)event);
        else SLog("listener %{public}s error", name);
    });
    return listener;
}

} // namespace

int main() {
    gSonaServiceLog = os_log_create("com.sona.audio-service", "service");
    gNextGeneration = ((uint64_t)time(nullptr) << 20) | 1;
    gQueue = dispatch_queue_create("com.sona.audio-service.control", DISPATCH_QUEUE_SERIAL);

    const char* stateDir = "/Library/Application Support/Sona";
    mkdir(stateDir, 0755);
    std::string stateFile = std::string(stateDir) + "/service-state.plist";
    if (const char* override = getenv("SONA_STATE_FILE")) stateFile = override;

    xpc_connection_t driverListener = Listen(kSonaTransportServiceName, HandleDriverPeer);
    xpc_connection_t statusListener = Listen(kSonaTransportStatusServiceName, HandleStatusPeer);
    if (!driverListener || !statusListener) return 1;
    dispatch_async(gQueue, ^{
        SonaEnginePushes pushes;
        pushes.status = PushStatus; pushes.clients = PushClients; pushes.config = PushConfig; pushes.master = PushMaster;
        SonaEngineStart(gQueue, pushes, stateFile.c_str());
        xpc_connection_resume(driverListener);
        xpc_connection_resume(statusListener);
        SLog("listening on %{public}s and %{public}s (version %s)", kSonaTransportServiceName, kSonaTransportStatusServiceName, kSonaDriverVersion);
    });
    dispatch_main();
}
