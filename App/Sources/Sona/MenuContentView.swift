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
    static let cardGap: CGFloat = 7
    static let control: CGFloat = 28
    static let appIcon: CGFloat = 24
    static let maxVisibleRows = 6
}

struct MenuContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !model.driverInstalled {
                soundHeading
                notice("需要安装 Sona 驱动", systemImage: "exclamationmark.triangle.fill",
                       detail: "驱动装在 /Library/Audio/Plug-Ins/HAL,需要管理员密码。请重新运行 Sona 安装包;从源码构建时在项目目录运行:", code: "sudo make install-driver")
            } else if !model.driverConnected {
                soundHeading
                notice("驱动已安装,等待设备出现", systemImage: "hourglass",
                       detail: "刚安装的话请稍等几秒;仍不出现就重启 coreaudiod。", code: "sudo killall coreaudiod")
            } else {
                if model.status?.serviceConnected == false {
                    notice("Sona Audio Service 未运行,暂时没有声音", systemImage: "speaker.slash.fill",
                           detail: "驱动已就绪但后台服务离线。launchd 通常会在几秒内拉起;仍无声则重新运行 Sona 安装包,或从源码:", code: "sudo make install-service")
                }
                masterCard
                appsSection
            }
        }
        .padding(UI.outerPadding)
        .frame(width: UI.width)
        .controlSize(.small)
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
            if model.routedOutputDevices.count > 1 {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(model.routedOutputDevices) { device in
                        VStack(alignment: .leading, spacing: 4) {
                            HStack(spacing: 6) {
                                Label(device.name, systemImage: device.symbolName)
                                    .font(.caption.weight(.medium)).lineLimit(1)
                                    .help(device.name)
                                Spacer(minLength: 4)
                                if device.uid == model.defaultTargetUID {
                                    Text("默认").font(.caption2).foregroundStyle(.secondary)
                                }
                            }
                            VolumeRow(
                                volume: model.outputVolume(device.uid),
                                muted: model.outputMuted(device.uid),
                                onVolume: { model.setOutputVolume($0, uid: device.uid) },
                                controlLabel: device.name,
                                onEditingChanged: { model.setOutputEditing($0, uid: device.uid) }
                            )
                            .disabled(!model.canControlOutput(device.uid))
                            if !model.canControlOutput(device.uid) {
                                Text("设备准备中；旧版驱动需更新后才能调节")
                                    .font(.caption2).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            } else {
                VolumeRow(volume: model.masterVolume, muted: model.masterMuted,
                          onVolume: { model.setMasterVolume($0) }, controlLabel: "输出")
            }
        } outputPicker: {
            outputPicker
        }
    }

    private var outputPicker: some View {
        DeviceMenu(devices: model.outputDevices,
                   selected: [model.defaultTargetUID], allowsMultiple: false,
                   onToggle: { model.selectDefaultTarget($0) }, onFollowDefault: nil)
            .accessibilityLabel("切换输出设备")
            .help("切换输出设备：" + model.deviceName(forUID: model.defaultTargetUID))
    }

    // MARK: - Apps

    private var appsSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("应用").font(.caption.weight(.semibold))
                Spacer()
            }
            .padding(.horizontal, 4)

            if model.apps.isEmpty {
                Card {
                    HStack(spacing: 8) {
                        Image(systemName: "speaker.wave.2").foregroundStyle(.secondary)
                        Text("还没有应用在播放声音,开始播放后会自动出现。")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
            } else if model.apps.count <= UI.maxVisibleRows {
                VStack(spacing: UI.cardGap) {
                    ForEach(model.apps) { app in AppCard(app: app) }
                }
            } else {
                ScrollView(showsIndicators: false) {
                    VStack(spacing: UI.cardGap) {
                        ForEach(model.apps) { app in AppCard(app: app) }
                    }
                }
                .frame(height: CGFloat(UI.maxVisibleRows) * (AppCard.estimatedHeight + UI.cardGap))
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

// MARK: - App card (row 1: icon · name · output picker; row 2: volume)

private struct AppCard: View {
    static let estimatedHeight: CGFloat = UI.control + 6 + UI.control + UI.cardPaddingV * 2
    @EnvironmentObject private var model: AppModel
    let app: AudioApp

    private var setting: AppAudioSetting { model.setting(for: app.key) }

    var body: some View {
        AudioControlCard {
            HStack(spacing: 8) {
                icon
                Text(app.name).font(.system(size: 13, weight: .semibold)).lineLimit(1).truncationMode(.tail)
            }
        } controls: {
            VolumeRow(volume: setting.volume, muted: setting.mute,
                      onVolume: { model.setAppVolume($0, for: app.key) }, controlLabel: app.name)
        } outputPicker: {
            DeviceMenu(devices: model.outputDevices,
                       selected: Set(setting.targets), allowsMultiple: true,
                       onToggle: { model.toggleTarget($0, for: app.key) },
                       onFollowDefault: { model.update(app.key) { $0.targets = [] } })
                .accessibilityLabel(app.name + "输出设备")
                .help("切换输出设备：" + routeDescription)
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

    private var routeDescription: String {
        switch setting.targets.count {
        case 0: return "跟随默认输出"
        case 1: return model.deviceName(forUID: setting.targets[0])
        default: return setting.targets.map { model.deviceName(forUID: $0) }.joined(separator: " + ")
        }
    }
}

// MARK: - Shared pieces

/// Both system and app controls share the same header, output button and full-width slider layout.
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

/// Control Center layout: quiet speaker · volume track · loud speaker.
private struct VolumeRow: View {
    let volume: Double
    let muted: Bool
    let onVolume: (Double) -> Void
    var controlLabel: String = ""
    var onEditingChanged: (Bool) -> Void = { _ in }

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: muted || volume == 0 ? "speaker.slash.fill" : "speaker.fill")
                .font(.system(size: 13)).frame(width: 16)
                .accessibilityHidden(true)
            SoundSlider(value: muted ? 0 : volume, label: controlLabel + "音量",
                        onChange: onVolume, onEditingChanged: onEditingChanged)
                .frame(height: UI.control)
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
        // The panel's NSGlassEffectView supplies the shared Liquid Glass surface.
        // Keep each group transparent so the native material remains visible.
        content
            .padding(.horizontal, UI.cardPaddingH)
            .padding(.vertical, UI.cardPaddingV)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A compact icon-only device picker. Single-select for the default output, multi-select for
/// per-app routing.
private struct DeviceMenu: View {
    @EnvironmentObject private var model: AppModel
    let devices: [OutputDevice]
    let selected: Set<String>
    let allowsMultiple: Bool
    let onToggle: (String) -> Void
    let onFollowDefault: (() -> Void)?

    var body: some View {
        Menu {
            if let onFollowDefault {
                Text("默认输出")
                Button(action: onFollowDefault) {
                    let name = model.deviceName(forUID: model.defaultTargetUID)
                    Text(selected.isEmpty ? "\(name)  •" : name)
                        .accessibilityLabel(selected.isEmpty ? "\(name)，跟随默认输出" : "跟随默认输出：\(name)")
                }
                .disabled(!devices.contains { $0.uid == model.defaultTargetUID })
                Divider()
                Text("自定义输出")
            }
            ForEach(devices) { d in
                Button { onToggle(d.uid) } label: {
                    let isOutput = selected.contains(d.uid)
                    Text(isOutput ? "\(d.name)  •" : d.name)
                        .accessibilityLabel(isOutput ? "\(d.name)，当前输出" : d.name)
                }
            }
            if allowsMultiple {
                Divider()
                Text("可以选多个设备同时输出")
            }
        } label: {
            Image(systemName: "airplay.audio")
                .font(.system(size: 14, weight: .medium))
                .frame(width: 14, height: 14)
        }
        .menuStyle(.button)
        .buttonStyle(glassStyle)
        .buttonBorderShape(.circle)
        .menuIndicator(.hidden)
        .frame(width: UI.control, height: UI.control)
    }

    private var glassStyle: GlassButtonStyle {
        if #available(macOS 26.1, *) { return GlassButtonStyle(.clear) }
        return GlassButtonStyle()
    }
}
