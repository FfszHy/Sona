import AppKit
import Combine
import QuartzCore
import SwiftUI

/// One unobtrusive trigger for the system's single selection and each app's routes.
struct OutputDevicePicker: View {
    enum Icon { case appOutput, airPlay }
    static func dismissActive() { OutputDevicePickerController.dismissActive() }

    @Environment(\.isEnabled) private var isEnabled
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @StateObject private var controller = OutputDevicePickerController()
    @State private var isHovered = false

    let devices: [OutputDevice]
    let selected: Set<String>
    let allowsMultiple: Bool
    let title: String
    var icon: Icon = .appOutput
    var volumeRowOffset: CGFloat = 34
    let onSelect: (String) -> Void

    var body: some View {
        Button {
            controller.activate()
        } label: {
            Group {
                if icon == .airPlay {
                    Image(systemName: "airplay.audio")
                        .font(.system(size: 17, weight: .medium))
                } else {
                    HStack(spacing: 1) {
                        Image(systemName: "speaker")
                            .font(.system(size: 15, weight: .medium))
                        Image(systemName: "arrow.right")
                            .font(.system(size: 9, weight: .semibold))
                            .offset(x: isHovered || controller.isPresented ? 1 : 0)
                    }
                }
            }
            .foregroundStyle(isHovered || controller.isPresented ? Color.primary : .secondary)
            .frame(width: 28, height: 28)
            .contentShape(Rectangle())
        }
        .buttonStyle(OutputPickerButtonStyle())
        .background {
            OutputPickerAnchor(controller: controller, devices: devices, selected: selected,
                               allowsMultiple: allowsMultiple, title: title,
                               volumeRowOffset: volumeRowOffset, isEnabled: isEnabled,
                               onSelect: onSelect)
        }
        .onHover { hovering in
            isHovered = hovering
            controller.hoverChanged(hovering)
        }
        .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: isHovered || controller.isPresented)
        .accessibilityLabel(title + "，选择输出设备")
        .accessibilityValue(controller.isPresented ? "列表已展开" : "列表已收起")
        .onDisappear { controller.dismiss(animated: false) }
    }
}

private struct OutputPickerButtonStyle: ButtonStyle {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed && !reduceMotion ? 0.95 : 1)
            .opacity(configuration.isPressed ? 0.72 : 1)
            .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: configuration.isPressed)
    }
}

private struct OutputPickerAnchor: NSViewRepresentable {
    let controller: OutputDevicePickerController
    let devices: [OutputDevice]
    let selected: Set<String>
    let allowsMultiple: Bool
    let title: String
    let volumeRowOffset: CGFloat
    let isEnabled: Bool
    let onSelect: (String) -> Void

    func makeNSView(context: Context) -> NSView { OutputPickerAnchorView() }

    func updateNSView(_ view: NSView, context: Context) {
        controller.anchor = view
        controller.devices = devices
        controller.selected = selected
        controller.allowsMultiple = allowsMultiple
        controller.title = title
        controller.volumeRowOffset = volumeRowOffset
        controller.isEnabled = isEnabled
        controller.onSelect = onSelect
        controller.refresh()
    }
}

private final class OutputPickerAnchorView: NSView {
    // The anchor only measures geometry; the SwiftUI button receives all input.
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

@MainActor
private final class OutputDevicePickerController: ObservableObject {
    @Published private(set) var isPresented = false
    private static weak var active: OutputDevicePickerController?
    static func dismissActive() { active?.dismiss(animated: false) }
    weak var anchor: NSView?
    var devices: [OutputDevice] = [] {
        didSet { if devices != oldValue { needsSizeMeasurement = true } }
    }
    var selected: Set<String> = []
    var allowsMultiple = false {
        didSet { if allowsMultiple != oldValue { needsSizeMeasurement = true } }
    }
    var title = "" {
        didSet { if title != oldValue { needsSizeMeasurement = true } }
    }
    var volumeRowOffset: CGFloat = 34
    var isEnabled = true
    var onSelect: (String) -> Void = { _ in }

    private var surfaceController: NSViewController?
    private var surface: OutputPickerSurface?
    private var host: NSHostingController<AnyView>?
    private var openTask: Task<Void, Never>?
    private var pointerTimer: Timer?
    private var displayLink: CADisplayLink?
    private var outsideSince: Date?
    private var keyboardOpened = false
    private weak var previousFirstResponder: NSResponder?
    private var presentedVolumeRowOffset: CGFloat = 34
    private var listWidth: CGFloat = OutputPickerPlacement.width
    private var listMaxHeight: CGFloat = .infinity
    private var measuredListSize = NSSize.zero
    private var needsSizeMeasurement = true
    private var keyMonitor: Any?
    private var clickMonitor: Any?
    private var transitionID = 0

    deinit {
        displayLink?.invalidate()
        pointerTimer?.invalidate()
    }

