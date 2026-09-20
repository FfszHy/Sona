#include "../SonaTarget.h"
#include <os/log.h>
os_log_t gSonaServiceLog = nullptr;
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

constexpr double pi = 3.14159265358979323846;

// Compare against analytic sine samples at exact fractional positions (no FFT leakage).
static void tone(double source, double destination, double hz, bool reject = false) {
    auto ring = std::make_unique<SonaStereoRing>();
    std::vector<float> input(SonaStereoRing::kCapacityFrames * 2);
    for (size_t i = 0; i < input.size() / 2; ++i) {
        input[2 * i] = float(0.5 * std::sin(2 * pi * hz * i / source));
        input[2 * i + 1] = 0; // Exercise channel separation too.
    }
    ring->write(input.data(), input.size() / 2, 1);
    auto view = ring->readView();
    double ratio = source / destination;
    double cutoff = SonaResampler::cutoff(ratio);
    int radius = SonaResampler::radius(cutoff);
    double signal = 0, reference = 0, error = 0, oldError = 0;
    constexpr int count = 4096;
    for (int i = 0; i < count; ++i) {
        double position = radius + 17.37 + i * ratio;
        float left, right;
        SonaResampler::sample(view, position, cutoff, radius, left, right);
        assert(std::isfinite(left) && right == 0);
        double expected = 0.5 * std::sin(2 * pi * hz * position / source);
        signal += left * double(left); reference += expected * expected;
        error += (left - expected) * (left - expected);
        float l0, r0, l1, r1;
        view.sample(uint32_t(position), l0, r0); view.sample(uint32_t(position) + 1, l1, r1);
        double old = l0 + (l1 - l0) * (position - std::floor(position));
        oldError += (old - expected) * (old - expected);
    }
    double gainDB = 10 * std::log10(signal / reference);
    double errorDB = 10 * std::log10(error / reference);
    std::printf("%.0f -> %.0f Hz, tone %.0f: gain %.4f dB, error %.1f dB, old error %.1f dB\n",
                source, destination, hz, gainDB, errorDB, 10 * std::log10(oldError / reference));
    if (reject) assert(gainDB < -80);
    else { assert(std::fabs(gainDB) < 0.05); assert(errorDB < -65); }
}

static void clockTest(double ppm, double rate, int frames) {
    SonaClockTracker clock;
    double fill = 0, previous = 0, peak = 0;
    double dt = frames / rate;
    for (int i = 0; i < int(600 / dt); ++i) {
        // Scheduling jitter, not clock drift: alternating producer batch arrival offset.
        double jitter = 128 * std::sin(i * 1.7);
        double ratio = clock.update((fill + jitter) / rate, dt);
        double correction = ratio - 1;
        assert(std::fabs(correction) <= SonaResampler::kMaxCorrection + 1e-12);
        assert(std::fabs(correction - previous) <= 20e-6 * dt + 1e-12);
        fill += rate * dt * (ppm * 1e-6 - correction);
        peak = std::max(peak, std::fabs(fill)); previous = correction;
    }
    std::printf("Clock %.0f ppm at %.0f/%d: final %.2f ppm, peak fill error %.1f frames\n",
                ppm, rate, frames, clock.correction() * 1e6, peak);
    assert(std::fabs(clock.correction() * 1e6 - ppm) < 15);
    assert(peak / rate < 0.025);
    clock.reset(); assert(clock.correction() == 0);
}

static void renderTest(double duration) {
    auto target = std::make_unique<SonaTarget>();
    target->active = true; target->sourceRate = 48000; target->deviceRate = 44100;
    std::vector<float> block(256 * 2), output(256 * 2);
    AudioBufferList list = {};
    list.mNumberBuffers = 1; list.mBuffers[0] = {2, UInt32(output.size() * sizeof(float)), output.data()};
    double producer = 0; uint64_t total = 0;
    const int cycles = int(duration * 44100 / 256);
    for (int cycle = 0; cycle < cycles; ++cycle) {
        // Ten simulated minutes with a -250 ppm source clock offset.
        producer += 256.0 * 48000 / 44100 * (1 - 250e-6);
        while (producer >= 256) {
            for (int i = 0; i < 256; ++i, ++total) {
                block[2 * i] = float(0.3 * std::sin(2 * pi * 1000 * total / 48000));
                block[2 * i + 1] = -block[2 * i];
            }
            assert(target->ring.write(block.data(), 256, 1) == 256);
            producer -= 256;
        }
        target->render(&list);
        for (size_t i = 0; i < output.size(); i += 2) {
            assert(std::isfinite(output[i]) && std::fabs(output[i]) < 0.31);
            assert(std::fabs(output[i] + output[i + 1]) < 1e-6);
        }
        if (cycle > 50) assert(target->ring.available() > 256);
    }
    assert(target->underruns == 0);
    // Starvation must reset safely, then recover with new data.
    for (int i = 0; i < 30; ++i) target->render(&list);
    assert(target->underruns > 0);
    for (int i = 0; i < 20; ++i) target->ring.write(block.data(), 256, 1);
    target->render(&list);
    bool audible = false; for (float x : output) audible |= std::fabs(x) > 1e-4;
    assert(audible);
    target->deviceRate = 48000;
    target->render(&list); // Safe format-change flush.
    assert(target->ring.available() == 0);
    for (int i = 0; i < 12; ++i) target->ring.write(block.data(), 256, 1);
    target->render(&list);
    for (float x : output) assert(std::isfinite(x));
    // Overflow is reported, flushed and re-primed rather than leaving a discontinuity queued.
    for (int i = 0; i < 140; ++i) target->ring.write(block.data(), 256, 1);
    assert(target->ring.readView().discontinuities > 0);
    target->render(&list);
    assert(target->ring.available() == 0);
    for (int i = 0; i < 12; ++i) target->ring.write(block.data(), 256, 1);
    target->render(&list);
    audible = false; for (float x : output) audible |= std::fabs(x) > 1e-4;
    assert(audible);
    puts("Render: drift run, ring wrap, stereo isolation, starvation/overflow recovery and rate change passed");
}

