import AppKit

/// Hosts a second glass surface in the sound panel's existing key window.
@MainActor
protocol OutputPickerHosting: AnyObject, Sendable {
    var outputPickerPrimaryFrame: NSRect { get }
    func showOutputPicker(_ controller: NSViewController, screenFrame: NSRect)
    func positionOutputPicker(screenFrame: NSRect)
    func removeOutputPicker(_ controller: NSViewController)
}
