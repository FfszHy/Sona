// Driver property snapshots: retained ownership, offline defaults, and concurrent service pushes.
// No HAL, XPC connection, or real device is used.

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "../SonaDriver.cpp"

static std::atomic<int> gFailures{0};
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++gFailures; } } while (0)

static CFPropertyListRef ReadProperty(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address = {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFPropertyListRef value = nullptr;
    UInt32 size = 0;
    OSStatus result = Sona_GetPropertyData(gDriverRef, kObjectID_Device, 0, &address,
                                         0, nullptr, sizeof(value), &size, &value);
    CHECK(result == noErr);
    CHECK(size == sizeof(value));
    CHECK(value != nullptr);
    return value;
}

static CFDictionaryRef NewSnapshot(int generation) {
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberIntType, &generation);
    const void* keys[] = {CFSTR("generation"), CFSTR("paired")};
    const void* values[] = {number, number};
    CFDictionaryRef snapshot = CFDictionaryCreate(nullptr, keys, values, 2,
                                                  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(number);
    return snapshot;
}

static int SnapshotGeneration(CFPropertyListRef value) {
    if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID()) { CHECK(false); return -1; }
    auto dictionary = static_cast<CFDictionaryRef>(value);
    auto first = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, CFSTR("generation")));
    auto second = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, CFSTR("paired")));
    if (!first || !second) { CHECK(false); return -1; }
    int generation = -1, paired = -2;
    CHECK(CFNumberGetValue(first, kCFNumberIntType, &generation));
    CHECK(CFNumberGetValue(second, kCFNumberIntType, &paired));
    CHECK(generation == paired);
    return generation;
}

static void ResetSnapshots() {
    std::lock_guard<std::mutex> lock(gSnapshotMutex);
    if (gConfig) CFRelease(gConfig);
    if (gStatus) CFRelease(gStatus);
    if (gClientsList) CFRelease(gClientsList);
    gConfig = nullptr;
    gStatus = nullptr;
    gClientsList = nullptr;
}

static void TestOfflineDefaults() {
    ResetSnapshots();
    auto config = static_cast<CFDictionaryRef>(ReadProperty(kSonaProperty_Config));
    CHECK(CFGetTypeID(config) == CFDictionaryGetTypeID());
    CHECK(CFDictionaryGetCount(config) == 0);
    CFRelease(config);

    auto clients = static_cast<CFArrayRef>(ReadProperty(kSonaProperty_Clients));
    CHECK(CFGetTypeID(clients) == CFArrayGetTypeID());
    CHECK(CFArrayGetCount(clients) == 0);
    CFRelease(clients);

    for (auto selector : {kSonaProperty_Status, kSonaProperty_OutputLevel}) {
        auto status = static_cast<CFDictionaryRef>(ReadProperty(selector));
        CHECK(CFGetTypeID(status) == CFDictionaryGetTypeID());
        CHECK(CFDictionaryGetValue(status, CFSTR("serviceConnected")) == kCFBooleanFalse);
        auto version = CFDictionaryGetValue(status, CFSTR(kSonaStatusKey_Version));
        CHECK(version && CFEqual(version, CFSTR(kSonaDriverVersion)));
        CFRelease(status);
    }
}

static void TestRetainedReplacement() {
    CFDictionaryRef original = NewSnapshot(1);
    SonaReplaceSnapshot((CFTypeRef&)gStatus, original, CFDictionaryGetTypeID());
    CFPropertyListRef retained = ReadProperty(kSonaProperty_Status);
    CHECK(retained == original);

    SonaReplaceSnapshot((CFTypeRef&)gStatus, NewSnapshot(2), CFDictionaryGetTypeID());
    CHECK(SnapshotGeneration(retained) == 1);
    auto current = ReadProperty(kSonaProperty_OutputLevel);
    CHECK(SnapshotGeneration(current) == 2);
    CFRelease(current);
    ResetSnapshots();
    CHECK(SnapshotGeneration(retained) == 1); // The caller still owns it after the slot is cleared.
    CFRelease(retained);
}

static void PublishSnapshots(int generation) {
    SonaReplaceSnapshot((CFTypeRef&)gStatus, NewSnapshot(generation), CFDictionaryGetTypeID());
    SonaReplaceSnapshot((CFTypeRef&)gConfig, NewSnapshot(generation), CFDictionaryGetTypeID());
    CFDictionaryRef item = NewSnapshot(generation);
    const void* items[] = {item};
    CFArrayRef clients = CFArrayCreate(nullptr, items, 1, &kCFTypeArrayCallBacks);
    CFRelease(item);
    SonaReplaceSnapshot((CFTypeRef&)gClientsList, clients, CFArrayGetTypeID());
}

static void TestConcurrentReads() {
    constexpr int kIterations = 20000;
    constexpr int kReaders = 4;
    PublishSnapshots(0);
    std::atomic<bool> start{false};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < kReaders; ++reader) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < kIterations; ++i) {
                for (auto selector : {kSonaProperty_Status, kSonaProperty_Config, kSonaProperty_Clients}) {
                    CFPropertyListRef snapshot = ReadProperty(selector);
                    CFPropertyListRef item = snapshot;
                    if (selector == kSonaProperty_Clients) {
                        auto clients = static_cast<CFArrayRef>(snapshot);
                        CHECK(CFGetTypeID(clients) == CFArrayGetTypeID());
                        CHECK(CFArrayGetCount(clients) == 1);
                        item = CFArrayGetValueAtIndex(clients, 0);
                    }
                    int generation = SnapshotGeneration(item);
                    CHECK(generation >= 0 && generation <= kIterations);
                    CFRelease(snapshot);
                }
            }
        });
    }
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 1; i <= kIterations; ++i) PublishSnapshots(i);
    });
    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& reader : readers) reader.join();
    ResetSnapshots();
}

int main() {
    TestOfflineDefaults();
    TestRetainedReplacement();
    TestConcurrentReads();
    if (int failures = gFailures.load()) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("Driver snapshot tests passed (offline defaults, retained replacement, 4 readers / 20000 replacements per slot)");
}
