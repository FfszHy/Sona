// Driver clock tests: Sona zero timestamps follow the service master's anchor, keep host time
// monotonic through lock/unlock, and fall back to host time on a stale anchor. No HAL, no XPC.

#include <cassert>
#include <cmath>
#include <cstdio>
#include "../SonaDriver.cpp"

int main() {
    const double tps = 1e9;                     // fake host ticks per second
    gHostTicksPerSecond = tps; gHostTicksPerFrame = tps / 48000; gSampleRate = 48000;
    gAnchorHostTime = (UInt64)tps; gNumberTimeStamps = 0; gClockLock = SonaClockLock{};
    const double devicePPM = 200;
    const double deviceRate = 48000 * (1 + devicePPM * 1e-6);
    UInt64 lastHost = 0; Float64 lastSample = -1;
    Float64 sample; UInt64 host; bool locked = false;
    UInt64 lockHost = 0; Float64 lockSample = 0;
    auto step = [&](double seconds, bool publish) {
        UInt64 now = (UInt64)(tps * (1 + seconds));
        SonaClockAnchor anchor;
        if (publish) {
            double deviceSample = 1000 + (seconds + 0.02) * deviceRate;
            anchor = {now + (UInt64)(0.02 * tps), deviceSample, 48000, 7};
        }
        SonaComputeZeroTimeStamp(now, anchor, 1, &sample, &host, &locked);
        assert(host <= now && host >= lastHost && sample >= lastSample);
        if (sample != lastSample) {
            if (lastSample >= 0) {
                double period = double(host - lastHost) / (gHostTicksPerFrame * kDevice_RingBufferSize);
                assert(std::fabs(period - 1) < 0.0021);
            }
            lastHost = host; lastSample = sample;
        }
    };
    for (double t = 0; t < 2; t += 0.01) step(t, false);
    assert(!locked);
    for (double t = 2; t < 5; t += 0.01) step(t, true);
    assert(locked);
    lockHost = lastHost; lockSample = lastSample;
    for (double t = 5; t < 125; t += 0.01) step(t, true);
    double measuredRate = (lastSample - lockSample) / (double(lastHost - lockHost) / tps);
    double measuredPPM = (measuredRate / 48000 - 1) * 1e6;
    std::printf("Clock lock: Sona timeline runs at %.1f ppm vs device %.0f ppm\n", measuredPPM, devicePPM);
    assert(std::fabs(measuredPPM - devicePPM) < 3);
    for (double t = 125; t < 128; t += 0.01) step(t, false);
    assert(!locked);
    UInt64 freeHost = lastHost; Float64 freeSample = lastSample;
    for (double t = 128; t < 158; t += 0.01) step(t, false);
    double freePPM = ((lastSample - freeSample) / (double(lastHost - freeHost) / tps) / 48000 - 1) * 1e6;
    assert(std::fabs(freePPM) < 3);
    // New anchor generation (device restarted): relocks without a jump.
    for (double t = 158; t < 160; t += 0.01) {
        UInt64 now = (UInt64)(tps * (1 + t));
        SonaClockAnchor anchor = {now, (t - 158) * deviceRate, 48000, 8};
        SonaComputeZeroTimeStamp(now, anchor, 1, &sample, &host, &locked);
        assert(host >= lastHost && sample >= lastSample);
        lastHost = host; lastSample = sample;
    }
    assert(locked && gClockLock.generation == 8);
    // Service reconnected (new region generation): the old lock is dropped immediately.
    {
        UInt64 now = (UInt64)(tps * 161);
        SonaClockAnchor stale = {now, 0, 48000, 8};
        SonaComputeZeroTimeStamp(now, stale, 2, &sample, &host, &locked);
        assert(gClockLock.region == 2);   // relocked onto the new region within the same call
    }
    puts("Driver clock lock tests passed");
}
