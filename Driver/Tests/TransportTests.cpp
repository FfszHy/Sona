// Transport ring tests. No HAL, no XPC: exercises the shared-memory contract in-process.

#include "../../Shared/SonaTransport.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static int gFailures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++gFailures; } } while (0)

static void* NewRegion(uint64_t generation) {
    void* r = nullptr;
    if (posix_memalign(&r, 4096, kSonaTransportRegionSize) != 0) std::abort();
    SonaTransportInitRegion(r, generation);
    return r;
}

static void TestLayoutAndValidation() {
    void* r = NewRegion(7);
    CHECK(SonaTransportValidateRegion(r, kSonaTransportRegionSize, 7));
    CHECK(!SonaTransportValidateRegion(r, kSonaTransportRegionSize, 8));          // wrong generation
    CHECK(!SonaTransportValidateRegion(r, kSonaTransportRegionSize - 1, 7));      // short mapping
    auto* h = static_cast<SonaTransportHeader*>(r);
    h->protocolVersion = 99;
    CHECK(!SonaTransportValidateRegion(r, kSonaTransportRegionSize, 7));
    h->protocolVersion = kSonaTransportProtocolVersion;
    h->ringStride = 1;
    CHECK(!SonaTransportValidateRegion(r, kSonaTransportRegionSize, 7));
    // Ring N must land inside the region and be cache-line aligned.
    auto* last = SonaTransportRing(r, kSonaTransportMaxClients - 1);
    CHECK((reinterpret_cast<uintptr_t>(last) & 63) == 0);
    CHECK(reinterpret_cast<uint8_t*>(last) + sizeof(SonaTransportClientRing) ==
          static_cast<uint8_t*>(r) + kSonaTransportRegionSize);
    std::free(r);
}

static void TestRoundTripAndWrap() {
    void* r = NewRegion(1);
    auto* ring = SonaTransportRing(r, 3);
    std::vector<float> in(kSonaTransportMaxBlockFrames * 2);
    uint64_t sent = 0, received = 0;
    // Push enough blocks to wrap both the sample ring and the descriptor ring several times.
    for (int i = 0; i < 2000; ++i) {
        uint32_t frames = 100 + (i % 900);
        for (uint32_t f = 0; f < frames * 2; ++f) in[f] = float(sent + f);
        SonaTransportBlock b = {};
        b.frames = frames; b.instance = 5; b.sampleTime = double(sent / 2);
        if (SonaTransportRingWrite(ring, in.data(), b)) sent += frames * 2;
        SonaTransportReadBlock rb;
        while (SonaTransportRingPeek(ring, rb)) {
            CHECK(rb.desc.instance == 5);
            CHECK(rb.desc.sampleTime == double(received / 2));
            uint64_t k = received;
            for (uint32_t f = 0; f < rb.firstFrames * 2; ++f) CHECK(rb.first[f] == float(k++));
            for (uint32_t f = 0; f < rb.secondFrames * 2; ++f) CHECK(rb.second[f] == float(k++));
            received += rb.desc.frames * 2;
            SonaTransportRingConsume(ring, rb);
        }
    }
    CHECK(sent == received);
    CHECK(ring->droppedBlocks.load() == 0);
    std::free(r);
}

