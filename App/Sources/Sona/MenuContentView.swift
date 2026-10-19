//
//  MenuContentView.swift
//  The glass panel shown from the menu bar icon: sound controls · app cards.
//

import SwiftUI

private enum UI {
    static let width: CGFloat = 336
    static let outerPadding: CGFloat = 12
    static let cardPaddingH: CGFloat = 12
    static let cardPaddingV: CGFloat = 10
    static let cardGap: CGFloat = 12
    static let control: CGFloat = 28
    static let appIcon: CGFloat = 24
}

struct MenuContentView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var contentHeight: CGFloat?
    var maxHeight: CGFloat = .infinity
    var onContentLayoutChange: (() -> Void)? = nil

    /// Volume/status polling must not restart layout animations. Only changes
    /// that actually add, remove or reorder panel content participate.
    private struct ContentLayout: Equatable {
        struct AppLayout: Equatable {
            let id: String
            let outputUIDs: [String]
            let hasSelection: Bool
        }

        let driverInstalled: Bool
        let driverConnected: Bool
        let serviceConnected: Bool?
        let canControlAppOutputs: Bool
        let apps: [AppLayout]
        let outputUIDs: [String]
        let routedUIDs: [String]
    }

    private var contentLayout: ContentLayout {
        ContentLayout(driverInstalled: model.driverInstalled,
                      driverConnected: model.driverConnected,
                      serviceConnected: model.status?.serviceConnected,
                      canControlAppOutputs: model.canControlAppOutputs,
                      apps: model.apps.map { app in
                          ContentLayout.AppLayout(
                            id: app.id,
                            outputUIDs: model.appOutputDevices(for: app.key).map(\.uid),
                            hasSelection: !model.setting(for: app.key)
                                .selectedTargetUIDs(defaultTarget: model.defaultTargetUID).isEmpty)
                      },
                      outputUIDs: model.outputDevices.map(\.uid),
                      routedUIDs: model.routedOutputDevices.map(\.uid))
    }

    private var contentTransition: AnyTransition {
        SonaMotion.transition(reduceMotion: reduceMotion)
    }

    var body: some View {
        // Keep the same content/control tree when the panel becomes scrollable.
        // Switching between two ViewThatFits branches would lose slider focus
        // and editing state precisely when another app/device arrives.
        ScrollView(showsIndicators: false) {
            panelContent
                .fixedSize(horizontal: false, vertical: true)
                .onGeometryChange(for: CGFloat.self) { geometry in
                    geometry.size.height
                } action: { height in
                    guard contentHeight != height else { return }
                    contentHeight = height
                    onContentLayoutChange?()
                }
        }
        .scrollBounceBehavior(.basedOnSize)
        // Keep an outgoing bottom card visible while the native window shrinks.
        // The window remains the final boundary for long, scrollable panels.
        .scrollClipDisabled()
        .defaultScrollAnchor(.top)
        .frame(height: contentHeight.map { min($0, availableHeight) })
        .frame(width: UI.width)
        .frame(maxHeight: availableHeight)
        .controlSize(.small)
        .onChange(of: contentLayout) { _, _ in onContentLayoutChange?() }
    }

    private var availableHeight: CGFloat {
        if maxHeight.isFinite { return max(1, maxHeight) }
        return max(1, (NSScreen.main?.visibleFrame.height ?? 736) - 16)
    }

    private var panelContent: some View {
        VStack(alignment: .leading, spacing: UI.cardGap) {
            if !model.driverInstalled {
                soundHeading
                notice("需要安装 Sona 驱动", systemImage: "exclamationmark.triangle.fill",
                       detail: "驱动装在 /Library/Audio/Plug-Ins/HAL,需要管理员密码。请重新运行 Sona 安装包;从源码构建时在项目目录运行:", code: "sudo make install-driver")
                    .transition(contentTransition)
            } else if !model.driverConnected {
                soundHeading
                notice("驱动已安装,等待设备出现", systemImage: "hourglass",
                       detail: "刚安装的话请稍等几秒;仍不出现就重启 coreaudiod。", code: "sudo killall coreaudiod")
                    .transition(contentTransition)
            } else {
                if model.status?.serviceConnected == false {
                    notice("Sona Audio Service 未运行,暂时没有声音", systemImage: "speaker.slash.fill",
                           detail: "驱动已就绪但后台服务离线。launchd 通常会在几秒内拉起;仍无声则重新运行 Sona 安装包,或从源码:", code: "sudo make install-service")
                        .transition(contentTransition)
                }
                masterCard
                    .transition(contentTransition)
                appsSection
            }
        }
        .padding(UI.outerPadding)
        .frame(width: UI.width)
        .animation(SonaMotion.content(reduceMotion: reduceMotion), value: contentLayout)
    }

    // MARK: - Sound heading

    private var soundHeading: some View {
        Text("声音").font(.system(size: 13, weight: .semibold))
    }

    // MARK: - Master

    private var masterCard: some View {
        AudioControlCard {
            soundHeading
        } controls: {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(model.routedOutputDevices, id: \.uid) { device in
                    DeviceVolumeRow(
                        device: device,
                        volume: model.outputVolume(device.uid),
                        muted: model.outputMuted(device.uid),
                        onVolume: { model.setOutputVolume($0, uid: device.uid) },
                        controlLabel: device.name,
                        unavailableReason: model.canControlOutput(device.uid)
                            ? nil : "设备准备中；旧版驱动需更新后才能调节",
                        onEditingChanged: { model.setOutputEditing($0, uid: device.uid) }
                    )
                    .disabled(!model.canControlOutput(device.uid))
                    .transition(contentTransition)
                }
                if model.routedOutputDevices.isEmpty {
                    Text("没有可用输出设备").font(.caption2).foregroundStyle(.secondary)
                        .transition(contentTransition)
                }
            }
            .animation(SonaMotion.content(reduceMotion: reduceMotion), value: model.routedOutputDevices.map(\.uid))
        } outputPicker: {
            outputPicker
        }
    }

    private var outputPicker: some View {
        DeviceMenu(devices: model.outputDevices,
                   selected: model.defaultTargetUID,
                   onSelect: { model.selectDefaultTarget($0) })
            .accessibilityLabel("切换输出设备")
            .help("切换输出设备：" + model.deviceName(forUID: model.defaultTargetUID))
    }

    // MARK: - Apps

    private var appsSection: some View {
        Group {
            if model.apps.isEmpty {
                Card {
                    HStack(spacing: 8) {
                        Image(systemName: "speaker.wave.2").foregroundStyle(.secondary)
                        Text("还没有应用在播放声音,开始播放后会自动出现。")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
                .transition(contentTransition)
            }
            if !model.apps.isEmpty && !model.canControlAppOutputs {
                Card {
                    Text("更新 Sona 音频服务后可调节各设备音量")
                        .font(.caption2).foregroundStyle(.secondary)
                }
                .transition(contentTransition)
            }
            // Keep one stable ForEach across the empty/nonempty boundary so
            // existing apps retain their native control and focus identity.
            ForEach(model.apps) { app in
                AppCard(app: app)
                    .transition(contentTransition)
            }
        }
    }

    private func notice(_ title: String, systemImage: String, detail: String, code: String) -> some View {
        Card {
            VStack(alignment: .leading, spacing: 6) {
                Label(title, systemImage: systemImage).font(.caption.weight(.semibold))
                Text(detail).font(.caption2).foregroundStyle(.secondary)
                Text(code)
                    .font(.system(.caption2, design: .monospaced))
                    .padding(.horizontal, 8).padding(.vertical, 4)
            }
        }
    }
}

// MARK: - App card (icon · name · device buttons; one volume row per selected device)

private struct AppCard: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let app: AudioApp

    private var setting: AppAudioSetting { model.setting(for: app.key) }
    private var devices: [OutputDevice] { model.appOutputDevices(for: app.key) }
    private var emptyMessage: String {
        setting.selectedTargetUIDs(defaultTarget: model.defaultTargetUID).isEmpty
            ? "未选择输出设备，当前应用不输出声音" : "所选输出设备未连接"
    }

    var body: some View {
        AudioControlCard {
            HStack(spacing: 8) {
                icon
                Text(app.name).font(.system(size: 13, weight: .semibold)).lineLimit(1).truncationMode(.tail)
            }
        } controls: {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(devices, id: \.uid) { device in
                    DeviceVolumeRow(
                        device: device,
                        volume: model.appVolume(device.uid, for: app.key),
                        muted: setting.mute,
                        onVolume: { model.setAppVolume($0, uid: device.uid, for: app.key) },
                        controlLabel: app.name + " · " + device.name,
                        onEditingChanged: { model.setAppEditing($0, for: app.key) }
                    )
                    .transition(SonaMotion.transition(reduceMotion: reduceMotion))
                }
                if devices.isEmpty {
                    Text(emptyMessage)
                        .font(.caption2).foregroundStyle(.secondary)
                        .contentTransition(.opacity)
                        .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: emptyMessage)
                        .transition(SonaMotion.transition(reduceMotion: reduceMotion))
                }
            }
            .animation(SonaMotion.content(reduceMotion: reduceMotion), value: devices.map(\.uid))
            .disabled(!model.canControlAppOutputs)
        } outputPicker: {
            DeviceSelectionButtons(
                devices: model.outputDevices,
                selected: Set(setting.selectedTargetUIDs(defaultTarget: model.defaultTargetUID)),
                appName: app.name,
                onToggle: { model.toggleTarget($0, for: app.key) }
            )
            .disabled(!model.canControlAppOutputs)
        }
        .contextMenu {
            if !setting.isDefault { Button("重置此应用") { model.reset(app.key) } }
        }
    }

    private var icon: some View {
        Group {
            if let img = app.icon {
                Image(nsImage: img).resizable().aspectRatio(contentMode: .fit)
            } else {
                ZStack {
                    RoundedRectangle(cornerRadius: 7, style: .continuous).fill(.quaternary)
                    Image(systemName: "app.dashed").foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: UI.appIcon, height: UI.appIcon)
    }
}

// MARK: - Shared pieces

/// System and app controls share the same header, device controls and volume-row container.
private struct AudioControlCard<Heading: View, Controls: View, OutputPicker: View>: View {
    @ViewBuilder let heading: Heading
    @ViewBuilder let controls: Controls
    @ViewBuilder let outputPicker: OutputPicker

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    heading
                    Spacer(minLength: 6)
                    outputPicker
                }
                .frame(height: UI.control)
                controls
            }
        }
    }
}

