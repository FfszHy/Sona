import SwiftUI

/// Shared contents for the default-output and per-app routing panels.
/// Selection remains owned by the caller so a click never recreates the list.
struct OutputDeviceList: View {
    let devices: [OutputDevice]
    let selected: Set<String>
    let allowsMultiple: Bool
    let title: String
    let onSelect: (String) -> Void
    var width: CGFloat = OutputPickerPlacement.width
    var maxHeight: CGFloat = .infinity

    private var listHeight: CGFloat {
        min(240, max(32, maxHeight - (allowsMultiple ? 12 : 35)),
            CGFloat(devices.count) * 32 + CGFloat(max(0, devices.count - 1)) * 2)
    }

    var body: some View {
        LiquidGlassCard(cornerRadius: 18) {
            VStack(alignment: .leading, spacing: 6) {
                if !allowsMultiple {
                    Text(title)
                        .font(.system(size: 11, weight: .semibold))
                        .lineLimit(1)
                        .foregroundStyle(.secondary)
                        .padding(.horizontal, 6)
                        .padding(.top, 2)
                }

                if devices.isEmpty {
                    Text("没有可用输出设备")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .padding(6)
                } else {
                    ScrollView(showsIndicators: false) {
                        VStack(spacing: 2) {
                            ForEach(devices, id: \.uid) { device in
                                OutputDeviceListRow(
                                    device: device,
                                    isSelected: selected.contains(device.uid),
                                    allowsMultiple: allowsMultiple,
                                    action: { onSelect(device.uid) }
                                )
                            }
                        }
                    }
                    .scrollBounceBehavior(.basedOnSize)
                    .frame(height: listHeight)
                }
            }
            .padding(6)
            .frame(width: width, alignment: .leading)
        }
        .frame(width: width)
    }
}

private struct OutputDeviceListRow: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var isHovered = false
    let device: OutputDevice
    let isSelected: Bool
    let allowsMultiple: Bool
    let action: () -> Void

    private let selectedColor = Color(red: 100 / 255, green: 176 / 255, blue: 1)

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                Image(systemName: device.symbolName)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(isSelected ? selectedColor : Color.primary.opacity(0.72))
                    .frame(width: 22, height: 22)
                    .background(isSelected ? Color.white.opacity(0.80) : Color.primary.opacity(0.06), in: Circle())
                Text(device.name)
                    .font(.system(size: 12))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "checkmark")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(selectedColor)
                    .opacity(isSelected ? 1 : 0)
                    .frame(width: 12)
            }
            .padding(.horizontal, 6)
            .frame(height: 32)
            .background(Color.primary.opacity(isHovered ? 0.055 : 0),
                        in: RoundedRectangle(cornerRadius: 8, style: .continuous))
            .contentShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
        .buttonStyle(.plain)
        .onHover { isHovered = $0 }
        .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: isHovered)
        .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: isSelected)
        .help(device.name)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(device.name)
        .accessibilityValue(isSelected ? "已选中" : "未选中")
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        .accessibilityHint(allowsMultiple
                           ? (isSelected ? "点击停止输出到此设备" : "点击添加此输出设备")
                           : "设为默认输出设备")
    }
}
