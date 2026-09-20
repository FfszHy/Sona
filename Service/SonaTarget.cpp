//
//  SonaTarget.cpp
//

#include "SonaTarget.h"
#include <os/log.h>
#include <cmath>

extern os_log_t gSonaServiceLog;
#define SonaLog(fmt, ...) os_log(gSonaServiceLog ? gSonaServiceLog : OS_LOG_DEFAULT, "SonaService: " fmt, ##__VA_ARGS__)

// MARK: - HAL helpers

AudioObjectID SonaTranslateUID(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;
    CFStringRef cfUID = CFStringCreateWithCString(kCFAllocatorDefault, uid.c_str(), kCFStringEncodingUTF8);
    if (!cfUID) return kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = { kAudioHardwarePropertyTranslateUIDToDevice,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                              sizeof(cfUID), &cfUID, &size, &device);
    CFRelease(cfUID);
    return err == noErr ? device : kAudioObjectUnknown;
}

std::string SonaCopyDeviceUID(AudioObjectID device) {
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyDeviceUID,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    CFStringRef cfUID = nullptr;
    UInt32 size = sizeof(cfUID);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &cfUID) != noErr || !cfUID) return "";
    char buf[512] = {0};
    CFStringGetCString(cfUID, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cfUID);
    return buf;
}

bool SonaDeviceHasOutput(AudioObjectID device) {
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyStreamConfiguration,
                                        kAudioObjectPropertyScopeOutput,
                                        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0) return false;
    AudioBufferList* list = (AudioBufferList*)malloc(size);
    bool has = false;
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
            if (list->mBuffers[i].mNumberChannels > 0) { has = true; break; }
        }
    }
    free(list);
    return has;
}

AudioObjectID SonaFindBuiltInOutputDevice() {
    AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr) return kAudioObjectUnknown;
    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID* devices = (AudioObjectID*)malloc(size);
    AudioObjectID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices) == noErr) {
        for (UInt32 i = 0; i < count; ++i) {
            AudioObjectPropertyAddress tAddr = { kAudioDevicePropertyTransportType,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
            UInt32 transport = 0, tSize = sizeof(transport);
            if (AudioObjectGetPropertyData(devices[i], &tAddr, 0, nullptr, &tSize, &transport) == noErr &&
                transport == kAudioDeviceTransportTypeBuiltIn && SonaDeviceHasOutput(devices[i])) {
                found = devices[i];
                break;
            }
        }
    }
    free(devices);
    return found;
}

// MARK: - Lifecycle (config queue)

bool SonaTarget::isDeviceAlive() const {
    if (deviceID == kAudioObjectUnknown) return false;
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyDeviceIsAlive,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    UInt32 alive = 0, size = sizeof(alive);
    return AudioObjectGetPropertyData(deviceID, &addr, 0, nullptr, &size, &alive) == noErr && alive != 0;
}

void SonaTarget::refreshDeviceRate() {
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyNominalSampleRate,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    Float64 rate = 0; UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(deviceID, &addr, 0, nullptr, &size, &rate) == noErr && rate > 0) {
        deviceRate.store(rate, std::memory_order_relaxed);
    }
}

OSStatus SonaTarget::RateListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* inClientData) {
    static_cast<SonaTarget*>(inClientData)->refreshDeviceRate();
    return noErr;
}

