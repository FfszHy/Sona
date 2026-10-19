//
//  StatusItemController.swift
//  Left click opens sound controls; right click opens the native settings menu.
//

import AppKit
import Combine
import QuartzCore
import SwiftUI

@MainActor
final class StatusItemController: NSObject, NSWindowDelegate, NSMenuDelegate {
    private let model: AppModel
    private let iconAnimator = IconAnimator()
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let panel = SoundPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    private let contentController: NSViewController
    private let settingsMenu = NSMenu()
    private var subscriptions: Set<AnyCancellable> = []
    private var isPanelPresented = false
    private var transitionID = 0

    init(model: AppModel) {
        self.model = model
        let content = NSHostingController(rootView: MenuContentView().environmentObject(model))
        content.sizingOptions = [.preferredContentSize]
        contentController = content
        super.init()

        // Only the individual modules draw glass. The hosting root and the
        // window remain transparent, including the spaces between modules.
        panel.contentViewController = content
        panel.backgroundColor = .clear
        panel.isOpaque = false
        // Keep each module's optical edge without a window-sized shadow.
        panel.hasShadow = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none
        // A status-item click need not activate this accessory app. Let the panel
        // take keyboard focus while another app remains active; resign-key closes it.
        panel.hidesOnDeactivate = false
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.delegate = self
        panel.onCancel = { [weak self] in self?.hidePanel() }

        if let button = statusItem.button {
            button.target = self
            button.action = #selector(statusItemClicked(_:))
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
            button.toolTip = "Sona"
            button.setAccessibilityLabel("Sona")
            button.publisher(for: \.effectiveAppearance, options: [.initial, .new])
                .receive(on: RunLoop.main)
                .sink { [weak self] _ in self?.updatePanelAppearance() }
                .store(in: &subscriptions)
        }

        settingsMenu.delegate = self
        addMenuItem("登录时启动", action: #selector(toggleLaunchAtLogin))
        addMenuItem("新设备接入时自动切换", action: #selector(toggleFollowNewDevices))
        settingsMenu.addItem(.separator())
        addMenuItem("退出 Sona", action: #selector(quit), keyEquivalent: "q")

        // ObservableObject sends before mutation; deliver after the model's new values settle.
        model.objectWillChange
            .merge(with: iconAnimator.objectWillChange)
            .receive(on: RunLoop.main)
            .sink { [weak self] in self?.updateIcon() }
            .store(in: &subscriptions)
        content.publisher(for: \.preferredContentSize, options: [.initial, .new])
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in self?.updatePanelFrame() }
            .store(in: &subscriptions)
        NotificationCenter.default.publisher(for: NSApplication.didResignActiveNotification)
            .sink { [weak self] _ in self?.hidePanel() }
            .store(in: &subscriptions)
        NotificationCenter.default.publisher(for: NSApplication.didChangeScreenParametersNotification)
            .sink { [weak self] _ in self?.updatePanelFrame() }
            .store(in: &subscriptions)
        updateIcon()
    }

    private func updateIcon() {
        let muted = model.isActive && model.outputsMuted
        iconAnimator.setPlaying(model.anyPlaying && model.isActive && !muted)
        let state: MenuBarIcon.State = muted ? .muted :
            (iconAnimator.isPlaying ? .playing(frame: iconAnimator.frame) : .idle)
        statusItem.button?.image = MenuBarIcon.image(for: state)
    }

    private func updatePanelAppearance() {
        // The menu bar can use light symbols over a dark wallpaper even when
        // the app uses Aqua. Match its appearance, rather than the app's theme.
        guard let name = statusItem.button?.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]),
              let appearance = NSAppearance(named: name) else { return }
        // Each module's glass and hosted controls inherit this appearance.
        // LiquidGlassCard refreshes its native tint when that appearance changes.
        contentController.view.appearance = appearance
    }

    @objc private func statusItemClicked(_ sender: NSStatusBarButton) {
        let event = NSApp.currentEvent
        if event?.type == .rightMouseUp ||
            (event?.type == .leftMouseUp && event?.modifierFlags.contains(.control) == true) {
            hidePanel(animated: false)
            sender.highlight(true)
            defer { sender.highlight(false) }
            settingsMenu.popUp(positioning: nil, at: NSPoint(x: sender.bounds.minX, y: sender.bounds.minY), in: sender)
        } else if isPanelPresented {
            hidePanel()
        } else {
            isPanelPresented = true
            transitionID += 1
            let openingTransition = transitionID
            panel.ignoresMouseEvents = false
            panel.reducesMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
            updatePanelAppearance()
            updatePanelFrame()
            if !panel.isVisible { panel.revealProgress = 0 }
            panel.makeKeyAndOrderFront(nil)
            // Finish the hosting view's first layout and drawing before starting
            // the clock, so that work cannot consume the low-opacity entrance.
            panel.contentView?.layoutSubtreeIfNeeded()
            panel.displayIfNeeded()
            sender.highlight(true)
            // Let the status-button's mouse tracking and initial window update
            // finish before the animation clock starts.
            DispatchQueue.main.async { [weak self] in
                guard let self, self.isPanelPresented,
                      self.transitionID == openingTransition else { return }
                self.animatePanel(to: 1)
            }
        }
    }

