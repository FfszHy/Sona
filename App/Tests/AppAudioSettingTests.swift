import Foundation

@main
struct AppAudioSettingTests {
    static func main() throws {
        let decoder = JSONDecoder()
        let encoder = JSONEncoder()
        func decode(_ json: String) throws -> AppAudioSetting {
            try decoder.decode(AppAudioSetting.self, from: Data(json.utf8))
        }

        // Existing saves distinguish their historical empty and explicit target arrays.
        let legacyDefault = try decode(#"{"volume":0.25,"mute":false,"targets":[]}"#)
        assert(legacyDefault.followDefault && legacyDefault.selectedTargetUIDs(defaultTarget: "speaker") == ["speaker"])
        assert(legacyDefault.volume(for: "speaker") == 0.25)
        let legacyExplicit = try decode(#"{"volume":0.4,"mute":false,"targets":["speaker","headphones"]}"#)
        assert(!legacyExplicit.followDefault && legacyExplicit.selectedTargetUIDs(defaultTarget: "other") == ["speaker", "headphones"])
        let initializedExplicit = AppAudioSetting(targets: ["speaker"])
        assert(!initializedExplicit.followDefault && initializedExplicit.selectedTargetUIDs(defaultTarget: "other") == ["speaker"])
        let missingFields = try decode("{}")
        assert(missingFields == AppAudioSetting())

        // Deselecting the last inherited output stays silent during this app launch.
        var selection = AppAudioSetting()
        assert(selection.isDefault)
        selection.toggleTarget("speaker", defaultTarget: "speaker")
        assert(!selection.followDefault && selection.targets.isEmpty && !selection.isDefault)
        let savedSilence = try decoder.decode(AppAudioSetting.self, from: encoder.encode(selection))
        assert(savedSilence == selection && savedSilence.selectedTargetUIDs(defaultTarget: "headphones").isEmpty)
        selection.toggleTarget("headphones", defaultTarget: "speaker")
        assert(selection.targets == ["headphones"])

        // Enabling another output while following default freezes both selected routes.
        var dualOutput = AppAudioSetting()
        dualOutput.toggleTarget("headphones", defaultTarget: "speaker")
        assert(!dualOutput.followDefault && dualOutput.targets == ["speaker", "headphones"])
        assert(dualOutput.selectedTargetUIDs(defaultTarget: "other") == ["speaker", "headphones"])
        dualOutput.setVolume(0.3, for: "speaker", defaultTarget: "speaker")
        dualOutput.setVolume(0.8, for: "headphones", defaultTarget: "speaker")
        assert(dualOutput.volume(for: "speaker") == 0.3 && dualOutput.volume(for: "headphones") == 0.8)
        dualOutput.toggleTarget("headphones", defaultTarget: "speaker")
        assert(dualOutput.followDefault && dualOutput.targets.isEmpty)
        assert(dualOutput.selectedTargetUIDs(defaultTarget: "new-default") == ["new-default"])
        dualOutput.toggleTarget("headphones", defaultTarget: "speaker")
        assert(dualOutput.volume(for: "headphones") == 0.8)
        dualOutput.setVolume(0, for: "speaker", defaultTarget: "speaker")
        assert(dualOutput.volume(for: "headphones") == 0.8 && !dualOutput.mute)

        // A migrated global mute must not be lifted on every route by one slider.
        var muted = try decode(#"{"volume":0.6,"mute":true,"targets":["speaker","headphones"]}"#)
        assert(muted.volume(for: "speaker") == 0 && muted.volume(for: "headphones") == 0)
        muted.setVolume(0.7, for: "speaker", defaultTarget: "speaker")
        assert(!muted.mute && muted.volume(for: "speaker") == 0.7 && muted.volume(for: "headphones") == 0)
        var mutedDefault = AppAudioSetting(volume: 0.6, mute: true)
        mutedDefault.setVolume(0.2, for: "speaker", defaultTarget: "speaker")
        assert(mutedDefault.followDefault && !mutedDefault.mute && mutedDefault.volume(for: "speaker") == 0.2)

        // Explicit followDefault takes precedence even if a stale target array exists.
        let follow = AppAudioSetting(targets: ["stale"], followDefault: true)
        assert(follow.selectedTargetUIDs(defaultTarget: "speaker") == ["speaker"])
        assert(follow.selectedTargetUIDs(defaultTarget: "").isEmpty)
        assert(AppAudioSetting().isDefault && !dualOutput.isDefault)

        // HAL receives a real property list with all new schema fields intact.
        let plistData = try PropertyListSerialization.data(fromPropertyList: dualOutput.propertyList,
                                                          format: .binary, options: 0)
        let plist = try PropertyListSerialization.propertyList(from: plistData, format: nil) as! [String: Any]
        assert(plist["followDefault"] as? Bool == false)
        assert(plist["targets"] as? [String] == ["speaker", "headphones"])
        assert(plist["targetVolumes"] as? [String: Double] == ["speaker": 0, "headphones": 0.8])
        assert(plist["volume"] as? Double == 1 && plist["mute"] as? Bool == false)
        let savedDualOutput = try decoder.decode(AppAudioSetting.self, from: encoder.encode(dualOutput))
        assert(savedDualOutput == dualOutput)

        // Disk snapshots and old preferences retain gains, never output selections.
        let preferences = AppAudioSetting.persistentSettings([
            "browser": dualOutput, "legacy": legacyExplicit,
            "silent": savedSilence, "muted": muted,
        ])
        let restored = try decoder.decode([String: AppAudioSetting].self, from: encoder.encode(preferences))
        assert(restored["browser"]?.selectedTargetUIDs(defaultTarget: "new-default") == ["new-default"])
        assert(restored["browser"]?.targetVolumes == dualOutput.targetVolumes)
        assert(restored["legacy"]?.volume == 0.4 && restored["legacy"]?.followDefault == true)
        assert(restored["silent"] == nil)
        assert(restored["muted"]?.targetVolumes == muted.targetVolumes)
        let savedMute = AppAudioSetting(volume: 0.6, mute: true, targets: ["old"]).persistentValue
        assert(savedMute.mute && savedMute.volume == 0.6 && savedMute.followDefault)

        let browser = AppProcessInstance(pid: 10, launchDate: Date(timeIntervalSince1970: 100))
        let reopened = AppProcessInstance(pid: 20, launchDate: Date(timeIntervalSince1970: 200))
        var lifetime = AppRoutingLifetime()
        var liveSettings = ["browser": restored["browser"]!]
        lifetime.refresh(owners: ["browser": [browser]], settings: &liveSettings)
        liveSettings["browser"]?.toggleTarget("headphones", defaultTarget: "speaker")
        assert(liveSettings["browser"]?.targets == ["speaker", "headphones"])

        // Audio client removal, silence and elapsed time do not change a live owner's identity.
        lifetime.refresh(owners: ["browser": [browser]], settings: &liveSettings)
        assert(liveSettings["browser"]?.targets == ["speaker", "headphones"])
        let expanded = lifetime.expandedSettings(liveSettings, owners: ["browser": [10, 11]])
        assert(expanded["browser"]?.followDefault == true && expanded["browser"]?.targets == [])
        assert(expanded["pid:10"] == liveSettings["browser"] && expanded["pid:11"] == liveSettings["browser"])
        assert(expanded["browser"]?.targetVolumes == dualOutput.targetVolumes)
        let newHelper = lifetime.expandedSettings(liveSettings, owners: ["browser": [10, 12]])
        assert(newHelper["pid:11"] == nil && newHelper["pid:12"] == liveSettings["browser"])

        // Explicit empty selection also survives pauses; source activity never reopens audio.
        liveSettings["browser"] = AppAudioSetting(volume: 0.6, targets: [], followDefault: false,
                                                  targetVolumes: dualOutput.targetVolumes)
        lifetime.refresh(owners: ["browser": [browser]], settings: &liveSettings)
        assert(liveSettings["browser"]?.followDefault == false && liveSettings["browser"]?.targets == [])
        lifetime.refresh(owners: [:], settings: &liveSettings)
        assert(liveSettings["browser"]?.followDefault == true && liveSettings["browser"]?.targets == [])
        assert(liveSettings["browser"]?.targetVolumes == dualOutput.targetVolumes)
        lifetime.refresh(owners: ["browser": [reopened]], settings: &liveSettings)
        liveSettings["browser"]?.toggleTarget("headphones", defaultTarget: "speaker")
        // A missed quit notification still resets the route when the owner process changes.
        lifetime.refresh(owners: ["browser": [browser]], settings: &liveSettings)
        assert(liveSettings["browser"]?.followDefault == true)
        liveSettings["browser"]?.toggleTarget("headphones", defaultTarget: "speaker")
        let reusedPID = AppProcessInstance(pid: 10, launchDate: Date(timeIntervalSince1970: 300))
        lifetime.refresh(owners: ["browser": [reusedPID]], settings: &liveSettings)
        assert(liveSettings["browser"]?.followDefault == true)

        // Unknown/old services get a safe legacy mute for empty or unavailable routes;
        // the stored preferences and the new-service payload remain unchanged.
        let silentLegacy = savedSilence.driverPropertyList(supportsAppTargetVolumes: false, connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(silentLegacy["mute"] as? Bool == true && !savedSilence.mute)
        let unavailable = AppAudioSetting(targets: ["offline"], followDefault: false, targetVolumes: ["offline": 0.4])
        let legacy = unavailable.driverPropertyList(supportsAppTargetVolumes: false, connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(legacy["mute"] as? Bool == true)
        assert(legacy["targets"] as? [String] == ["offline"])
        assert(legacy["targetVolumes"] as? [String: Double] == ["offline": 0.4])
        let current = unavailable.driverPropertyList(supportsAppTargetVolumes: true, connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(current["mute"] as? Bool == false && !unavailable.mute)
        let connectedLegacy = dualOutput.driverPropertyList(supportsAppTargetVolumes: false, connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(connectedLegacy["mute"] as? Bool == false)
        let followingLegacy = follow.driverPropertyList(supportsAppTargetVolumes: false, connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(followingLegacy["targets"] as? [String] == [])

        // An old service ignores targetVolumes, so reduce all connected routes to
        // their quietest effective gain rather than reopening any silent output.
        let allZero = AppAudioSetting(targets: ["speaker", "headphones"],
                                      targetVolumes: ["speaker": 0, "headphones": 0])
        let allZeroLegacy = allZero.driverPropertyList(supportsAppTargetVolumes: false,
                                                       connectedTargetUIDs: ["speaker", "headphones"], defaultTarget: "speaker")
        assert(allZeroLegacy["volume"] as? Double == 0)
        let mixedZeroLegacy = dualOutput.driverPropertyList(supportsAppTargetVolumes: false,
                                                            connectedTargetUIDs: ["speaker", "headphones"], defaultTarget: "speaker")
        assert(mixedZeroLegacy["volume"] as? Double == 0 && dualOutput.volume(for: "headphones") == 0.8)
        let defaultZero = AppAudioSetting(targetVolumes: ["speaker": 0])
        let defaultZeroLegacy = defaultZero.driverPropertyList(supportsAppTargetVolumes: false,
                                                                connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(defaultZeroLegacy["volume"] as? Double == 0 && defaultZero.volume == 1)
        let defaultZeroCurrent = defaultZero.driverPropertyList(supportsAppTargetVolumes: true,
                                                                 connectedTargetUIDs: ["speaker"], defaultTarget: "speaker")
        assert(defaultZeroCurrent["volume"] as? Double == 1)
        assert(defaultZeroCurrent["targetVolumes"] as? [String: Double] == ["speaker": 0])
        let unequal = AppAudioSetting(targets: ["speaker", "headphones", "offline"],
                                      targetVolumes: ["speaker": 0.3, "headphones": 0.8, "offline": 0])
        let unequalLegacy = unequal.driverPropertyList(supportsAppTargetVolumes: false,
                                                       connectedTargetUIDs: ["speaker", "headphones"], defaultTarget: "speaker")
        assert(unequalLegacy["volume"] as? Double == 0.3)

        // Config writes acknowledged by the HAL while the service is offline may
        // be dropped. A fresh attachment must replay even with unchanged capabilities.
        let session = DriverStatus(version: "test", sampleRate: 48_000, ioRunning: true,
                                   defaultTarget: "speaker", targets: [], serviceConnected: true,
                                   supportsAppTargetVolumes: true, serviceSessionID: "session-a")
        var reattached = session
        reattached.serviceSessionID = "session-b"
        assert(reattached.shouldReplayConfig(after: session))
        assert(!session.shouldReplayConfig(after: session))
        assert(session.shouldReplayConfig(after: nil))
        var unsupported = session
        unsupported.supportsAppTargetVolumes = false
        assert(session.shouldReplayConfig(after: unsupported))

        print("App routing tests passed: volume-only preferences, owner app lifetime, PID-only selections, default following, explicit silence, independent gains, old-service compatibility, service-session replay")
    }
}
