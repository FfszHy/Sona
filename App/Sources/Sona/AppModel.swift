//
//  AppModel.swift
//  Central observable state: driver connection, device list, per-app settings.
//

import AppKit
import Combine
import CoreAudio
import Foundation
import ServiceManagement

/// A row in the UI: an app (helper processes grouped under their owner) connected to Sona.
struct AudioApp: Identifiable, Equatable {
    let key: String
    let name: String
    let icon: NSImage?
    let pids: [pid_t]
    let clientKeys: Set<String>
    let running: Bool
    var id: String { key }
}

@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var driverInstalled = false
    @Published private(set) var driverConnected = false
    @Published private(set) var status: DriverStatus?
    @Published private(set) var outputDevices: [OutputDevice] = []
    @Published private(set) var isActive = false            // system default output == Sona
    @Published private(set) var apps: [AudioApp] = []
    @Published var defaultTargetUID: String = "" { didSet { if defaultTargetUID != oldValue { persist(); pushConfig(immediately: true) } } }
    /// User picked a default output by hand: keep it even if an auto-selected device disconnects.
    func selectDefaultTarget(_ uid: String) {
        targetBeforeAutoSwitch = nil
        defaultTargetUID = uid
    }
    @Published private(set) var appSettings: [String: AppAudioSetting] = [:]
    @Published private(set) var masterVolume: Double = 1.0     // Default output volume scalar 0...1, proxied by Sona
    @Published private(set) var masterMuted: Bool = false
    /// Mirror macOS: a newly connected headphone/Bluetooth/USB output becomes the default target,
    /// and unplugging it returns to the device that was in use before.
    @Published var followNewDevices: Bool = UserDefaults.standard.object(forKey: Keys.followNewDevices) as? Bool ?? true {
        didSet { defaults.set(followNewDevices, forKey: Keys.followNewDevices) }
    }
    @Published var launchAtLogin: Bool = SMAppService.mainApp.status == .enabled {
        didSet {
            do {
                if launchAtLogin { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
            } catch {
                NSLog("Sona: launch at login failed: \(error)")
            }
        }
    }

    let driver = DriverConnection()
    private var deviceListListener: AudioObjectPropertyListenerBlock?
    private var defaultDeviceListener: AudioObjectPropertyListenerBlock?
    private var reconnectTimer: Timer?
    private var pushTask: Task<Void, Never>?
    private var configPushRequested = false
    private var clientOwners: [String: Set<pid_t>] = [:]
    private var routingLifetime = AppRoutingLifetime()
    private var appLifecycleObservers: [NSObjectProtocol] = []
    private var restoringSystemOutput = false
    private var lastPushedApps: [String: AppAudioSetting] = [:]
    private var lastAppsSummary = ""
    private var volumeListener: AudioObjectPropertyListenerBlock?
    private var muteListener: AudioObjectPropertyListenerBlock?
    private var listenedDevice: AudioObjectID?
    private let defaults = UserDefaults.standard

    /// True while any visible app is playing; drives the menu bar icon animation.
    var anyPlaying: Bool { apps.contains(where: \.running) }
    var canControlAppOutputs: Bool { status?.supportsAppTargetVolumes == true }

    private enum Keys {
        static let appSettings = "appSettings"
        static let defaultTarget = "defaultTarget"
        static let previousDefault = "previousDefaultDeviceUID"
        static let followNewDevices = "followNewDevices"
    }

    /// Transports macOS itself switches to when such a device appears. Virtual, aggregate,
    /// HDMI/DisplayPort and AirPlay devices never steal the default.
    private static let hotplugTransports: Set<UInt32> = [
        kAudioDeviceTransportTypeBuiltIn,      // headphone jack shows up as a separate built-in device
        kAudioDeviceTransportTypeUSB,
        kAudioDeviceTransportTypeBluetooth,
        kAudioDeviceTransportTypeBluetoothLE,
        kAudioDeviceTransportTypeThunderbolt,
    ]
    private var knownDeviceUIDs: Set<String>? = nil          // nil until the first enumeration
    private var targetBeforeAutoSwitch: String? = nil        // restored when the auto-chosen device leaves

    /// Audio-system plumbing that must never appear as an "app": coreaudiod itself, the helper
    /// process that hosts HAL plug-ins (which is where the Sona driver's own IOProc runs), and Sona.
    static let hiddenBundleIDs: Set<String> = [
        "com.apple.audio.coreaudiod",
        "com.apple.audio.Core-Audio-Driver-Service.helper",
        "com.apple.audio.Core-Audio-Driver-Service",
        "com.apple.audiomxd",
        "com.apple.audioaccessoryd",
        "audioaccessoryd",
        "systemsoundserverd",
        "com.apple.systemsoundserverd",
        "com.apple.PowerChime",
        "com.sona.app",
    ]

    init() {
        load()
        driver.onClientsChanged = { [weak self] in self?.refreshClients() }
        driver.onStatusChanged = { [weak self] in self?.refreshStatus() }
        for name in [NSWorkspace.didLaunchApplicationNotification, NSWorkspace.didTerminateApplicationNotification] {
            appLifecycleObservers.append(NSWorkspace.shared.notificationCenter.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refreshClients() }
            })
        }
        let system = AudioObjectID(kAudioObjectSystemObject)
        deviceListListener = AudioSystem.addListener(system, kAudioHardwarePropertyDevices) { [weak self] in
            self?.refreshDevices()
            self?.reconnectIfNeeded()
        }
        defaultDeviceListener = AudioSystem.addListener(system, kAudioHardwarePropertyDefaultOutputDevice) { [weak self] in
            self?.systemDefaultChanged()
        }
        refreshDevices()
        reconnectIfNeeded()
        reconnectTimer = Timer.scheduledTimer(withTimeInterval: 15, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.reconnectIfNeeded()
                self?.refreshClients()   // Recovery poll; driver notifications handle playback immediately.
            }
        }
        reconnectTimer?.tolerance = 3
    }

    // MARK: - Driver

    private func reconnectIfNeeded() {
        let installed = driver.isInstalled
        if driverInstalled != installed { driverInstalled = installed }
        let wasConnected = driverConnected
        let connected = driver.connect()
        let deviceChanged = connected && listenedDevice != driver.deviceID
        if driverConnected != connected { driverConnected = connected }
        if driverConnected && (!wasConnected || deviceChanged) {
            NSLog("Sona: connected to driver")
            pushConfig(immediately: true)
            refreshClients()
            refreshStatus()
            attachMasterListeners()
            // Running Sona means using Sona: take over the system output right away.
            if !isActive { setActive(true) }
        } else if !driverConnected && wasConnected {
            NSLog("Sona: disconnected from driver")
            if !apps.isEmpty { apps = [] }
            // A service/driver reconnect is not an owner-app relaunch. Keep its
            // live process mappings and manual choices for config replay.
            if status != nil { status = nil }
            detachMasterListeners()
        }
        refreshActive()
    }

    // MARK: - Default output volume (proxied through Sona; volume keys use the same controls)

    private func attachMasterListeners() {
        guard let id = driver.deviceID, listenedDevice != id else { return }
        detachMasterListeners()
        listenedDevice = id
        volumeListener = AudioSystem.addListener(id, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput) { [weak self] in self?.refreshMaster() }
        muteListener = AudioSystem.addListener(id, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput) { [weak self] in self?.refreshMaster() }
        refreshMaster()
    }

    private func detachMasterListeners() {
        if let id = listenedDevice {
            if let l = volumeListener { AudioSystem.removeListener(id, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput, l) }
            if let l = muteListener { AudioSystem.removeListener(id, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput, l) }
        }
        listenedDevice = nil
        volumeListener = nil
        muteListener = nil
    }

    private func refreshMaster() {
        guard let id = driver.deviceID else { return }
        if let v = AudioSystem.getFloat32(id, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput) {
            if masterVolume != Double(v) { masterVolume = Double(v) }
        }
        if let m = AudioSystem.getUInt32(id, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput) {
            if masterMuted != (m != 0) { masterMuted = m != 0 }
        }
    }

    func setMasterVolume(_ v: Double) {
        guard v.isFinite, let id = driver.deviceID else { return }
        let volume = min(max(v, 0), 1)
        masterVolume = volume
        AudioSystem.setFloat32(id, kAudioDevicePropertyVolumeScalar, scope: kAudioObjectPropertyScopeOutput, Float32(volume))
        // Zero volume is already silent. Clear a separate mute latch so the system's
        // volume-only keyboard writes can raise the level again.
        setMasterMuted(false)
    }

    func setMasterMuted(_ m: Bool) {
        guard let id = driver.deviceID else { return }
        masterMuted = m
        AudioSystem.setUInt32(id, kAudioDevicePropertyMute, scope: kAudioObjectPropertyScopeOutput, m ? 1 : 0)
    }

    /// Keep the default first; show the connected routes of visible apps even when muted.
    var routedOutputDevices: [OutputDevice] {
        var uids: Set<String> = [defaultTargetUID]
        for app in apps { uids.formUnion(setting(for: app.key).selectedTargetUIDs(defaultTarget: defaultTargetUID)) }
        return orderedOutputDevices(matching: uids)
    }

    private func orderedOutputDevices(matching uids: Set<String>) -> [OutputDevice] {
        outputDevices.filter { uids.contains($0.uid) }.sorted {
            if $0.uid == defaultTargetUID { return true }
            if $1.uid == defaultTargetUID { return false }
            let comparison = $0.name.localizedStandardCompare($1.name)
            return comparison == .orderedSame ? $0.uid < $1.uid : comparison == .orderedAscending
        }
    }

    var outputsMuted: Bool {
        let devices = routedOutputDevices
        return !devices.isEmpty && devices.allSatisfy { outputMuted($0.uid) || outputVolume($0.uid) == 0 }
    }

    func outputVolume(_ uid: String) -> Double {
        uid == defaultTargetUID ? masterVolume : (requestedOutputVolumes[uid] ?? status?.targets.first { $0.uid == uid }?.volume ?? 1)
    }

    func outputMuted(_ uid: String) -> Bool {
        uid == defaultTargetUID ? masterMuted : (requestedOutputMutes[uid] ?? status?.targets.first { $0.uid == uid }?.mute ?? false)
    }

    func canControlOutput(_ uid: String) -> Bool {
        uid == defaultTargetUID || status?.targets.first { $0.uid == uid }?.volume != nil
    }

    @Published private var requestedOutputVolumes: [String: Double] = [:]
    @Published private var requestedOutputMutes: [String: Bool] = [:]
    private var outputRevisions: [String: Int] = [:]
    private var editingOutputs: Set<String> = []
    private lazy var outputWriter = OutputLevelWriter { [weak self] uid, change in
        guard let self else { return }
        let revision = self.outputRevisions[uid]
        let result = await self.driver.setOutputLevel(uid: uid, volume: change.volume, mute: change.mute)
        // SetPropertyData acknowledges enqueueing, not the hardware write. Keep the user's
        // latest value while dragging and allow delayed/quantized hardware state to settle.
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: result == noErr ? 1_000_000_000 : 0)
            guard let self, self.outputRevisions[uid] == revision,
                  !self.editingOutputs.contains(uid) else { return }
            self.requestedOutputVolumes.removeValue(forKey: uid)
            self.requestedOutputMutes.removeValue(forKey: uid)
            self.refreshStatus()
        }
    }

    func setOutputEditing(_ editing: Bool, uid: String) {
        if editing { editingOutputs.insert(uid) }
        else {
            editingOutputs.remove(uid)
            // Always flush the final position, including a drag held still before release.
            if let volume = requestedOutputVolumes[uid] { setOutputVolume(volume, uid: uid) }
        }
    }

    func setOutputVolume(_ volume: Double, uid: String) {
        guard volume.isFinite else { return }
        let volume = min(max(volume, 0), 1)
        if uid == defaultTargetUID {
            requestedOutputVolumes.removeValue(forKey: uid)
            requestedOutputMutes.removeValue(forKey: uid)
            setMasterVolume(volume)
            return
        }
        requestedOutputVolumes[uid] = volume
        requestedOutputMutes[uid] = false
        outputRevisions[uid, default: 0] += 1
        outputWriter.submit(uid: uid, volume: volume, mute: false)
    }

    func setOutputMuted(_ mute: Bool, uid: String) {
        if uid == defaultTargetUID {
            requestedOutputMutes.removeValue(forKey: uid)
            setMasterMuted(mute)
            return
        }
        requestedOutputMutes[uid] = mute
        outputRevisions[uid, default: 0] += 1
        outputWriter.submit(uid: uid, mute: mute)
    }

    private var statusRefreshTask: Task<Void, Never>?
    private var statusRefreshRequested = false

    private func refreshStatus() {
        statusRefreshRequested = true
        guard statusRefreshTask == nil else { return }
        statusRefreshTask = Task { [weak self] in
            guard let self else { return }
            while self.statusRefreshRequested {
                try? await Task.sleep(nanoseconds: 33_000_000)
                self.statusRefreshRequested = false
                let device = self.driver.deviceID
                let updated = await self.driver.fetchStatusInBackground()
                if device == self.driver.deviceID, self.status != updated {
                    let replayConfig = updated?.shouldReplayConfig(after: self.status) == true
                    self.status = updated
                    // A first, capability-unknown push may have used the safe legacy
                    // serialization; an offline service can also drop acknowledged
                    // writes. Replay current preferences on capability/session changes.
                    if replayConfig { self.pushConfig(immediately: true) }
                }
            }
            self.statusRefreshTask = nil
        }
    }

    private func refreshClients() {
        guard driverConnected else { return }
        let clients = driver.fetchClients()
        var instances: [String: Set<AppProcessInstance>] = [:]
        for app in NSWorkspace.shared.runningApplications {
            guard let owner = ProcessOwner.userApp(app), !Self.hiddenBundleIDs.contains(owner.key) else { continue }
            instances[owner.key, default: []].insert(owner.instance)
        }

        // A pause may destroy its Core Audio client while the source process stays
        // alive. Retain that PID route so recreating the client cannot leak to default.
        var ownersByPID: [pid_t: ProcessOwner.Owner] = [:]
        for c in clients { ownersByPID[c.pid] = ProcessOwner.owner(of: c.pid, fallbackBundleID: c.bundleID) }
        let knownPIDs = clientOwners.values.reduce(into: Set<pid_t>()) { $0.formUnion($1) }
        for pid in knownPIDs where ownersByPID[pid] == nil {
            guard ProcessOwner.processName(pid) != nil else { continue }
            ownersByPID[pid] = ProcessOwner.owner(of: pid, fallbackBundleID: nil)
        }
        clientOwners = [:]
        for (pid, owner) in ownersByPID where owner.isUserApp && !Self.hiddenBundleIDs.contains(owner.key) {
            instances[owner.key, default: []].insert(owner.instance)
            clientOwners[owner.key, default: []].insert(pid)
        }
        let previousSettings = appSettings
        routingLifetime.refresh(owners: instances, settings: &appSettings)
        if appSettings != previousSettings { persist() }

        // Group helper processes (WebKit GPU, Chrome Helper, ...) under the app that owns them.
        var grouped: [String: (owner: ProcessOwner.Owner, clients: [DriverClient], running: Bool)] = [:]
        for c in clients {
            if Self.hiddenBundleIDs.contains(c.bundleID ?? "") { continue }
            guard let owner = ownersByPID[c.pid] else { continue }
            guard owner.isUserApp else { continue }
            let key = owner.key
            if Self.hiddenBundleIDs.contains(key) { continue }
            let running = c.running
            var entry = grouped[key] ?? (owner, [], false)
            entry.clients.append(c)
            entry.running = entry.running || running
            grouped[key] = entry
        }
        // Saved settings and historical activity never keep a silent app on screen.
        let visibleApps = grouped.compactMap { key, e -> AudioApp? in
            guard e.running else { return nil }
            guard key != Bundle.main.bundleIdentifier else { return nil }
            return AudioApp(key: key, name: e.owner.name, icon: e.owner.icon,
                            pids: e.clients.map(\.pid), clientKeys: Set(e.clients.map(\.key)), running: e.running)
        }
        .sorted { ($0.name.lowercased(), $0.key) < ($1.name.lowercased(), $1.key) }
        if apps != visibleApps { apps = visibleApps }

        let summary = apps.map { "\($0.name)[\($0.key) pids=\($0.pids)]\($0.running ? "▶" : "")" }.joined(separator: ", ")
        if summary != lastAppsSummary {
            lastAppsSummary = summary
            NSLog("Sona: apps -> \(summary)")
        }

        // The set of helper pids may have changed; make sure the driver has pid-level entries.
        let expanded = expandedSettings()
        if expanded != lastPushedApps { pushConfig() }
    }

    // MARK: - Devices

    private func refreshDevices() {
        outputDevices = AudioSystem.outputDevices().filter { !$0.isSona }.sorted { $0.name < $1.name }
        let current = Set(outputDevices.map(\.uid))
        if let known = knownDeviceUIDs {
            let arrived = outputDevices.filter { !known.contains($0.uid) && Self.hotplugTransports.contains($0.transportType) }
            if followNewDevices, let device = arrived.first, device.uid != defaultTargetUID {
                NSLog("Sona: \(device.name) connected, following it")
                if targetBeforeAutoSwitch == nil { targetBeforeAutoSwitch = defaultTargetUID }
                defaultTargetUID = device.uid
            }
            if let previous = targetBeforeAutoSwitch, !current.contains(defaultTargetUID) {
                // The auto-chosen device left: go back to what the user had before it appeared.
                targetBeforeAutoSwitch = nil
                if current.contains(previous) {
                    NSLog("Sona: auto-selected device left, back to \(deviceName(forUID: previous))")
                    defaultTargetUID = previous
                }
            } else if targetBeforeAutoSwitch != nil, let previous = targetBeforeAutoSwitch, !current.contains(previous) {
                targetBeforeAutoSwitch = nil   // the fallback itself is gone; nothing to return to
            }
        }
        knownDeviceUIDs = current
        if defaultTargetUID.isEmpty || !outputDevices.contains(where: { $0.uid == defaultTargetUID }) {
            // Prefer whatever the system is currently using (unless that is Sona itself), else built-in.
            if let cur = AudioSystem.defaultOutputDevice(), let uid = AudioSystem.getString(cur, kAudioDevicePropertyDeviceUID),
               uid != SonaProtocol.deviceUID, outputDevices.contains(where: { $0.uid == uid }) {
                defaultTargetUID = uid
            } else if let builtIn = outputDevices.first(where: { $0.transportType == kAudioDeviceTransportTypeBuiltIn }) {
                defaultTargetUID = builtIn.uid
            } else if let first = outputDevices.first {
                defaultTargetUID = first.uid
            }
        }
        // Legacy services fall back to default for disconnected explicit routes.
        // Re-evaluate their compatibility mute when the set of connected outputs changes.
        if !canControlAppOutputs { pushConfig() }
    }

    private func refreshActive() {
        let active = AudioSystem.defaultOutputDevice().map {
            AudioSystem.getString($0, kAudioDevicePropertyDeviceUID) == SonaProtocol.deviceUID
        } ?? false
        if isActive != active { isActive = active }
    }

    private func systemDefaultChanged() {
        // macOS may select a real output on Bluetooth connection or in System Settings.
        // Adopt that target, then keep audio flowing through Sona for per-app routing.
        if !restoringSystemOutput, driverConnected, let sona = driver.deviceID,
           let device = AudioSystem.defaultOutputDevice(), device != sona,
           let uid = AudioSystem.getString(device, kAudioDevicePropertyDeviceUID),
           uid != SonaProtocol.deviceUID, AudioSystem.hasOutput(device) {
            if uid != defaultTargetUID { targetBeforeAutoSwitch = nil }
            defaultTargetUID = uid
            defaults.set(uid, forKey: Keys.previousDefault)
            pushConfig(immediately: true)
            // A physical selection must remain usable as a recovery path while the
            // audio service is offline, even though the HAL device still exists.
            if status?.serviceConnected != false {
                let result = AudioSystem.setDefaultOutputDevice(sona)
                if result != noErr { NSLog("Sona: failed to resume virtual output after system default change: \(result)") }
            }
        }
        refreshActive()
    }

    func deviceName(forUID uid: String) -> String {
        outputDevices.first { $0.uid == uid }?.name ?? uid
    }

    // MARK: - Activation

    func setActive(_ on: Bool) {
        if on {
            restoringSystemOutput = false
            guard let sona = driver.deviceID else { return }
            if let cur = AudioSystem.defaultOutputDevice(), let uid = AudioSystem.getString(cur, kAudioDevicePropertyDeviceUID),
               uid != SonaProtocol.deviceUID {
                defaults.set(uid, forKey: Keys.previousDefault)
                if outputDevices.contains(where: { $0.uid == uid }) { defaultTargetUID = uid }
            }
            pushConfig(immediately: true)
            AudioSystem.setDefaultOutputDevice(sona)
        } else {
            restoreSystemOutput()
        }
        refreshActive()
    }

    /// Points the system default output back at a real device.
    func restoreSystemOutput() {
        restoringSystemOutput = true
        let uid = defaultTargetUID.isEmpty ? (defaults.string(forKey: Keys.previousDefault) ?? "") : defaultTargetUID
        if let id = AudioSystem.deviceID(forUID: uid) ?? outputDevices.first?.id {
            AudioSystem.setDefaultOutputDevice(id)
        }
    }

    // MARK: - Per-app settings

    func setting(for key: String) -> AppAudioSetting {
        appSettings[key] ?? AppAudioSetting()
    }

    func appOutputDevices(for key: String) -> [OutputDevice] {
        orderedOutputDevices(matching: Set(setting(for: key).selectedTargetUIDs(defaultTarget: defaultTargetUID)))
    }

    func appVolume(_ uid: String, for key: String) -> Double {
        setting(for: key).volume(for: uid)
    }

    func setAppVolume(_ volume: Double, uid: String, for key: String) {
        guard volume.isFinite else { return }
        let defaultUID = defaultTargetUID
        update(key) {
            $0.setVolume(volume, for: uid, defaultTarget: defaultUID)
        }
    }

    func setAppEditing(_ editing: Bool, for _: String) {
        if !editing { pushConfig(immediately: true) }
    }

    func update(_ key: String, _ change: (inout AppAudioSetting) -> Void) {
        var s = setting(for: key)
        change(&s)
        if s.isDefault { appSettings.removeValue(forKey: key) } else { appSettings[key] = s }
        NSLog("Sona: update \(key) -> volume=\(s.volume) mute=\(s.mute) followDefault=\(s.followDefault) targets=\(s.targets) targetVolumes=\(s.targetVolumes)")
        persist()
        pushConfig()
    }

    func toggleTarget(_ uid: String, for key: String) {
        guard outputDevices.contains(where: { $0.uid == uid }) else { return }
        let defaultUID = defaultTargetUID
        update(key) { s in
            s.toggleTarget(uid, defaultTarget: defaultUID)
        }
    }

    func reset(_ key: String) {
        appSettings.removeValue(forKey: key)
        persist()
        pushConfig()
    }

    // MARK: - Config sync

    /// Known source PIDs inherit this owner's choices; new app processes start at default.
    private func expandedSettings() -> [String: AppAudioSetting] {
        routingLifetime.expandedSettings(appSettings, owners: clientOwners)
    }

    private func pushConfig(immediately: Bool = false) {
        configPushRequested = true
        guard pushTask == nil else { return }
        pushTask = Task { @MainActor [weak self] in
            guard let self else { return }
            var skipDelay = immediately
            while self.configPushRequested {
                // Throttle instead of restarting a debounce on every slider event.
                // A slow HAL write keeps only the latest pending configuration.
                if !skipDelay { try? await Task.sleep(nanoseconds: 33_000_000) }
                skipDelay = false
                self.configPushRequested = false
                guard self.driverConnected else { break }
                let expanded = self.expandedSettings()
                let defaultUID = self.defaultTargetUID
                let device = self.driver.deviceID
                let err = await self.driver.pushConfig(defaultTarget: defaultUID, apps: expanded,
                                                      supportsAppTargetVolumes: self.canControlAppOutputs,
                                                      connectedTargetUIDs: Set(self.outputDevices.map(\.uid)))
                if err == noErr, device == self.driver.deviceID { self.lastPushedApps = expanded }
                else if err != noErr { NSLog("Sona: pushConfig failed: \(err)") }
            }
            self.pushTask = nil
        }
    }

    private func load() {
        if let data = defaults.data(forKey: Keys.appSettings),
           let decoded = try? JSONDecoder().decode([String: AppAudioSetting].self, from: data) {
            appSettings = AppAudioSetting.persistentSettings(decoded)
        }
        defaultTargetUID = defaults.string(forKey: Keys.defaultTarget) ?? ""
        persist()
    }

    private func persist() {
        if let data = try? JSONEncoder().encode(AppAudioSetting.persistentSettings(appSettings)) {
            defaults.set(data, forKey: Keys.appSettings)
        }
        defaults.set(defaultTargetUID, forKey: Keys.defaultTarget)
    }
}
