import SwiftUI

/// Let the system render the thumb, including its pressed Liquid Glass material.
struct SoundSlider: View {
    @Environment(\.isEnabled) private var isEnabled
    @State private var isHovered = false
    @State private var isEditing = false
    @FocusState private var isFocused: Bool

    let value: Double
    let label: String
    let onChange: (Double) -> Void
    var onEditingChanged: (Bool) -> Void = { _ in }

    var body: some View {
        Slider(
            value: Binding(get: { value }, set: onChange),
            in: 0...1,
            onEditingChanged: { editing in
                isEditing = editing
                onEditingChanged(editing)
            }
        )
        .labelsHidden()
        .controlSize(.small)
        .tint(.primary)
        // Structural animations may move this row, but the volume itself must
        // always follow mouse/keyboard input and hardware updates immediately.
        .transaction { $0.animation = nil }
        // Use the public visibility API instead of overriding NSSliderCell drawing.
        .sliderThumbVisibility(isEnabled && (isHovered || isEditing || isFocused) ? .visible : .hidden)
        .focused($isFocused)
        .onHover { isHovered = $0 }
        .accessibilityLabel(label)
        .accessibilityValue(value == 0 ? "静音" : "\(Int((value * 100).rounded()))%")
    }
}
