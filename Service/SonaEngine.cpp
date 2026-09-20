//
//  SonaEngine.cpp
//  Ported from the in-process prototype in Driver/SonaDriver.cpp. The virtual device no
//  longer mixes: it tees each client's PCM into a shared ring and this engine does the rest.
//

#include "SonaEngine.h"
#include "SonaTarget.h"
#include "SonaHardwareControls.h"
#include "../Shared/SonaProtocol.h"
#include "../Shared/SonaTransport.h"

#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <os/log.h>
#include <atomic>
#include <climits>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern os_log_t gSonaServiceLog;
#define SonaLog(fmt, ...) os_log(gSonaServiceLog ? gSonaServiceLog : OS_LOG_DEFAULT, "SonaService: " fmt, ##__VA_ARGS__)

// MARK: - State

static const UInt32 kMaxClients = kSonaTransportMaxClients;
static const UInt32 kMaxTargets = 8;

struct ClientSlot {
    std::atomic<bool>     used{false};
    std::atomic<uint32_t> instance{0};
    std::atomic<float>    gain{1.0f};
    std::atomic<UInt32>   routeMask{0};      // 0 = follow default target
    std::atomic<UInt64>   lastSignalHostTime{0};
    std::atomic<bool>     played{false};
    // Control queue only.
    bool        notifiedAudible = false;
    UInt32      clientID = 0;
    pid_t       pid = 0;
    std::string bundleID;
    std::string key;
};

struct AppSetting {
    float gain = 1.0f;
    bool  mute = false;
    std::vector<std::string> targets;
};

static dispatch_queue_t          gConfigQueue = nullptr;
static SonaEnginePushes          gPushes;
static std::string               gStateFile;
static dispatch_source_t         gActivityTimer = nullptr;
static bool                      gActivityTimerEnabled = false;
static UInt64                    gActivityWindowTicks = 0;
static Float64                   gHostTicksPerSecond = 0.0;
static std::recursive_mutex      gStateMutex;

static std::atomic<Float64>      gSampleRate{48000.0};
static std::atomic<bool>         gIORunning{false};

static std::atomic<float>        gMasterVolume{1.0f};
static std::atomic<bool>         gMasterMute{false};
static std::atomic<bool>         gHardwareVolume{false};
static std::atomic<bool>         gHardwareMute{false};
static SonaHardwareControls      gHardwareControls;
static std::atomic<UInt32>       gPendingHardwareWrites{0};
static std::atomic<UInt64>       gHardwareGeneration{0};

static ClientSlot                gClients[kMaxClients];
static SonaTarget                gTargets[kMaxTargets];
struct OutputControl {
    std::string uid;
    SonaHardwareControls hardware;
    std::atomic<float> volume{1}, gain{1};
    std::atomic<bool> mute{false};
};
static OutputControl gOutputControls[kMaxTargets];
struct OutputLevel { float volume = 1; bool mute = false; };
static std::unordered_map<std::string, OutputLevel> gOutputLevels;
static std::string gBoundDefaultUID;

static std::atomic<UInt32>       gDefaultMask{0};
static std::atomic<UInt32>       gClockMasterSlot{UINT32_MAX};
static std::string               gDefaultTargetUID;
static std::unordered_map<std::string, AppSetting> gAppSettings;
static CFDictionaryRef           gConfig = nullptr;
static bool                      gTargetsReconciled = false;
static bool                      gHALClientReady = false;

// Shared region handed over by the transport layer.
static std::atomic<void*>        gRegion{nullptr};
static std::atomic<uint64_t>     gMixerPass{0};          // odd while the mixer thread is inside a pass
static std::atomic<uint32_t>     gMixResetRequested{0};  // control queue -> mixer thread
static std::atomic<uint32_t>     gFormatGeneration{0};
static_assert(SonaEngineRetireTicket::kSinks == kMaxTargets, "retire ticket covers every sink");

static void SonaRefreshOutputControls(AudioObjectID device);
static void SonaSaveState();
static void SonaScheduleSave();
static void SonaPushStatus();
static void SonaPushClients();
static void SonaMixResetAnchor();
static std::atomic<uint64_t> gMixLateBlocks{0}, gMixResets{0};
static std::atomic<int64_t>  gMixLagFrames{0};
static std::atomic<int64_t>  gMixMarginTicks{0};

// MARK: - Helpers

static CFStringRef SonaCFString(const char* s) { return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8); }
static std::string SonaStdString(CFStringRef s) {
    if (!s) return "";
    char buf[1024] = {0};
    return CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8) ? buf : "";
}

static inline float SonaMasterGain() {
    if (!gHardwareMute.load(std::memory_order_relaxed) && gMasterMute.load(std::memory_order_relaxed)) return 0.0f;
    if (gHardwareVolume.load(std::memory_order_relaxed)) return 1.0f;
    float s = gMasterVolume.load(std::memory_order_relaxed);
    return s * s * s;
}

static void SonaPushMaster() {
    if (gPushes.master) gPushes.master(gPushes.ctx, gMasterVolume.load(), gMasterMute.load());
}
static void SonaPublishVolume(Float32 value) { if (value != gMasterVolume.exchange(value)) SonaPushMaster(); }
static void SonaPublishMute(bool value) { if (value != gMasterMute.exchange(value)) SonaPushMaster(); }

// MARK: - Hardware controls (control queue)

static void SonaRefreshHardwareControls(bool targetChanged = false) {
    if (!targetChanged && gPendingHardwareWrites.load() != 0) return;
    Float32 volume; bool mute;
    if (gHardwareControls.volume(volume)) SonaPublishVolume(volume);
    if (gHardwareControls.muted(mute)) SonaPublishMute(mute);
}

static OSStatus SonaHardwareListener(AudioObjectID device, UInt32, const AudioObjectPropertyAddress*, void*) {
    dispatch_async(gConfigQueue, ^{
        if (device == gHardwareControls.device) SonaRefreshHardwareControls();
        SonaRefreshOutputControls(device);
        SonaPushStatus();
    });
    return noErr;
}

static void SonaBindHardwareControls(const std::string& uid) {
    AudioObjectID device = uid == kSonaDeviceUID ? kAudioObjectUnknown : SonaTranslateUID(uid);
    if (device == gHardwareControls.device && uid == gBoundDefaultUID) { SonaRefreshHardwareControls(); return; }
    if (!gBoundDefaultUID.empty()) gOutputLevels[gBoundDefaultUID] = {gMasterVolume.load(), gMasterMute.load()};
    gBoundDefaultUID = uid;
    gHardwareGeneration.fetch_add(1);
    for (auto selector : {kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute}) {
        auto addr = SonaHardwareControls::address(selector, kAudioObjectPropertyElementWildcard);
        if (gHardwareControls.device != kAudioObjectUnknown)
            AudioObjectRemovePropertyListener(gHardwareControls.device, &addr, SonaHardwareListener, nullptr);
    }
    gHardwareControls.bind(device);
    gHardwareVolume.store(!gHardwareControls.volumeElements.empty());
    gHardwareMute.store(!gHardwareControls.muteElements.empty());
    auto saved = gOutputLevels[uid];
    SonaPublishVolume(saved.volume);
    SonaPublishMute(saved.mute);
    for (auto selector : {kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute}) {
        auto addr = SonaHardwareControls::address(selector, kAudioObjectPropertyElementWildcard);
        if (device != kAudioObjectUnknown) AudioObjectAddPropertyListener(device, &addr, SonaHardwareListener, nullptr);
    }
    SonaRefreshHardwareControls(true);
}

