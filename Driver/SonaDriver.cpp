//
//  SonaDriver.cpp
//  Sona virtual audio device — an AudioServerPlugIn HAL driver.
//
//  Object model:
//    PlugIn (1)
//      └─ Device "Sona" (2)   virtual, output only, stereo Float32
//           ├─ Output stream (3)
//           ├─ Master volume control (4)
//           └─ Master mute control (5)
//
//  This plug-in is a transport adapter. In kAudioServerPlugInIOOperationProcessOutput, which the
//  HAL calls once per client before mixing, the client's PCM is copied into that client's ring
//  in the region shared with Sona Audio Service and the buffer is silenced so the host mix
//  carries nothing. Gain, routing, mixing, resampling and the physical devices live in the
//  service. Nothing here calls the Core Audio client HAL; see ../ARCHITECTURE.md.
//
//  The app-facing custom properties (config, clients, status, output level) remain on this
//  device: writes are forwarded to the service, reads return the last snapshot it pushed.
//

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cmath>
#include <cstddef>

#include "../Shared/SonaProtocol.h"
#include "../Shared/SonaTransport.h"
#include "SonaTransportClient.h"

static os_log_t gLog = nullptr;
#define SonaLog(fmt, ...) os_log(gLog ? gLog : OS_LOG_DEFAULT, "SonaDriver: " fmt, ##__VA_ARGS__)

// MARK: - Constants

enum {
    kObjectID_PlugIn               = kAudioObjectPlugInObject,
    kObjectID_Device               = 2,
    kObjectID_Stream_Output        = 3,
    kObjectID_Volume_Output_Master = 4,
    kObjectID_Mute_Output_Master   = 5,
};

static const UInt32  kDevice_RingBufferSize = 16384;   // zero timestamp period (frames)
static const UInt32  kDevice_Channels       = 2;
static const UInt32  kDevice_LatencyFrames  = 2048;    // service sink fill; see ARCHITECTURE.md latency notes
static const Float32 kVolume_MinDB          = -96.0f;
static const Float32 kVolume_MaxDB          = 0.0f;
static const UInt32  kMaxClients            = kSonaTransportMaxClients;
static const UInt32  kMaxFramesPerCycle     = kSonaTransportMaxBlockFrames;
static const Float64 kSupportedRates[]      = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

// MARK: - State

struct ClientSlot {
    std::atomic<bool>   used{false};
    std::atomic<UInt32> clientID{0};
};

static AudioServerPlugInHostRef  gHost = nullptr;
static std::atomic<UInt32>       gRefCount{0};
static dispatch_queue_t          gConfigQueue = nullptr;
static std::mutex                gSnapshotMutex;

static std::atomic<Float64>      gSampleRate{48000.0};
static std::atomic<UInt64>       gIORunningCount{0};
static UInt64                    gAnchorHostTime = 0;
static UInt64                    gNumberTimeStamps = 0;
static Float64                   gHostTicksPerFrame = 0.0;
static Float64                   gHostTicksPerSecond = 0.0;

// Clock slaving (Sona IO thread only). When the service's master output publishes anchors into
// the shared region, the Sona timeline is rate-locked to that device.
struct SonaClockLock {
    bool     locked = false;
    UInt64   generation = 0;
    UInt64   region = 0;          // transport generation the lock refers to
    Float64  sonaRate = 0;
    Float64  sonaSample0 = 0;
    Float64  deviceSample0 = 0;
    UInt64   lastHostTime = 0;
};
static SonaClockLock             gClockLock;
static constexpr Float64         kClockSlewLimit = 0.002;

// Cached control values; the service reports actual hardware state back.
static std::atomic<float>        gMasterVolume{1.0f};
static std::atomic<bool>         gMasterMute{false};

static ClientSlot                gClients[kMaxClients];
static SonaTransportClient*      gTransport = nullptr;

// Snapshots pushed by the service (gSnapshotMutex).
static CFDictionaryRef           gStatus = nullptr;
static CFArrayRef                gClientsList = nullptr;
static CFDictionaryRef           gConfig = nullptr;

// MARK: - Helpers

static CFStringRef SonaCFString(const char* s) { return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8); }

static Float32 SonaScalarToDB(Float32 scalar) {
    if (scalar <= 0.0f) return kVolume_MinDB;
    Float32 db = 60.0f * log10f(scalar);
    return std::fmax(kVolume_MinDB, std::fmin(kVolume_MaxDB, db));
}

static Float32 SonaDBToScalar(Float32 db) {
    db = std::fmax(kVolume_MinDB, std::fmin(kVolume_MaxDB, db));
    if (db <= kVolume_MinDB) return 0.0f;
    return powf(10.0f, db / 60.0f);
}

static void SonaNotify(AudioObjectID object, AudioObjectPropertySelector selector,
                       AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    if (!gHost) return;
    AudioObjectPropertyAddress addr = { selector, scope, kAudioObjectPropertyElementMain };
    gHost->PropertiesChanged(gHost, object, 1, &addr);
}

static void SonaPublishVolume(Float32 value) {
    if (value != gMasterVolume.exchange(value)) {
        SonaNotify(kObjectID_Volume_Output_Master, kAudioLevelControlPropertyScalarValue);
        SonaNotify(kObjectID_Volume_Output_Master, kAudioLevelControlPropertyDecibelValue);
    }
}

static void SonaPublishMute(bool value) {
    if (value != gMasterMute.exchange(value)) SonaNotify(kObjectID_Mute_Output_Master, kAudioBooleanControlPropertyValue);
}

static ClientSlot* SonaFindClient(UInt32 clientID) {
    for (UInt32 i = 0; i < kMaxClients; ++i)
        if (gClients[i].used.load(std::memory_order_acquire) && gClients[i].clientID.load(std::memory_order_relaxed) == clientID)
            return &gClients[i];
    return nullptr;
}

// MARK: - Service pushes (config queue)

