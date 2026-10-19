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

/// One app-level setting as the driver understands it. Persisted by the app, pushed to the driver.
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
        // Older saves used an empty array to mean "follow default". Once the flag is
        // present, an explicit empty selection must survive relaunch as silence.
        followDefault = try values.decodeIfPresent(Bool.self, forKey: .followDefault) ?? targets.isEmpty
        targetVolumes = try values.decodeIfPresent([String: Double].self, forKey: .targetVolumes) ?? [:]
    }

    var isDefault: Bool {
        volume == 1.0 && !mute && followDefault && targets.isEmpty && targetVolumes.isEmpty
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
        targets = selected
        followDefault = false
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
