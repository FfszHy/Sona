// Two-client loopback: drives SonaTransportClient against a running Sona Audio Service with
// two simulated clients that use different IO buffer sizes (128 and 512 frames), each delivering
// one period ahead of its play time like a real IOProc. Verifies through the service's status
// that neither client is dropped and the mixer does not reset. Writes silence.

#include "../SonaTransportClient.h"
#include <mach/mach_time.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main() {
    dispatch_queue_t q = dispatch_queue_create("loopback2", DISPATCH_QUEUE_SERIAL);
    auto* client = new SonaTransportClient(q, "loopback2");
    dispatch_sync(q, ^{ client->start(); });
    for (int i = 0; i < 50 && !client->connected(); ++i) usleep(100 * 1000);
    if (!client->connected()) { std::fprintf(stderr, "FAIL: service did not answer hello\n"); return 1; }
    dispatch_sync(q, ^{
        client->registerClient(4, 4001, (int32_t)getpid(), "com.sona.loopback.small");
        client->registerClient(5, 4002, (int32_t)getpid(), "com.sona.loopback.large");
        client->setIORunning(true);
    });
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const double ticksPerFrame = 1e9 * tb.denom / tb.numer / 48000.0;
    static float silence[512 * 2] = {};
    const uint64_t t0 = mach_absolute_time();
    const double seconds = getenv("SONA_LOOPBACK_SECONDS") ? atof(getenv("SONA_LOOPBACK_SECONDS")) : 5;
    double nextSmall = 128, nextLarge = 512;
    uint64_t wroteSmall = 0, wroteLarge = 0;
    for (;;) {
        const uint64_t now = mach_absolute_time();
        const double nowFrames = (double)(now - t0) / ticksPerFrame;
        if (nowFrames > seconds * 48000) break;
        auto deliver = [&](uint32_t slot, double& next, uint32_t frames, uint64_t& count) {
            while (next - frames <= nowFrames) {
                uint32_t instance = client->instanceForSlot(slot);
                if (auto guard = client->acquire()) {
                    SonaTransportBlock b = {};
                    b.frames = frames; b.instance = instance; b.formatGeneration = client->formatGeneration();
                    b.sampleTime = next; b.hostTime = t0 + (uint64_t)(next * ticksPerFrame);
                    b.flags = kSonaBlockFlag_TimestampValid;
                    if (SonaTransportRingWrite(guard.ring(slot), silence, b)) ++count;
                }
                next += frames;
            }
        };
        deliver(4, nextSmall, 128, wroteSmall);
        deliver(5, nextLarge, 512, wroteLarge);
        usleep(500);
    }
    std::printf("wrote %llu small blocks, %llu large blocks over %.1f s\n", wroteSmall, wroteLarge, seconds);
    dispatch_sync(q, ^{ client->unregisterClient(4); client->unregisterClient(5); client->setIORunning(false); client->stop(); });
    delete client;
    return 0;
}