static CFPropertyListRef SonaPlistFromMessage(xpc_object_t message) {
    size_t length = 0;
    const void* bytes = xpc_dictionary_get_data(message, kSonaMsg_Plist, &length);
    if (!bytes || length == 0 || length > (1u << 20)) return nullptr;
    CFDataRef data = CFDataCreate(nullptr, (const UInt8*)bytes, (CFIndex)length);
    CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    return plist;
}

// Takes ownership of `value`; rejects the wrong type.
static void SonaReplaceSnapshot(CFTypeRef& slot, CFPropertyListRef value, CFTypeID type) {
    if (!value) return;
    if (CFGetTypeID(value) != type) { CFRelease(value); return; }
    std::lock_guard<std::mutex> lock(gSnapshotMutex);
    if (slot) CFRelease(slot);
    slot = value;
}

static void SonaHandleServiceMessage(xpc_object_t message) {
    const char* op = xpc_dictionary_get_string(message, kSonaMsg_Op);
    if (!op) return;
    if (strcmp(op, kSonaOp_StatusPush) == 0) {
        SonaReplaceSnapshot((CFTypeRef&)gStatus, SonaPlistFromMessage(message), CFDictionaryGetTypeID());
        SonaNotify(kObjectID_Device, kSonaProperty_Status);
    } else if (strcmp(op, kSonaOp_ClientsPush) == 0) {
        SonaReplaceSnapshot((CFTypeRef&)gClientsList, SonaPlistFromMessage(message), CFArrayGetTypeID());
        SonaNotify(kObjectID_Device, kSonaProperty_Clients);
    } else if (strcmp(op, kSonaOp_ConfigPush) == 0) {
        SonaReplaceSnapshot((CFTypeRef&)gConfig, SonaPlistFromMessage(message), CFDictionaryGetTypeID());
        SonaNotify(kObjectID_Device, kSonaProperty_Config);
    } else if (strcmp(op, kSonaOp_Master) == 0) {
        if (xpc_object_t v = xpc_dictionary_get_value(message, kSonaMsg_Volume)) SonaPublishVolume((Float32)xpc_double_get_value(v));
        if (xpc_object_t m = xpc_dictionary_get_value(message, kSonaMsg_Mute)) SonaPublishMute(xpc_bool_get_value(m));
    }
}

static void SonaSendPlist(const char* op, CFPropertyListRef plist) {
    CFDataRef data = CFPropertyListCreateData(nullptr, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    if (!data) return;
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, op);
    xpc_dictionary_set_data(msg, kSonaMsg_Plist, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
    CFRelease(data);
    gTransport->sendControl(msg);   // consumes msg
}

static void SonaSendMaster(bool hasVolume, float volume, bool hasMute, bool mute) {
    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_Master);
    if (hasVolume) xpc_dictionary_set_double(msg, kSonaMsg_Volume, volume);
    if (hasMute) xpc_dictionary_set_bool(msg, kSonaMsg_Mute, mute);
    gTransport->sendControl(msg);
}

// MARK: - COM plumbing

extern AudioServerPlugInDriverInterface gDriverInterface;
static AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePtr;

static HRESULT Sona_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (!outInterface) return E_POINTER;
    if (inDriver != gDriverRef) { *outInterface = nullptr; return E_NOINTERFACE; }
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, inUUID);
    bool ok = requested && (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID));
    if (requested) CFRelease(requested);
    if (!ok) { *outInterface = nullptr; return E_NOINTERFACE; }
    gRefCount.fetch_add(1);
    *outInterface = gDriverRef;
    return S_OK;
}

static ULONG Sona_AddRef(void* inDriver) { return inDriver == gDriverRef ? gRefCount.fetch_add(1) + 1 : 0; }
static ULONG Sona_Release(void* inDriver) {
    if (inDriver != gDriverRef) return 0;
    UInt32 v = gRefCount.load();
    if (v > 0) v = gRefCount.fetch_sub(1) - 1;
    return v;
}

// MARK: - Basic operations

static OSStatus Sona_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    gLog = os_log_create("com.sona.driver", "plugin");
    gConfigQueue = dispatch_queue_create("com.sona.driver.config", DISPATCH_QUEUE_SERIAL);

    struct mach_timebase_info tb;
    mach_timebase_info(&tb);
    gHostTicksPerSecond = (Float64)tb.denom / (Float64)tb.numer * 1000000000.0;
    gHostTicksPerFrame = gHostTicksPerSecond / gSampleRate.load();

    gTransport = new SonaTransportClient(gConfigQueue, kSonaDriverVersion);
    gTransport->setMessageHandler(SonaHandleServiceMessage);
    dispatch_async(gConfigQueue, ^{ gTransport->start(); });
    SonaLog("initialized (version %s)", kSonaDriverVersion);
    return noErr;
}

static OSStatus Sona_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) {
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus Sona_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }

static OSStatus Sona_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                     const AudioServerPlugInClientInfo* inClientInfo) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device || !inClientInfo) return kAudioHardwareBadObjectError;
    ClientSlot* slot = nullptr;
    for (UInt32 i = 0; i < kMaxClients; ++i) if (!gClients[i].used.load(std::memory_order_acquire)) { slot = &gClients[i]; break; }
    if (!slot) { SonaLog("client table full"); return noErr; }
    const uint32_t index = (uint32_t)(slot - gClients);
    const uint32_t clientID = inClientInfo->mClientID;
    const int32_t pid = inClientInfo->mProcessID;
    char bundle[512] = {0};
    if (inClientInfo->mBundleID) CFStringGetCString(inClientInfo->mBundleID, bundle, sizeof(bundle), kCFStringEncodingUTF8);
    const std::string bundleID = bundle;
    slot->clientID.store(clientID, std::memory_order_relaxed);
    slot->used.store(true, std::memory_order_release);
    dispatch_async(gConfigQueue, ^{ if (gTransport) gTransport->registerClient(index, clientID, pid, bundleID); });
    return noErr;
}

