// Offline FIR microbenchmark; this does not measure live CPU use or energy.
// Build with the service's optimization level:
//   xcrun clang++ -std=c++17 -O2 Service/Tests/ResamplerBench.cpp -o /tmp/sona-resampler-bench
//   /tmp/sona-resampler-bench
#include "../SonaResampler.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Keep the original sample loop as a numerical and timing reference. Both paths
// use the same kernel values, FIR length, cutoff and double-precision sums.
struct ReferenceResampler {
    inline static std::array<float, SonaResampler::kLobes * SonaResampler::kResolution + 1> kernel{};

    static void prepare() {
        constexpr double pi = 3.14159265358979323846;
        for (size_t i = 0; i < kernel.size(); ++i) {
            double x = double(i) / SonaResampler::kResolution;
            double a = pi * x / SonaResampler::kLobes;
            double window = 0.35875 + 0.48829 * std::cos(a) +
                            0.14128 * std::cos(2 * a) + 0.01168 * std::cos(3 * a);
            kernel[i] = float((x == 0 ? 1 : std::sin(pi * x) / (pi * x)) * window);
        }
    }

    static void sample(const SonaStereoRing::ReadView& input, double position,
                       double cutoff, int radius, float& left, float& right) {
        auto center = uint32_t(position);
        double fraction = position - center;
        double l = 0, r = 0, sum = 0;
        for (int tap = -radius; tap <= radius; ++tap) {
            double x = std::fabs((tap - fraction) * cutoff) * SonaResampler::kResolution;
            if (x >= SonaResampler::kLobes * SonaResampler::kResolution) continue;
            auto index = size_t(x);
            float weight = kernel[index] + float(x - index) * (kernel[index + 1] - kernel[index]);
            float sl = 0, sr = 0;
            int64_t offset = int64_t(center) + tap;
            if (offset >= 0) input.sample(uint32_t(offset), sl, sr);
            l += sl * double(weight);
            r += sr * double(weight);
            sum += weight;
        }
        left = float(l / sum);
        right = float(r / sum);
    }
};

volatile double checksum;

template<typename Resampler>
__attribute__((noinline)) double run(const SonaStereoRing::ReadView& input, double ratio, int count) {
    const double cutoff = SonaResampler::cutoff(ratio);
    const int radius = SonaResampler::radius(cutoff);
    double total = 0;
    for (int i = 0; i < count; ++i) {
        float l, r;
        Resampler::sample(input, radius + 32.37 + (i % 4096) * ratio, cutoff, radius, l, r);
        total += l + r;
    }
    checksum = total;
    return total;
}

template<typename Resampler>
double timed(const SonaStereoRing::ReadView& input, double ratio) {
    constexpr int count = 65536;
    auto start = std::chrono::steady_clock::now();
    run<Resampler>(input, ratio, count);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / count;
}

void checkSamples(SonaStereoRing::ReadView input) {
    // Include startup zero-padding, fractional phases, both ring-wrap directions,
    // large monotonic indices, and the renderer's accepted ratio extremes.
    for (uint64_t read : {uint64_t(0), uint64_t(SonaStereoRing::kCapacityFrames - 7), uint64_t(1) << 40}) {
        input.read = read;
        for (double ratio : {1.0 / 32, 44100.0 / 48000, 1.0, 48000.0 / 44100, 2.0, 192000.0 / 44100, 32.0}) {
            const double cutoff = SonaResampler::cutoff(ratio);
            const int radius = SonaResampler::radius(cutoff);
            auto check = [&](double position) {
                float l, r, referenceL, referenceR;
                SonaResampler::sample(input, position, cutoff, radius, l, r);
                ReferenceResampler::sample(input, position, cutoff, radius, referenceL, referenceR);
                assert(std::isfinite(l) && std::isfinite(r));
                assert(std::memcmp(&l, &referenceL, sizeof(l)) == 0 &&
                       std::memcmp(&r, &referenceR, sizeof(r)) == 0);
            };
            for (double position : {0.0, 0.00001, 0.5, 0.999999, 1.0, double(radius) - 0.00001, double(radius), double(radius) + 0.5})
                check(position);
            for (int i = 0; i < 1024; ++i) check(radius + 17.37 + (i % 128) * ratio);
        }
    }
    puts("Reference comparison: bit-exact samples across startup, phases, wrap and ratio extremes passed");
}

} // namespace

int main(int argc, const char* argv[]) {
    SonaResampler::prepare();
    ReferenceResampler::prepare();
    std::vector<float> data(SonaStereoRing::kCapacityFrames * 2);
    uint32_t random = 42;
    for (auto& sample : data) {
        random = random * 1664525u + 1013904223u;
        sample = float(int32_t(random)) * 0x1p-31f;
    }
    SonaStereoRing::ReadView input{data.data(), SonaStereoRing::kCapacityFrames - 1024,
                                 SonaStereoRing::kCapacityFrames, 0, 256};
    checkSamples(input);
    if (argc == 2 && std::strcmp(argv[1], "--check") == 0) return 0;
    puts("Offline FIR timings: median of 9 alternating runs, microseconds per stereo frame");
    for (auto rates : {std::pair<double, double>{48000, 48000}, {44100, 48000}, {48000, 44100},
                       {96000, 48000}, {192000, 44100}, {44100, 192000}}) {
        const double ratio = rates.first / rates.second;
        std::array<double, 9> reference, current;
        run<ReferenceResampler>(input, ratio, 2048);
        run<SonaResampler>(input, ratio, 2048);
        for (size_t i = 0; i < reference.size(); ++i) {
            if (i & 1) {
                current[i] = timed<SonaResampler>(input, ratio);
                reference[i] = timed<ReferenceResampler>(input, ratio);
            } else {
                reference[i] = timed<ReferenceResampler>(input, ratio);
                current[i] = timed<SonaResampler>(input, ratio);
            }
        }
        std::sort(reference.begin(), reference.end());
        std::sort(current.begin(), current.end());
        std::printf("%.0f -> %.0f: reference %.4f, current %.4f, change %+.1f%%\n",
                    rates.first, rates.second, reference[4], current[4], 100 * (current[4] / reference[4] - 1));
    }
}
