//
//  SonaProtocol.swift
//  Mirrors Shared/SonaProtocol.h.
//

import CoreAudio
import Foundation

enum SonaProtocol {
    static let deviceUID = "com.sona.driver.device"
    static let driverBundlePath = "/Library/Audio/Plug-Ins/HAL/SonaDriver.driver"

    static let propertyConfig: AudioObjectPropertySelector = fourCC("sncf")
    static let propertyClients: AudioObjectPropertySelector = fourCC("sncl")
    static let propertyOutputLevel: AudioObjectPropertySelector = fourCC("snvo")
    static let propertyStatus: AudioObjectPropertySelector = fourCC("snst")

    static func fourCC(_ s: String) -> UInt32 {
        s.utf8.reduce(0) { ($0 << 8) | UInt32($1) }
    }
}

/// App gains are saved; output choices last only for the owner's current app launch.
struct AppAudioSetting: Codable, Equatable {
    var volume: Double = 1.0      // legacy linear gain 0...4 (UI slider caps at 1.0)
    var mute: Bool = false
    var targets: [String] = []    // explicit device UIDs; empty can mean silence
    var followDefault: Bool = true
    var targetVolumes: [String: Double] = [:]

    init(volume: Double = 1.0, mute: Bool = false, targets: [String] = [],
         followDefault: Bool? = nil, targetVolumes: [String: Double] = [:]) {
        self.volume = volume
        self.mute = mute
        self.targets = targets
        self.followDefault = followDefault ?? targets.isEmpty
        self.targetVolumes = targetVolumes
    }

    private enum CodingKeys: String, CodingKey {
        case volume, mute, targets, followDefault, targetVolumes
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        volume = try values.decodeIfPresent(Double.self, forKey: .volume) ?? 1.0
        mute = try values.decodeIfPresent(Bool.self, forKey: .mute) ?? false
        targets = try values.decodeIfPresent([String].self, forKey: .targets) ?? []
        // Preserve explicit silence in transport. Disk preferences are normalized
        // with persistentValue so every launch starts following the default output.
        followDefault = try values.decodeIfPresent(Bool.self, forKey: .followDefault) ?? targets.isEmpty
        targetVolumes = try values.decodeIfPresent([String: Double].self, forKey: .targetVolumes) ?? [:]
    }

    var isDefault: Bool {
        volume == 1.0 && !mute && followDefault && targets.isEmpty && targetVolumes.isEmpty
    }

    mutating func resetOutputSelection() {
        targets = []
        followDefault = true
    }

    var persistentValue: AppAudioSetting {
        var saved = self
        saved.resetOutputSelection()
        return saved
    }

    static func persistentSettings(_ settings: [String: AppAudioSetting]) -> [String: AppAudioSetting] {
        settings.mapValues(\.persistentValue).filter { !$0.value.isDefault }
    }

    func selectedTargetUIDs(defaultTarget: String) -> [String] {
        if followDefault { return defaultTarget.isEmpty ? [] : [defaultTarget] }
        var seen: Set<String> = []
        return targets.filter { !$0.isEmpty && seen.insert($0).inserted }
    }

    func volume(for uid: String) -> Double {
        mute ? 0 : (targetVolumes[uid] ?? volume)
    }

    mutating func setVolume(_ newVolume: Double, for uid: String, defaultTarget: String) {
        guard newVolume.isFinite else { return }
        if mute {
            // Migrated global mute gates every route. Raising one slider must leave
            // all other currently selected routes silent when that gate is cleared.
            for selected in selectedTargetUIDs(defaultTarget: defaultTarget) { targetVolumes[selected] = 0 }
            mute = false
        }
        targetVolumes[uid] = min(max(newVolume, 0), 1)
    }

    mutating func toggleTarget(_ uid: String, defaultTarget: String) {
        var selected = selectedTargetUIDs(defaultTarget: defaultTarget)
        if let index = selected.firstIndex(of: uid) { selected.remove(at: index) }
        else { selected.append(uid) }
        followDefault = selected == [defaultTarget]
        targets = followDefault ? [] : selected
        // Keep the device's previous volume so deselecting and reselecting restores it.
    }