static OSStatus Sona_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                        const AudioServerPlugInClientInfo* inClientInfo) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device || !inClientInfo) return kAudioHardwareBadObjectError;
    if (ClientSlot* c = SonaFindClient(inClientInfo->mClientID)) {
        c->used.store(false, std::memory_order_release);
        const uint32_t index = (uint32_t)(c - gClients);
        dispatch_async(gConfigQueue, ^{ if (gTransport) gTransport->unregisterClient(index); });
    }
    return noErr;
}

static OSStatus Sona_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                                      UInt64 inChangeAction, void*) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    Float64 newRate = (Float64)inChangeAction;
    bool ok = false;
    for (Float64 r : kSupportedRates) if (r == newRate) ok = true;
    if (!ok) return kAudioHardwareBadObjectError;
    gSampleRate.store(newRate);
    gHostTicksPerFrame = gHostTicksPerSecond / newRate;
    dispatch_async(gConfigQueue, ^{ if (gTransport) gTransport->setFormat(newRate); });
    return noErr;
}

static OSStatus Sona_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64, void*) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return noErr;
}

// MARK: - Property helpers

static void SonaFillStreamFormat(AudioStreamBasicDescription& f, Float64 rate) {
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = kDevice_Channels * sizeof(Float32);
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = kDevice_Channels * sizeof(Float32);
    f.mChannelsPerFrame = kDevice_Channels;
    f.mBitsPerChannel = 32;
    f.mReserved = 0;
}

// MARK: - HasProperty

static Boolean Sona_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t,
                                const AudioObjectPropertyAddress* inAddress) {
    if (inDriver != gDriverRef || !inAddress) return false;
    const AudioObjectPropertySelector s = inAddress->mSelector;
    switch (inObjectID) {
        case kObjectID_PlugIn:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                    return true;
            }
            return false;
        case kObjectID_Device:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
                case kAudioObjectPropertyControlList: case kAudioObjectPropertyCustomPropertyInfoList:
                case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
                case kAudioDevicePropertyStreams: case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyPreferredChannelLayout:
                case kSonaProperty_Config: case kSonaProperty_OutputLevel: case kSonaProperty_Clients: case kSonaProperty_Status:
                    return true;
            }
            return false;
        case kObjectID_Stream_Output:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName: case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
            }
            return false;
        case kObjectID_Volume_Output_Master:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
                case kAudioControlPropertyScope: case kAudioControlPropertyElement:
                case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
                case kAudioLevelControlPropertyConvertDecibelsToScalar:
                    return true;
            }
            return false;
        case kObjectID_Mute_Output_Master:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
                case kAudioControlPropertyScope: case kAudioControlPropertyElement: case kAudioBooleanControlPropertyValue:
                    return true;
            }
            return false;
    }
    return false;
}

// MARK: - IsPropertySettable

static OSStatus Sona_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t,
                                        const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    if (inDriver != gDriverRef || !inAddress || !outIsSettable) return kAudioHardwareIllegalOperationError;
    if (!Sona_HasProperty(inDriver, inObjectID, 0, inAddress)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = inAddress->mSelector;
    *outIsSettable = false;
    switch (inObjectID) {
        case kObjectID_Device:
            if (s == kAudioDevicePropertyNominalSampleRate || s == kSonaProperty_Config || s == kSonaProperty_OutputLevel) *outIsSettable = true;
            break;
        case kObjectID_Stream_Output:
            if (s == kAudioStreamPropertyIsActive || s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat) *outIsSettable = true;
            break;
        case kObjectID_Volume_Output_Master:
            if (s == kAudioLevelControlPropertyScalarValue || s == kAudioLevelControlPropertyDecibelValue) *outIsSettable = true;
            break;
        case kObjectID_Mute_Output_Master:
            if (s == kAudioBooleanControlPropertyValue) *outIsSettable = true;
            break;
    }
    return noErr;
}

// MARK: - GetPropertyDataSize

static OSStatus Sona_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t,
                                         const AudioObjectPropertyAddress* inAddress,
                                         UInt32, const void*, UInt32* outDataSize) {
    if (inDriver != gDriverRef || !inAddress || !outDataSize) return kAudioHardwareIllegalOperationError;
    if (!Sona_HasProperty(inDriver, inObjectID, 0, inAddress)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = inAddress->mSelector;
    switch (inObjectID) {
        case kObjectID_PlugIn:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyManufacturer: case kAudioPlugInPropertyResourceBundle: *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); return noErr;
            }
            break;
        case kObjectID_Device:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
                case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioObjectPropertyOwnedObjects: *outDataSize = 3 * sizeof(AudioObjectID); return noErr;
                case kAudioObjectPropertyControlList: *outDataSize = 2 * sizeof(AudioObjectID); return noErr;
                case kAudioObjectPropertyCustomPropertyInfoList: *outDataSize = 4 * sizeof(AudioServerPlugInCustomPropertyInfo); return noErr;
                case kAudioDevicePropertyTransportType: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyRelatedDevices: *outDataSize = sizeof(AudioObjectID); return noErr;
                case kAudioDevicePropertyStreams: *outDataSize = inAddress->mScope == kAudioObjectPropertyScopeInput ? 0 : sizeof(AudioObjectID); return noErr;
                case kAudioDevicePropertyNominalSampleRate: *outDataSize = sizeof(Float64); return noErr;
                case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = (UInt32)(sizeof(kSupportedRates) / sizeof(kSupportedRates[0])) * sizeof(AudioValueRange); return noErr;
                case kAudioDevicePropertyPreferredChannelsForStereo: *outDataSize = 2 * sizeof(UInt32); return noErr;
                case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + kDevice_Channels * sizeof(AudioChannelDescription); return noErr;
                case kSonaProperty_Config: case kSonaProperty_OutputLevel: case kSonaProperty_Clients: case kSonaProperty_Status: *outDataSize = sizeof(CFPropertyListRef); return noErr;
            }
            break;
        case kObjectID_Stream_Output:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyName: *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency: *outDataSize = sizeof(UInt32); return noErr;
                case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *outDataSize = sizeof(AudioStreamBasicDescription); return noErr;
                case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
                    *outDataSize = (UInt32)(sizeof(kSupportedRates) / sizeof(kSupportedRates[0])) * sizeof(AudioStreamRangedDescription); return noErr;
            }
            break;
        case kObjectID_Volume_Output_Master:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioControlPropertyScope: *outDataSize = sizeof(AudioObjectPropertyScope); return noErr;
                case kAudioControlPropertyElement: *outDataSize = sizeof(AudioObjectPropertyElement); return noErr;
                case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
                case kAudioLevelControlPropertyConvertScalarToDecibels: case kAudioLevelControlPropertyConvertDecibelsToScalar: *outDataSize = sizeof(Float32); return noErr;
                case kAudioLevelControlPropertyDecibelRange: *outDataSize = sizeof(AudioValueRange); return noErr;
            }
            break;
        case kObjectID_Mute_Output_Master:
            switch (s) {
                case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioControlPropertyScope: *outDataSize = sizeof(AudioObjectPropertyScope); return noErr;
                case kAudioControlPropertyElement: *outDataSize = sizeof(AudioObjectPropertyElement); return noErr;
                case kAudioBooleanControlPropertyValue: *outDataSize = sizeof(UInt32); return noErr;
            }
            break;
    }
    return kAudioHardwareUnknownPropertyError;
}

