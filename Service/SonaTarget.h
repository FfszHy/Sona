//
//  SonaTarget.h
//  A real output device that the Sona virtual device forwards audio to.
//
//  Lives in Sona Audio Service, the only Sona process allowed to use the Core Audio client
//  HAL. The mixer thread writes into `ring`; this device's IOProc resamples out of it.
//

#ifndef SonaTarget_h
#define SonaTarget_h

#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <string>
#include "SonaRingBuffer.h"
#include "SonaResampler.h"
#include "../Shared/SonaTransport.h"

class SonaTarget {
public:
    static constexpr uint32_t kMaxFramesPerCycle = 4096;
    static constexpr uint32_t kTargetFillFrames  = 2048;   // steady-state buffered frames (source rate)
    static constexpr uint32_t kPassthroughMargin = 512;    // scheduling-jitter cushion when clock-locked

    SonaTarget() = default;
    SonaTarget(const SonaTarget&) = delete;
    SonaTarget& operator=(const SonaTarget&) = delete;

    // --- Configuration (config queue only) -------------------------------------------------
    std::string   uid;                     // empty = slot unused
    AudioObjectID deviceID = kAudioObjectUnknown;
    AudioDeviceIOProcID procID = nullptr;
    bool          ioStarted = false;

    // Resolves `uid` to a device and creates the IOProc. Returns true if the device is usable.
    bool open();
    void close();
    void start();
    void stop();
    bool isDeviceAlive() const;

    // --- Shared state ----------------------------------------------------------------------
    std::atomic<bool>     active{false};       // IOProc is running
    std::atomic<double>   deviceRate{0.0};
    std::atomic<double>   sourceRate{48000.0};
    std::atomic<uint32_t> underruns{0};
    // clockMaster (config queue): the virtual device is meant to slave its clock to this
    // target, so the IOProc publishes anchors into the shared clock cell. clockLocked (mixer
    // thread copies the plug-in's report): the virtual timeline is actually following those
    // anchors. With equal rates and a lock, frames are copied 1:1.
    std::atomic<bool>     clockMaster{false};
    std::atomic<bool>     clockLocked{false};
    std::atomic<bool>     passthrough{false};  // Reported: currently copying instead of resampling
    std::atomic<SonaTransportClock*> clockCell{nullptr};   // in the shared region; null when offline
    // Odd while the IOProc is inside a use of clockCell. The engine clears clockCell and then waits
    // for this counter to move past the use in flight before the region may be unmapped.
    std::atomic<uint64_t> clockPass{0};

    SonaStereoRing ring;

    // Per-cycle accumulator for clients explicitly routed to this target. Touched only by the
    // virtual device IO thread between ProcessOutput and WriteMix of the same cycle.
    float    accum[kMaxFramesPerCycle * 2] = {};
    bool     accumDirty = false;

    // Consumer-side resampler state (IOProc thread only).
    void render(AudioBufferList* out, const AudioTimeStamp* outputTime = nullptr);

private:
    double mPhase = 0.0;
    SonaClockTracker mClock;
    double mSourceRate = 0, mDeviceRate = 0;
    double mTargetFill = 0;
    uint64_t mDiscontinuities = 0;
    UInt32 mFadeRemaining = 0;
    float mLastLeft = 0, mLastRight = 0;
    static constexpr UInt32 kFadeFrames = 128;
    bool   mPrimed = false;
    bool   mPassthroughMode = false;
    uint64_t mAnchorGeneration = 0;
    double mLastAnchorSample = -1;
    bool   mWasMaster = false;

    void publishAnchor(const AudioTimeStamp* outputTime, double rate);

    static OSStatus IOProc(AudioObjectID inDevice,
                           const AudioTimeStamp* inNow,
                           const AudioBufferList* inInputData,
                           const AudioTimeStamp* inInputTime,
                           AudioBufferList* outOutputData,
                           const AudioTimeStamp* inOutputTime,
                           void* inClientData);
    static OSStatus RateListener(AudioObjectID inObjectID, UInt32 inNumberAddresses,
                                 const AudioObjectPropertyAddress* inAddresses, void* inClientData);
    void refreshDeviceRate();
};

// Helpers shared with the driver body.
AudioObjectID SonaTranslateUID(const std::string& uid);
std::string   SonaCopyDeviceUID(AudioObjectID device);
AudioObjectID SonaFindBuiltInOutputDevice();
bool          SonaDeviceHasOutput(AudioObjectID device);

#endif /* SonaTarget_h */
