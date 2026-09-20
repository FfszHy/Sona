//
//  AudioSystem.swift
//  Thin wrappers over the Core Audio HAL client API. No private API, no TCC permissions.
//

import CoreAudio
import Foundation

struct OutputDevice: Identifiable, Equatable, Hashable {
    let id: AudioObjectID
    let uid: String
    let name: String
    let transportType: UInt32

    var isSona: Bool { uid == SonaProtocol.deviceUID }
}

enum AudioSystem {
    static func address(_ selector: AudioObjectPropertySelector,
                        scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                        element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: element)
    }

    static func getUInt32(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                          scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> UInt32? {
        var addr = address(selector, scope: scope)
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &value) == noErr else { return nil }
        return value
    }

    static func getFloat32(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                           scope: AudioObjectPropertyScope) -> Float32? {
        var addr = address(selector, scope: scope)
        var value: Float32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &value) == noErr else { return nil }
        return value
    }

    @discardableResult
    static func setFloat32(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                           scope: AudioObjectPropertyScope, _ value: Float32) -> OSStatus {
        var addr = address(selector, scope: scope)
        var v = value
        return AudioObjectSetPropertyData(object, &addr, 0, nil, UInt32(MemoryLayout<Float32>.size), &v)
    }

    @discardableResult
    static func setUInt32(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                          scope: AudioObjectPropertyScope, _ value: UInt32) -> OSStatus {
        var addr = address(selector, scope: scope)
        var v = value
        return AudioObjectSetPropertyData(object, &addr, 0, nil, UInt32(MemoryLayout<UInt32>.size), &v)
    }

    static func getString(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String? {
        var addr = address(selector)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &value) == noErr, let v = value else { return nil }
        return v.takeRetainedValue() as String
    }

    static func getPropertyList(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> Any? {
        var addr = address(selector)
        var value: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &value) == noErr, let v = value else { return nil }
        return v.takeRetainedValue()
    }

    static func setPropertyList(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector, _ plist: CFPropertyList) -> OSStatus {
        var addr = address(selector)
        return withExtendedLifetime(plist) {
            var value: Unmanaged<CFPropertyList>? = Unmanaged.passUnretained(plist)
            return AudioObjectSetPropertyData(object, &addr, 0, nil, UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size), &value)
        }
    }

    static func allDeviceIDs() -> [AudioObjectID] {
        var addr = address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
        return ids
    }

    static func hasOutput(_ device: AudioObjectID) -> Bool {
        var addr = address(kAudioDevicePropertyStreamConfiguration, scope: kAudioObjectPropertyScopeOutput)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(device, &addr, 0, nil, &size) == noErr, size > 0 else { return false }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(device, &addr, 0, nil, &size, raw) == noErr else { return false }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return list.contains { $0.mNumberChannels > 0 }
    }

    /// All devices that can play audio, including the Sona device (callers filter it out where needed).
    static func outputDevices() -> [OutputDevice] {
        allDeviceIDs().compactMap { id in
            guard hasOutput(id), let uid = getString(id, kAudioDevicePropertyDeviceUID) else { return nil }
            let name = getString(id, kAudioObjectPropertyName) ?? uid
            let transport = getUInt32(id, kAudioDevicePropertyTransportType) ?? 0
            return OutputDevice(id: id, uid: uid, name: name, transportType: transport)
        }
    }

    static func deviceID(forUID uid: String) -> AudioObjectID? {
        var addr = address(kAudioHardwarePropertyTranslateUIDToDevice)
        var cfUID: CFString = uid as CFString
        var device: AudioObjectID = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        let err = withUnsafeMutablePointer(to: &cfUID) { ptr in
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr,
                                       UInt32(MemoryLayout<CFString>.size), ptr, &size, &device)
        }
        guard err == noErr, device != kAudioObjectUnknown else { return nil }
        return device
    }

    static func defaultOutputDevice() -> AudioObjectID? {
        guard let id = getUInt32(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultOutputDevice),
              id != kAudioObjectUnknown else { return nil }
        return id
    }

    /// PIDs of every process the HAL reports as currently running output on `device`.
    /// Public API, no TCC.
    static func pidsRunningOutput(on device: AudioObjectID) -> Set<pid_t> {
        var addr = address(kAudioHardwarePropertyProcessObjectList)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
        var result = Set<pid_t>()
        for p in ids where getUInt32(p, kAudioProcessPropertyIsRunningOutput) == 1 {
            var devAddr = address(kAudioProcessPropertyDevices, scope: kAudioObjectPropertyScopeOutput)
            var devSize: UInt32 = 0
            guard AudioObjectGetPropertyDataSize(p, &devAddr, 0, nil, &devSize) == noErr, devSize > 0 else { continue }
            var devs = [AudioObjectID](repeating: 0, count: Int(devSize) / MemoryLayout<AudioObjectID>.size)
            guard AudioObjectGetPropertyData(p, &devAddr, 0, nil, &devSize, &devs) == noErr, devs.contains(device) else { continue }
            var pidAddr = address(kAudioProcessPropertyPID)
            var pid: pid_t = 0
            var pidSize = UInt32(MemoryLayout<pid_t>.size)
            if AudioObjectGetPropertyData(p, &pidAddr, 0, nil, &pidSize, &pid) == noErr { result.insert(pid) }
        }
        return result
    }

    @discardableResult
    static func setDefaultOutputDevice(_ device: AudioObjectID, systemSoundsToo: Bool = true) -> OSStatus {
        var id = device
        var addr = address(kAudioHardwarePropertyDefaultOutputDevice)
        let err = AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil,
                                             UInt32(MemoryLayout<AudioObjectID>.size), &id)
        if systemSoundsToo {
            var sysAddr = address(kAudioHardwarePropertyDefaultSystemOutputDevice)
            AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &sysAddr, 0, nil,
                                       UInt32(MemoryLayout<AudioObjectID>.size), &id)
        }
        return err
    }

    /// Registers a listener that runs on the main queue; returns a token to remove it later.
    static func addListener(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                            scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                            _ block: @escaping @MainActor () -> Void) -> AudioObjectPropertyListenerBlock? {
        var addr = address(selector, scope: scope)
        let listener: AudioObjectPropertyListenerBlock = { _, _ in
            MainActor.assumeIsolated { block() }
        }
        guard AudioObjectAddPropertyListenerBlock(object, &addr, .main, listener) == noErr else { return nil }
        return listener
    }

    static func removeListener(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                               scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
                               _ listener: @escaping AudioObjectPropertyListenerBlock) {
        var addr = address(selector, scope: scope)
        AudioObjectRemovePropertyListenerBlock(object, &addr, .main, listener)
    }
}

extension OutputDevice {
    /// SF Symbol that best matches the device's transport, like the Sound menu does.
    var symbolName: String {
        switch transportType {
        case kAudioDeviceTransportTypeBuiltIn: return "laptopcomputer"
        case kAudioDeviceTransportTypeBluetooth, kAudioDeviceTransportTypeBluetoothLE:
            let n = name.lowercased()
            if n.contains("airpods max") { return "airpodsmax" }
            if n.contains("airpods pro") { return "airpodspro" }
            if n.contains("airpods") { return "airpods" }
            if n.contains("beats") { return "beats.headphones" }
            return "headphones"
        case kAudioDeviceTransportTypeUSB, kAudioDeviceTransportTypeFireWire, kAudioDeviceTransportTypeThunderbolt, kAudioDeviceTransportTypePCI:
            return "hifispeaker"
        case kAudioDeviceTransportTypeDisplayPort, kAudioDeviceTransportTypeHDMI: return "display"
        case kAudioDeviceTransportTypeAirPlay: return "airplayaudio"
        case kAudioDeviceTransportTypeVirtual, kAudioDeviceTransportTypeAggregate: return "waveform"
        default: return "speaker.wave.2"
        }
    }
}