/// Every route uses the same device symbol · volume track · loud speaker layout.
private struct DeviceVolumeRow: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let device: OutputDevice
    let volume: Double
    let muted: Bool
    let onVolume: (Double) -> Void
    var controlLabel: String = ""
    var unavailableReason: String? = nil
    var onEditingChanged: (Bool) -> Void = { _ in }

    var body: some View {
        HStack(spacing: 8) {
            DeviceSymbol(device: device)
                .font(.system(size: 13)).frame(width: 16)
                .help(device.name)
                .accessibilityHidden(true)
            ZStack(alignment: .leading) {
                SoundSlider(value: muted ? 0 : volume, label: controlLabel + "音量",
                            onChange: onVolume, onEditingChanged: onEditingChanged)
                    .opacity(unavailableReason == nil ? 1 : 0)
                    .accessibilityHidden(unavailableReason != nil)
                if let unavailableReason {
                    Text(unavailableReason)
                        .font(.caption2).foregroundStyle(.secondary)
                        .lineLimit(1)
                        .help(unavailableReason)
                        .accessibilityLabel(controlLabel + "，" + unavailableReason)
                        .transition(.opacity)
                }
            }
            // Routing state arrives before the hardware level snapshot. Keep
            // pending/ready in the same slot instead of inserting a second row
            // that would reverse the card's height as soon as status arrives.
            .frame(height: UI.control)
            .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: unavailableReason)
            .onDisappear { onEditingChanged(false) }
            .layoutPriority(1)
            Image(systemName: "speaker.wave.3.fill")
                .font(.system(size: 16)).frame(width: 24)
                .accessibilityHidden(true)
        }
    }
}