void SonaEngineSetMasterVolume(Float32 value) {
    value = std::fmax(0.0f, std::fmin(1.0f, value));
    UInt64 generation = gHardwareGeneration.load();
    gPendingHardwareWrites.fetch_add(1);
    SonaPublishVolume(value);
    dispatch_async(gConfigQueue, ^{
        if (generation == gHardwareGeneration.load() && gHardwareVolume.load()) {
            if (!gHardwareControls.setVolume(value)) SonaLog("hardware volume write failed");
        }
        gPendingHardwareWrites.fetch_sub(1);
        SonaRefreshHardwareControls();
        SonaScheduleSave();
        SonaPushStatus();
    });
}

void SonaEngineSetMasterMute(bool value) {
    UInt64 generation = gHardwareGeneration.load();
    gPendingHardwareWrites.fetch_add(1);
    SonaPublishMute(value);
    dispatch_async(gConfigQueue, ^{
        if (generation == gHardwareGeneration.load() && gHardwareMute.load()) {
            if (!gHardwareControls.setMuted(value)) SonaLog("hardware mute write failed");
        }
        gPendingHardwareWrites.fetch_sub(1);
        SonaRefreshHardwareControls();
        SonaScheduleSave();
        SonaPushStatus();
    });
}

void SonaEngineMasterLevel(float& volume, bool& mute) { volume = gMasterVolume.load(); mute = gMasterMute.load(); }

static void SonaRefreshOutputControl(OutputControl& control) {
    float volume = control.volume.load();
    bool mute = control.mute.load();
    Float32 hardwareVolume; bool hardwareMute;
    if (control.hardware.volume(hardwareVolume)) volume = hardwareVolume;
    if (control.hardware.muted(hardwareMute)) mute = hardwareMute;
    control.volume.store(volume);
    control.mute.store(mute);
    const float scalar = control.hardware.volumeElements.empty() ? volume : 1.0f;
    control.gain.store(control.hardware.muteElements.empty() && mute ? 0.0f : scalar * scalar * scalar);
    if (!control.uid.empty()) gOutputLevels[control.uid] = {volume, mute};
}

static void SonaRefreshOutputControls(AudioObjectID device) {
    for (auto& control : gOutputControls)
        if (!control.uid.empty() && control.hardware.device == device) SonaRefreshOutputControl(control);
}

static void SonaBindOutputControls() {
    for (auto& control : gOutputControls) {
        if (!control.uid.empty()) gOutputLevels[control.uid] = {control.volume.load(), control.mute.load()};
        for (auto selector : {kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute}) {
            auto addr = SonaHardwareControls::address(selector, kAudioObjectPropertyElementWildcard);
            if (control.hardware.device != kAudioObjectUnknown)
                AudioObjectRemovePropertyListener(control.hardware.device, &addr, SonaHardwareListener, nullptr);
        }
        control.uid.clear();
        control.hardware.bind(kAudioObjectUnknown);
    }
    SonaBindHardwareControls(gDefaultTargetUID);
    for (UInt32 i = 0; i < kMaxTargets; ++i) {
        const auto& uid = gTargets[i].uid;
        if (uid.empty() || uid == gDefaultTargetUID || uid == kSonaDeviceUID) continue;
        auto& control = gOutputControls[i];
        control.uid = uid;
        auto saved = gOutputLevels[uid];
        control.volume.store(saved.volume);
        control.mute.store(saved.mute);
        control.hardware.bind(SonaTranslateUID(uid));
        for (auto selector : {kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyMute}) {
            auto addr = SonaHardwareControls::address(selector, kAudioObjectPropertyElementWildcard);
            if (control.hardware.device != kAudioObjectUnknown)
                AudioObjectAddPropertyListener(control.hardware.device, &addr, SonaHardwareListener, nullptr);
        }
        SonaRefreshOutputControl(control);
    }
}

static void SonaSetOutputLevel(const std::string& uid, bool hasVolume, float volume, bool hasMute, bool mute) {
    if (uid == gDefaultTargetUID) {
        if (hasVolume) { if (gHardwareVolume.load()) gHardwareControls.setVolume(volume); else SonaPublishVolume(volume); }
        if (hasMute)   { if (gHardwareMute.load()) gHardwareControls.setMuted(mute); else SonaPublishMute(mute); }
        SonaRefreshHardwareControls(true);
    } else {
        for (auto& control : gOutputControls) {
            if (control.uid != uid) continue;
            if (hasVolume) {
                if (control.hardware.volumeElements.empty()) control.volume.store(volume);
                else if (!control.hardware.setVolume(volume)) SonaLog("output volume write failed");
            }
            if (hasMute) {
                if (control.hardware.muteElements.empty()) control.mute.store(mute);
                else if (!control.hardware.setMuted(mute)) SonaLog("output mute write failed");
            }
            SonaRefreshOutputControl(control);
            break;
        }
    }
    SonaScheduleSave();
    SonaPushStatus();
}

struct PendingOutputLevel { bool hasVolume = false, hasMute = false; float volume = 1; bool mute = false; };
static std::mutex gOutputCommandMutex;
static std::unordered_map<std::string, PendingOutputLevel> gOutputCommands;
static bool gOutputCommandScheduled = false;

// A slow Bluetooth/USB control must not build up seconds of obsolete slider positions.
void SonaEngineSetOutputLevel(const std::string& uid, bool hasVolume, float volume, bool hasMute, bool mute) {
    if (uid.empty() || uid == kSonaDeviceUID || !std::isfinite(volume) || volume < 0 || volume > 1) return;
    std::lock_guard<std::mutex> lock(gOutputCommandMutex);
    auto& command = gOutputCommands[uid];
    if (hasVolume) { command.hasVolume = true; command.volume = volume; }
    if (hasMute) { command.hasMute = true; command.mute = mute; }
    if (gOutputCommandScheduled) return;
    gOutputCommandScheduled = true;
    dispatch_async(gConfigQueue, ^{
        std::unordered_map<std::string, PendingOutputLevel> batch;
        {
            std::lock_guard<std::mutex> lock(gOutputCommandMutex);
            batch.swap(gOutputCommands);
            gOutputCommandScheduled = false;
        }
        for (const auto& entry : batch) {
            const auto& c = entry.second;
            SonaSetOutputLevel(entry.first, c.hasVolume, c.volume, c.hasMute, c.mute);
        }
    });
}

// MARK: - Persistence (control queue)

