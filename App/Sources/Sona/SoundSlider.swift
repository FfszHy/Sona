import AppKit
import QuartzCore
import SwiftUI

/// A native NSSlider whose AppKit knob appears on hover, while dragging, or with keyboard focus.
struct SoundSlider: NSViewRepresentable {
    @Environment(\.isEnabled) private var isEnabled
    let value: Double
    let label: String
    let onChange: (Double) -> Void
    var onEditingChanged: (Bool) -> Void = { _ in }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> TrackingSoundSlider {
        let slider = TrackingSoundSlider()
        slider.cell = HoverSoundSliderCell()
        slider.minValue = 0
        slider.maxValue = 1
        slider.isContinuous = true
        slider.controlSize = .small
        slider.tintProminence = .primary
        slider.trackFillColor = .labelColor
        slider.target = context.coordinator
        slider.action = #selector(Coordinator.changed(_:))
        slider.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return slider
    }

    func updateNSView(_ slider: TrackingSoundSlider, context: Context) {
        context.coordinator.parent = self
        slider.doubleValue = value
        slider.isEnabled = isEnabled
        slider.tintProminence = .primary
        slider.trackFillColor = .labelColor
        slider.onEditingChanged = onEditingChanged
        slider.setAccessibilityLabel(label)
        let description = value == 0 ? "静音" : "\(Int((value * 100).rounded()))%"
        slider.setAccessibilityValueDescription(description)
        slider.cell?.setAccessibilityLabel(label)
        slider.cell?.setAccessibilityValueDescription(description)
        slider.needsDisplay = true
    }

    final class Coordinator: NSObject {
        var parent: SoundSlider
        init(_ parent: SoundSlider) { self.parent = parent }

        @objc func changed(_ slider: NSSlider) {
            parent.onChange(slider.doubleValue)
        }
    }
}

final class TrackingSoundSlider: NSSlider {
    var onEditingChanged: (Bool) -> Void = { _ in }
    private var hoverTrackingArea: NSTrackingArea?
    private var pointerInside = false
    private var isMouseTracking = false
    private var hasKeyboardFocus = false
    private var showsKnob = false

    @objc dynamic private(set) var knobOpacity: CGFloat = 0 {
        didSet { needsDisplay = true }
    }

    override class func defaultAnimation(forKey key: NSAnimatablePropertyKey) -> Any? {
        if key == "knobOpacity" { return CABasicAnimation() }
        return super.defaultAnimation(forKey: key)
    }

    override var isEnabled: Bool {
        didSet { updateKnobVisibility() }
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let hoverTrackingArea { removeTrackingArea(hoverTrackingArea) }
        let area = NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        hoverTrackingArea = area
        updatePointerLocation()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil { hasKeyboardFocus = false }
        updatePointerLocation()
    }

    override func mouseEntered(with event: NSEvent) {
        super.mouseEntered(with: event)
        pointerInside = true
        updateKnobVisibility()
    }

    override func mouseExited(with event: NSEvent) {
        super.mouseExited(with: event)
        pointerInside = false
        updateKnobVisibility()
    }

    override func becomeFirstResponder() -> Bool {
        let accepted = super.becomeFirstResponder()
        if accepted {
            // Popovers can assign initial focus without a keyboard interaction.
            hasKeyboardFocus = !isMouseTracking && NSApp.currentEvent?.type == .keyDown
            updateKnobVisibility()
        }
        return accepted
    }

    override func resignFirstResponder() -> Bool {
        let resigned = super.resignFirstResponder()
        if resigned {
            hasKeyboardFocus = false
            updateKnobVisibility()
        }
        return resigned
    }

    override func keyDown(with event: NSEvent) {
        if isEnabled {
            hasKeyboardFocus = true
            updateKnobVisibility()
        }
        super.keyDown(with: event)
    }

    override func mouseDown(with event: NSEvent) {
        guard isEnabled else { return }
        hasKeyboardFocus = false
        isMouseTracking = true
        updateKnobVisibility()
        onEditingChanged(true)
        defer {
            isMouseTracking = false
            updatePointerLocation()
            onEditingChanged(false)
        }
        super.mouseDown(with: event)
    }

    private func updatePointerLocation() {
        if let window, window.isVisible {
            pointerInside = visibleRect.contains(convert(window.mouseLocationOutsideOfEventStream, from: nil))
        } else {
            pointerInside = false
        }
        updateKnobVisibility()
    }

    private func updateKnobVisibility() {
        let visible = isEnabled && (pointerInside || isMouseTracking || hasKeyboardFocus)
        // Hover fades can reverse from their current opacity. Direct interaction
        // stays immediate, including a press before a fade-in has finished.
        let immediate = isMouseTracking || hasKeyboardFocus || !isEnabled || window?.isVisible != true
        guard showsKnob != visible || immediate else { return }
        showsKnob = visible
        let opacity: CGFloat = visible ? 1 : 0
        NSAnimationContext.runAnimationGroup { context in
            context.duration = immediate ? 0 : (visible ? 0.12 : 0.16)
            context.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            animator().knobOpacity = opacity
        }
        if immediate { knobOpacity = opacity }
    }
}

/// AppKit draws both the track and knob; only the native knob's opacity changes.
private final class HoverSoundSliderCell: NSSliderCell {
    override func drawKnob(_ knobRect: NSRect) {
        guard let slider = controlView as? TrackingSoundSlider, slider.knobOpacity > 0 else { return }
        if slider.knobOpacity >= 1 {
            super.drawKnob(knobRect)
            return
        }
        guard let context = NSGraphicsContext.current?.cgContext else { return }
        context.saveGState()
        context.setAlpha(slider.knobOpacity)
        context.beginTransparencyLayer(auxiliaryInfo: nil)
        super.drawKnob(knobRect)
        context.endTransparencyLayer()
        context.restoreGState()
    }
}