private struct Card<Content: View>: View {
    @ViewBuilder let content: Content
    var body: some View {
        LiquidGlassCard {
            content
                .padding(.horizontal, UI.cardPaddingH)
                .padding(.vertical, UI.cardPaddingV)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// A device has the same symbol in its routing button and volume row.
private struct DeviceSymbol: View {
    let device: OutputDevice
    var fittingSize: CGFloat? = nil

    @ViewBuilder
    var body: some View {
        if let fittingSize {
            Image(systemName: device.symbolName)
                .resizable()
                .symbolRenderingMode(.palette)
                .scaledToFit()
                .frame(width: fittingSize, height: fittingSize)
        } else {
            Image(systemName: device.symbolName)
        }
    }
}

/// Keep the existing native glass button material for both routing controls.
private enum DeviceControlStyle {
    static let selectedSymbolColor = Color(red: 100 / 255, green: 176 / 255, blue: 1)

    static func glass(selected: Bool = false) -> GlassButtonStyle {
        if #available(macOS 26.1, *) {
            // Tint the native material itself; keep the optical edge and pressed
            // feedback without adding another filled background over the glass.
            return GlassButtonStyle(.clear.tint(selected ? .white.opacity(0.16) : nil))
        }
        return GlassButtonStyle()
    }
}

/// Independent multi-select state with a neutral native surface and a blue symbol.
private struct DeviceSelectionButton: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let device: OutputDevice
    let isSelected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            DeviceSymbol(device: device, fittingSize: 16)
                .fontWeight(.medium)
                .foregroundStyle(isSelected ? DeviceControlStyle.selectedSymbolColor : Color.secondary)
        }
        .buttonStyle(DeviceControlStyle.glass(selected: isSelected))
        .buttonBorderShape(.circle)
        .frame(width: UI.control, height: UI.control)
        .animation(SonaMotion.feedback(reduceMotion: reduceMotion), value: isSelected)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        .accessibilityValue(isSelected ? "已选中" : "未选中")
    }
}