static void SonaSaveState() {
    if (!gBoundDefaultUID.empty()) gOutputLevels[gBoundDefaultUID] = {gMasterVolume.load(), gMasterMute.load()};
    if (gStateFile.empty()) return;
    auto levels = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (const auto& entry : gOutputLevels) {
        auto key = SonaCFString(entry.first.c_str());
        auto value = CFNumberCreate(nullptr, kCFNumberFloat32Type, &entry.second.volume);
        const void* keys[] = {CFSTR("volume"), CFSTR("mute")};
        const void* values[] = {value, entry.second.mute ? kCFBooleanTrue : kCFBooleanFalse};
        auto level = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(levels, key, level);
        CFRelease(key); CFRelease(value); CFRelease(level);
    }
    auto root = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(root, CFSTR("outputLevels"), levels);
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        if (gConfig) CFDictionarySetValue(root, CFSTR("config"), gConfig);
    }
    CFRelease(levels);
    CFDataRef data = CFPropertyListCreateData(nullptr, root, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
    CFRelease(root);
    if (!data) return;
    std::string tmp = gStateFile + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f) {
        fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), f);
        fclose(f);
        rename(tmp.c_str(), gStateFile.c_str());
    } else {
        SonaLog("cannot write state file %{public}s", tmp.c_str());
    }
    CFRelease(data);
}

static void SonaScheduleSave() {
    if (!gBoundDefaultUID.empty()) gOutputLevels[gBoundDefaultUID] = {gMasterVolume.load(), gMasterMute.load()};
    static UInt64 generation = 0;
    UInt64 requested = ++generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), gConfigQueue, ^{
        if (requested == generation) SonaSaveState();
    });
}

static CFDictionaryRef SonaLoadState() {
    if (gStateFile.empty()) return nullptr;
    FILE* f = fopen(gStateFile.c_str(), "rb");
    if (!f) return nullptr;
    std::vector<UInt8> bytes;
    UInt8 buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    fclose(f);
    CFDataRef data = CFDataCreate(nullptr, bytes.data(), (CFIndex)bytes.size());
    CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (plist && CFGetTypeID(plist) != CFDictionaryGetTypeID()) { CFRelease(plist); return nullptr; }
    return (CFDictionaryRef)plist;
}

static void SonaRestoreOutputLevels(CFDictionaryRef state) {
    auto stored = state ? CFDictionaryGetValue(state, CFSTR("outputLevels")) : nullptr;
    if (!stored || CFGetTypeID(stored) != CFDictionaryGetTypeID()) return;
    auto dict = (CFDictionaryRef)stored;
    CFIndex count = CFDictionaryGetCount(dict);
    std::vector<const void*> keys(count), values(count);
    CFDictionaryGetKeysAndValues(dict, keys.data(), values.data());
    for (CFIndex i = 0; i < count; ++i) {
        if (CFGetTypeID(keys[i]) != CFStringGetTypeID() || CFGetTypeID(values[i]) != CFDictionaryGetTypeID()) continue;
        auto volume = CFDictionaryGetValue((CFDictionaryRef)values[i], CFSTR("volume"));
        auto mute = CFDictionaryGetValue((CFDictionaryRef)values[i], CFSTR("mute"));
        OutputLevel level;
        if (!volume || CFGetTypeID(volume) != CFNumberGetTypeID() ||
            !CFNumberGetValue((CFNumberRef)volume, kCFNumberFloat32Type, &level.volume) ||
            !std::isfinite(level.volume) || level.volume < 0 || level.volume > 1) continue;
        level.mute = mute && CFGetTypeID(mute) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)mute);
        gOutputLevels[SonaStdString((CFStringRef)keys[i])] = level;
    }
}

// MARK: - Routing (control queue)

static int SonaTargetSlotForUID(const std::string& uid) {
    if (uid.empty()) return -1;
    for (UInt32 i = 0; i < kMaxTargets; ++i) if (gTargets[i].uid == uid) return (int)i;
    return -1;
}

static const AppSetting* SonaSettingForClient(const ClientSlot& c, const std::unordered_map<std::string, AppSetting>& settings) {
    auto it = settings.find("pid:" + std::to_string(c.pid));
    if (it == settings.end()) it = settings.find(c.key);
    return it == settings.end() ? nullptr : &it->second;
}

static void SonaApplySettingsToClient(ClientSlot& c, bool updateRoute = true) {
    const auto* setting = SonaSettingForClient(c, gAppSettings);
    c.gain.store(setting ? (setting->mute ? 0.0f : setting->gain) : 1.0f, std::memory_order_relaxed);
    if (!updateRoute) return;
    UInt32 mask = 0;
    if (setting) for (const std::string& uid : setting->targets) {
        int slot = SonaTargetSlotForUID(uid);
        if (slot >= 0) mask |= (1u << slot);
    }
    c.routeMask.store(mask, std::memory_order_relaxed);
}

static bool SonaRoutesChanged(const std::unordered_map<std::string, AppSetting>& before,
                              const std::unordered_map<std::string, AppSetting>& after) {
    auto differs = [](const auto& a, const auto& b) {
        for (const auto& entry : a) {
            if (entry.second.targets.empty()) continue;
            auto it = b.find(entry.first);
            if (it == b.end() || it->second.targets != entry.second.targets) return true;
        }
        return false;
    };
    if (differs(before, after) || differs(after, before)) return true;
    const std::vector<std::string> empty;
    for (const auto& client : gClients) {
        if (!client.used.load(std::memory_order_acquire)) continue;
        auto* oldSetting = SonaSettingForClient(client, before);
        auto* newSetting = SonaSettingForClient(client, after);
        if ((oldSetting ? oldSetting->targets : empty) != (newSetting ? newSetting->targets : empty)) return true;
    }
    return false;
}

static std::string SonaConfiguredDefault(CFDictionaryRef config) {
    if (!config) return "";
    auto value = CFDictionaryGetValue(config, CFSTR(kSonaConfigKey_DefaultTarget));
    return value && CFGetTypeID(value) == CFStringGetTypeID() ? SonaStdString((CFStringRef)value) : "";
}

static UInt64 gTargetStopGeneration[kMaxTargets] = {};
static bool gTargetStopPending[kMaxTargets] = {};
static constexpr int64_t kTargetIdleDelay = 2 * NSEC_PER_SEC;

// Demand: every registered client while the virtual device runs. The host does not tell us
// which clients are producing, so all routes of registered clients stay open while IO runs.
static UInt32 SonaNeededTargetMask() {
    if (!gIORunning.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::recursive_mutex> lock(gStateMutex);
    UInt32 needed = gDefaultMask.load(std::memory_order_acquire);
    for (auto& client : gClients) {
        if (!client.used.load(std::memory_order_acquire)) continue;
        needed |= client.routeMask.load(std::memory_order_relaxed);
    }
    return needed;
}

static void SonaFinishTargetStop(UInt32 slot, UInt64 generation) {
    if (!gTargetStopPending[slot] || gTargetStopGeneration[slot] != generation) return;
    gTargetStopPending[slot] = false;
    if (!(SonaNeededTargetMask() & (1u << slot))) { gTargets[slot].stop(); SonaPushStatus(); }
}

static void SonaStartTargetsIfNeeded() {
    UInt32 needed = SonaNeededTargetMask();
    for (UInt32 i = 0; i < kMaxTargets; ++i) {
        SonaTarget& t = gTargets[i];
        if (t.uid.empty() || !t.procID || (needed & (1u << i))) {
            ++gTargetStopGeneration[i];
            gTargetStopPending[i] = false;
            if (!t.uid.empty() && t.procID && (needed & (1u << i))) t.start();
        } else if (t.ioStarted && !gTargetStopPending[i]) {
            gTargetStopPending[i] = true;
            UInt64 generation = ++gTargetStopGeneration[i];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kTargetIdleDelay), gConfigQueue, ^{ SonaFinishTargetStop(i, generation); });
        }
    }
}