bool SonaTarget::open() {
    close();
    SonaResampler::prepare();
    deviceID = SonaTranslateUID(uid);
    if (deviceID == kAudioObjectUnknown || !SonaDeviceHasOutput(deviceID)) {
        SonaLog("target '%s' not found or has no output", uid.c_str());
        deviceID = kAudioObjectUnknown;
        return false;
    }
    refreshDeviceRate();
    AudioObjectPropertyAddress rateAddr = { kAudioDevicePropertyNominalSampleRate,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(deviceID, &rateAddr, RateListener, this);

    OSStatus err = AudioDeviceCreateIOProcID(deviceID, IOProc, this, &procID);
    if (err != noErr) {
        SonaLog("AudioDeviceCreateIOProcID failed for '%s': %d", uid.c_str(), (int)err);
        procID = nullptr;
        deviceID = kAudioObjectUnknown;
        return false;
    }
    mPrimed = false;
    mPhase = 0.0;
    mClock.reset();
    ring.flush();
    SonaLog("opened target '%s' (id %u, %.0f Hz)", uid.c_str(), (unsigned)deviceID, deviceRate.load());
    return true;
}

void SonaTarget::start() {
    if (procID && !ioStarted) {
        ring.flush();
        mDiscontinuities = ring.readView().discontinuities;
        mPrimed = false;
        mPassthroughMode = false;
        passthrough.store(false, std::memory_order_relaxed);
        clockLocked.store(false, std::memory_order_relaxed);   // Fresh timeline; the driver relocks.
        mPhase = 0.0;
        mClock.reset();
        mSourceRate = mDeviceRate = 0;
        mLastLeft = mLastRight = 0;
        ++mAnchorGeneration;      // New IO run: device sample time restarts, old anchors are stale.
        if (auto* cell = clockCell.load(std::memory_order_acquire)) SonaClockClear(cell);
        OSStatus err = AudioDeviceStart(deviceID, procID);
        if (err == noErr) {
            ioStarted = true;
            active.store(true, std::memory_order_release);
        } else {
            SonaLog("AudioDeviceStart failed for '%s': %d", uid.c_str(), (int)err);
        }
    }
}

void SonaTarget::stop() {
    if (procID && ioStarted) {
        active.store(false, std::memory_order_release);
        passthrough.store(false, std::memory_order_relaxed);
        AudioDeviceStop(deviceID, procID);
        ioStarted = false;
        if (auto* cell = clockCell.load(std::memory_order_acquire)) SonaClockClear(cell);   // IOProc stopped: single writer
    }
}

void SonaTarget::close() {
    stop();
    if (procID) {
        AudioDeviceDestroyIOProcID(deviceID, procID);
        procID = nullptr;
    }
    if (deviceID != kAudioObjectUnknown) {
        AudioObjectPropertyAddress rateAddr = { kAudioDevicePropertyNominalSampleRate,
                                                kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
        AudioObjectRemovePropertyListener(deviceID, &rateAddr, RateListener, this);
        deviceID = kAudioObjectUnknown;
    }
    active.store(false, std::memory_order_release);
}

// MARK: - Realtime rendering (IOProc thread)

OSStatus SonaTarget::IOProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                            const AudioTimeStamp*, AudioBufferList* outOutputData,
                            const AudioTimeStamp* inOutputTime, void* inClientData) {
    static_cast<SonaTarget*>(inClientData)->render(outOutputData, inOutputTime);
    return noErr;
}

// Publish this device's timeline so the Sona device clock can follow it. A sample-time jump
// that does not match the frames we rendered means the device restarted or resynced; that
// starts a new generation so the driver re-locks instead of reporting a bogus rate.
void SonaTarget::publishAnchor(const AudioTimeStamp* t, double rate) {
    const bool master = clockMaster.load(std::memory_order_relaxed);
    // Reader pass around the shared cell (odd = inside). The increment precedes the pointer load and
    // both are sequentially consistent, so once the engine has cleared clockCell and seen this pass
    // end (or none in flight) this thread can never reach the old region again; see SonaEngine.cpp.
    struct Pass {
        std::atomic<uint64_t>& seq;
        explicit Pass(std::atomic<uint64_t>& s) : seq(s) { seq.fetch_add(1, std::memory_order_seq_cst); }
        ~Pass() { seq.fetch_add(1, std::memory_order_release); }
    } pass(clockPass);
    auto* cell = clockCell.load(std::memory_order_seq_cst);
    if (!master || !cell || !t || !(t->mFlags & kAudioTimeStampSampleTimeValid) || !(t->mFlags & kAudioTimeStampHostTimeValid)) {
        if (mWasMaster) { if (cell) SonaClockClear(cell); mWasMaster = false; }
        mLastAnchorSample = -1;
        return;
    }
    if (!mWasMaster || mLastAnchorSample < 0 || t->mSampleTime < mLastAnchorSample ||
        t->mSampleTime - mLastAnchorSample > 4 * kMaxFramesPerCycle) {
        ++mAnchorGeneration;
    }
    mWasMaster = true;
    mLastAnchorSample = t->mSampleTime;
    SonaClockPublish(cell, {t->mHostTime, t->mSampleTime, rate, mAnchorGeneration});
}

void SonaTarget::render(AudioBufferList* out, const AudioTimeStamp* outputTime) {
    // Silence everything first; we only fill the first two channels of the first stream.
    for (UInt32 b = 0; b < out->mNumberBuffers; ++b) {
        if (out->mBuffers[b].mData) memset(out->mBuffers[b].mData, 0, out->mBuffers[b].mDataByteSize);
    }
    if (!active.load(std::memory_order_acquire) || out->mNumberBuffers == 0) return;

    AudioBuffer& buf = out->mBuffers[0];
    const UInt32 channels = buf.mNumberChannels;
    if (channels == 0 || !buf.mData) return;
    const UInt32 frames = buf.mDataByteSize / (channels * sizeof(float));
    float* dst = (float*)buf.mData;

    const double src = sourceRate.load(std::memory_order_relaxed);
    const double dstRate = deviceRate.load(std::memory_order_relaxed);
    if (src <= 0 || dstRate <= 0) return;
    publishAnchor(outputTime, dstRate);

    // Invalid/extreme formats are rejected before any indexing or unbounded work.
    const double nominalRatio = src / dstRate;
    if (!std::isfinite(nominalRatio) || nominalRatio < 1.0 / 32 || nominalRatio > 32) return;

    auto fadeOut = [&] {
        UInt32 count = std::min(frames, kFadeFrames);
        for (UInt32 f = 0; f < count; ++f) {
            float gain = 1.0f - float(f + 1) / count;
            if (channels == 1) dst[f] = (mLastLeft + mLastRight) * 0.5f * gain;
            else { dst[f * channels] = mLastLeft * gain; dst[f * channels + 1] = mLastRight * gain; }
        }
        mLastLeft = mLastRight = 0;
    };
    auto reprime = [&] {
        ring.flush(); mPrimed = false; mClock.reset();
        passthrough.store(false, std::memory_order_relaxed);
        fadeOut();
    };
    if ((mSourceRate != 0 && src != mSourceRate) || (mDeviceRate != 0 && dstRate != mDeviceRate)) {
        // Discard queued frames from the previous format; restart filter and PLL together.
        mSourceRate = src; mDeviceRate = dstRate;
        reprime();
        return;
    }
    mSourceRate = src; mDeviceRate = dstRate;
    // Passthrough: the Sona device clock is slaved to this device and rates are equal, so the
    // producer writes exactly as many frames as we consume. No filter, no PLL, no delay beyond
    // a small jitter cushion. Any other target still resamples against its own clock.
    const bool wantPassthrough = clockLocked.load(std::memory_order_relaxed) && src == dstRate;
    if (wantPassthrough != mPassthroughMode) {
        mPassthroughMode = wantPassthrough;
        if (mPrimed) { reprime(); return; }
    }
    const auto input = ring.readView();
    if (input.discontinuities != mDiscontinuities) {
        mDiscontinuities = input.discontinuities;
        reprime();
        return;
    }

    if (mPassthroughMode) {
        // Occupancy only rises at producer writes, so priming happens right after one; the
        // cushion must survive one full producer period plus this callback plus jitter.
        const uint32_t cushion = kPassthroughMargin + frames + input.burst;
        if (!mPrimed) {
            if (input.available < cushion) return;
            mPrimed = true;
            mFadeRemaining = kFadeFrames;
            passthrough.store(true, std::memory_order_relaxed);
        }
        if (input.available < frames) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            reprime();
            return;
        }
        // Lockstep clocks cannot drift, so a large excess means the timeline jumped. Re-prime
        // with a fade rather than letting latency grow without bound.
        if (input.available > cushion + 4 * kMaxFramesPerCycle) { reprime(); return; }
        for (UInt32 f = 0; f < frames; ++f) {
            float l, r;
            input.sample(f, l, r);
            if (mFadeRemaining) {
                float gain = float(kFadeFrames - mFadeRemaining + 1) / kFadeFrames;
                l *= gain; r *= gain; --mFadeRemaining;
            }
            if (channels == 1) dst[f] = (l + r) * 0.5f;
            else { dst[f * channels] = l; dst[f * channels + 1] = r; }
            mLastLeft = l; mLastRight = r;
        }
        ring.consume(frames);
        return;
    }

    const double cutoff = SonaResampler::cutoff(nominalRatio);
    const int radius = SonaResampler::radius(cutoff);
    if (!mPrimed) {
        // Include FIR history/lookahead, enough source data for a full device callback, and
        // one producer period (occupancy dips by that much between writes).
        double minimum = 2 * radius + nominalRatio * (1 + SonaResampler::kMaxCorrection) * frames + 2;
        double primeAt = std::max(double(kTargetFillFrames), minimum + input.burst + 128);
        if (input.available < primeAt) return;
        mPrimed = true;
        mPhase = 0;
        mClock.reset();
        mTargetFill = input.available + radius;
        mFadeRemaining = kFadeFrames;
    }
    double ratio = nominalRatio * mClock.update((double(input.available) - mTargetFill) / src,
                                                double(frames) / dstRate);
    double needed = mPhase + ratio * frames + radius + 1;
    if (input.available < needed) {
        underruns.fetch_add(1, std::memory_order_relaxed);
        // Exceptional loss is a bounded re-prime, not a large pitch/speed excursion.
        reprime();
        return;
    }

    double phase = mPhase;
    for (UInt32 f = 0; f < frames; ++f) {
        float l, r;
        SonaResampler::sample(input, phase, cutoff, radius, l, r);
        if (mFadeRemaining) {
            float gain = float(kFadeFrames - mFadeRemaining + 1) / kFadeFrames;
            l *= gain; r *= gain; --mFadeRemaining;
        }
        if (channels == 1) dst[f] = (l + r) * 0.5f;
        else { dst[f * channels] = l; dst[f * channels + 1] = r; }
        mLastLeft = l; mLastRight = r;
        phase += ratio;
    }
    // Keep left-hand filter history in the ring; the producer cannot overwrite it.
    uint32_t consumed = uint32_t(phase) > uint32_t(radius) ? uint32_t(phase) - radius : 0;
    mPhase = phase - consumed;
    ring.consume(consumed);
}
