//
//  DriverConnection.swift
//  Finds the Sona virtual device and exchanges property lists with the driver.
//

import CoreAudio
import Foundation

final class DriverConnection {
    private(set) var deviceID: AudioObjectID?
    private var clientsListener: AudioObjectPropertyListenerBlock?
    private var statusListener: AudioObjectPropertyListenerBlock?

    var onClientsChanged: (@MainActor () -> Void)?
    var onStatusChanged: (@MainActor () -> Void)?

    var isInstalled: Bool { FileManager.default.fileExists(atPath: SonaProtocol.driverBundlePath) }
    var isConnected: Bool { deviceID != nil }

    /// Locates the device. Returns true if the device is present (freshly found or already known).
    @discardableResult
    func connect() -> Bool {
        if let id = deviceID, AudioSystem.getUInt32(id, kAudioDevicePropertyDeviceIsAlive) == 1 { return true }
        disconnect()
        guard let id = AudioSystem.deviceID(forUID: SonaProtocol.deviceUID) else { return false }
        deviceID = id
        clientsListener = AudioSystem.addListener(id, SonaProtocol.propertyClients) { [weak self] in
            self?.onClientsChanged?()
        }
        statusListener = AudioSystem.addListener(id, SonaProtocol.propertyStatus) { [weak self] in
            self?.onStatusChanged?()
        }
        return true
    }

    func disconnect() {
        if let id = deviceID {
            if let l = clientsListener { AudioSystem.removeListener(id, SonaProtocol.propertyClients, l) }
            if let l = statusListener { AudioSystem.removeListener(id, SonaProtocol.propertyStatus, l) }
        }
        deviceID = nil
        clientsListener = nil
        statusListener = nil
    }

    func fetchClients() -> [DriverClient] {
        guard let id = deviceID, let arr = AudioSystem.getPropertyList(id, SonaProtocol.propertyClients) as? [[String: Any]] else { return [] }
        return arr.compactMap { d in
            guard let cid = d["clientID"] as? Int, let pid = d["pid"] as? Int, let key = d["key"] as? String else { return nil }
            return DriverClient(clientID: cid, pid: pid_t(pid), bundleID: d["bundleID"] as? String,
                                key: key, running: d["running"] as? Bool ?? false,
                                played: d["played"] as? Bool ?? (d["running"] as? Bool ?? false))
        }
    }

    func fetchStatus() -> DriverStatus? { Self.readStatus(deviceID: deviceID) }

    @MainActor
    func fetchStatusInBackground() async -> DriverStatus? {
        let id = deviceID
        return await withCheckedContinuation { continuation in
            outputQueue.async { continuation.resume(returning: Self.readStatus(deviceID: id)) }
        }
    }

    private static func readStatus(deviceID: AudioObjectID?) -> DriverStatus? {
        guard let id = deviceID, let d = AudioSystem.getPropertyList(id, SonaProtocol.propertyStatus) as? [String: Any] else { return nil }
        let targets = (d["targets"] as? [[String: Any]] ?? []).compactMap { t -> DriverTargetStatus? in
            guard let uid = t["uid"] as? String else { return nil }
            return DriverTargetStatus(slot: t["slot"] as? Int ?? 0, uid: uid, active: t["active"] as? Bool ?? false,
                                      underruns: t["underruns"] as? Int ?? 0, deviceRate: t["deviceRate"] as? Double ?? 0,
                                      volume: t["volume"] as? Double, mute: t["mute"] as? Bool ?? false,
                                      passthrough: t["passthrough"] as? Bool ?? false,
                                      clockLocked: t["clockLocked"] as? Bool ?? false)
        }
        return DriverStatus(version: d["version"] as? String ?? "?", sampleRate: d["sampleRate"] as? Double ?? 0,
                            ioRunning: d["ioRunning"] as? Bool ?? false, defaultTarget: d["defaultTarget"] as? String ?? "",
                            targets: targets, serviceConnected: d["serviceConnected"] as? Bool ?? true)
    }

    private let outputQueue = DispatchQueue(label: "com.sona.app.output-controls", qos: .userInitiated)

    @MainActor
    func setOutputLevel(uid: String, volume: Double? = nil, mute: Bool? = nil) async -> OSStatus {
        guard let id = deviceID else { return OSStatus(kAudioHardwareNotRunningError) }
        return await withCheckedContinuation { continuation in
            outputQueue.async {
                var command: [String: Any] = ["uid": uid]
                // Float32, not Double: the driver reads a Float32 and older builds reject a lossy conversion.
                if let volume { command["volume"] = Float32(min(1, max(0, volume))) }
                if let mute { command["mute"] = mute }
                let result = AudioSystem.setPropertyList(id, SonaProtocol.propertyOutputLevel, command as CFDictionary)
                continuation.resume(returning: result)
            }
        }
    }

    /// Pushes the full configuration. The driver persists it in HAL plug-in storage.
    @discardableResult
    func pushConfig(defaultTarget: String, apps: [String: AppAudioSetting]) -> OSStatus {
        guard let id = deviceID else { return OSStatus(kAudioHardwareNotRunningError) }
        var appsDict: [String: Any] = [:]
        for (key, s) in apps where !s.isDefault {
            appsDict[key] = ["volume": s.volume, "mute": s.mute, "targets": s.targets]
        }
        let config: [String: Any] = ["defaultTarget": defaultTarget, "apps": appsDict]
        return AudioSystem.setPropertyList(id, SonaProtocol.propertyConfig, config as CFDictionary)
    }
}