static void SonaReconcileTargets() {
    if (!gHALClientReady) return;
    std::vector<int> toClose, toOpen;
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        if (gDefaultTargetUID.empty()) {
            AudioObjectID builtIn = SonaFindBuiltInOutputDevice();
            if (builtIn != kAudioObjectUnknown) gDefaultTargetUID = SonaCopyDeviceUID(builtIn);
            if (!gDefaultTargetUID.empty()) SonaLog("no default target configured, using built-in '%{public}s'", gDefaultTargetUID.c_str());
        }
        std::vector<std::string> desired;
        auto addDesired = [&](const std::string& uid) {
            if (uid.empty() || uid == kSonaDeviceUID) return;
            for (auto& d : desired) if (d == uid) return;
            if (desired.size() < kMaxTargets) desired.push_back(uid);
        };
        addDesired(gDefaultTargetUID);
        for (auto& kv : gAppSettings) for (auto& uid : kv.second.targets) addDesired(uid);
        for (UInt32 i = 0; i < kMaxTargets; ++i) {
            SonaTarget& t = gTargets[i];
            if (t.uid.empty()) continue;
            bool wanted = false;
            for (auto& d : desired) if (d == t.uid) { wanted = true; break; }
            if (!wanted) { toClose.push_back((int)i); t.uid.clear(); }
        }
        for (auto& uid : desired) {
            int slot = SonaTargetSlotForUID(uid);
            if (slot < 0) for (UInt32 i = 0; i < kMaxTargets; ++i) if (gTargets[i].uid.empty()) { slot = (int)i; gTargets[i].uid = uid; break; }
            if (slot >= 0) toOpen.push_back(slot);
        }
    }
    for (int i : toClose) gTargets[i].close();
    for (int i : toOpen) {
        SonaTarget& t = gTargets[i];
        t.sourceRate.store(gSampleRate.load(), std::memory_order_relaxed);
        if (t.procID && !t.isDeviceAlive()) { SonaLog("target '%{public}s' disappeared", t.uid.c_str()); t.close(); }
        if (!t.procID) t.open();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        int defSlot = SonaTargetSlotForUID(gDefaultTargetUID);
        gDefaultMask.store(defSlot >= 0 ? (1u << defSlot) : 0, std::memory_order_release);
        for (UInt32 i = 0; i < kMaxTargets; ++i) gTargets[i].clockMaster.store((int)i == defSlot, std::memory_order_relaxed);
        gClockMasterSlot.store(defSlot >= 0 ? (UInt32)defSlot : UINT32_MAX, std::memory_order_release);
        for (UInt32 i = 0; i < kMaxClients; ++i)
            if (gClients[i].used.load(std::memory_order_acquire)) SonaApplySettingsToClient(gClients[i]);
    }
    gTargetsReconciled = true;
    SonaBindOutputControls();
    SonaStartTargetsIfNeeded();
    SonaPushStatus();
}

static void SonaApplyConfig(CFDictionaryRef config, bool persist) {
    bool routesChanged;
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        auto previousSettings = std::move(gAppSettings);
        gAppSettings.clear();
        auto requestedDefault = SonaConfiguredDefault(config);
        routesChanged = !gConfig || requestedDefault != SonaConfiguredDefault(gConfig);
        if (routesChanged) gDefaultTargetUID = requestedDefault;
        if (config) {
            CFDictionaryRef apps = (CFDictionaryRef)CFDictionaryGetValue(config, CFSTR(kSonaConfigKey_Apps));
            if (apps && CFGetTypeID(apps) == CFDictionaryGetTypeID()) {
                CFIndex n = CFDictionaryGetCount(apps);
                std::vector<const void*> keys(n), values(n);
                CFDictionaryGetKeysAndValues(apps, keys.data(), values.data());
                for (CFIndex i = 0; i < n; ++i) {
                    CFStringRef key = (CFStringRef)keys[i];
                    CFDictionaryRef d = (CFDictionaryRef)values[i];
                    if (CFGetTypeID(key) != CFStringGetTypeID() || CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;
                    AppSetting s;
                    CFNumberRef vol = (CFNumberRef)CFDictionaryGetValue(d, CFSTR(kSonaAppKey_Volume));
                    if (vol && CFGetTypeID(vol) == CFNumberGetTypeID()) {
                        double v = 1.0; CFNumberGetValue(vol, kCFNumberDoubleType, &v);
                        s.gain = (float)std::fmax(0.0, std::fmin(4.0, v));
                    }
                    CFBooleanRef mute = (CFBooleanRef)CFDictionaryGetValue(d, CFSTR(kSonaAppKey_Mute));
                    if (mute && CFGetTypeID(mute) == CFBooleanGetTypeID()) s.mute = CFBooleanGetValue(mute);
                    CFArrayRef targets = (CFArrayRef)CFDictionaryGetValue(d, CFSTR(kSonaAppKey_Targets));
                    if (targets && CFGetTypeID(targets) == CFArrayGetTypeID()) {
                        CFIndex tn = CFArrayGetCount(targets);
                        for (CFIndex t = 0; t < tn; ++t) {
                            CFStringRef uid = (CFStringRef)CFArrayGetValueAtIndex(targets, t);
                            if (uid && CFGetTypeID(uid) == CFStringGetTypeID()) s.targets.push_back(SonaStdString(uid));
                        }
                    }
                    gAppSettings[SonaStdString(key)] = s;
                }
            }
        }
        routesChanged = routesChanged || !gTargetsReconciled || SonaRoutesChanged(previousSettings, gAppSettings);
        if (!routesChanged)
            for (auto& client : gClients) if (client.used.load(std::memory_order_acquire)) SonaApplySettingsToClient(client, false);
        auto retainedConfig = config ? (CFDictionaryRef)CFRetain(config) : nullptr;
        if (gConfig) CFRelease(gConfig);
        gConfig = retainedConfig;
    }
    if (persist) SonaScheduleSave();
    if (routesChanged) SonaReconcileTargets();
}

void SonaEngineApplyConfig(CFDictionaryRef config) {
    if (!config) return;
    SonaApplyConfig(config, true);
    if (gPushes.config) gPushes.config(gPushes.ctx, config);
}

static OSStatus SonaDeviceListListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void*) {
    dispatch_async(gConfigQueue, ^{ SonaReconcileTargets(); });
    return noErr;
}

// MARK: - Snapshots

static bool SonaClientAudible(const ClientSlot& client, UInt64 now) {
    UInt64 last = client.lastSignalHostTime.load(std::memory_order_relaxed);
    return last != 0 && (now < last || now - last < gActivityWindowTicks);
}

