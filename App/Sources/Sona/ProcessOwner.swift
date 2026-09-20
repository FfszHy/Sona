//
//  ProcessOwner.swift
//  Maps helper processes (WebKit GPU, Chrome Helper, ...) back to the app the user recognises.
//

import AppKit
import Foundation

enum ProcessOwner {
    private typealias ResponsibleFn = @convention(c) (pid_t) -> pid_t
    private static let responsibleFn: ResponsibleFn? = {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "responsibility_get_pid_responsible_for_pid") else { return nil }
        return unsafeBitCast(sym, to: ResponsibleFn.self)
    }()

    /// The pid of the app "responsible" for `pid` (the app that launched the XPC/helper process).
    /// Falls back to `pid` itself when the lookup is unavailable.
    static func responsiblePID(_ pid: pid_t) -> pid_t {
        guard let fn = responsibleFn else { return pid }
        let r = fn(pid)
        return r > 0 ? r : pid
    }

    struct Owner {
        let pid: pid_t
        let bundleID: String?
        let name: String
        let icon: NSImage?
        let isUserApp: Bool
    }

    /// Only user applications, including their responsible WebKit/Chrome helper processes.
    /// System services have no eligible .app owner and must never become mixer rows.
    static func owner(of pid: pid_t, fallbackBundleID: String?) -> Owner {
        let ownerPID = responsiblePID(pid)
        for candidate in [ownerPID, pid] {
            guard let app = NSRunningApplication(processIdentifier: candidate),
                  app.activationPolicy != .prohibited,
                  let url = app.bundleURL, url.pathExtension == "app",
                  !url.path.hasPrefix("/System/Library/"),
                  let name = app.localizedName else { continue }
            return Owner(pid: candidate, bundleID: app.bundleIdentifier, name: name,
                         icon: app.icon, isUserApp: true)
        }
        let name = processName(ownerPID) ?? processName(pid) ?? fallbackBundleID ?? "pid \(pid)"
        return Owner(pid: ownerPID, bundleID: fallbackBundleID, name: name, icon: nil,
                     isUserApp: false)
    }

    static func processName(_ pid: pid_t) -> String? {
        var buf = [CChar](repeating: 0, count: 4096)
        guard proc_pidpath(pid, &buf, UInt32(buf.count)) > 0 else { return nil }
        return URL(fileURLWithPath: String(cString: buf)).lastPathComponent
    }
}