// MARK: - GetPropertyData

#define SONA_REQUIRE_SIZE(n) do { if (inDataSize < (n)) return kAudioHardwareBadPropertySizeError; } while (0)

static CFPropertyListRef SonaCopySnapshot(CFTypeRef snapshot, CFTypeRef emptyValue) {
    std::lock_guard<std::mutex> lock(gSnapshotMutex);
    return CFRetain(snapshot ? snapshot : emptyValue);
}

static OSStatus Sona_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t,
                                     const AudioObjectPropertyAddress* inAddress,
                                     UInt32 inQualifierDataSize, const void* inQualifierData,
                                     UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    if (inDriver != gDriverRef || !inAddress || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    if (!Sona_HasProperty(inDriver, inObjectID, 0, inAddress)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = inAddress->mSelector;

    switch (inObjectID) {
        case kObjectID_PlugIn:
            switch (s) {
                case kAudioObjectPropertyBaseClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioPlugInClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyOwner: SONA_REQUIRE_SIZE(sizeof(AudioObjectID)); *(AudioObjectID*)outData = kAudioObjectUnknown; *outDataSize = sizeof(AudioObjectID); return noErr;
                case kAudioObjectPropertyManufacturer: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = SonaCFString(kSonaManufacturer); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioPlugInPropertyResourceBundle: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = CFSTR(""); CFRetain(*(CFStringRef*)outData); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList: {
                    UInt32 n = inDataSize / sizeof(AudioObjectID);
                    if (n >= 1) { ((AudioObjectID*)outData)[0] = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); } else *outDataSize = 0;
                    return noErr;
                }
                case kAudioPlugInPropertyTranslateUIDToDevice: {
                    SONA_REQUIRE_SIZE(sizeof(AudioObjectID));
                    if (inQualifierDataSize != sizeof(CFStringRef) || !inQualifierData) return kAudioHardwareBadPropertySizeError;
                    CFStringRef uid = *(const CFStringRef*)inQualifierData;
                    *(AudioObjectID*)outData = (uid && CFStringCompare(uid, CFSTR(kSonaDeviceUID), 0) == kCFCompareEqualTo) ? kObjectID_Device : kAudioObjectUnknown;
                    *outDataSize = sizeof(AudioObjectID);
                    return noErr;
                }
            }
            break;

        case kObjectID_Device:
            switch (s) {
                case kAudioObjectPropertyBaseClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioDeviceClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyOwner: SONA_REQUIRE_SIZE(sizeof(AudioObjectID)); *(AudioObjectID*)outData = kObjectID_PlugIn; *outDataSize = sizeof(AudioObjectID); return noErr;
                case kAudioObjectPropertyName: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = SonaCFString(kSonaDeviceName); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioObjectPropertyManufacturer: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = SonaCFString(kSonaManufacturer); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioDevicePropertyDeviceUID: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = SonaCFString(kSonaDeviceUID); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioDevicePropertyModelUID: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = SonaCFString(kSonaDeviceModelUID); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioObjectPropertyOwnedObjects: {
                    AudioObjectID ids[] = { kObjectID_Stream_Output, kObjectID_Volume_Output_Master, kObjectID_Mute_Output_Master };
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioObjectID), 3);
                    if (inAddress->mScope == kAudioObjectPropertyScopeInput) n = 0;
                    for (UInt32 i = 0; i < n; ++i) ((AudioObjectID*)outData)[i] = ids[i];
                    *outDataSize = n * sizeof(AudioObjectID);
                    return noErr;
                }
                case kAudioObjectPropertyControlList: {
                    AudioObjectID ids[] = { kObjectID_Volume_Output_Master, kObjectID_Mute_Output_Master };
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioObjectID), 2);
                    for (UInt32 i = 0; i < n; ++i) ((AudioObjectID*)outData)[i] = ids[i];
                    *outDataSize = n * sizeof(AudioObjectID);
                    return noErr;
                }
                case kAudioObjectPropertyCustomPropertyInfoList: {
                    AudioServerPlugInCustomPropertyInfo infos[4];
                    const AudioObjectPropertySelector sels[4] = { kSonaProperty_Config, kSonaProperty_Clients, kSonaProperty_Status, kSonaProperty_OutputLevel };
                    for (int i = 0; i < 4; ++i) {
                        infos[i].mSelector = sels[i];
                        infos[i].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
                        infos[i].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
                    }
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioServerPlugInCustomPropertyInfo), 4);
                    memcpy(outData, infos, n * sizeof(AudioServerPlugInCustomPropertyInfo));
                    *outDataSize = n * sizeof(AudioServerPlugInCustomPropertyInfo);
                    return noErr;
                }
                case kAudioDevicePropertyTransportType: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = kAudioDeviceTransportTypeVirtual; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyRelatedDevices: {
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioObjectID), 1);
                    if (n) ((AudioObjectID*)outData)[0] = kObjectID_Device;
                    *outDataSize = n * sizeof(AudioObjectID);
                    return noErr;
                }
                case kAudioDevicePropertyClockDomain: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyDeviceIsAlive: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyDeviceIsRunning: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = gIORunningCount.load() > 0 ? 1 : 0; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                    SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = inAddress->mScope == kAudioObjectPropertyScopeInput ? 0 : 1; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyLatency: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = kDevice_LatencyFrames; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertySafetyOffset: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyIsHidden: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyZeroTimeStampPeriod: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = kDevice_RingBufferSize; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioDevicePropertyStreams: {
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioObjectID), 1);
                    if (inAddress->mScope == kAudioObjectPropertyScopeInput) n = 0;
                    if (n) ((AudioObjectID*)outData)[0] = kObjectID_Stream_Output;
                    *outDataSize = n * sizeof(AudioObjectID);
                    return noErr;
                }
                case kAudioDevicePropertyNominalSampleRate: SONA_REQUIRE_SIZE(sizeof(Float64)); *(Float64*)outData = gSampleRate.load(); *outDataSize = sizeof(Float64); return noErr;
                case kAudioDevicePropertyAvailableNominalSampleRates: {
                    const UInt32 count = sizeof(kSupportedRates) / sizeof(kSupportedRates[0]);
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioValueRange), count);
                    for (UInt32 i = 0; i < n; ++i) { ((AudioValueRange*)outData)[i].mMinimum = kSupportedRates[i]; ((AudioValueRange*)outData)[i].mMaximum = kSupportedRates[i]; }
                    *outDataSize = n * sizeof(AudioValueRange);
                    return noErr;
                }
                case kAudioDevicePropertyPreferredChannelsForStereo: SONA_REQUIRE_SIZE(2 * sizeof(UInt32)); ((UInt32*)outData)[0] = 1; ((UInt32*)outData)[1] = 2; *outDataSize = 2 * sizeof(UInt32); return noErr;
                case kAudioDevicePropertyPreferredChannelLayout: {
                    UInt32 size = offsetof(AudioChannelLayout, mChannelDescriptions) + kDevice_Channels * sizeof(AudioChannelDescription);
                    SONA_REQUIRE_SIZE(size);
                    AudioChannelLayout* l = (AudioChannelLayout*)outData;
                    l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                    l->mChannelBitmap = 0;
                    l->mNumberChannelDescriptions = kDevice_Channels;
                    for (UInt32 i = 0; i < kDevice_Channels; ++i) {
                        l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Left + i;
                        l->mChannelDescriptions[i].mChannelFlags = 0;
                        l->mChannelDescriptions[i].mCoordinates[0] = l->mChannelDescriptions[i].mCoordinates[1] = l->mChannelDescriptions[i].mCoordinates[2] = 0;
                    }
                    *outDataSize = size;
                    return noErr;
                }
                case kSonaProperty_Config: {
                    SONA_REQUIRE_SIZE(sizeof(CFPropertyListRef));
                    CFDictionaryRef empty = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    *(CFPropertyListRef*)outData = SonaCopySnapshot(gConfig, empty);
                    CFRelease(empty);
                    *outDataSize = sizeof(CFPropertyListRef);
                    return noErr;
                }
                case kSonaProperty_OutputLevel:
                case kSonaProperty_Status: {
                    SONA_REQUIRE_SIZE(sizeof(CFPropertyListRef));
                    CFMutableDictionaryRef offline = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    CFDictionarySetValue(offline, CFSTR(kSonaStatusKey_Version), CFSTR(kSonaDriverVersion));
                    CFDictionarySetValue(offline, CFSTR("serviceConnected"), kCFBooleanFalse);
                    *(CFPropertyListRef*)outData = SonaCopySnapshot(gStatus, offline);
                    CFRelease(offline);
                    *outDataSize = sizeof(CFPropertyListRef);
                    return noErr;
                }
                case kSonaProperty_Clients: {
                    SONA_REQUIRE_SIZE(sizeof(CFPropertyListRef));
                    CFArrayRef empty = CFArrayCreate(kCFAllocatorDefault, nullptr, 0, &kCFTypeArrayCallBacks);
                    *(CFPropertyListRef*)outData = SonaCopySnapshot(gClientsList, empty);
                    CFRelease(empty);
                    *outDataSize = sizeof(CFPropertyListRef);
                    return noErr;
                }
            }
            break;

        case kObjectID_Stream_Output:
            switch (s) {
                case kAudioObjectPropertyBaseClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioStreamClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyOwner: SONA_REQUIRE_SIZE(sizeof(AudioObjectID)); *(AudioObjectID*)outData = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); return noErr;
                case kAudioObjectPropertyName: SONA_REQUIRE_SIZE(sizeof(CFStringRef)); *(CFStringRef*)outData = SonaCFString("Sona Output"); *outDataSize = sizeof(CFStringRef); return noErr;
                case kAudioStreamPropertyIsActive: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioStreamPropertyDirection: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioStreamPropertyTerminalType: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = kAudioStreamTerminalTypeSpeaker; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioStreamPropertyStartingChannel: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 1; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioStreamPropertyLatency: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = 0; *outDataSize = sizeof(UInt32); return noErr;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                    SONA_REQUIRE_SIZE(sizeof(AudioStreamBasicDescription));
                    SonaFillStreamFormat(*(AudioStreamBasicDescription*)outData, gSampleRate.load());
                    *outDataSize = sizeof(AudioStreamBasicDescription);
                    return noErr;
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats: {
                    const UInt32 count = sizeof(kSupportedRates) / sizeof(kSupportedRates[0]);
                    UInt32 n = std::min<UInt32>(inDataSize / sizeof(AudioStreamRangedDescription), count);
                    for (UInt32 i = 0; i < n; ++i) {
                        AudioStreamRangedDescription& r = ((AudioStreamRangedDescription*)outData)[i];
                        SonaFillStreamFormat(r.mFormat, kSupportedRates[i]);
                        r.mSampleRateRange.mMinimum = r.mSampleRateRange.mMaximum = kSupportedRates[i];
                    }
                    *outDataSize = n * sizeof(AudioStreamRangedDescription);
                    return noErr;
                }
            }
            break;

        case kObjectID_Volume_Output_Master:
            switch (s) {
                case kAudioObjectPropertyBaseClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioLevelControlClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioVolumeControlClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyOwner: SONA_REQUIRE_SIZE(sizeof(AudioObjectID)); *(AudioObjectID*)outData = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); return noErr;
                case kAudioControlPropertyScope: SONA_REQUIRE_SIZE(sizeof(AudioObjectPropertyScope)); *(AudioObjectPropertyScope*)outData = kAudioObjectPropertyScopeOutput; *outDataSize = sizeof(AudioObjectPropertyScope); return noErr;
                case kAudioControlPropertyElement: SONA_REQUIRE_SIZE(sizeof(AudioObjectPropertyElement)); *(AudioObjectPropertyElement*)outData = kAudioObjectPropertyElementMain; *outDataSize = sizeof(AudioObjectPropertyElement); return noErr;
                case kAudioLevelControlPropertyScalarValue: SONA_REQUIRE_SIZE(sizeof(Float32)); *(Float32*)outData = gMasterVolume.load(); *outDataSize = sizeof(Float32); return noErr;
                case kAudioLevelControlPropertyDecibelValue: SONA_REQUIRE_SIZE(sizeof(Float32)); *(Float32*)outData = SonaScalarToDB(gMasterVolume.load()); *outDataSize = sizeof(Float32); return noErr;
                case kAudioLevelControlPropertyDecibelRange: SONA_REQUIRE_SIZE(sizeof(AudioValueRange)); ((AudioValueRange*)outData)->mMinimum = kVolume_MinDB; ((AudioValueRange*)outData)->mMaximum = kVolume_MaxDB; *outDataSize = sizeof(AudioValueRange); return noErr;
                case kAudioLevelControlPropertyConvertScalarToDecibels: SONA_REQUIRE_SIZE(sizeof(Float32)); *(Float32*)outData = SonaScalarToDB(std::fmax(0.0f, std::fmin(1.0f, *(Float32*)outData))); *outDataSize = sizeof(Float32); return noErr;
                case kAudioLevelControlPropertyConvertDecibelsToScalar: SONA_REQUIRE_SIZE(sizeof(Float32)); *(Float32*)outData = SonaDBToScalar(*(Float32*)outData); *outDataSize = sizeof(Float32); return noErr;
            }
            break;

        case kObjectID_Mute_Output_Master:
            switch (s) {
                case kAudioObjectPropertyBaseClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioBooleanControlClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyClass: SONA_REQUIRE_SIZE(sizeof(AudioClassID)); *(AudioClassID*)outData = kAudioMuteControlClassID; *outDataSize = sizeof(AudioClassID); return noErr;
                case kAudioObjectPropertyOwner: SONA_REQUIRE_SIZE(sizeof(AudioObjectID)); *(AudioObjectID*)outData = kObjectID_Device; *outDataSize = sizeof(AudioObjectID); return noErr;
                case kAudioControlPropertyScope: SONA_REQUIRE_SIZE(sizeof(AudioObjectPropertyScope)); *(AudioObjectPropertyScope*)outData = kAudioObjectPropertyScopeOutput; *outDataSize = sizeof(AudioObjectPropertyScope); return noErr;
                case kAudioControlPropertyElement: SONA_REQUIRE_SIZE(sizeof(AudioObjectPropertyElement)); *(AudioObjectPropertyElement*)outData = kAudioObjectPropertyElementMain; *outDataSize = sizeof(AudioObjectPropertyElement); return noErr;
                case kAudioBooleanControlPropertyValue: SONA_REQUIRE_SIZE(sizeof(UInt32)); *(UInt32*)outData = gMasterMute.load() ? 1 : 0; *outDataSize = sizeof(UInt32); return noErr;
            }
            break;
    }
    return kAudioHardwareUnknownPropertyError;
}