    func hoverChanged(_ hovering: Bool) {
        openTask?.cancel()
        if hovering {
            outsideSince = nil
            guard isEnabled, !isPresented, NSEvent.pressedMouseButtons == 0 else { return }
            openTask = Task { [weak self] in
                try? await Task.sleep(for: .milliseconds(100))
                guard !Task.isCancelled else { return }
                self?.present(keyboard: false)
            }
        }
    }

    func activate() {
        openTask?.cancel()
        let keyboard = NSApp.currentEvent.map { $0.type == .keyDown || $0.type == .keyUp } ?? true
        if isPresented {
            if keyboard {
                keyboardOpened = true
                outsideSince = nil
                anchor?.window?.makeFirstResponder(host?.view)
            }
        } else {
            present(keyboard: keyboard)
        }
    }

    func refresh() {
        guard isEnabled else {
            if isPresented { dismiss(animated: false) }
            return
        }
        guard isPresented else { return }
        host?.rootView = listContent
        // The display callback follows the next rendered layout. Repositioning
        // here can sample the destination before SwiftUI starts interpolating.
    }

    private var listContent: AnyView {
        AnyView(OutputDeviceList(devices: devices, selected: selected,
                                 allowsMultiple: allowsMultiple, title: title,
                                 onSelect: { [weak self] uid in self?.onSelect(uid) },
                                 width: listWidth, maxHeight: listMaxHeight))
    }

    private func present(keyboard: Bool) {
        guard isEnabled, let window = anchor?.window, window.isVisible,
              !window.ignoresMouseEvents, window is OutputPickerHosting,
              visibleAnchorFrame != nil else { return }
        if Self.active !== self { Self.active?.dismiss(animated: false) }
        Self.active = self
        if surface == nil {
            let host = NSHostingController(rootView: listContent)
            host.sizingOptions = [.preferredContentSize]
            let surface = OutputPickerSurface(content: host.view)
            let container = NSViewController()
            container.addChild(host)
            container.view = surface
            self.host = host
            self.surface = surface
            self.surfaceController = container
        }
        guard let surface else { return }
        previousFirstResponder = window.firstResponder
        keyboardOpened = keyboard
        presentedVolumeRowOffset = volumeRowOffset
        listWidth = OutputPickerPlacement.width
        listMaxHeight = max(1, (window.screen?.visibleFrame.height ?? .infinity) - 16)
        needsSizeMeasurement = true
        outsideSince = nil
        host?.rootView = listContent
        surface.isClosing = false
        surface.reducesMotion = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion || keyboard
        if surface.superview == nil { surface.revealProgress = 0 }
        positionSurface()
        // Both cards and the picker now use the same active native window.
        // Making a separate child window key would deactivate the main glass.
        window.makeKeyAndOrderFront(nil)
        if keyboard { window.makeFirstResponder(host?.view) }
        surface.layoutSubtreeIfNeeded()
        isPresented = true
        animate(to: 1, animated: !keyboard)
        installMonitors()
    }

    private var visibleAnchorFrame: NSRect? {
        guard let anchor, let window = anchor.window,
              !anchor.isHiddenOrHasHiddenAncestor, !anchor.visibleRect.isEmpty else { return nil }
        let frame = window.convertToScreen(anchor.convert(anchor.bounds, to: nil))
        guard window.frame.intersects(frame) else { return nil }
        return frame
    }

    private func positionSurface() {
        guard let controller = surfaceController, let host, let window = anchor?.window,
              let parent = window as? OutputPickerHosting,
              let frame = visibleAnchorFrame, let screen = window.screen else { return }
        let availableHeight = max(1, screen.visibleFrame.height - 16)
        if listMaxHeight != availableHeight {
            listMaxHeight = availableHeight
            host.rootView = listContent
            needsSizeMeasurement = true
        }
        if needsSizeMeasurement {
            host.view.layoutSubtreeIfNeeded()
            measuredListSize = host.sizeThatFits(in: NSSize(width: listWidth,
                                                          height: .greatestFiniteMagnitude))
            needsSizeMeasurement = false
        }
        let target = OutputPickerPlacement.popupFrame(parentFrame: parent.outputPickerPrimaryFrame,
            anchorFrame: frame.offsetBy(dx: 0, dy: -presentedVolumeRowOffset),
            desiredSize: NSSize(width: OutputPickerPlacement.width, height: max(1, measuredListSize.height)),
            visibleFrame: screen.visibleFrame.insetBy(dx: 8, dy: 8))
        if listWidth != target.width {
            listWidth = target.width
            host.rootView = listContent
            needsSizeMeasurement = true
        }
        if controller.parent == nil {
            parent.showOutputPicker(controller, screenFrame: target)
        } else {
            parent.positionOutputPicker(screenFrame: target)
        }
    }