    private func hidePanel(animated: Bool = true) {
        guard isPanelPresented || (!animated && panel.isVisible) else { return }
        isPanelPresented = false
        panel.ignoresMouseEvents = true
        statusItem.button?.highlight(false)
        animatePanel(to: 0, animated: animated)
    }

    private func animatePanel(to progress: CGFloat, animated: Bool = true) {
        transitionID += 1
        let currentTransition = transitionID
        let reduceMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        panel.reducesMotion = reduceMotion
        let remaining = abs(progress - panel.revealProgress)
        let duration = animated ? (reduceMotion ? 0.14 : (progress == 1 ? 0.26 : 0.18)) * Double(remaining) : 0
        NSAnimationContext.runAnimationGroup { context in
            context.duration = duration
            context.timingFunction = CAMediaTimingFunction(controlPoints: 0.25, 0, 0.25, 1)
            panel.animator().revealProgress = progress
        } completionHandler: { [weak self] in
            Task { @MainActor in
                guard let self, self.transitionID == currentTransition,
                      self.isPanelPresented == (progress == 1) else { return }
                self.panel.revealProgress = progress
                if progress == 0 { self.panel.orderOut(nil) }
            }
        }
        if duration == 0 {
            panel.revealProgress = progress
            if progress == 0 { panel.orderOut(nil) }
        }
    }

    private func updatePanelFrame() {
        guard let button = statusItem.button, let buttonWindow = button.window else { return }
        let anchor = buttonWindow.convertToScreen(button.convert(button.bounds, to: nil))
        guard let screen = buttonWindow.screen ?? NSScreen.screens.first(where: { $0.frame.contains(anchor.origin) }) ?? NSScreen.main else { return }
        let visible = screen.visibleFrame.insetBy(dx: 8, dy: 8)
        let ideal = contentController.preferredContentSize
        let fitting = contentController.view.fittingSize
        let size = NSSize(width: min(max(1, ideal.width > 0 ? ideal.width : fitting.width), visible.width),
                          height: min(max(1, ideal.height > 0 ? ideal.height : fitting.height), visible.height))
        let origin = NSPoint(x: min(max(anchor.midX - size.width / 2, visible.minX), visible.maxX - size.width),
                             y: min(max(anchor.minY - 6 - size.height, visible.minY), visible.maxY - size.height))
        panel.setRestingFrame(NSRect(origin: origin, size: size))
    }

    func windowDidResignKey(_ notification: Notification) {
        // The status button handles its own click. Keeping the panel until that action
        // prevents a mouse-down dismissal followed by mouse-up reopening it.
        if let button = statusItem.button, let event = NSApp.currentEvent,
           event.window == button.window,
           button.bounds.contains(button.convert(event.locationInWindow, from: nil)) { return }
        let currentTransition = transitionID
        DispatchQueue.main.async { [weak self] in
            guard let self, self.transitionID == currentTransition, self.isPanelPresented,
                  self.panel.isVisible, !self.panel.isKeyWindow else { return }
            // Native menus and other auxiliary windows may temporarily take focus.
            var parent = NSApp.keyWindow?.parent
            while let window = parent {
                if window == self.panel { return }
                parent = window.parent
            }
            self.hidePanel()
        }
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.item(at: 0)?.state = model.launchAtLogin ? .on : .off
        menu.item(at: 1)?.state = model.followNewDevices ? .on : .off
    }

    private func addMenuItem(_ title: String, action: Selector, keyEquivalent: String = "") {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: keyEquivalent)
        item.target = self
        settingsMenu.addItem(item)
    }

    @objc private func toggleLaunchAtLogin() { model.launchAtLogin.toggle() }
    @objc private func toggleFollowNewDevices() { model.followNewDevices.toggle() }
    @objc private func quit() { NSApp.terminate(nil) }
}

/// A borderless panel still participates in native keyboard focus and Escape dismissal.
private final class SoundPanel: NSPanel {
    var onCancel: (() -> Void)?
    var reducesMotion = false
    private var restingFrame: NSRect?

    // AppKit retargets this scalar from its current value on a rapid reversal.
    @objc dynamic var revealProgress: CGFloat = 1 {
        didSet {
            alphaValue = revealProgress
            if let restingFrame { setFrameOrigin(revealedFrame(restingFrame).origin) }
        }
    }

    override class func defaultAnimation(forKey key: NSAnimatablePropertyKey) -> Any? {
        if key == "revealProgress" { return CABasicAnimation() }
        return super.defaultAnimation(forKey: key)
    }

    func setRestingFrame(_ frame: NSRect) {
        restingFrame = frame
        setFrame(revealedFrame(frame), display: isVisible)
    }

    private func revealedFrame(_ frame: NSRect) -> NSRect {
        // Move the whole native surface toward its menu-bar source without
        // resizing the glass, text or controls during the transition.
        frame.offsetBy(dx: 0, dy: reducesMotion ? 0 : 12 * (1 - revealProgress))
    }

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
    override func cancelOperation(_ sender: Any?) { onCancel?() }
}