// MARK: - SetPropertyData

static OSStatus Sona_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t,
                                     const AudioObjectPropertyAddress* inAddress,
                                     UInt32, const void*, UInt32 inDataSize, const void* inData) {
    if (inDriver != gDriverRef || !inAddress || !inData) return kAudioHardwareIllegalOperationError;
    if (!Sona_HasProperty(inDriver, inObjectID, 0, inAddress)) return kAudioHardwareUnknownPropertyError;
    const AudioObjectPropertySelector s = inAddress->mSelector;

    switch (inObjectID) {
        case kObjectID_Device:
            switch (s) {
                case kAudioDevicePropertyNominalSampleRate: {
                    SONA_REQUIRE_SIZE(sizeof(Float64));
                    Float64 rate = *(const Float64*)inData;
                    bool ok = false;
                    for (Float64 r : kSupportedRates) if (r == rate) ok = true;
                    if (!ok) return kAudioHardwareIllegalOperationError;
                    if (rate != gSampleRate.load())
                        dispatch_async(gConfigQueue, ^{ gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, nullptr); });
                    return noErr;
                }
                case kSonaProperty_OutputLevel: {
                    SONA_REQUIRE_SIZE(sizeof(CFPropertyListRef));
                    auto plist = *(const CFPropertyListRef*)inData;
                    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
                    auto dict = (CFDictionaryRef)plist;
                    auto uidValue = CFDictionaryGetValue(dict, CFSTR("uid"));
                    auto volumeValue = CFDictionaryGetValue(dict, CFSTR("volume"));
                    auto muteValue = CFDictionaryGetValue(dict, CFSTR("mute"));
                    if (!uidValue || CFGetTypeID(uidValue) != CFStringGetTypeID() || (!volumeValue && !muteValue)) return kAudioHardwareIllegalOperationError;
                    // Read as Float64: CFNumberGetValue reports a lossy Double->Float32 conversion as failure.
                    double volume = 1;
                    if (volumeValue && (CFGetTypeID(volumeValue) != CFNumberGetTypeID() ||
                        !CFNumberGetValue((CFNumberRef)volumeValue, kCFNumberFloat64Type, &volume) ||
                        !std::isfinite(volume) || volume < 0 || volume > 1)) return kAudioHardwareIllegalOperationError;
                    if (muteValue && CFGetTypeID(muteValue) != CFBooleanGetTypeID()) return kAudioHardwareIllegalOperationError;
                    char uid[512] = {0};
                    CFStringGetCString((CFStringRef)uidValue, uid, sizeof(uid), kCFStringEncodingUTF8);
                    if (uid[0] == 0 || strcmp(uid, kSonaDeviceUID) == 0) return kAudioHardwareIllegalOperationError;
                    xpc_object_t msg = xpc_dictionary_create(nullptr, nullptr, 0);
                    xpc_dictionary_set_string(msg, kSonaMsg_Op, kSonaOp_OutputLevel);
                    xpc_dictionary_set_string(msg, kSonaMsg_UID, uid);
                    if (volumeValue) xpc_dictionary_set_double(msg, kSonaMsg_Volume, volume);
                    if (muteValue) xpc_dictionary_set_bool(msg, kSonaMsg_Mute, CFBooleanGetValue((CFBooleanRef)muteValue));
                    dispatch_async(gConfigQueue, ^{ gTransport->sendControl(msg); });
                    return noErr;
                }
                case kSonaProperty_Config: {
                    SONA_REQUIRE_SIZE(sizeof(CFPropertyListRef));
                    CFPropertyListRef plist = *(const CFPropertyListRef*)inData;
                    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
                    CFDictionaryRef config = (CFDictionaryRef)CFRetain(plist);
                    dispatch_async(gConfigQueue, ^{
                        SonaReplaceSnapshot((CFTypeRef&)gConfig, CFRetain(config), CFDictionaryGetTypeID());
                        SonaSendPlist(kSonaOp_Config, config);
                        CFRelease(config);
                        SonaNotify(kObjectID_Device, kSonaProperty_Config);
                    });
                    return noErr;
                }
            }
            break;
        case kObjectID_Stream_Output:
            switch (s) {
                case kAudioStreamPropertyIsActive: return noErr;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat: {
                    SONA_REQUIRE_SIZE(sizeof(AudioStreamBasicDescription));
                    const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*)inData;
                    if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != kDevice_Channels ||
                        f->mBitsPerChannel != 32 || !(f->mFormatFlags & kAudioFormatFlagIsFloat)) return kAudioDeviceUnsupportedFormatError;
                    bool ok = false;
                    for (Float64 r : kSupportedRates) if (r == f->mSampleRate) ok = true;
                    if (!ok) return kAudioDeviceUnsupportedFormatError;
                    if (f->mSampleRate != gSampleRate.load()) {
                        Float64 rate = f->mSampleRate;
                        dispatch_async(gConfigQueue, ^{ gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, nullptr); });
                    }
                    return noErr;
                }
            }
            break;
        case kObjectID_Volume_Output_Master:
            switch (s) {
                case kAudioLevelControlPropertyScalarValue:
                case kAudioLevelControlPropertyDecibelValue: {
                    SONA_REQUIRE_SIZE(sizeof(Float32));
                    Float32 v = s == kAudioLevelControlPropertyScalarValue ? std::fmax(0.0f, std::fmin(1.0f, *(const Float32*)inData))
                                                                            : SonaDBToScalar(*(const Float32*)inData);
                    // Optimistic cache keeps key repeats responsive; the service reports the actual level.
                    SonaPublishVolume(v);
                    dispatch_async(gConfigQueue, ^{ SonaSendMaster(true, v, false, false); });
                    return noErr;
                }
            }
            break;
        case kObjectID_Mute_Output_Master:
            if (s == kAudioBooleanControlPropertyValue) {
                SONA_REQUIRE_SIZE(sizeof(UInt32));
                bool m = *(const UInt32*)inData != 0;
                SonaPublishMute(m);
                dispatch_async(gConfigQueue, ^{ SonaSendMaster(false, 0, true, m); });
                return noErr;
            }
            break;
    }
    return kAudioHardwareUnknownPropertyError;
}

