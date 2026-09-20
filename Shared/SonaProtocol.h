//
//  SonaProtocol.h
//  Shared constants between the Sona HAL driver and the Sona menu bar app.
//
//  The app talks to the driver exclusively through custom Core Audio
//  properties on the virtual device. Keep this file in sync with
//  App/Sources/Sona/SonaProtocol.swift.
//

#ifndef SonaProtocol_h
#define SonaProtocol_h

#define kSonaDriverVersion      "0.6.0"
#define kSonaDriverBundleID     "com.sona.driver"
#define kSonaDeviceUID          "com.sona.driver.device"
#define kSonaDeviceModelUID     "com.sona.driver.model"
#define kSonaDeviceName         "Sona"
#define kSonaManufacturer       "Sona"

// Custom property selectors on the Sona device object (scope global, element main).
// All of them carry a CFPropertyListRef.
enum {
    // Settable. CFDictionary:
    //   defaultTarget : CFString  (device UID that receives audio from apps without an explicit route)
    //   apps          : CFDictionary keyed by app key ("bundle.id" or "pid:1234") ->
    //                     { volume: CFNumber 0..4 (linear gain), mute: CFBoolean, targets: CFArray<CFString UID> }
    // Reading it back returns the last dictionary the driver accepted (or persisted).
    kSonaProperty_Config    = 'sncf',

    // Read-only. CFArray of CFDictionary:
    //   clientID : CFNumber, pid : CFNumber, bundleID : CFString (optional), key : CFString, running : CFBoolean
    // running means non-silent source samples within 300 ms (before Sona gain/mute).
    // Notifications cover client changes and signal activity transitions, checked every 50 ms.
    kSonaProperty_Clients   = 'sncl',

    // Read-only. CFDictionary:
    //   version : CFString, sampleRate : CFNumber, ioRunning : CFBoolean, defaultTarget : CFString,
    //   targets : CFArray of { slot : CFNumber, uid : CFString, active : CFBoolean,
    //                          volume : CFNumber 0..1, mute : CFBoolean,
    //                          underruns : CFNumber, deviceRate : CFNumber,
    //                          passthrough : CFBoolean (frames copied 1:1, no resampling),
    //                          clockLocked : CFBoolean (Sona clock follows this device) }
    kSonaProperty_Status    = 'snst',

    // Set { uid: CFString, volume?: CFNumber 0..1, mute?: CFBoolean }.
    // Serialized on the config queue; status targets report actual volume/mute.
    // Reading returns the current status dictionary. Hardware when available, digital otherwise.
    kSonaProperty_OutputLevel = 'snvo',
};

#define kSonaConfigKey_DefaultTarget  "defaultTarget"
#define kSonaConfigKey_Apps           "apps"
#define kSonaAppKey_Volume            "volume"
#define kSonaAppKey_Mute              "mute"
#define kSonaAppKey_Targets           "targets"

#define kSonaClientKey_ClientID       "clientID"
#define kSonaClientKey_PID            "pid"
#define kSonaClientKey_BundleID       "bundleID"
#define kSonaClientKey_Key            "key"
#define kSonaClientKey_Running        "running"
#define kSonaClientKey_Played         "played"     // true once the client has delivered non-silent source samples

#define kSonaStatusKey_Version        "version"
#define kSonaStatusKey_SampleRate     "sampleRate"
#define kSonaStatusKey_IORunning      "ioRunning"
#define kSonaStatusKey_DefaultTarget  "defaultTarget"
#define kSonaStatusKey_Targets        "targets"
#define kSonaTargetKey_Slot           "slot"
#define kSonaTargetKey_UID            "uid"
#define kSonaTargetKey_Active         "active"
#define kSonaTargetKey_Underruns      "underruns"
#define kSonaTargetKey_DeviceRate     "deviceRate"
#define kSonaTargetKey_Passthrough    "passthrough"
#define kSonaTargetKey_ClockLocked    "clockLocked"

#endif /* SonaProtocol_h */
