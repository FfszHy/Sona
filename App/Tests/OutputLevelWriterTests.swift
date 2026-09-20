import Foundation

@main
struct OutputLevelWriterTests {
    @MainActor static func main() async {
        var sent: [(String, OutputLevelWriter.Change)] = []
        var releaseSlowWrite: CheckedContinuation<Void, Never>?
        let writer = OutputLevelWriter { uid, change in
            sent.append((uid, change))
            if sent.count == 1 {
                await withCheckedContinuation { releaseSlowWrite = $0 }
            }
        }
        // A burst keeps the final position, with no work executed inline on the UI thread.
        for i in 0...100 { writer.submit(uid: "speaker", volume: Double(i) / 100) }
        assert(sent.isEmpty)
        try? await Task.sleep(nanoseconds: 100_000_000)
        assert(sent.count == 1 && sent[0].1.volume == 1)
        assert(releaseSlowWrite != nil)
        // UI input can continue during a blocked transport. Preserve latest volume AND mute.
        for i in 0...100 { writer.submit(uid: "speaker", volume: Double(i) / 200) }
        writer.submit(uid: "speaker", mute: true)
        writer.submit(uid: "headphones", volume: 0.8)
        assert(sent.count == 1)
        releaseSlowWrite?.resume()
        try? await Task.sleep(nanoseconds: 100_000_000)
        assert(sent.count == 3)
        let speaker = sent.last { $0.0 == "speaker" }!.1
        assert(speaker.volume == 0.5 && speaker.mute == true)
        assert(sent.last { $0.0 == "headphones" }!.1.volume == 0.8)
        // After going idle, a new gesture starts another writer and flushes its last value.
        writer.submit(uid: "speaker", volume: 0.25)
        try? await Task.sleep(nanoseconds: 100_000_000)
        assert(sent.count == 4 && sent.last!.1.volume == 0.25)
        print("Output slider tests passed: burst coalescing, slow transport, independent devices, final flush")
    }
}