// MARK: - IO

// Zero timestamps, Sona IO thread only. While the service's master output publishes anchors
// into the shared region, each zero timestamp is placed where that device's sample counter says
// it belongs (rate-locked). Otherwise the schedule free-runs on host time from the last emitted
// timestamp, so the timeline never jumps. The anchor read is a bounded seqlock read; the service
// being offline just means no anchor.
static void SonaComputeZeroTimeStamp(UInt64 now, const SonaClockAnchor& anchor, UInt64 regionGeneration,
                                     Float64* outSampleTime, UInt64* outHostTime, bool* outLocked) {
    const Float64 ticksPerRing = gHostTicksPerFrame * (Float64)kDevice_RingBufferSize;
    const Float64 sonaRate = gSampleRate.load(std::memory_order_relaxed);
    SonaClockLock& lock = gClockLock;
    const UInt64 second = (UInt64)gHostTicksPerSecond;
    const bool anchorUsable = anchor.valid() && anchor.hostTime + second > now && anchor.hostTime < now + second;

    if (lock.lastHostTime == 0) { lock.lastHostTime = gAnchorHostTime; lock.locked = false; }
    if (lock.locked && lock.region != regionGeneration) { lock.locked = false; lock.generation = 0; }
    if (!anchorUsable || (lock.locked && lock.sonaRate != sonaRate)) {
        lock.locked = false;
        lock.generation = 0;
    } else if (!lock.locked || lock.generation != anchor.generation) {
        Float64 deviceAtLast = anchor.sampleTime + ((Float64)lock.lastHostTime - (Float64)anchor.hostTime) * anchor.rate / gHostTicksPerSecond;
        lock.locked = true;
        lock.region = regionGeneration;
        lock.generation = anchor.generation;
        lock.sonaRate = sonaRate;
        lock.sonaSample0 = (Float64)(gNumberTimeStamps * kDevice_RingBufferSize);
        lock.deviceSample0 = deviceAtLast;
    }

    Float64 period = ticksPerRing;
    if (lock.locked) {
        Float64 nextSona = (Float64)((gNumberTimeStamps + 1) * kDevice_RingBufferSize);
        Float64 nextDevice = lock.deviceSample0 + (nextSona - lock.sonaSample0) * (anchor.rate / sonaRate);
        Float64 nextHost = (Float64)anchor.hostTime + (nextDevice - anchor.sampleTime) * gHostTicksPerSecond / anchor.rate;
        Float64 wanted = nextHost - (Float64)lock.lastHostTime;
        period = std::fmax(ticksPerRing * (1 - kClockSlewLimit), std::fmin(ticksPerRing * (1 + kClockSlewLimit), wanted));
    }
    UInt64 next = lock.lastHostTime + (UInt64)period;
    if (next <= now) { ++gNumberTimeStamps; lock.lastHostTime = next; }
    *outSampleTime = (Float64)(gNumberTimeStamps * kDevice_RingBufferSize);
    *outHostTime = lock.lastHostTime;
    *outLocked = lock.locked;
}