// Clock-locked, equal rates: frames are copied bit-exactly with only the jitter cushion of
// latency. Losing the lock or changing rates falls back to the resampler with a fade.
static void passthroughTest() {
    auto target = std::make_unique<SonaTarget>();
    target->active = true; target->sourceRate = 48000; target->deviceRate = 48000;
    target->clockLocked = true;
    std::vector<float> written, output(256 * 2), block(256 * 2);
    AudioBufferList list = {};
    list.mNumberBuffers = 1; list.mBuffers[0] = {2, UInt32(output.size() * sizeof(float)), output.data()};
    uint32_t seed = 12345;
    auto produce = [&] {
        for (auto& x : block) { seed = seed * 1664525u + 1013904223u; x = float(int32_t(seed)) / 2147483648.0f; }
        assert(target->ring.write(block.data(), 256, 1) == 256);
        written.insert(written.end(), block.begin(), block.end());
    };
    // Below the cushion (margin + callback + one producer burst) nothing is emitted.
    produce(); produce(); produce();
    target->render(&list);
    assert(!target->passthrough && target->ring.available() == 768);
    produce();
    size_t consumed = 0;
    for (int cycle = 0; cycle < 2000; ++cycle) {
        target->render(&list);          // Lockstep: exactly one block in, one block out.
        if (cycle == 0) assert(target->passthrough);
        for (size_t i = 0; i < output.size(); ++i) {
            size_t k = consumed * 2 + i;
            if (consumed + i / 2 >= 128) assert(output[i] == written[k]); // Bit-exact after fade-in.
            else assert(std::fabs(output[i]) <= std::fabs(written[k]) + 1e-7f);
        }
        consumed += 256;
        produce();
        assert(target->ring.available() == 1024); // Latency stays at the cushion.
    }
    assert(target->underruns == 0);
    // Lock lost: fade out, re-prime through the resampler, no longer bit-exact but continuous.
    target->clockLocked = false;
    target->render(&list);
    assert(!target->passthrough && target->ring.available() == 0);
    for (int i = 0; i < 12; ++i) produce();
    target->render(&list);
    bool audible = false; for (float x : output) { assert(std::isfinite(x)); audible |= std::fabs(x) > 1e-4; }
    assert(audible && !target->passthrough);
    // A lock with unequal rates must still resample.
    target->deviceRate = 44100; target->clockLocked = true;
    target->render(&list);
    for (int i = 0; i < 12; ++i) produce();
    target->render(&list);
    assert(!target->passthrough);
    // Producer stall in passthrough is an underrun and a bounded re-prime.
    target->deviceRate = 48000; target->render(&list);
    for (int i = 0; i < 4; ++i) produce();
    target->render(&list); assert(target->passthrough);
    for (int i = 0; i < 4; ++i) target->render(&list);
    assert(target->underruns > 0 && !target->passthrough);
    puts("Passthrough: bit-exact lockstep copy, cushion latency, lock loss, rate mismatch and stall passed");
}

int main(int argc, char**) {
    SonaResampler::prepare();
    passthroughTest();
    for (auto rates : {std::pair<double, double>{48000, 48000}, {44100, 48000}, {48000, 44100},
                       {96000, 48000}, {192000, 44100}, {44100, 192000}})
        for (double hz : {100.0, 1000.0, 10000.0, 20000.0}) tone(rates.first, rates.second, hz);
    tone(96000, 48000, 30000, true);
    tone(192000, 44100, 40000, true);
    tone(48000, 44100, 23000, true);
    tone(96000, 44100, 22100, true);
    // DC unity, mono downmix and extra channel silence through the actual renderer.
    auto mono = std::make_unique<SonaTarget>();
    mono->active = true; mono->sourceRate = 48000; mono->deviceRate = 48000;
    std::vector<float> dc(4096 * 2);
    for (int i = 0; i < 4096; ++i) { dc[2*i] = 0.2f; dc[2*i+1] = 0.6f; }
    for (int i = 0; i < 4096; i += 512) mono->ring.write(dc.data() + 2 * i, 512, 1);
    float monoOut[256] = {};
    AudioBufferList monoList = {}; monoList.mNumberBuffers = 1;
    monoList.mBuffers[0] = {1, sizeof(monoOut), monoOut};
    mono->render(&monoList);
    assert(std::fabs(monoOut[255] - 0.4f) < 1e-5);
    float multiOut[256 * 4];
    std::fill(std::begin(multiOut), std::end(multiOut), 1.0f);
    monoList.mBuffers[0] = {4, sizeof(multiOut), multiOut};
    mono->render(&monoList);
    assert(std::fabs(multiOut[0] - 0.2f) < 1e-5 && std::fabs(multiOut[1] - 0.6f) < 1e-5);
    for (int i = 0; i < 256; ++i) assert(multiOut[4*i+2] == 0 && multiOut[4*i+3] == 0);
    for (double ppm : {-300.0, -100.0, 0.0, 100.0, 300.0})
        for (int frames : {128, 512, 1024}) clockTest(ppm, 48000, frames);
    auto start = std::chrono::steady_clock::now();
    double duration = argc > 1 ? 5 : 600;
    renderTest(duration);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("Offline render scenario: %.3f s wall time for %.0f s stereo audio (not live CPU/energy)\n", elapsed, duration);
    puts("Resampler quality and clock tests passed");
}
