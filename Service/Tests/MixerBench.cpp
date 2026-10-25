// Offline accumulation/commit benchmark. Does not open devices or start the mixer thread.
#include <os/log.h>
os_log_t gSonaServiceLog = nullptr;
#include "../SonaEngine.cpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>

// Original commit loop, using the same current ring writer as the optimized path.
// The comparison isolates removing the intermediate copy and inactive PCM work.
static void referenceCommit(UInt32 defaultMask, double commitTo) {
    const uint32_t frames = uint32_t(std::min<double>(std::floor(commitTo) - gMixedUpTo, kMixWindowFrames / 2));
    static float scratch[kMixWindowFrames * 2];
    for (UInt32 sink = 0; sink < kMaxTargets; ++sink) {
        auto& t = gTargets[sink];
        float* accum = gMixAccum[sink];
        for (uint32_t f = 0; f < frames; ++f) {
            uint32_t index = uint32_t((uint64_t(gMixedUpTo) + f) & kMixWindowMask) * 2;
            scratch[f * 2] = accum[index]; scratch[f * 2 + 1] = accum[index + 1];
            accum[index] = accum[index + 1] = 0;
        }
        if (!t.active.load(std::memory_order_acquire)) continue;
        t.ring.write(scratch, frames, defaultMask & (1u << sink) ? SonaMasterGain() : gOutputControls[sink].gain.load());
    }
    gMixedUpTo += frames;
}

static volatile double checksum;

template<bool Reference>
static double run(uint32_t outputs, uint32_t frames, int iterations) {
    SonaMixReset(0);
    for (UInt32 sink = 0; sink < kMaxTargets; ++sink) {
        gTargets[sink].active = sink < outputs;
        gTargets[sink].ring.flush();
        gOutputControls[sink].gain = 0.5f;
    }
    std::array<float, 512 * 2> pcm;
    pcm.fill(0.25f);
    SonaTransportReadBlock block = {};
    block.desc.frames = frames; block.first = pcm.data(); block.firstFrames = frames;
    double total = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        for (uint32_t sink = 0; sink < outputs; ++sink) SonaMixAccumulate(sink, gMixedUpTo, 0, block, 1);
        if constexpr (Reference) referenceCommit(1, gMixedUpTo + frames);
        else SonaMixCommit(1, gMixedUpTo + frames);
        for (uint32_t sink = 0; sink < outputs; ++sink) {
            float l, r; gTargets[sink].ring.sample(frames - 1, l, r);
            assert(l == (sink == 0 ? 0.25f : 0.125f) && r == l);
            total += l + r;
            gTargets[sink].ring.consume(frames);
        }
    }
    checksum = total;
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations;
}

int main() {
    gHardwareVolume = true; gHardwareMute = true;
    puts("Offline accumulation/commit timings: median of 9 alternating runs, microseconds per block");
    for (uint32_t frames : {128u, 512u}) for (uint32_t outputs : {1u, 2u, 8u}) {
        std::array<double, 9> reference, current;
        run<true>(outputs, frames, 100); run<false>(outputs, frames, 100);
        for (size_t i = 0; i < reference.size(); ++i) {
            if (i & 1) {
                current[i] = run<false>(outputs, frames, 10000);
                reference[i] = run<true>(outputs, frames, 10000);
            } else {
                reference[i] = run<true>(outputs, frames, 10000);
                current[i] = run<false>(outputs, frames, 10000);
            }
        }
        std::sort(reference.begin(), reference.end()); std::sort(current.begin(), current.end());
        std::printf("%u frames / %u outputs: reference %.3f, current %.3f, change %+.1f%%\n",
                    frames, outputs, reference[4], current[4], 100 * (current[4] / reference[4] - 1));
    }
}