static OSStatus Sona_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (gIORunningCount.fetch_add(1) == 0) {
        gAnchorHostTime = mach_absolute_time();
        gNumberTimeStamps = 0;
        gClockLock = SonaClockLock{};
    }
    dispatch_async(gConfigQueue, ^{ if (gTransport) gTransport->setIORunning(true); });
    return noErr;
}

static OSStatus Sona_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    UInt64 prev = gIORunningCount.load();
    while (prev > 0 && !gIORunningCount.compare_exchange_weak(prev, prev - 1)) {}
    dispatch_async(gConfigQueue, ^{ if (gTransport) gTransport->setIORunning(gIORunningCount.load(std::memory_order_acquire) > 0); });
    return noErr;
}

static OSStatus Sona_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32,
                                      Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    SonaClockAnchor anchor;
    bool locked = false;
    if (gTransport) {
        if (auto guard = gTransport->acquire()) {
            SonaTransportClock* cell = SonaTransportClockCell(guard.mapping->base);
            anchor = SonaClockRead(cell);
            SonaComputeZeroTimeStamp(mach_absolute_time(), anchor, guard.mapping->generation, outSampleTime, outHostTime, &locked);
            cell->driverLocked.store(locked ? 1 : 0, std::memory_order_relaxed);
            *outSeed = 1;
            return noErr;
        }
    }
    SonaComputeZeroTimeStamp(mach_absolute_time(), anchor, 0, outSampleTime, outHostTime, &locked);
    *outSeed = 1;
    return noErr;
}