/// Available devices stay directly reachable, including when the strip overflows.
private struct DeviceSelectionButtons: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let devices: [OutputDevice]
    let selected: Set<String>
    let appName: String
    let onToggle: (String) -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                ForEach(devices, id: \.uid) { device in
                    let isSelected = selected.contains(device.uid)
                    DeviceSelectionButton(device: device, isSelected: isSelected) {
                        onToggle(device.uid)
                    }
                    .accessibilityLabel(appName + "输出到" + device.name)
                    .help(device.name + (isSelected ? "：已选中，点击停止输出" : "：未选中，点击输出"))
                    .transition(SonaMotion.transition(reduceMotion: reduceMotion))
                }
            }
            .animation(SonaMotion.content(reduceMotion: reduceMotion), value: devices.map(\.uid))
        }
        .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
        .scrollClipDisabled(false)
        .frame(width: min(CGFloat(devices.count) * UI.control + CGFloat(max(0, devices.count - 1)) * 6, 130),
               height: UI.control)
    }
}

/// The top-level menu retains single selection for the default output.
private struct DeviceMenu: View {
    let devices: [OutputDevice]
    let selected: String
    let onSelect: (String) -> Void

    var body: some View {
        Menu {
            ForEach(devices, id: \.uid) { device in
                Button { onSelect(device.uid) } label: {
                    let isOutput = selected == device.uid
                    Text(isOutput ? "\(device.name)  •" : device.name)
                        .accessibilityLabel(isOutput ? "\(device.name)，当前输出" : device.name)
                }
            }
        } label: {
            Image(systemName: "airplay.audio")
                .font(.system(size: 14, weight: .medium))
                .frame(width: 14, height: 14)
        }
        .menuStyle(.button)
        .buttonStyle(DeviceControlStyle.glass())
        .buttonBorderShape(.circle)
        .menuIndicator(.hidden)
        .frame(width: UI.control, height: UI.control)
    }
}