CFArrayRef SonaEngineCopyClients() {
    std::lock_guard<std::recursive_mutex> lock(gStateMutex);
    const UInt64 now = mach_absolute_time();
    CFMutableArrayRef arr = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (UInt32 i = 0; i < kMaxClients; ++i) {
        ClientSlot& c = gClients[i];
        if (!c.used.load(std::memory_order_acquire)) continue;
        const bool running = SonaClientAudible(c, now);
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        SInt64 cid = c.clientID; SInt64 pid = c.pid;
        CFNumberRef cidN = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &cid);
        CFNumberRef pidN = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &pid);
        CFStringRef key = SonaCFString(c.key.c_str());
        CFDictionarySetValue(d, CFSTR(kSonaClientKey_ClientID), cidN);
        CFDictionarySetValue(d, CFSTR(kSonaClientKey_PID), pidN);
        CFDictionarySetValue(d, CFSTR(kSonaClientKey_Key), key);
        CFDictionarySetValue(d, CFSTR(kSonaClientKey_Running), running ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(d, CFSTR(kSonaClientKey_Played), c.played.load() ? kCFBooleanTrue : kCFBooleanFalse);
        if (!c.bundleID.empty()) { CFStringRef b = SonaCFString(c.bundleID.c_str()); CFDictionarySetValue(d, CFSTR(kSonaClientKey_BundleID), b); CFRelease(b); }
        CFRelease(cidN); CFRelease(pidN); CFRelease(key);
        CFArrayAppendValue(arr, d);
        CFRelease(d);
    }
    return arr;
}

CFDictionaryRef SonaEngineCopyStatus() {
    std::lock_guard<std::recursive_mutex> lock(gStateMutex);
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, CFSTR(kSonaStatusKey_Version), CFSTR(kSonaDriverVersion));
    Float64 rate = gSampleRate.load();
    CFNumberRef rateN = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &rate);
    CFDictionarySetValue(d, CFSTR(kSonaStatusKey_SampleRate), rateN); CFRelease(rateN);
    CFDictionarySetValue(d, CFSTR(kSonaStatusKey_IORunning), gIORunning.load() ? kCFBooleanTrue : kCFBooleanFalse);
    CFStringRef def = SonaCFString(gDefaultTargetUID.c_str());
    CFDictionarySetValue(d, CFSTR(kSonaStatusKey_DefaultTarget), def); CFRelease(def);
    CFDictionarySetValue(d, CFSTR("serviceConnected"), gRegion.load() ? kCFBooleanTrue : kCFBooleanFalse);
    CFMutableArrayRef targets = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (UInt32 i = 0; i < kMaxTargets; ++i) {
        SonaTarget& t = gTargets[i];
        if (t.uid.empty()) continue;
        CFMutableDictionaryRef td = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        SInt32 slot = (SInt32)i; SInt32 under = (SInt32)t.underruns.load(); Float64 drate = t.deviceRate.load();
        CFNumberRef slotN = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &slot);
        CFNumberRef underN = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &under);
        CFNumberRef drateN = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat64Type, &drate);
        const bool isDefault = t.uid == gDefaultTargetUID;
        Float32 volume = isDefault ? gMasterVolume.load() : gOutputControls[i].volume.load();
        bool mute = isDefault ? gMasterMute.load() : gOutputControls[i].mute.load();
        CFNumberRef volumeN = CFNumberCreate(nullptr, kCFNumberFloat32Type, &volume);
        CFDictionarySetValue(td, CFSTR("volume"), volumeN); CFRelease(volumeN);
        CFDictionarySetValue(td, CFSTR("mute"), mute ? kCFBooleanTrue : kCFBooleanFalse);
        CFStringRef uid = SonaCFString(t.uid.c_str());
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_Slot), slotN);
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_UID), uid);
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_Active), t.active.load() ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_Passthrough), t.passthrough.load() ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_ClockLocked), t.clockLocked.load() ? kCFBooleanTrue : kCFBooleanFalse);
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_Underruns), underN);
        CFDictionarySetValue(td, CFSTR(kSonaTargetKey_DeviceRate), drateN);
        // Latency instrumentation: frames queued between the mixer and this device's IOProc.
        SInt32 fill = (SInt32)t.ring.available();
        CFNumberRef fillN = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fill);
        CFDictionarySetValue(td, CFSTR("sinkFillFrames"), fillN); CFRelease(fillN);
        CFRelease(slotN); CFRelease(underN); CFRelease(drateN); CFRelease(uid);
        CFArrayAppendValue(targets, td);
        CFRelease(td);
    }
    CFDictionarySetValue(d, CFSTR(kSonaStatusKey_Targets), targets);
    CFRelease(targets);
    // Mixer instrumentation.
    SInt64 late = (SInt64)gMixLateBlocks.load(), resets = (SInt64)gMixResets.load();
    SInt64 lag = (SInt64)gMixLagFrames.load();
    CFNumberRef lateN = CFNumberCreate(nullptr, kCFNumberSInt64Type, &late);
    CFNumberRef resetsN = CFNumberCreate(nullptr, kCFNumberSInt64Type, &resets);
    CFNumberRef lagN = CFNumberCreate(nullptr, kCFNumberSInt64Type, &lag);
    CFDictionarySetValue(d, CFSTR("mixLateBlocks"), lateN);
    CFDictionarySetValue(d, CFSTR("mixResets"), resetsN);
    CFDictionarySetValue(d, CFSTR("mixLagFrames"), lagN);   // commit edge minus committed position at last pass
    SInt64 marginUs = (SInt64)(gMixMarginTicks.load() / gHostTicksPerSecond * 1e6);
    CFNumberRef marginN = CFNumberCreate(nullptr, kCFNumberSInt64Type, &marginUs);
    CFDictionarySetValue(d, CFSTR("mixMarginMicros"), marginN); CFRelease(marginN);
    CFRelease(lateN); CFRelease(resetsN); CFRelease(lagN);
    return d;
}

CFDictionaryRef SonaEngineCopyConfig() {
    std::lock_guard<std::recursive_mutex> lock(gStateMutex);
    return gConfig ? (CFDictionaryRef)CFRetain(gConfig) : nullptr;
}

static void SonaPushStatus() {
    if (!gPushes.status) return;
    CFDictionaryRef s = SonaEngineCopyStatus();
    gPushes.status(gPushes.ctx, s);
    CFRelease(s);
}

static void SonaPushClients() {
    if (!gPushes.clients) return;
    CFArrayRef c = SonaEngineCopyClients();
    gPushes.clients(gPushes.ctx, c);
    CFRelease(c);
}

static void SonaNotifyActivityChanges() {
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        UInt64 now = mach_absolute_time();
        for (auto& client : gClients) {
            bool audible = client.used.load(std::memory_order_acquire) && SonaClientAudible(client, now);
            if (audible != client.notifiedAudible) { client.notifiedAudible = audible; changed = true; }
        }
    }
    if (changed) SonaPushClients();
}

static void SonaUpdateActivityTimer() {
    bool enabled = gIORunning.load(std::memory_order_acquire);
    if (enabled == gActivityTimerEnabled || !gActivityTimer) return;
    gActivityTimerEnabled = enabled;
    dispatch_source_set_timer(gActivityTimer, enabled ? dispatch_time(DISPATCH_TIME_NOW, 0) : DISPATCH_TIME_FOREVER,
                              enabled ? 50 * NSEC_PER_MSEC : DISPATCH_TIME_FOREVER, 5 * NSEC_PER_MSEC);
    if (!enabled) {
        { std::lock_guard<std::recursive_mutex> lock(gStateMutex); for (auto& client : gClients) client.lastSignalHostTime.store(0); }
        SonaNotifyActivityChanges();
    }
}

