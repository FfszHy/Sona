//
//  SonaApp.swift
//  Menu bar entry point.
//

import AppKit
import SwiftUI

@main
struct SonaApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel()
    @StateObject private var iconAnimator = IconAnimator()

    var body: some Scene {
        MenuBarExtra {
            MenuContentView()
                .environmentObject(model)
        } label: {
            Image(nsImage: MenuBarIcon.image(for: iconState))
                .onChange(of: model.anyPlaying, initial: true) { _, playing in
                    iconAnimator.setPlaying(playing && model.isActive && !model.outputsMuted)
                }
                .onChange(of: model.isActive) { _, _ in
                    iconAnimator.setPlaying(model.anyPlaying && model.isActive && !model.outputsMuted)
                }
                .onChange(of: model.outputsMuted) { _, _ in
                    iconAnimator.setPlaying(model.anyPlaying && model.isActive && !model.outputsMuted)
                }
        }
        .menuBarExtraStyle(.window)
    }

    private var iconState: MenuBarIcon.State {
        if model.isActive && model.outputsMuted { return .muted }
        if iconAnimator.isPlaying { return .playing(frame: iconAnimator.frame) }
        return .idle
    }
}

/// Cycles the "playing" icon through its three ring frames while audio is flowing.
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

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
    }
}