    /// The same schema is used by persistence tests and the driver's property-list transport.
    var propertyList: [String: Any] {
        ["volume": volume, "mute": mute, "targets": targets,
         "followDefault": followDefault, "targetVolumes": targetVolumes]
    }

    func driverPropertyList(supportsAppTargetVolumes: Bool, connectedTargetUIDs: Set<String>,
                            defaultTarget: String) -> [String: Any] {
        var serialized = propertyList
        if !supportsAppTargetVolumes {
            // Older services infer follow-default from an empty target list and fall
            // back to it when none of the explicit routes exists. Preserve silence
            // without changing the richer preferences saved by this app.
            if followDefault { serialized["targets"] = [String]() }
            let connected = selectedTargetUIDs(defaultTarget: defaultTarget).filter(connectedTargetUIDs.contains)
            if let safeVolume = connected.map({ volume(for: $0) }).min() {
                // A legacy service has only one app gain. Use the quietest selected
                // route so replaying preferences cannot audibly reopen a silent route.
                serialized["volume"] = safeVolume
            } else { serialized["mute"] = true }
        }
        return serialized
    }
}

/// A process currently connected to the Sona device, as reported by the driver.
struct DriverClient: Identifiable, Equatable {
    let clientID: Int
    let pid: pid_t
    let bundleID: String?
    let key: String
    let running: Bool
    let played: Bool

    var id: Int { clientID }
}

struct AppProcessInstance: Hashable {
    let pid: pid_t
    let launchDate: Date?
}

/// Audio activity is only for row visibility. Output choices follow the owner's process lifetime.
struct AppRoutingLifetime {
    private var instances: [String: Set<AppProcessInstance>] = [:]

    mutating func refresh(owners current: [String: Set<AppProcessInstance>], settings: inout [String: AppAudioSetting]) {
        for key in Array(settings.keys) {
            let previous = instances[key] ?? []
            let live = current[key] ?? []
            guard live.isEmpty || previous.isDisjoint(with: live) else { continue }
            settings[key]?.resetOutputSelection()
            if settings[key]?.isDefault == true { settings.removeValue(forKey: key) }
        }
        instances = current
    }

    func expandedSettings(_ settings: [String: AppAudioSetting], owners: [String: Set<pid_t>]) -> [String: AppAudioSetting] {
        // New processes must not inherit a previous launch's explicit bundle route
        // before the App has identified their owner. Only known PIDs receive choices.
        var expanded = AppAudioSetting.persistentSettings(settings)
        for (owner, pids) in owners {
            let setting = settings[owner] ?? AppAudioSetting()
            for pid in pids { expanded["pid:\(pid)"] = setting }
        }
        return expanded
    }
}

struct DriverTargetStatus: Equatable {
    let slot: Int
    let uid: String
    let active: Bool
    let underruns: Int
    let deviceRate: Double
    var volume: Double? = nil
    var mute: Bool = false
    var passthrough: Bool = false   // frames copied 1:1, no resampling
    var clockLocked: Bool = false   // Sona clock follows this device
}

struct DriverStatus: Equatable {
    let version: String
    let sampleRate: Double
    let ioRunning: Bool
    let defaultTarget: String
    let targets: [DriverTargetStatus]
    /// Sona Audio Service is connected to the driver. Missing key (older driver) counts as connected.
    let serviceConnected: Bool
    /// New routing controls require the service to understand explicit silence and route gains.
    var supportsAppTargetVolumes: Bool = false
    /// Changes on each service attachment so settings lost while offline can be replayed.
    var serviceSessionID: String? = nil

    func shouldReplayConfig(after previous: DriverStatus?) -> Bool {
        supportsAppTargetVolumes != previous?.supportsAppTargetVolumes || serviceSessionID != previous?.serviceSessionID
    }
}