// MARK: - Mixer thread

// Mixing is driven by host time, not by "cycles". Clients on the same virtual device run with
// different IO buffer sizes and therefore deliver blocks with different leads (block.hostTime
// minus arrival). The commit edge is the virtual-device sample time that will play at
// now + margin, where margin tracks the smallest lead recently observed; every client has then
// had its chance to deliver. Blocks are accumulated at their absolute sample position, so a
// client that runs far ahead simply waits in the window. Missing data commits as silence;
// a block that arrives behind the edge contributes only the part not yet committed.
static constexpr uint32_t kMixWindowFrames = 1u << 14;   // per-sink accumulator (power of two)
static constexpr uint32_t kMixWindowMask   = kMixWindowFrames - 1;
static float    gMixAccum[kMaxTargets][kMixWindowFrames * 2];
static bool     gMixTimelineValid = false;
static double   gMixedUpTo = 0;         // Sona sample time up to which sinks have been written
static double   gMixAccumEnd = -1;      // end of the newest audio accumulated; [gMixedUpTo, here) is still owed
static uint32_t gMixFormatGeneration = 0;
static bool     gMixAnchorValid = false; // most recent (sampleTime, hostTime) pair seen
static double   gMixAnchorSample = 0;
static uint64_t gMixAnchorHost = 0;
static int64_t  gMixMinLead[2] = {INT64_MAX, INT64_MAX};   // per-second minimum lead, current/previous
static uint64_t gMixLeadWindowStart = 0;

static void SonaMixResetAnchor() { gMixAnchorValid = false; gMixMinLead[0] = gMixMinLead[1] = INT64_MAX; }
static void SonaMixReset(double sampleTime) {
    for (auto& accum : gMixAccum) memset(accum, 0, sizeof(accum));
    gMixTimelineValid = sampleTime >= 0;
    gMixedUpTo = sampleTime;
    gMixAccumEnd = sampleTime;
    for (auto& t : gTargets) t.ring.markDiscontinuity();
    gMixResets.fetch_add(1, std::memory_order_relaxed);
}

// Blocks carry the client instance and format generation the producer knew when it wrote them.
// The control messages announcing a new instance or format travel separately and can arrive
// after the first blocks tagged with them, so a newer tag means "not registered yet": that block
// waits at the head of its ring until the control plane catches up. An older tag is a leftover
// of a retired instance or format and is consumed by the mixer, the only thread that moves read
// indices; nothing on the control queue touches a ring.
enum class SonaBlockClass { Current, Stale, Future };
static inline SonaBlockClass SonaClassifyBlock(const SonaTransportBlock& d, uint32_t instance, uint32_t formatGeneration) {
    if (d.instance == instance) {
        if (d.formatGeneration == formatGeneration) return SonaBlockClass::Current;
        return (int32_t)(d.formatGeneration - formatGeneration) > 0 ? SonaBlockClass::Future : SonaBlockClass::Stale;
    }
    return (int32_t)(d.instance - instance) > 0 ? SonaBlockClass::Future : SonaBlockClass::Stale;
}

static inline void SonaMixAccumulate(uint32_t sink, double start, uint32_t skip, const SonaTransportReadBlock& b, float gain) {
    const uint64_t base = (uint64_t)start;
    float* accum = gMixAccum[sink];
    uint32_t f = 0;
    for (uint32_t i = 0; i < b.firstFrames; ++i, ++f) {
        if (f < skip) continue;
        uint32_t idx = (uint32_t)((base + f) & kMixWindowMask) * 2;
        accum[idx] += b.first[i * 2] * gain; accum[idx + 1] += b.first[i * 2 + 1] * gain;
    }
    for (uint32_t i = 0; i < b.secondFrames; ++i, ++f) {
        if (f < skip) continue;
        uint32_t idx = (uint32_t)((base + f) & kMixWindowMask) * 2;
        accum[idx] += b.second[i * 2] * gain; accum[idx + 1] += b.second[i * 2 + 1] * gain;
    }
}

static inline bool SonaBlockHasSignal(const SonaTransportReadBlock& b) {
    for (uint32_t i = 0; i < b.firstFrames * 2; ++i) if (std::isfinite(b.first[i]) && std::fabs(b.first[i]) > 1.0e-6f) return true;
    for (uint32_t i = 0; i < b.secondFrames * 2; ++i) if (std::isfinite(b.second[i]) && std::fabs(b.second[i]) > 1.0e-6f) return true;
    return false;
}

// Commits [gMixedUpTo, commitTo) to every active sink and clears that part of the window.
static void SonaMixCommit(UInt32 defaultMask, double commitTo) {
    commitTo = std::floor(commitTo);
    if (commitTo <= gMixedUpTo) return;
    const uint32_t frames = (uint32_t)std::min<double>(commitTo - gMixedUpTo, kMixWindowFrames / 2);
    const float master = SonaMasterGain();
    static float scratch[kMixWindowFrames * 2];
    for (UInt32 sink = 0; sink < kMaxTargets; ++sink) {
        SonaTarget& t = gTargets[sink];
        float* accum = gMixAccum[sink];
        const uint64_t base = (uint64_t)gMixedUpTo;
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t idx = (uint32_t)((base + f) & kMixWindowMask) * 2;
            scratch[f * 2] = accum[idx]; scratch[f * 2 + 1] = accum[idx + 1];
            accum[idx] = accum[idx + 1] = 0;
        }
        if (!t.active.load(std::memory_order_acquire)) continue;
        const bool isDefault = (defaultMask & (1u << sink)) != 0;
        t.ring.write(scratch, frames, isDefault ? master : gOutputControls[sink].gain.load(std::memory_order_relaxed));
    }
    gMixedUpTo += frames;
}

