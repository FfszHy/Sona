// Mock-HAL regression tests for the Sona Audio Service engine. No real device is touched.
// Ported from the in-process prototype's HardwareControlsTests; the driver-only parts
// (zero timestamps) live in Driver/Tests/ClockTests.cpp.

#include <CoreAudio/CoreAudio.h>
#include <cassert>
#include <cmath>
#include <map>
#include <tuple>
#include <cstdio>
#include <unistd.h>

using Key = std::tuple<AudioObjectID, AudioObjectPropertySelector, AudioObjectPropertyElement>;
struct Property { float value; bool writable = true; };
static std::map<Key, Property> properties;
static bool failReads = false, failWrites = false;
static int writes = 0;
static int hardwareReads = 0;
static Key key(AudioObjectID id, const AudioObjectPropertyAddress* a) {
    assert(a->mScope == kAudioObjectPropertyScopeOutput);
    return {id, a->mSelector, a->mElement};
}
static Boolean has(AudioObjectID id, const AudioObjectPropertyAddress* a) { return properties.count(key(id, a)); }
static OSStatus mockSettable(AudioObjectID id, const AudioObjectPropertyAddress* a, Boolean* value) {
    if (!has(id, a)) return kAudioHardwareUnknownPropertyError;
    *value = properties.at(key(id, a)).writable;
    return noErr;
}
static OSStatus getSize(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32* size) {
    *size = sizeof(AudioBufferList); return noErr;
}
static OSStatus get(AudioObjectID id, const AudioObjectPropertyAddress* a, UInt32, const void* qualifier, UInt32*, void* data) {
    ++hardwareReads;
    if (failReads) return kAudioHardwareUnspecifiedError;
    if (a->mSelector == kAudioHardwarePropertyTranslateUIDToDevice) {
        auto uid = *static_cast<const CFStringRef*>(qualifier);
        *static_cast<AudioObjectID*>(data) = CFEqual(uid, CFSTR("device-a")) ? 10 :
            CFEqual(uid, CFSTR("device-b")) ? 20 : CFEqual(uid, CFSTR("device-c")) ? 30 : kAudioObjectUnknown;
        return noErr;
    }
    if (a->mSelector == kAudioDevicePropertyStreamConfiguration) {
        auto* list = static_cast<AudioBufferList*>(data);
        list->mNumberBuffers = 1; list->mBuffers[0].mNumberChannels = 2; return noErr;
    }
    if (!has(id, a)) return kAudioHardwareUnknownPropertyError;
    float value = properties.at(key(id, a)).value;
    if (a->mSelector == kAudioDevicePropertyMute) *static_cast<UInt32*>(data) = value;
    else *static_cast<Float32*>(data) = value;
    return noErr;
}
static OSStatus set(AudioObjectID id, const AudioObjectPropertyAddress* a, UInt32, const void*, UInt32, const void* data) {
    ++writes;
    if (failWrites || !has(id, a) || !properties.at(key(id, a)).writable) return kAudioHardwareUnspecifiedError;
    properties.at(key(id, a)).value = a->mSelector == kAudioDevicePropertyMute
        ? *static_cast<const UInt32*>(data) : *static_cast<const Float32*>(data);
    return noErr;
}
static OSStatus mockListener(AudioObjectID, const AudioObjectPropertyAddress*, AudioObjectPropertyListenerProc, void*) { return noErr; }
#define AudioObjectAddPropertyListener mockListener
#define AudioObjectRemovePropertyListener mockListener
#define AudioObjectHasProperty has
#define AudioObjectIsPropertySettable mockSettable
#define AudioObjectGetPropertyDataSize getSize
#define AudioObjectGetPropertyData get
#define AudioObjectSetPropertyData set
#include <os/log.h>
os_log_t gSonaServiceLog = nullptr;
#include "../SonaEngine.cpp"
static int deviceStarts = 0, deviceStops = 0;
static OSStatus mockDeviceStart(AudioObjectID, AudioDeviceIOProcID) { ++deviceStarts; return noErr; }
static OSStatus mockDeviceStop(AudioObjectID, AudioDeviceIOProcID) { ++deviceStops; return noErr; }
#define AudioDeviceStart mockDeviceStart
#define AudioDeviceStop mockDeviceStop
#include "../SonaTarget.cpp"

