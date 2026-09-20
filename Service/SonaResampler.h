// Windowed-sinc asynchronous sample-rate conversion. No allocation or transcendental
// functions in the per-sample path. The shared kernel is prepared on the config queue.
// Algorithm background: https://www.dsprelated.com/freebooks/pasp/Windowed_Sinc_Interpolation.html
#pragma once
#include "SonaRingBuffer.h"
#include <algorithm>
#include <array>
#include <cmath>

class SonaResampler {
public:
    static constexpr int kLobes = 128;
    static constexpr int kResolution = 1024;
    static constexpr double kMaxCorrection = 500e-6;

    static void prepare() {
        if (sReady) return; // Config queue only, before any target IO starts.
        constexpr double pi = 3.14159265358979323846;
        for (size_t i = 0; i < sKernel.size(); ++i) {
            double x = double(i) / kResolution;
            double a = pi * x / kLobes;
            double window = 0.35875 + 0.48829 * std::cos(a) +
                            0.14128 * std::cos(2 * a) + 0.01168 * std::cos(3 * a);
            sKernel[i] = float((x == 0 ? 1 : std::sin(pi * x) / (pi * x)) * window);
        }
        sReady = true;
    }

    // Leave a transition band below the lower Nyquist limit, including clock correction.
    static double cutoff(double nominalRatio) {
        return 0.94 / std::max(1.0, nominalRatio * (1 + kMaxCorrection));
    }
    static int radius(double cutoff) { return int(std::ceil(kLobes / cutoff)); }

    static void sample(const SonaStereoRing::ReadView& input, double position,
                       double cutoff, int radius, float& left, float& right) {
        auto center = uint32_t(position);
        double fraction = position - center;
        double l = 0, r = 0, sum = 0;
        for (int tap = -radius; tap <= radius; ++tap) {
            double x = std::fabs((tap - fraction) * cutoff) * kResolution;
            if (x >= kLobes * kResolution) continue;
            auto index = size_t(x);
            float weight = sKernel[index] + float(x - index) * (sKernel[index + 1] - sKernel[index]);
            float sl = 0, sr = 0;
            int64_t offset = int64_t(center) + tap;
            // At startup there is no preceding audio. Zero-pad instead of discarding it.
            if (offset >= 0) input.sample(uint32_t(offset), sl, sr);
            l += sl * double(weight);
            r += sr * double(weight);
            sum += weight;
        }
        // Normalizing also maintains unity DC gain at every fractional phase.
        left = float(l / sum);
        right = float(r / sum);
    }

private:
    inline static std::array<float, kLobes * kResolution + 1> sKernel{};
    inline static bool sReady = false;
};

// Slow buffer-occupancy PLL: low-pass scheduling jitter, then PI control with bounded
// correction and slew. dt is audio duration, so behavior is independent of callback size.
class SonaClockTracker {
public:
    void reset() { mFiltered = mIntegral = mCorrection = 0; }
    double update(double errorSeconds, double dt) {
        dt = std::clamp(dt, 0.0, 0.25);
        mFiltered += (dt / (2.0 + dt)) * (errorSeconds - mFiltered);
        constexpr double limit = SonaResampler::kMaxCorrection;
        double proposedIntegral = mIntegral + mFiltered * 0.0001 * dt;
        double proposed = 0.02 * mFiltered + proposedIntegral;
        // Anti-windup: don't accumulate error driving further into a saturated limit.
        if (std::fabs(proposed) <= limit || proposed * mFiltered < 0)
            mIntegral = std::clamp(proposedIntegral, -limit, limit);
        double desired = std::clamp(0.02 * mFiltered + mIntegral, -limit, limit);
        double maxStep = 20e-6 * dt; // At most 20 ppm per second.
        mCorrection += std::clamp(desired - mCorrection, -maxStep, maxStep);
        return 1 + mCorrection;
    }
    double correction() const { return mCorrection; }
private:
    double mFiltered = 0, mIntegral = 0, mCorrection = 0;
};
