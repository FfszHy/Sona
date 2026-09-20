//
//  SonaEngine.h
//  Sona Audio Service audio engine: physical outputs, routing, gain, mixing, clock and
//  hardware controls. Everything that touches the Core Audio client HAL lives here.
//
//  Threads:
//    control queue  — all configuration, HAL property access, persistence, status pushes
//    mixer thread   — consumes client rings from the shared region, writes sink rings
//    device IOProcs — one per physical output, resample out of their sink ring
//

#ifndef SonaEngine_h
#define SonaEngine_h

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <cstdint>
#include <string>

// Called on the control queue whenever the plug-in needs an update.
struct SonaEnginePushes {
    void (*status)(void* ctx, CFDictionaryRef status) = nullptr;
    void (*clients)(void* ctx, CFArrayRef clients) = nullptr;
    void (*config)(void* ctx, CFDictionaryRef config) = nullptr;
    void (*master)(void* ctx, float volume, bool mute) = nullptr;
    void* ctx = nullptr;
};

void SonaEngineStart(dispatch_queue_t controlQueue, const SonaEnginePushes& pushes, const char* stateFile);

// Session (control queue). Attach publishes a region to the mixer thread and the sink IOProcs; the
// caller keeps it mapped. Detach unpublishes it and returns a ticket recording where every reader
// stood at that moment. The region may be unmapped only once SonaEngineRegionRetired(ticket) is
// true (usually right away; poll again later otherwise). A reader that never confirms keeps the
// mapping alive forever; the engine never declares a region free on a timeout.
struct SonaEngineRetireTicket {
    static constexpr unsigned kSinks = 8;
    uint64_t mixerPass = 0;
    uint64_t sinkPass[kSinks] = {};
};
void SonaEngineAttachRegion(void* region, uint64_t generation);
SonaEngineRetireTicket SonaEngineDetachRegion();
bool SonaEngineRegionRetired(const SonaEngineRetireTicket& ticket);
void SonaEngineClientAdd(uint32_t slot, uint32_t instance, uint32_t clientID, int32_t pid, const std::string& bundleID);
void SonaEngineClientRemove(uint32_t slot, uint32_t instance);
void SonaEngineSetFormat(double sampleRate, uint32_t formatGeneration);
void SonaEngineSetIORunning(bool running);

// Control plane forwarded from the plug-in facade (control queue).
void SonaEngineApplyConfig(CFDictionaryRef config);
void SonaEngineSetOutputLevel(const std::string& uid, bool hasVolume, float volume, bool hasMute, bool mute);
void SonaEngineSetMasterVolume(float volume);
void SonaEngineSetMasterMute(bool mute);

// Snapshots (control queue). Caller releases.
CFDictionaryRef SonaEngineCopyStatus();
CFArrayRef      SonaEngineCopyClients();
CFDictionaryRef SonaEngineCopyConfig();
void            SonaEngineMasterLevel(float& volume, bool& mute);

#endif /* SonaEngine_h */