static void TestDropAndDiscontinuity() {
    void* r = NewRegion(1);
    auto* ring = SonaTransportRing(r, 0);
    std::vector<float> in(kSonaTransportMaxBlockFrames * 2, 0.5f);
    SonaTransportBlock b = {};
    b.frames = kSonaTransportMaxBlockFrames;
    // Fill the sample ring without consuming.
    uint32_t ok = 0;
    for (uint32_t i = 0; i < kSonaTransportRingFrames / kSonaTransportMaxBlockFrames + 4; ++i)
        if (SonaTransportRingWrite(ring, in.data(), b)) ++ok;
    CHECK(ok == kSonaTransportRingFrames / kSonaTransportMaxBlockFrames);
    CHECK(ring->droppedBlocks.load() == 4);
    CHECK(ring->droppedFrames.load() == 4 * kSonaTransportMaxBlockFrames);
    // Consumer never had its read index moved by the producer.
    CHECK(ring->frameRead.load() == 0 && ring->blockRead.load() == 0);
    SonaTransportReadBlock rb;
    while (SonaTransportRingPeek(ring, rb)) { CHECK(!(rb.desc.flags & kSonaBlockFlag_Discontinuity)); SonaTransportRingConsume(ring, rb); }
    // The next block after a drop carries the discontinuity flag, exactly once.
    b.frames = 64;
    CHECK(SonaTransportRingWrite(ring, in.data(), b));
    CHECK(SonaTransportRingWrite(ring, in.data(), b));
    CHECK(SonaTransportRingPeek(ring, rb) && (rb.desc.flags & kSonaBlockFlag_Discontinuity));
    SonaTransportRingConsume(ring, rb);
    CHECK(SonaTransportRingPeek(ring, rb) && !(rb.desc.flags & kSonaBlockFlag_Discontinuity));
    SonaTransportRingConsume(ring, rb);
    // Descriptor ring can fill before the sample ring does.
    b.frames = 1;
    ok = 0;
    for (uint32_t i = 0; i < kSonaTransportBlockCapacity + 10; ++i) if (SonaTransportRingWrite(ring, in.data(), b)) ++ok;
    CHECK(ok == kSonaTransportBlockCapacity);
    // Invalid sizes are refused without touching the ring.
    b.frames = 0; CHECK(!SonaTransportRingWrite(ring, in.data(), b));
    b.frames = kSonaTransportMaxBlockFrames + 1; CHECK(!SonaTransportRingWrite(ring, in.data(), b));
    SonaTransportRingFlush(ring);
    CHECK(!SonaTransportRingPeek(ring, rb));
    std::free(r);
}

static void TestConcurrentSPSC() {
    void* r = NewRegion(1);
    auto* ring = SonaTransportRing(r, 9);
    constexpr uint64_t kBlocks = 200000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::vector<float> in(512 * 2);
        uint64_t seq = 0;
        for (uint64_t i = 0; i < kBlocks; ++i) {
            uint32_t frames = 64 + uint32_t(i % 449);
            for (uint32_t f = 0; f < frames * 2; ++f) in[f] = float((seq + f) & 0xFFFF);
            SonaTransportBlock b = {};
            b.frames = frames; b.instance = 1; b.cycle = i; b.sampleTime = double(seq);
            while (!SonaTransportRingWrite(ring, in.data(), b)) std::this_thread::yield();
            seq += frames * 2;
        }
        done.store(true);
    });
    uint64_t expect = 0, blocks = 0;
    for (;;) {
        SonaTransportReadBlock rb;
        if (!SonaTransportRingPeek(ring, rb)) {
            if (done.load() && !SonaTransportRingPeek(ring, rb)) break;
            std::this_thread::yield();
            continue;
        }
        CHECK(rb.desc.cycle == blocks);
        CHECK(rb.desc.sampleTime == double(expect));
        uint64_t k = expect;
        bool ok = true;
        for (uint32_t f = 0; f < rb.firstFrames * 2; ++f) ok &= rb.first[f] == float(k++ & 0xFFFF);
        for (uint32_t f = 0; f < rb.secondFrames * 2; ++f) ok &= rb.second[f] == float(k++ & 0xFFFF);
        CHECK(ok);
        expect += rb.desc.frames * 2;
        ++blocks;
        SonaTransportRingConsume(ring, rb);
    }
    producer.join();
    CHECK(blocks == kBlocks);
    std::free(r);
}

int main() {
    TestLayoutAndValidation();
    TestRoundTripAndWrap();
    TestDropAndDiscontinuity();
    TestConcurrentSPSC();
    if (gFailures) { std::fprintf(stderr, "%d failure(s)\n", gFailures); return 1; }
    std::printf("transport tests passed (region %llu bytes, %u client rings)\n",
                (unsigned long long)kSonaTransportRegionSize, kSonaTransportMaxClients);
    return 0;
}
