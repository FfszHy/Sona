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
    var volume: Double = 1.0      // linear gain 0...2 (UI slider caps at 1.0 by default)
    var mute: Bool = false
    var targets: [String] = []    // device UIDs; empty = follow default target

    var isDefault: Bool { volume == 1.0 && !mute && targets.isEmpty }
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
}