static void SonaMixOnce(void* region, uint64_t now) {
    if (gMixResetRequested.exchange(0, std::memory_order_acq_rel)) { SonaMixResetAnchor(); SonaMixReset(-1); }
    const UInt32 defaultMask = gDefaultMask.load(std::memory_order_acquire);
    const uint32_t formatGeneration = gFormatGeneration.load(std::memory_order_acquire);
    if (formatGeneration != gMixFormatGeneration) { gMixFormatGeneration = formatGeneration; gMixAnchorValid = false; SonaMixReset(-1); }
    const double framesPerTick = gSampleRate.load(std::memory_order_relaxed) / gHostTicksPerSecond;

    // Mirror the plug-in's lock report into the master sink so it may copy 1:1.
    const UInt32 masterSlot = gClockMasterSlot.load(std::memory_order_acquire);
    const bool driverLocked = SonaTransportClockCell(region)->driverLocked.load(std::memory_order_relaxed) != 0;
    for (UInt32 i = 0; i < kMaxTargets; ++i) gTargets[i].clockLocked.store(i == masterSlot && driverLocked, std::memory_order_relaxed);

    // Lead statistics roll over once per second; the margin uses the smaller of two windows so
    // a client that stops delivering cannot keep the margin large forever.
    const uint64_t second = (uint64_t)gHostTicksPerSecond;
    if (now - gMixLeadWindowStart > second) { gMixMinLead[1] = gMixMinLead[0]; gMixMinLead[0] = INT64_MAX; gMixLeadWindowStart = now; }

    // Pass 1: the newest timestamp pair anchors the timeline; leads feed the margin. Only current
    // blocks count. Leftovers of a retired instance or format at the head of a ring are consumed
    // here, on the ring's only consumer thread; the control queue never moves read indices.
    double queuedOldest = -1;
    bool any = false;
    for (uint32_t slot = 0; slot < kMaxClients; ++slot) {
        ClientSlot& c = gClients[slot];
        if (!c.used.load(std::memory_order_acquire)) continue;
        const uint32_t instance = c.instance.load(std::memory_order_relaxed);
        SonaTransportClientRing* ring = SonaTransportRing(region, slot);
        SonaTransportReadBlock stale;
        uint32_t guard = 0;
        while (SonaTransportRingPeek(ring, stale) && ++guard <= kSonaTransportBlockCapacity &&
               SonaClassifyBlock(stale.desc, instance, formatGeneration) == SonaBlockClass::Stale)
            SonaTransportRingConsume(ring, stale);
        const uint64_t br = ring->blockRead.load(std::memory_order_relaxed);
        const uint64_t bw = ring->blockWrite.load(std::memory_order_acquire);
        for (uint64_t i = br; i < bw && i - br < kSonaTransportBlockCapacity; ++i) {
            const SonaTransportBlock& d = ring->blocks[i & (kSonaTransportBlockCapacity - 1)];
            if (SonaClassifyBlock(d, instance, formatGeneration) != SonaBlockClass::Current) continue;
            any = true;
            if (queuedOldest < 0 || d.sampleTime < queuedOldest) queuedOldest = d.sampleTime;
            if ((d.flags & kSonaBlockFlag_TimestampValid) && d.hostTime != 0) {
                if (!gMixAnchorValid || d.hostTime > gMixAnchorHost) { gMixAnchorValid = true; gMixAnchorHost = d.hostTime; gMixAnchorSample = d.sampleTime; }
                if (i == bw - 1) {   // only the block just delivered says anything about the client's lead
                    int64_t lead = (int64_t)d.hostTime - (int64_t)now;
                    if (lead < gMixMinLead[0]) gMixMinLead[0] = lead;
                }
            }
        }
    }

    // Commit edge in Sona sample time. Without any anchor there is no timeline: commit everything.
    double edge = 1e300;
    if (gMixAnchorValid) {
        int64_t minLead = std::min(gMixMinLead[0], gMixMinLead[1]);
        int64_t margin = minLead == INT64_MAX ? 0 : std::max<int64_t>(0, minLead - (int64_t)(gHostTicksPerSecond * 0.002));
        gMixMarginTicks.store(margin, std::memory_order_relaxed);
        edge = gMixAnchorSample + ((double)now + (double)margin - (double)gMixAnchorHost) * framesPerTick;
        // Nothing can be committed past the newest block delivered plus one block: that would only
        // write silence ahead of data still in flight.
        edge = std::min(edge, gMixAnchorSample + 2.0 * kSonaTransportMaxBlockFrames);
    }

    if (!any) {
        // Nothing new is queued, but a block is only committed up to the edge of the pass that
        // consumed it: the rest still sits in the accumulator. Keep committing that tail as the
        // edge advances so the end of the last sound is not cut off.
        if (gMixTimelineValid && edge < 1e299) SonaMixCommit(defaultMask, std::min(edge, gMixAccumEnd));
        return;
    }
    if (!gMixTimelineValid) { gMixTimelineValid = true; gMixedUpTo = queuedOldest; gMixAccumEnd = queuedOldest; }

    // Timeline jump (IO restart or host reseeding): the edge moved far from the committed one.
    if (edge < gMixedUpTo - 2.0 * kSonaTransportMaxBlockFrames || (edge < 1e299 && edge > gMixedUpTo + kMixWindowFrames / 2)) {
        SonaMixReset(std::min(queuedOldest, edge));
    }
    gMixLagFrames.store((int64_t)(edge < 1e299 ? edge - gMixedUpTo : 0), std::memory_order_relaxed);

    // Pass 2: accumulate every block that starts before the edge; blocks beyond it wait, and so
    // do blocks tagged with an instance or format the control plane has not announced yet.
    for (uint32_t slot = 0; slot < kMaxClients; ++slot) {
        ClientSlot& c = gClients[slot];
        if (!c.used.load(std::memory_order_acquire)) continue;
        const uint32_t instance = c.instance.load(std::memory_order_relaxed);
        SonaTransportClientRing* ring = SonaTransportRing(region, slot);
        SonaTransportReadBlock b;
        uint32_t guard = 0;
        while (SonaTransportRingPeek(ring, b) && ++guard <= kSonaTransportBlockCapacity) {
            const SonaBlockClass kind = SonaClassifyBlock(b.desc, instance, formatGeneration);
            if (kind == SonaBlockClass::Stale) { SonaTransportRingConsume(ring, b); continue; }
            if (kind == SonaBlockClass::Future) break;
            const double start = b.desc.sampleTime, end = start + b.desc.frames;
            if (start >= edge) break;
            if (end <= gMixedUpTo) { gMixLateBlocks.fetch_add(1, std::memory_order_relaxed); SonaTransportRingConsume(ring, b); continue; }
            if (end > gMixedUpTo + kMixWindowFrames) { SonaTransportRingConsume(ring, b); continue; }   // beyond window: drop
            const uint32_t skip = start < gMixedUpTo ? (uint32_t)(gMixedUpTo - start) : 0;
            if (skip) gMixLateBlocks.fetch_add(1, std::memory_order_relaxed);
            if (SonaBlockHasSignal(b)) { c.lastSignalHostTime.store(now, std::memory_order_relaxed); c.played.store(true, std::memory_order_relaxed); }
            const float gain = c.gain.load(std::memory_order_relaxed);
            UInt32 mask = c.routeMask.load(std::memory_order_relaxed);
            if (mask == 0) mask = defaultMask;
            if (gain != 0.0f) for (UInt32 sink = 0; sink < kMaxTargets; ++sink)
                if ((mask & (1u << sink)) && gTargets[sink].active.load(std::memory_order_acquire))
                    SonaMixAccumulate(sink, start, skip, b, gain);
            if (end > gMixAccumEnd) gMixAccumEnd = end;
            SonaTransportRingConsume(ring, b);
        }
    }

    // Pass 3: commit [gMixedUpTo, edge) to every active sink.
    if (edge > 1e299) return;   // no timeline yet: data waits in the accumulator
    SonaMixCommit(defaultMask, edge);
}

