import Foundation

@main
struct PreferenceSaverTests {
    @MainActor static func main() async {
        var settings: [String: AppAudioSetting] = [:]
        var defaultTarget = "speaker"
        var saved: [(Data, String)] = []
        // Exercise the actual scheduler and JSON snapshot without touching UserDefaults or HAL.
        let saver = PreferenceSaver(delay: .milliseconds(100)) {
            let data = try! JSONEncoder().encode(AppAudioSetting.persistentSettings(settings))
            saved.append((data, defaultTarget))
        }
        for i in 0...100 {
            settings["player"] = AppAudioSetting(targets: ["headphones"],
                                                targetVolumes: ["headphones": Double(i) / 100])
            saver.request()
        }
        assert(saved.isEmpty, "A slider event must not serialize preferences inline")
        try? await Task.sleep(for: .milliseconds(60))
        settings["player"]?.targetVolumes["headphones"] = 0.5
        saver.request()
        try? await Task.sleep(for: .milliseconds(60))
        assert(saved.isEmpty, "New input must restart the quiet period")
        try? await Task.sleep(for: .milliseconds(80))
        assert(saved.count == 1, "A settled burst must save once")
        let decoded = try! JSONDecoder().decode([String: AppAudioSetting].self, from: saved[0].0)
        assert(decoded["player"]?.targetVolumes["headphones"] == 0.5)
        assert(decoded["player"]?.followDefault == true && decoded["player"]?.targets.isEmpty == true)

        // A gesture ending or the app terminating saves its final value synchronously.
        settings["player"]?.targetVolumes["headphones"] = 0.25
        saver.request()
        saver.flush()
        assert(saved.count == 2)
        let final = try! JSONDecoder().decode([String: AppAudioSetting].self, from: saved[1].0)
        assert(final["player"]?.targetVolumes["headphones"] == 0.25)
        saver.flush()
        try? await Task.sleep(for: .milliseconds(140))
        assert(saved.count == 2, "Flushing must cancel the timer and an idle flush must do nothing")

        // Immediate changes (reset/default-output selection) supersede a pending slider save.
        settings["player"]?.targetVolumes["headphones"] = 0.9
        saver.request()
        settings.removeAll()
        defaultTarget = "headphones"
        saver.request(immediately: true)
        assert(saved.count == 3 && saved[2].1 == "headphones")
        assert((try! JSONDecoder().decode([String: AppAudioSetting].self, from: saved[2].0)).isEmpty)
        try? await Task.sleep(for: .milliseconds(140))
        assert(saved.count == 3, "The superseded save must not run later")

        settings["player"] = AppAudioSetting(volume: 0.75)
        saver.request()
        try? await Task.sleep(for: .milliseconds(140))
        assert(saved.count == 4, "Saving must resume after an immediate flush")

        // Let the old sleep expire while MainActor is busy, then supersede it before
        // its continuation runs. It must not flush the new request ahead of its delay.
        saver.request()
        try? await Task.sleep(for: .milliseconds(20))
        holdMainActor()
        saver.request()
        try? await Task.sleep(for: .milliseconds(20))
        assert(saved.count == 4, "An expired, cancelled callback must not flush a newer request")
        try? await Task.sleep(for: .milliseconds(120))
        assert(saved.count == 5)
        print("Preference save tests passed: burst debounce, latest JSON, gesture/exit flush, reset/default supersession, idle restart, expired callback cancellation")
    }

    @MainActor private static func holdMainActor() { Thread.sleep(forTimeInterval: 0.15) }
}
