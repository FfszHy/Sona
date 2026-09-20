//
//  SonaRingBuffer.h
//  Single-producer / single-consumer ring buffer of interleaved stereo float frames.
//
//  Producer: the Sona Audio Service mixer thread.
//  Consumer: the IOProc of a real output device (see SonaTarget.cpp).
//  Both indices grow monotonically; capacity is a power of two.
//

#ifndef SonaRingBuffer_h
#define SonaRingBuffer_h

#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>

class SonaStereoRing {
public:
    static constexpr uint32_t kCapacityFrames = 1u << 15;   // 32768 frames (~0.68 s @ 48 kHz)
    static constexpr uint32_t kMask = kCapacityFrames - 1;

    // One acquire per render block. Retain the entire FIR history until consume().
    struct ReadView {
        const float* data;
        uint64_t read;
        uint32_t available;
        uint64_t discontinuities;
        uint32_t burst;            // frames per producer write; occupancy dips by this between writes
        void sample(uint32_t offset, float& l, float& r) const {
            uint32_t index = uint32_t((read + offset) & kMask) * 2;
            l = data[index]; r = data[index + 1];
        }
    };
    ReadView readView() const {
        uint64_t read = mRead.load(std::memory_order_relaxed);
        uint64_t written = mWrite.load(std::memory_order_acquire);
        return {mData, read, uint32_t(written - read), mDiscontinuities.load(std::memory_order_acquire),
                mBurst.load(std::memory_order_relaxed)};
    }

    uint32_t available() const {
        return (uint32_t)(mWrite.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire));
    }
    uint32_t space() const { return kCapacityFrames - available(); }

    // Writes up to `frames` stereo frames scaled by `gain`. Frames that do not fit are dropped
    // so latency stays bounded when the consumer stalls.
    uint32_t write(const float* interleaved, uint32_t frames, float gain) {
        uint32_t requested = frames;
        frames = std::min(frames, space());
        if (frames < requested) mDiscontinuities.fetch_add(1, std::memory_order_release);
        mBurst.store(requested, std::memory_order_relaxed);
        uint64_t w = mWrite.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t idx = (uint32_t)((w + i) & kMask) * 2;
            mData[idx]     = interleaved[i * 2]     * gain;
            mData[idx + 1] = interleaved[i * 2 + 1] * gain;
        }
        mWrite.store(w + frames, std::memory_order_release);
        return frames;
    }

    // Reads the frame at read position + offset (caller guarantees offset < available()).
    inline void sample(uint32_t offset, float& l, float& r) const {
        uint64_t rp = mRead.load(std::memory_order_relaxed);
        uint32_t idx = (uint32_t)((rp + offset) & kMask) * 2;
        l = mData[idx];
        r = mData[idx + 1];
    }

    void consume(uint32_t frames) { mRead.fetch_add(frames, std::memory_order_release); }

    // Producer-side: the source timeline jumped; the consumer re-primes with a fade.
    void markDiscontinuity() { mDiscontinuities.fetch_add(1, std::memory_order_release); }

    // Consumer-side flush: discard everything currently buffered.
    void flush() { mRead.store(mWrite.load(std::memory_order_acquire), std::memory_order_release); }

private:
    float mData[kCapacityFrames * 2] = {};
    std::atomic<uint64_t> mWrite{0};
    std::atomic<uint64_t> mRead{0};
    std::atomic<uint64_t> mDiscontinuities{0};
    std::atomic<uint32_t> mBurst{0};
};

#endif /* SonaRingBuffer_h */
