// sonactl — tiny CLI for poking the Sona driver from a terminal.
//
//   swiftc -O -o /tmp/sonactl Tools/sonactl.swift && /tmp/sonactl status
//
//   sonactl status            print driver status
//   sonactl clients           list connected clients
//   sonactl config            print the config the driver currently holds
//   sonactl devices           list output devices (id, uid, name)
//   sonactl service           query Sona Audio Service over XPC (transport status)
//   sonactl set <json>        push a config dictionary, e.g.
//                             '{"defaultTarget":"BuiltInSpeakerDevice","apps":{"com.apple.Music":{"volume":0.5}}}'

import CoreAudio
import Foundation

func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { ($0 << 8) | UInt32($1) } }
let kSonaUID = "com.sona.driver.device"
let kSonaServiceName = "com.sona.audio-service.status"   // read-only status listener; the driver listener rejects tools

func serviceStatus() -> Never {
    let conn = xpc_connection_create_mach_service(kSonaServiceName, nil, 0)
    xpc_connection_set_event_handler(conn) { _ in }
    xpc_connection_resume(conn)
    let msg = xpc_dictionary_create(nil, nil, 0)
    xpc_dictionary_set_string(msg, "op", "status")
    let reply = xpc_connection_send_message_with_reply_sync(conn, msg)
    if xpc_get_type(reply) == XPC_TYPE_ERROR {
        let desc = xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION).map { String(cString: $0) } ?? "?"
        print("service unreachable:", desc)
        exit(2)
    }
    guard let status = xpc_dictionary_get_value(reply, "status") else { print("no status"); exit(2) }
    var out: [String: Any] = [
        "connected": xpc_dictionary_get_bool(status, "connected"),
        "generation": xpc_dictionary_get_uint64(status, "generation"),
        "retiredRegions": xpc_dictionary_get_uint64(status, "retiredRegions"),
        "version": xpc_dictionary_get_string(status, "version").map { String(cString: $0) } ?? "?",
    ]
    func plist(_ key: String) -> Any? {
        var length = 0
        guard let bytes = xpc_dictionary_get_data(status, key, &length), length > 0 else { return nil }
        return try? PropertyListSerialization.propertyList(from: Data(bytes: bytes, count: length), format: nil)
    }
    if let engine = plist("enginePlist") { out["engine"] = engine }
    if let clients = plist("clientsPlist") { out["clients"] = clients }
    if let rings = xpc_dictionary_get_value(status, "rings") {
        var list: [[String: UInt64]] = []
        xpc_array_apply(rings) { _, d in
            list.append(["slot": xpc_dictionary_get_uint64(d, "slot"), "blocks": xpc_dictionary_get_uint64(d, "blocks"),
                         "queued": xpc_dictionary_get_uint64(d, "queued"), "droppedBlocks": xpc_dictionary_get_uint64(d, "droppedBlocks")])
            return true
        }
        out["rings"] = list
    }
    printJSON(out)
    exit(0)
}

func addr(_ sel: UInt32, _ scope: UInt32 = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
}

func getString(_ obj: AudioObjectID, _ sel: UInt32) -> String? {
    var a = addr(sel)
    var v: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(obj, &a, 0, nil, &size, &v) == noErr, let v else { return nil }
    return v.takeRetainedValue() as String
}

func getPlist(_ obj: AudioObjectID, _ sel: UInt32) -> Any? {
    var a = addr(sel)
    var v: Unmanaged<CFPropertyList>?
    var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
    let err = AudioObjectGetPropertyData(obj, &a, 0, nil, &size, &v)
    guard err == noErr, let v else { print("get \(sel) failed: \(err)"); return nil }
    return v.takeRetainedValue()
}

func sonaDevice() -> AudioObjectID? {
    var a = addr(kAudioHardwarePropertyTranslateUIDToDevice)
    var cf: CFString = kSonaUID as CFString
    var dev: AudioObjectID = 0
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    let err = withUnsafeMutablePointer(to: &cf) { p in
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, UInt32(MemoryLayout<CFString>.size), p, &size, &dev)
    }
    return err == noErr && dev != 0 ? dev : nil
}

func printJSON(_ x: Any?) {
    guard let x, JSONSerialization.isValidJSONObject(x),
          let data = try? JSONSerialization.data(withJSONObject: x, options: [.prettyPrinted, .sortedKeys]) else { print(String(describing: x)); return }
    print(String(decoding: data, as: UTF8.self))
}

let args = CommandLine.arguments.dropFirst()
guard let cmd = args.first else { print("usage: sonactl status|clients|config|devices|service|set <json>"); exit(1) }

if cmd == "service" { serviceStatus() }

if cmd == "devices" {
    var a = addr(kAudioHardwarePropertyDevices)
    var size: UInt32 = 0
    AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size)
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / 4)
    AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids)
    for id in ids {
        print(id, getString(id, kAudioDevicePropertyDeviceUID) ?? "?", "|", getString(id, kAudioObjectPropertyName) ?? "?")
    }
    exit(0)
}

guard let dev = sonaDevice() else { print("Sona device not found"); exit(2) }
switch cmd {
case "status":  printJSON(getPlist(dev, fourCC("snst")))
case "clients": printJSON(getPlist(dev, fourCC("sncl")))
case "config":  printJSON(getPlist(dev, fourCC("sncf")))
case "set":
    guard args.count >= 2, let data = args.dropFirst().first?.data(using: .utf8),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { print("bad json"); exit(1) }
    var a = addr(fourCC("sncf"))
    let cf: CFDictionary = obj as CFDictionary
    let err = withExtendedLifetime(cf) {
        var v: Unmanaged<CFPropertyList>? = Unmanaged.passUnretained(cf)
        return AudioObjectSetPropertyData(dev, &a, 0, nil, UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size), &v)
    }
    print("set -> \(err)")
default: print("unknown command")
}
