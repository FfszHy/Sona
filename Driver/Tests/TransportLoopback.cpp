// Loopback test: drives SonaTransportClient against a running Sona Audio Service
// (any launchd domain reachable from this process). Registers a client, writes blocks
// as the realtime thread would, and reports what the service saw. No HAL involved.

#include "../SonaTransportClient.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main() {
    dispatch_queue_t q = dispatch_queue_create("loopback", DISPATCH_QUEUE_SERIAL);
    auto* client = new SonaTransportClient(q, "loopback");
    dispatch_sync(q, ^{ client->start(); });
    for (int i = 0; i < 50 && !client->connected(); ++i) usleep(100 * 1000);
    if (!client->connected()) { std::fprintf(stderr, "FAIL: service did not answer hello\n"); return 1; }
    std::printf("connected, generation %llu\n", client->generation());

    dispatch_sync(q, ^{ client->registerClient(4, 4242, (int32_t)getpid(), "com.sona.loopback"); client->setIORunning(true); });
    // Silence by default: the service may be routing this to a real speaker.
    float pcm[512 * 2] = {};
    if (getenv("SONA_LOOPBACK_TONE")) for (int i = 0; i < 512; ++i) pcm[i * 2] = pcm[i * 2 + 1] = 0.1f * sinf(float(i) * 0.1f);
    uint64_t written = 0, droppedOffline = 0, generations = 1, lastGeneration = client->generation();
    const int iterations = getenv("SONA_LOOPBACK_ITERATIONS") ? atoi(getenv("SONA_LOOPBACK_ITERATIONS")) : 300;
    for (int i = 0; i < iterations; ++i) {
        uint32_t instance = client->instanceForSlot(4);
        if (auto guard = client->acquire()) {
            if (guard.mapping->generation != lastGeneration) {
                ++generations;
                lastGeneration = guard.mapping->generation;
                std::printf("reconnected, generation %llu\n", lastGeneration);
            }
            SonaTransportBlock b = {};
            b.frames = 512; b.instance = instance; b.cycle = i; b.sampleTime = double(i) * 512.0;
            b.formatGeneration = client->formatGeneration();
            b.flags = kSonaBlockFlag_TimestampValid;
            if (SonaTransportRingWrite(guard.ring(4), pcm, b)) ++written;
        } else {
            ++droppedOffline;
        }
        usleep(2000);
    }
    std::printf("wrote %llu blocks, dropped %llu while offline, %llu generation(s)\n", written, droppedOffline, generations);
    dispatch_sync(q, ^{ client->unregisterClient(4); client->setIORunning(false); client->stop(); });
    delete client;
    return written > 0 ? 0 : 1;
}