    private func installMonitors() {
        removeMonitors()
        if let surface {
            let observer = OutputPickerFrameObserver { [weak self] in self?.positionSurface() }
            let link = surface.displayLink(target: observer, selector: #selector(OutputPickerFrameObserver.update(_:)))
            displayLink = link
            link.add(to: .main, forMode: .common)
        }
        pointerTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.checkPointer() }
        }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard event.keyCode == 53, let self, self.isPresented else { return event }
            self.dismiss(animated: false)
            return nil
        }
        clickMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] event in
            guard let self else { return event }
            if !self.pointerIsInside() { self.dismiss() }
            return event
        }
    }

    private func checkPointer() {
        guard let window = anchor?.window, window.isVisible, !window.ignoresMouseEvents,
              window.alphaValue > 0, visibleAnchorFrame != nil else {
            dismiss(animated: false)
            return
        }
        guard !keyboardOpened else { return }
        if pointerIsInside() {
            outsideSince = nil
        } else if let outsideSince {
            if Date().timeIntervalSince(outsideSince) >= 0.22 { dismiss() }
        } else {
            outsideSince = Date()
        }
    }

    private func pointerIsInside() -> Bool {
        guard let trigger = visibleAnchorFrame, let surface, let window = surface.window else { return false }
        let picker = window.convertToScreen(surface.convert(surface.bounds, to: nil))
        let mouse = NSEvent.mouseLocation
        if trigger.insetBy(dx: -3, dy: -3).contains(mouse) || picker.contains(mouse) { return true }
        let bridge = NSRect(x: trigger.maxX - 3, y: trigger.minY - 5,
                            width: max(0, picker.minX - trigger.maxX + 6), height: trigger.height + 10)
        return bridge.contains(mouse)
    }

    func dismiss(animated: Bool = true) {
        openTask?.cancel()
        guard isPresented || (!animated && surface?.superview != nil) else { return }
        isPresented = false
        removeMonitors()
        surface?.isClosing = true
        if keyboardOpened, let window = surface?.window ?? anchor?.window {
            window.makeFirstResponder(previousFirstResponder)
        }
        animate(to: 0, animated: animated && !keyboardOpened)
    }

    private func removeMonitors() {
        displayLink?.invalidate()
        displayLink = nil
        pointerTimer?.invalidate()
        pointerTimer = nil
        if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
        if let clickMonitor { NSEvent.removeMonitor(clickMonitor) }
        keyMonitor = nil
        clickMonitor = nil
    }

    private func animate(to progress: CGFloat, animated: Bool) {
        guard let surface, let controller = surfaceController,
              let parent = (surface.window ?? anchor?.window) as? OutputPickerHosting else { return }
        transitionID += 1
        let currentTransition = transitionID
        let duration = animated ? (progress == 1 ? 0.18 : 0.13) * Double(abs(progress - surface.revealProgress)) : 0
        NSAnimationContext.runAnimationGroup { context in
            context.duration = duration
            context.timingFunction = CAMediaTimingFunction(controlPoints: 0.23, 1, 0.32, 1)
            surface.animator().revealProgress = progress
        } completionHandler: { [weak self] in
            Task { @MainActor in
                guard let self else {
                    parent.removeOutputPicker(controller)
                    return
                }
                guard self.transitionID == currentTransition else { return }
                if progress == 0 {
                    if Self.active === self { Self.active = nil }
                    parent.removeOutputPicker(controller)
                }
            }
        }
        if duration == 0 {
            surface.revealProgress = progress
            if progress == 0 {
                if Self.active === self { Self.active = nil }
                parent.removeOutputPicker(controller)
            }
        }
    }
}

/// CADisplayLink retains its target; the callback holds the owner weakly so a
/// removed app card can release its picker even during a layout transition.
@MainActor
private final class OutputPickerFrameObserver: NSObject {
    private let onFrame: () -> Void

    init(onFrame: @escaping () -> Void) { self.onFrame = onFrame }

    @objc func update(_ link: CADisplayLink) { onFrame() }
}

/// Transparent sibling of the main cards, with its contents inside the shared
/// LiquidGlassCard. Animating this view preserves one window's active backdrop.
private final class OutputPickerSurface: NSView {
    private let content: NSView
    var reducesMotion = false
    var isClosing = false
    @objc dynamic var revealProgress: CGFloat = 0 {
        didSet {
            alphaValue = revealProgress
            positionContent()
        }
    }

    init(content: NSView) {
        self.content = content
        super.init(frame: .zero)
        clipsToBounds = false
        content.clipsToBounds = false
        content.autoresizingMask = []
        addSubview(content)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? {
        isClosing ? nil : super.hitTest(point)
    }
    override func layout() {
        super.layout()
        positionContent()
    }
    private func positionContent() {
        content.frame = bounds.offsetBy(dx: reducesMotion ? 0 : -6 * (1 - revealProgress), dy: 0)
    }
    override class func defaultAnimation(forKey key: NSAnimatablePropertyKey) -> Any? {
        key == "revealProgress" ? CABasicAnimation() : super.defaultAnimation(forKey: key)
    }
}