static int clientPushes = 0, statusPushes = 0, masterPushes = 0;
static float pushedMasterVolume = -1; static bool pushedMasterMute = false;

static CFDictionaryRef configFor(const std::string& entries) {
    std::string xml = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>defaultTarget</key><string>device-a</string><key>apps</key><dict>" + entries + "</dict></dict></plist>";
    auto data = CFDataCreate(nullptr, (const UInt8*)xml.data(), xml.size());
    auto config = (CFDictionaryRef)CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    assert(config);
    return config;
}

static void drain() { dispatch_sync(gConfigQueue, ^{}); }

int main() {
    gConfigQueue = dispatch_queue_create("com.sona.tests", DISPATCH_QUEUE_SERIAL);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    gHostTicksPerSecond = 1e9 * tb.denom / tb.numer;
    gActivityWindowTicks = UInt64(0.3 * gHostTicksPerSecond);
    gPushes.clients = [](void*, CFArrayRef) { ++clientPushes; };
    gPushes.status = [](void*, CFDictionaryRef) { ++statusPushes; };
    gPushes.master = [](void*, float v, bool m) { ++masterPushes; pushedMasterVolume = v; pushedMasterMute = m; };

    // ---- Config: volume-only updates must not enter HAL device reconciliation.
    auto initialConfig = configFor("");
    SonaApplyConfig(initialConfig, false);
    auto& configClient = gClients[0];
    configClient.used = true; configClient.pid = 123; configClient.key = "test.app"; configClient.routeMask = 0;
    gHALClientReady = true;
    gTargetsReconciled = true;
    int readsBefore = hardwareReads;
    auto quietConfig = configFor("<key>test.app</key><dict><key>volume</key><real>0.25</real></dict>");
    SonaApplyConfig(quietConfig, false);
    assert(configClient.gain == 0.25f && configClient.routeMask == 0);
    assert(hardwareReads == readsBefore);
    auto mutedConfig = configFor("<key>test.app</key><dict><key>volume</key><real>0.25</real><key>mute</key><true/></dict>");
    SonaApplyConfig(mutedConfig, false);
    assert(configClient.gain == 0 && hardwareReads == readsBefore);
    SonaApplyConfig(initialConfig, false);
    assert(configClient.gain == 1 && hardwareReads == readsBefore);
    std::unordered_map<std::string, AppSetting> before, after;
    before["test.app"].targets = {"device-b"};
    after = before; after["test.app"].gain = 0.2f; after["test.app"].mute = true;
    assert(!SonaRoutesChanged(before, after));
    after["pid:123"].gain = 0.5f;
    assert(SonaRoutesChanged(before, after));
    after["pid:123"].targets = {"device-b"};
    assert(SonaRoutesChanged(before, after));
    after = before; after["test.app"].targets = {"device-c"};
    assert(SonaRoutesChanged(before, after));
    after.clear();
    assert(SonaRoutesChanged(before, after));
    gAppSettings = before;
    auto routedConfig = configFor("<key>test.app</key><dict><key>targets</key><array><string>device-b</string></array></dict>");
    CFRelease(gConfig); gConfig = (CFDictionaryRef)CFRetain(routedConfig);
    configClient.routeMask = 8;
    auto routedQuiet = configFor("<key>test.app</key><dict><key>volume</key><real>0.3</real><key>targets</key><array><string>device-b</string></array></dict>");
    SonaApplyConfig(routedQuiet, false);
    assert(configClient.gain == 0.3f && configClient.routeMask == 8);
    assert(hardwareReads == readsBefore);
    gHALClientReady = false; gTargetsReconciled = false;
    gAppSettings.clear(); gDefaultTargetUID.clear();
    CFRelease(gConfig); gConfig = nullptr;
    for (auto config : {initialConfig, quietConfig, mutedConfig, routedConfig, routedQuiet}) CFRelease(config);
    configClient.used = false; configClient.routeMask = 0; configClient.gain = 1;

    // ---- Hardware controls.
    auto vol = kAudioDevicePropertyVolumeScalar;
    auto mute = kAudioDevicePropertyMute;
    properties[{10, vol, 0}] = {0.4f};
    properties[{10, mute, 0}] = {1};
    properties[{20, vol, 1}] = {0.8f};
    properties[{20, vol, 2}] = {0.4f};
    properties[{20, mute, 1}] = {0};
    properties[{20, mute, 2}] = {1};
    properties[{30, vol, 0}] = {1, false};
    SonaHardwareControls controls;
    Float32 value = 0; bool muted = false;
    controls.bind(10);
    assert(writes == 0);
    assert(controls.volume(value) && value == 0.4f);
    assert(controls.muted(muted) && muted);
    assert(controls.setVolume(0.6f) && controls.volume(value) && value == 0.6f);
    assert(controls.setMuted(false) && controls.muted(muted) && !muted);
    controls.bind(20);
    assert(controls.volumeElements.size() == 2);
    assert(controls.volume(value) && value == 0.8f);
    assert(controls.setVolume(0.4f));
    assert((properties.at({20, vol, 1}).value == 0.4f));
    assert((properties.at({20, vol, 2}).value == 0.2f));
    assert(controls.setMuted(true) && controls.muted(muted) && muted);
    assert(controls.setVolume(0) && controls.setVolume(0.5f));
    assert((properties.at({20, vol, 2}).value == 0.5f));
    failWrites = true;
    assert(!controls.setVolume(0.8f) && !controls.setMuted(false));
    failWrites = false; failReads = true;
    assert(!controls.volume(value) && !controls.setVolume(0.8f));
    failReads = false;
    controls.bind(30);
    assert(controls.volumeElements.empty() && !controls.volume(value));
    controls.bind(kAudioObjectUnknown);

    // ---- Master volume proxy through the engine API.
    gHardwareControls.bind(10);
    gHardwareVolume = true; gHardwareMute = true;
    SonaRefreshHardwareControls();
    assert(gMasterVolume == 0.6f && pushedMasterVolume == 0.6f);
    SonaEngineSetMasterVolume(0.3f);
    SonaEngineSetMasterMute(true);
    drain();
    assert((properties.at({10, vol, 0}).value == 0.3f));
    assert((properties.at({10, mute, 0}).value == 1));
    assert(SonaMasterGain() == 1.0f);
    dispatch_suspend(gConfigQueue);
    for (int i = 1; i <= 16; ++i) SonaEngineSetMasterVolume(i / 16.0f);
    assert(gMasterVolume == 1.0f);
    SonaRefreshHardwareControls();   // pending writes: an old notification cannot rewind key presses
    assert(gMasterVolume == 1.0f);
    dispatch_resume(gConfigQueue);
    drain();
    assert((properties.at({10, vol, 0}).value == 1.0f));
    failWrites = true;
    SonaEngineSetMasterVolume(0.2f);
    drain();
    assert(gMasterVolume == 1.0f);   // failed write restores actual hardware state
    failWrites = false;
    dispatch_suspend(gConfigQueue);
    SonaEngineSetMasterVolume(0.1f);
    gHardwareGeneration.fetch_add(1);
    gHardwareControls.bind(20);
    dispatch_resume(gConfigQueue);
    drain();
    assert((properties.at({20, vol, 1}).value == 0.5f));   // stale write never touches new device
    assert(gMasterVolume == 0.5f);
    gHardwareControls.bind(30);
    gHardwareVolume = false; gHardwareMute = false;
    SonaEngineSetMasterVolume(0.5f); SonaEngineSetMasterMute(false);
    drain();
    assert(SonaMasterGain() == 0.125f);
    SonaEngineSetMasterMute(true);
    drain();
    assert(SonaMasterGain() == 0 && pushedMasterMute);

    // ---- Per-device controls: hardware, digital fallback, failed writes, UID isolation.
    gTargets[0].uid = "device-a"; gTargets[1].uid = "device-b"; gTargets[2].uid = "device-c";
    gDefaultTargetUID = "device-a";
    gBoundDefaultUID.clear();
    gHardwareControls.bind(kAudioObjectUnknown);
    SonaBindOutputControls();
    const float originalDefault = gMasterVolume.load();
    SonaSetOutputLevel("device-b", true, 0.25f, true, false);
    assert((properties.at({20, vol, 1}).value == 0.25f));
    assert(gMasterVolume == originalDefault && gOutputControls[1].gain == 1);
    failWrites = true;
    SonaSetOutputLevel("device-b", true, 0.9f, false, false);
    assert(gOutputControls[1].volume == 0.25f);
    failWrites = false; failReads = true;
    SonaRefreshOutputControl(gOutputControls[1]);
    assert(gOutputControls[1].volume == 0.25f && gOutputControls[1].gain == 1);
    failReads = false;
    properties[{20, vol, 1}].value = 0.6f;
    SonaHardwareListener(20, 0, nullptr, nullptr);
    drain();
    assert(gOutputControls[1].volume == 0.6f);
    SonaSetOutputLevel("device-c", true, 0.5f, true, false);
    assert(gOutputControls[2].gain == 0.125f);
    SonaSetOutputLevel("device-c", false, 1, true, true);
    assert(gOutputControls[2].gain == 0);
    int writesBefore = writes;
    SonaSetOutputLevel("removed-device", true, 0.1f, true, true);
    assert(writes == writesBefore && gOutputControls[1].volume == 0.6f);
    gDefaultTargetUID = "device-c";
    SonaBindOutputControls();
    assert(gMasterVolume == 0.5f && gMasterMute && SonaMasterGain() == 0);
    SonaEngineSetMasterMute(false);
    drain();
    assert(SonaMasterGain() == 0.125f);
    gDefaultTargetUID = "device-a";
    SonaBindOutputControls();
    assert(gOutputControls[2].volume == 0.5f && !gOutputControls[2].mute && gOutputControls[2].gain == 0.125f);
    // Coalescing: a blocked queue keeps only the latest drag position per device.
    dispatch_suspend(gConfigQueue);
    for (int i = 0; i <= 500; ++i) SonaEngineSetOutputLevel("device-b", true, float(i) / 1000, false, false);
    SonaEngineSetOutputLevel("device-b", false, 1, true, true);
    SonaEngineSetOutputLevel("device-c", true, 0.3f, false, false);
    assert(gOutputCommands.size() == 2);
    int burstWrites = writes;
    dispatch_resume(gConfigQueue);
    drain();
    assert(gOutputControls[1].volume == 0.5f && gOutputControls[1].mute);
    assert(gOutputControls[2].volume == 0.3f);
    assert(writes - burstWrites == 4);
    // Input validation at the engine boundary.
    SonaEngineSetOutputLevel("device-c", true, NAN, false, false);
    SonaEngineSetOutputLevel("device-c", true, -1, false, false);
    SonaEngineSetOutputLevel("device-c", true, 1.1f, false, false);
    SonaEngineSetOutputLevel(kSonaDeviceUID, true, 0.1f, false, false);
    drain();
    assert(gOutputControls[2].volume == 0.3f);
    auto status = SonaEngineCopyStatus();
    auto targets = (CFArrayRef)CFDictionaryGetValue(status, CFSTR("targets"));
    auto target = (CFDictionaryRef)CFArrayGetValueAtIndex(targets, 2);
    float reported = 0;
    CFNumberGetValue((CFNumberRef)CFDictionaryGetValue(target, CFSTR("volume")), kCFNumberFloat32Type, &reported);
    assert(reported == 0.3f);
    CFRelease(status);

    // ---- Persistence through the state file (config + output levels).
    char stateFile[] = "/tmp/sona-engine-test-XXXXXX";
    close(mkstemp(stateFile));
    gStateFile = stateFile;
    auto persistedConfig = configFor("<key>persist.app</key><dict><key>volume</key><real>0.75</real></dict>");
    { std::lock_guard<std::recursive_mutex> lock(gStateMutex); if (gConfig) CFRelease(gConfig); gConfig = (CFDictionaryRef)CFRetain(persistedConfig); }
    SonaSaveState();
    gOutputLevels.clear();
    CFDictionaryRef loaded = SonaLoadState();
    assert(loaded);
    SonaRestoreOutputLevels(loaded);
    assert(gOutputLevels["device-c"].volume == 0.3f && !gOutputLevels["device-c"].mute);
    auto loadedConfig = (CFDictionaryRef)CFDictionaryGetValue(loaded, CFSTR("config"));
    assert(loadedConfig && CFEqual(loadedConfig, persistedConfig));
    CFRelease(loaded); CFRelease(persistedConfig);
    unlink(stateFile);
    gStateFile.clear();
    for (auto& control : gOutputControls) { control.uid.clear(); control.gain = 1; }
    for (auto& t : gTargets) t.uid.clear();

    // ---- Mixer: host-time-driven commit edge, per-client gain and routing, different client
    // buffer sizes, late partial blocks, signal detection, resets.
    {
        void* region = nullptr;
        assert(posix_memalign(&region, 4096, kSonaTransportRegionSize) == 0);
        SonaTransportInitRegion(region, 1);
        for (auto& c : gClients) { c.used = false; c.instance = 0; c.lastSignalHostTime = 0; c.played = false; }
        gDefaultMask = 1;
        gTargets[0].active = true; gTargets[1].active = true; gTargets[2].active = false;
        gTargets[0].ring.flush(); gTargets[1].ring.flush();
        gHardwareVolume = true; gHardwareMute = true;   // master gain 1
        gOutputControls[1].gain = 0.5f;
        auto& a = gClients[0]; a.used = true; a.instance = 1; a.gain = 0.5f; a.routeMask = 0;        // default sink, 128-frame buffers
        auto& b = gClients[1]; b.used = true; b.instance = 1; b.gain = 1.0f; b.routeMask = 2;        // sink 1 only, 512-frame buffers
        auto& idle = gClients[2]; idle.used = true; idle.instance = 1;                               // never writes
        gFormatGeneration = 7; gSampleRate = 48000;
        SonaMixResetAnchor(); SonaMixReset(-1); gMixFormatGeneration = 7;
        const double tpf = gHostTicksPerSecond / 48000;   // host ticks per frame
        const uint64_t t0 = mach_absolute_time();
        // sampleTime s plays at host time t0 + s * tpf. A client with N-frame buffers delivers block
        // [s, s+N) at host time (s - lead) * tpf where lead is its own buffer size (one period ahead).
        auto write = [&](uint32_t slot, uint32_t instance, double sampleTime, uint32_t frames, float l, float r) {
            static float pcm[kSonaTransportMaxBlockFrames * 2];
            for (uint32_t i = 0; i < frames; ++i) { pcm[i * 2] = l; pcm[i * 2 + 1] = r; }
            SonaTransportBlock blk = {}; blk.frames = frames; blk.instance = instance; blk.formatGeneration = gFormatGeneration.load();
            blk.sampleTime = sampleTime; blk.hostTime = t0 + (uint64_t)(sampleTime * tpf); blk.flags = kSonaBlockFlag_TimestampValid;
            assert(SonaTransportRingWrite(SonaTransportRing(region, slot), pcm, blk));
        };
        auto hostAt = [&](double sampleTime) { return t0 + (uint64_t)(sampleTime * tpf); };
        // Run 4096 frames of simulated time in 64-frame mixer steps. Client a delivers every 128
        // frames one period ahead; client b every 512 frames one period ahead.
        double nextA = 128, nextB = 512;   // first blocks are delivered one period ahead like a real IOProc
        for (double s = 0; s < 24576; s += 64) {
            while (nextA - 128 <= s) { write(0, 1, nextA, 128, 0.8f, 0.6f); nextA += 128; }
            while (nextB - 512 <= s) { write(1, 1, nextB, 512, 0.2f, 0.2f); nextB += 512; }
            SonaMixOnce(region, hostAt(s));
        }
        // After 4096 frames of wall time both sinks must have committed close to 4096 frames minus
        // the margin (b's lead is 512 frames, minus 2 ms slack), never running ahead of data.
        uint32_t committed0 = gTargets[0].ring.available(), committed1 = gTargets[1].ring.available();
        std::printf("mixer: committed %u/%u frames after 24576, margin %lld us\n", committed0, committed1, (long long)(gMixMarginTicks.load() / gHostTicksPerSecond * 1e6));
        assert(committed0 == committed1);
        assert(committed0 >= 24576 - 512 - 128 && committed0 <= 24576);
        assert(gMixMarginTicks.load() >= 0);   // the simulated clients deliver 64..128 frames ahead: margin is 0..0.7 ms
        float l, r;
        // Every committed frame carries both clients once both have started (the timeline begins
        // at a's first block, 128; b's first block covers 512..1024): a × 0.5 on sink 0, b × 0.5 on sink 1.
        for (uint32_t f = 512; f < committed0; f += 97) {
            gTargets[0].ring.sample(f, l, r);
            assert(std::fabs(l - 0.4f) < 1e-6f && std::fabs(r - 0.3f) < 1e-6f);
            gTargets[1].ring.sample(f, l, r);
            assert(std::fabs(l - 0.1f) < 1e-6f && std::fabs(r - 0.1f) < 1e-6f);
        }
        assert(a.played && b.played && !idle.played);
        assert(gMixLateBlocks.load() == 0);
        // A block arriving behind the edge contributes only its uncommitted tail; the rest is counted late.
        SonaMixOnce(region, hostAt(gMixedUpTo + 4096));   // drain everything still queued
        uint64_t lateBefore = gMixLateBlocks.load();
        double edge = gMixedUpTo;
        gTargets[0].ring.flush();
        write(0, 1, edge - 64, 128, 1.0f, 1.0f);
        SonaMixOnce(region, hostAt(edge + 1024));   // a little ahead: commits the tail plus silence
        assert(gMixLateBlocks.load() == lateBefore + 1);
        assert(gTargets[0].ring.available() >= 64);
        gTargets[0].ring.sample(0, l, r); assert(std::fabs(l - 0.5f) < 1e-6f);   // 1.0 × gain 0.5
        gTargets[0].ring.sample(64, l, r); assert(l == 0);                          // silence past the block
        // Instance tags. A block of an instance the control plane has already retired is consumed
        // and ignored by the mixer (the only thread that moves read indices); a block tagged with
        // an instance not announced yet waits at the head until the registration arrives.
        b.instance = 2;   // re-registration announced
        write(1, 1, gMixedUpTo + 64, 64, 1.0f, 1.0f);
        SonaMixOnce(region, hostAt(gMixedUpTo + 4096));   // past b's queued (now stale) blocks
        assert(SonaTransportRing(region, 1)->blockRead.load() == SonaTransportRing(region, 1)->blockWrite.load());
        write(1, 3, gMixedUpTo + 64, 64, 1.0f, 1.0f);      // written before clientAdd(instance 3) was processed
        SonaMixOnce(region, hostAt(gMixedUpTo + 4096));
        assert(SonaTransportRing(region, 1)->blockWrite.load() - SonaTransportRing(region, 1)->blockRead.load() == 1);
        b.instance = 3;
        SonaMixOnce(region, hostAt(gMixedUpTo + 4096));
        assert(SonaTransportRing(region, 1)->blockRead.load() == SonaTransportRing(region, 1)->blockWrite.load());
        // Timeline jump (IO restart): sample time restarts near zero while host time keeps going.
        uint64_t resetsBefore = gMixResets.load();
        auto disc = gTargets[0].ring.readView().discontinuities;
        {
            SonaTransportBlock blk = {}; float pcm[256] = {};
            blk.frames = 128; blk.instance = 1; blk.formatGeneration = 7; blk.sampleTime = 0;
            blk.hostTime = hostAt(gMixedUpTo + 1024 + 128); blk.flags = kSonaBlockFlag_TimestampValid;
            assert(SonaTransportRingWrite(SonaTransportRing(region, 0), pcm, blk));
        }
        SonaMixOnce(region, hostAt(gMixedUpTo + 1024 + 200));
        assert(gMixResets.load() == resetsBefore + 1);
        assert(gTargets[0].ring.readView().discontinuities == disc + 1);
        // Format generation change resets as well and drops blocks of the old generation.
        gFormatGeneration = 8;
        {
            SonaTransportBlock blk = {}; float pcm[256] = {};
            blk.frames = 128; blk.instance = 1; blk.formatGeneration = 7; blk.sampleTime = 500;   // still tagged 7
            blk.hostTime = hostAt(500); blk.flags = kSonaBlockFlag_TimestampValid;
            assert(SonaTransportRingWrite(SonaTransportRing(region, 0), pcm, blk));
        }
        SonaMixOnce(region, mach_absolute_time());
        assert(gMixFormatGeneration == 8);
        assert(SonaTransportRing(region, 0)->blockRead.load() == SonaTransportRing(region, 0)->blockWrite.load());
        // Tail drain. A block is committed only up to the edge of the pass that consumed it; once
        // the ring runs empty the remainder must still reach the sinks as the edge advances. The
        // old code returned early on an empty ring and cut the end of the last sound off.
        SonaMixResetAnchor(); SonaMixReset(-1);
        gTargets[0].ring.flush();
        write(0, 1, 1000, 128, 0.5f, 0.5f);
        SonaMixOnce(region, hostAt(1064));                 // edge ≈ 1064: half of the block is committed
        assert(gTargets[0].ring.available() >= 63 && gTargets[0].ring.available() <= 64);
        SonaMixOnce(region, hostAt(1128 + 200));           // ring empty, edge past the block end
        assert(gTargets[0].ring.available() == 128);
        gTargets[0].ring.sample(127, l, r); assert(std::fabs(l - 0.25f) < 1e-6f);   // 0.5 × gain 0.5, to the last frame
        SonaMixOnce(region, hostAt(1000 + 4096));          // nothing is owed: no silence is invented
        assert(gTargets[0].ring.available() == 128);
        gTargets[0].active = false; gTargets[1].active = false;
        gTargets[0].ring.flush(); gTargets[1].ring.flush();
        for (auto& c : gClients) { c.used = false; c.instance = 0; }
        free(region);
    }

    // ---- Session. The control queue never moves a ring's read indices (the mixer thread is the
    // only consumer; a flush from here raced with a peek/consume pair and could wedge the ring), a
    // block whose instance is not announced yet waits, and a region counts as retired only once
    // every reader has provably left it.
    {
        void* region = nullptr;
        assert(posix_memalign(&region, 4096, kSonaTransportRegionSize) == 0);
        SonaTransportInitRegion(region, 2);
        const double tpf = gHostTicksPerSecond / 48000;
        const uint64_t t0 = mach_absolute_time();
        auto hostAt = [&](double sampleTime) { return t0 + (uint64_t)(sampleTime * tpf); };
        float pcm[64 * 2]; for (auto& s : pcm) s = 0.25f;
        auto put = [&](uint32_t slot, uint32_t instance, double sampleTime) {
            SonaTransportBlock blk = {}; blk.frames = 64; blk.instance = instance; blk.formatGeneration = gFormatGeneration.load();
            blk.sampleTime = sampleTime; blk.hostTime = hostAt(sampleTime); blk.flags = kSonaBlockFlag_TimestampValid;
            assert(SonaTransportRingWrite(SonaTransportRing(region, slot), pcm, blk));
        };
        gMixResetRequested = 0;
        SonaEngineAttachRegion(region, 2);
        assert(gRegion.load() == region && gMixResetRequested.load() == 1);
        assert(gTargets[0].clockCell.load() == SonaTransportClockCell(region));
        auto* ring = SonaTransportRing(region, 5);
        put(5, 1, 0); put(5, 1, 64);                        // instance 1, about to be retired
        put(5, 2, 128);                                     // instance 2, written before its clientAdd arrived
        SonaEngineClientAdd(5, 2, 77, 4242, "com.sona.test");
        assert(ring->blockRead.load() == 0 && ring->blockWrite.load() == 3);   // registration touched no index
        assert(gClients[5].used.load() && gClients[5].instance.load() == 2);
        gDefaultMask = 1; gTargets[0].active = true; gTargets[0].ring.flush();
        SonaMixOnce(region, hostAt(128 + 64 + 200));
        assert(gMixResetRequested.load() == 0);            // the mixer thread applied the reset itself
        assert(ring->blockRead.load() == 3);               // stale blocks consumed, the current one mixed
        uint32_t committed = gTargets[0].ring.available(); // the block plus silence up to the edge
        assert(committed >= 64 && committed <= 264 && gClients[5].played.load());
        float l, r;
        gTargets[0].ring.sample(0, l, r);  assert(std::fabs(l - 0.25f) < 1e-6f);
        gTargets[0].ring.sample(63, l, r); assert(std::fabs(l - 0.25f) < 1e-6f);
        if (committed > 64) { gTargets[0].ring.sample(64, l, r); assert(l == 0); }
        const double next = gMixedUpTo + 64;
        put(5, 3, next);                                    // newer than anything announced: waits
        SonaMixOnce(region, hostAt(next + 64 + 200));
        assert(ring->blockRead.load() == 3 && ring->blockWrite.load() == 4);
        SonaEngineClientAdd(5, 3, 77, 4242, "com.sona.test");
        assert(ring->blockRead.load() == 3);
        committed = gTargets[0].ring.available();
        SonaMixOnce(region, hostAt(next + 64 + 200));
        assert(ring->blockRead.load() == 4 && gTargets[0].ring.available() >= committed + 64);
        // Detach while the mixer is inside a pass: the ticket stays open until that pass ends.
        gMixerPass.store(1);
        SonaEngineRetireTicket ticket = SonaEngineDetachRegion();
        assert(gRegion.load() == nullptr && gTargets[0].clockCell.load() == nullptr);
        assert(!SonaEngineRegionRetired(ticket));
        assert(!gClients[5].used.load() && gMixResetRequested.load() == 1);
        gMixerPass.store(2);
        assert(SonaEngineRegionRetired(ticket));
        // Same for a sink IOProc inside publishAnchor.
        SonaEngineAttachRegion(region, 3);
        gTargets[2].clockPass.store(7);
        ticket = SonaEngineDetachRegion();
        assert(!SonaEngineRegionRetired(ticket));
        gTargets[2].clockPass.store(8);
        assert(SonaEngineRegionRetired(ticket));
        // A detach recorded while the mixer is between passes needs no confirmation at all.
        SonaEngineAttachRegion(region, 4);
        ticket = SonaEngineDetachRegion();
        assert(SonaEngineRegionRetired(ticket));
        gMixerPass.store(4);
        assert(SonaEngineRegionRetired(SonaEngineDetachRegion()));   // nothing attached: trivially retired
        gMixResetRequested = 0;
        gTargets[0].active = false; gTargets[0].ring.flush();
        free(region);
    }

    // ---- Playback activity notifications.
    {
        clientPushes = 0;
        auto& client = gClients[0];
        client.used = true; client.notifiedAudible = false; client.lastSignalHostTime = 0;
        SonaNotifyActivityChanges();
        assert(clientPushes == 0);
        client.lastSignalHostTime = mach_absolute_time();
        SonaNotifyActivityChanges();
        assert(clientPushes == 1);
        SonaNotifyActivityChanges();
        assert(clientPushes == 1);   // no storm during continuous playback
        client.lastSignalHostTime = mach_absolute_time() - gActivityWindowTicks;
        SonaNotifyActivityChanges();
        assert(clientPushes == 2);
        client.used = false;
    }

    // ---- Demand routing and mock device lifecycle.
    for (auto& c : gClients) { c.used = false; c.routeMask = 0; }
    gDefaultMask = 1;
    gIORunning = true;
    auto& client = gClients[0]; client.used = true; client.routeMask = 0;
    auto& second = gClients[1]; second.used = true; second.routeMask = 2;
    assert(SonaNeededTargetMask() == 3);
    client.routeMask = 4;
    assert(SonaNeededTargetMask() == 7);   // default sink always stays open while IO runs
    second.used = false;
    assert(SonaNeededTargetMask() == 5);
    gIORunning = false;
    assert(SonaNeededTargetMask() == 0);
    dispatch_sync(gConfigQueue, ^{
        for (UInt32 i = 0; i < 3; ++i) {
            gTargets[i].uid = "test-output-" + std::to_string(i);
            gTargets[i].procID = reinterpret_cast<AudioDeviceIOProcID>(uintptr_t(i + 1));
            gTargets[i].ioStarted = false; gTargets[i].active = false;
        }
        client.routeMask = 0; second.used = false; gIORunning = true;
        SonaStartTargetsIfNeeded();
        assert(deviceStarts == 1 && gTargets[0].ioStarted && !gTargets[1].ioStarted);
        second.used = true; second.routeMask = 2;
        SonaStartTargetsIfNeeded();
        assert(deviceStarts == 2 && gTargets[1].ioStarted);
        second.used = false;
        SonaStartTargetsIfNeeded();
        assert(gTargetStopPending[1] && deviceStops == 0);
        UInt64 oldStop = gTargetStopGeneration[1];
        second.used = true;
        SonaStartTargetsIfNeeded();
        SonaFinishTargetStop(1, oldStop);
        assert(gTargets[1].ioStarted && deviceStops == 0);
        second.used = false;
        SonaStartTargetsIfNeeded();
        SonaFinishTargetStop(1, gTargetStopGeneration[1]);
        assert(!gTargets[1].ioStarted && deviceStops == 1);
        gIORunning = false;
        SonaStartTargetsIfNeeded();
        SonaFinishTargetStop(0, gTargetStopGeneration[0]);
        assert(!gTargets[0].ioStarted && deviceStops == 2);
        for (UInt32 i = 0; i < 3; ++i) { gTargets[i].uid.clear(); gTargets[i].procID = nullptr; }
    });

    puts("Engine: config, hardware controls, output levels, persistence, mixer, session, activity and lifecycle tests passed");
}