static void* SonaMixerThread(void*) {
    pthread_setname_np("com.sona.audio-service.mixer");
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t periodTicks = (uint64_t)(1'000'000.0 * tb.denom / tb.numer);   // 1 ms
    thread_time_constraint_policy_data_t policy = {
        (uint32_t)periodTicks, (uint32_t)(periodTicks / 10), (uint32_t)(periodTicks / 2), 1 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    uint64_t next = mach_absolute_time();
    for (;;) {
        next += periodTicks;
        mach_wait_until(next);
        // Reader pass (odd = inside). The increment is ordered before the pointer load and both
        // are sequentially consistent, which is what lets SonaEngineDetachRegion prove that a
        // pass it saw as finished can never reach the region it just unpublished.
        gMixerPass.fetch_add(1, std::memory_order_seq_cst);
        void* region = gRegion.load(std::memory_order_seq_cst);
        if (region) SonaMixOnce(region, mach_absolute_time());
        gMixerPass.fetch_add(1, std::memory_order_release);
    }
    return nullptr;
}

// MARK: - Session (control queue)

// A reader recorded outside a pass (even) at detach time loaded, or will load, the cleared
// pointer; one recorded inside (odd) is done with the region once its counter moves.
static bool SonaReaderRetired(uint64_t recorded, uint64_t current) { return (recorded & 1) == 0 || current != recorded; }

bool SonaEngineRegionRetired(const SonaEngineRetireTicket& ticket) {
    if (!SonaReaderRetired(ticket.mixerPass, gMixerPass.load(std::memory_order_seq_cst))) return false;
    for (UInt32 i = 0; i < kMaxTargets; ++i)
        if (!SonaReaderRetired(ticket.sinkPass[i], gTargets[i].clockPass.load(std::memory_order_seq_cst))) return false;
    return true;
}

void SonaEngineAttachRegion(void* region, uint64_t generation) {
    if (gRegion.load(std::memory_order_relaxed)) {
        SonaLog("attach while a region is still published; the caller must detach first");
        SonaEngineDetachRegion();
    }
    SonaTransportClock* cell = SonaTransportClockCell(region);
    for (auto& t : gTargets) t.clockCell.store(cell, std::memory_order_release);
    gMixResetRequested.store(1, std::memory_order_release);   // the mixer starts the new timeline itself
    gRegion.store(region, std::memory_order_release);
    SonaLog("engine attached region generation %llu", generation);
    SonaPushStatus();
}

SonaEngineRetireTicket SonaEngineDetachRegion() {
    SonaEngineRetireTicket ticket;   // all zero: nothing to wait for
    void* old = gRegion.exchange(nullptr, std::memory_order_seq_cst);
    for (auto& t : gTargets) t.clockCell.store(nullptr, std::memory_order_seq_cst);
    if (!old) return ticket;
    // Record every reader after unpublishing. The caller unmaps only once the ticket retires;
    // a short wait here just makes that the common case. A reader that is descheduled or
    // stopped for longer keeps the mapping alive instead of being run into freed memory.
    ticket.mixerPass = gMixerPass.load(std::memory_order_seq_cst);
    for (UInt32 i = 0; i < kMaxTargets; ++i) ticket.sinkPass[i] = gTargets[i].clockPass.load(std::memory_order_seq_cst);
    for (int spins = 0; spins < 200 && !SonaEngineRegionRetired(ticket); ++spins) usleep(100);
    if (!SonaEngineRegionRetired(ticket)) SonaLog("region still in use by a reader after 20 ms; unmap deferred");
    for (auto& t : gTargets) { if (t.ioStarted) t.stop(); }
    for (UInt32 i = 0; i < kMaxClients; ++i) {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        gClients[i].used.store(false, std::memory_order_release);
        gClients[i].instance.store(0);
        gClients[i].bundleID.clear(); gClients[i].key.clear();
    }
    gIORunning.store(false);
    SonaUpdateActivityTimer();
    gMixResetRequested.store(1, std::memory_order_release);   // mixer state belongs to the mixer thread
    SonaPushClients();
    SonaPushStatus();
    return ticket;
}

// The ring is not touched here: the mixer thread is its only consumer and drops blocks of a
// retired instance itself (see SonaClassifyBlock). A flush from this queue would race with a
// peek/consume pair in flight and could move the read index past the write index for good.
void SonaEngineClientAdd(uint32_t slot, uint32_t instance, uint32_t clientID, int32_t pid, const std::string& bundleID) {
    if (slot >= kMaxClients) return;
    ClientSlot& c = gClients[slot];
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        c.used.store(false, std::memory_order_release);
        c.clientID = clientID;
        c.pid = pid;
        c.bundleID = bundleID;
        c.key = bundleID.empty() ? ("pid:" + std::to_string(pid)) : bundleID;
        c.lastSignalHostTime.store(0);
        c.played.store(false);
        c.notifiedAudible = false;
        c.instance.store(instance, std::memory_order_relaxed);
        SonaApplySettingsToClient(c);
        c.used.store(true, std::memory_order_release);
    }
    SonaStartTargetsIfNeeded();
    SonaPushClients();
}

void SonaEngineClientRemove(uint32_t slot, uint32_t instance) {
    if (slot >= kMaxClients) return;
    ClientSlot& c = gClients[slot];
    {
        std::lock_guard<std::recursive_mutex> lock(gStateMutex);
        if (!c.used.load() || c.instance.load() != instance) return;
        c.used.store(false, std::memory_order_release);
        c.instance.store(0);
        c.bundleID.clear(); c.key.clear();
    }
    SonaStartTargetsIfNeeded();
    SonaPushClients();
}

void SonaEngineSetFormat(double sampleRate, uint32_t formatGeneration) {
    if (sampleRate > 0) {
        gSampleRate.store(sampleRate);
        for (auto& t : gTargets) t.sourceRate.store(sampleRate);
    }
    gFormatGeneration.store(formatGeneration, std::memory_order_release);
    SonaPushStatus();
}

void SonaEngineSetIORunning(bool running) {
    if (running == gIORunning.load()) return;
    gIORunning.store(running);
    SonaStartTargetsIfNeeded();
    SonaUpdateActivityTimer();
    SonaPushStatus();
}

// MARK: - Start

void SonaEngineStart(dispatch_queue_t controlQueue, const SonaEnginePushes& pushes, const char* stateFile) {
    gConfigQueue = controlQueue;
    gPushes = pushes;
    gStateFile = stateFile ? stateFile : "";
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    gHostTicksPerSecond = (Float64)tb.denom / (Float64)tb.numer * 1000000000.0;
    gActivityWindowTicks = (UInt64)(0.3 * gHostTicksPerSecond);

    gActivityTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gConfigQueue);
    dispatch_source_set_timer(gActivityTimer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 5 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(gActivityTimer, ^{ SonaNotifyActivityChanges(); });
    dispatch_resume(gActivityTimer);

    pthread_t mixer;
    pthread_create(&mixer, nullptr, SonaMixerThread, nullptr);
    pthread_detach(mixer);

    gHALClientReady = true;
    AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, SonaDeviceListListener, nullptr);

    CFDictionaryRef state = SonaLoadState();
    SonaRestoreOutputLevels(state);
    auto config = state ? CFDictionaryGetValue(state, CFSTR("config")) : nullptr;
    if (config && CFGetTypeID(config) == CFDictionaryGetTypeID()) {
        SonaLog("restored persisted config");
        SonaApplyConfig((CFDictionaryRef)config, false);
    } else {
        SonaReconcileTargets();
    }
    if (state) CFRelease(state);
}
