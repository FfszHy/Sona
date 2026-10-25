//
//  SonaApp.swift
//  Menu bar entry point.
//

import AppKit
import SwiftUI

@main
struct SonaApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        Settings { EmptyView() }
    }
}

/// Cycles the shared mark through three opacity frames while audio is flowing.
@MainActor
final class IconAnimator: ObservableObject {
    @Published private(set) var frame = 0
    @Published private(set) var isPlaying = false
    private var timer: Timer?

    func setPlaying(_ playing: Bool) {
        guard playing != isPlaying else { return }
        isPlaying = playing
        timer?.invalidate()
        timer = nil
        if playing {
            frame = 0
            timer = Timer.scheduledTimer(withTimeInterval: 0.45, repeats: true) { [weak self] _ in
                Task { @MainActor in
                    guard let self else { return }
                    self.frame = (self.frame + 1) % 3
                }
            }
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var model: AppModel?
    private var statusItemController: StatusItemController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        let model = AppModel()
        self.model = model
        statusItemController = StatusItemController(model: model)
    }

    func applicationWillTerminate(_ notification: Notification) {
        model?.flushPreferences()
        model?.restoreSystemOutput()
    }
}