static OSStatus Sona_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32,
                                       UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (outWillDo) *outWillDo = inOperationID == kAudioServerPlugInIOOperationProcessOutput;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return noErr;
}

static OSStatus Sona_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32, UInt32, UInt32,
                                      const AudioServerPlugInIOCycleInfo*) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return noErr;
}

static OSStatus Sona_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID,
                                   UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                   const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void*) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (inStreamObjectID != kObjectID_Stream_Output || !ioMainBuffer) return noErr;
    if (inOperationID != kAudioServerPlugInIOOperationProcessOutput) return noErr;
    const UInt32 frames = std::min<UInt32>(inIOBufferFrameSize, kMaxFramesPerCycle);
    float* buf = (float*)ioMainBuffer;

    // Tee this client's audio to Sona Audio Service. Service offline or ring full: drop and
    // return; never wait here. The host mix is silenced either way because nothing renders it.
    ClientSlot* c = SonaFindClient(inClientID);
    if (c && gTransport) {
        const uint32_t slot = (uint32_t)(c - gClients);
        const uint32_t instance = gTransport->instanceForSlot(slot);
        if (instance != 0) {
            if (auto guard = gTransport->acquire()) {
                SonaTransportBlock block = {};
                block.frames = frames;
                block.formatGeneration = gTransport->formatGeneration();
                block.instance = instance;
                if (inIOCycleInfo) {
                    block.cycle = inIOCycleInfo->mIOCycleCounter;
                    const AudioTimeStamp& t = inIOCycleInfo->mOutputTime;
                    if (t.mFlags & kAudioTimeStampSampleTimeValid) {
                        block.sampleTime = t.mSampleTime;
                        block.hostTime = (t.mFlags & kAudioTimeStampHostTimeValid) ? t.mHostTime : 0;
                        block.flags |= kSonaBlockFlag_TimestampValid;
                    }
                }
                SonaTransportRingWrite(guard.ring(slot), buf, block);
            }
        }
    }
    memset(buf, 0, frames * kDevice_Channels * sizeof(float));
    return noErr;
}

static OSStatus Sona_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32, UInt32, UInt32,
                                    const AudioServerPlugInIOCycleInfo*) {
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return noErr;
}

// MARK: - Factory

AudioServerPlugInDriverInterface gDriverInterface = {
    nullptr,
    Sona_QueryInterface, Sona_AddRef, Sona_Release,
    Sona_Initialize, Sona_CreateDevice, Sona_DestroyDevice,
    Sona_AddDeviceClient, Sona_RemoveDeviceClient,
    Sona_PerformDeviceConfigurationChange, Sona_AbortDeviceConfigurationChange,
    Sona_HasProperty, Sona_IsPropertySettable, Sona_GetPropertyDataSize, Sona_GetPropertyData, Sona_SetPropertyData,
    Sona_StartIO, Sona_StopIO, Sona_GetZeroTimeStamp,
    Sona_WillDoIOOperation, Sona_BeginIOOperation, Sona_DoIOOperation, Sona_EndIOOperation,
};

extern "C" __attribute__((visibility("default")))
void* SonaDriver_Create(CFAllocatorRef, CFUUIDRef inRequestedTypeUUID) {
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        gRefCount.fetch_add(1);
        return gDriverRef;
    }
    return nullptr;
}
