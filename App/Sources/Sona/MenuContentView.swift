//
//  MenuContentView.swift
//  The popover shown from the menu bar icon: header · master card · app cards.
//

import SwiftUI

private enum UI {
    static let width: CGFloat = 336
    static let outerPadding: CGFloat = 12
    static let cardRadius: CGFloat = 20
    static let controlRadius: CGFloat = 11
    static let cardPaddingH: CGFloat = 12
    static let cardPaddingV: CGFloat = 10
    static let cardGap: CGFloat = 7
    static let control: CGFloat = 24      // mute buttons, menu height
    static let headerControl: CGFloat = 34 // full-height settings menu target
    static let appIcon: CGFloat = 28
    static let maxVisibleRows = 6
}

struct MenuContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if !model.driverInstalled {
                notice("需要安装 Sona 驱动", systemImage: "exclamationmark.triangle.fill",
                       detail: "驱动装在 /Library/Audio/Plug-Ins/HAL,需要管理员密码。请重新运行 Sona 安装包;从源码构建时在项目目录运行:", code: "sudo make install-driver")
            } else if !model.driverConnected {
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

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 10) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: 34, height: 34)
            VStack(alignment: .leading, spacing: 0) {
                Text("Sona").font(.headline)
                Text("Sound, your way.").font(.caption2).foregroundStyle(.secondary)
            }
            Spacer(minLength: 8)
            Menu {
                Toggle("登录时启动", isOn: $model.launchAtLogin)
                Toggle("新设备接入时自动切换", isOn: $model.followNewDevices)
                Divider()
                Button("退出 Sona") {
                    model.restoreSystemOutput()
                    NSApp.terminate(nil)
                }
            } label: {
                Image(systemName: "gearshape")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.primary)
                    .frame(width: UI.headerControl, height: UI.headerControl)
                    .contentShape(Rectangle())
            }
            .menuStyle(.button)
            .buttonStyle(.plain)
            .menuIndicator(.hidden)
            .controlSize(.regular)
            .fixedSize()
            .accessibilityLabel("设置")
            .help("Sona 设置")
        }
        .padding(.horizontal, 2)
    }

    // MARK: - Master

    private var masterCard: some View {
        Card {
            if model.routedOutputDevices.count > 1 {
                VStack(alignment: .leading, spacing: 10) {
                    Text("输出设备").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
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
                                onMute: { model.setOutputMuted(!model.outputMuted(device.uid), uid: device.uid) },
                                routeSymbol: device.symbolName,
                                routeHelp: "切换默认输出",
                                devices: model.outputDevices,
                                selected: [model.defaultTargetUID],
                                allowsMultiple: false,
                                onToggle: { model.selectDefaultTarget($0) },
                                onFollowDefault: nil,
                                showsDeviceMenu: device.uid == model.defaultTargetUID,
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
                VolumeRow(
                    volume: model.masterVolume,
                    muted: model.masterMuted,
                    onVolume: { model.setMasterVolume($0) },
                    onMute: { model.setMasterMuted(!model.masterMuted) },
                    routeSymbol: model.outputDevices.first { $0.uid == model.defaultTargetUID }?.symbolName ?? "speaker.wave.2",
                    routeHelp: model.deviceName(forUID: model.defaultTargetUID),
                    devices: model.outputDevices,
                    selected: [model.defaultTargetUID],
                    allowsMultiple: false,
                    onToggle: { uid in model.selectDefaultTarget(uid) },
                    onFollowDefault: nil
                )
            }
        }
    }

    // MARK: - Apps

    private var appsSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("应用").font(.caption.weight(.semibold))
                Spacer()
                let n = model.apps.filter(\.running).count
                Text(n == 0 ? "没有在播放" : "\(n) 个正在播放").font(.caption2).foregroundStyle(.secondary)
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
                    .background(.quaternary.opacity(0.5), in: Capsule())
            }
        }
    }
}

// MARK: - App card (row 1: icon · name · status; row 2: the same controls as the master card)

private struct AppCard: View {
    static let estimatedHeight: CGFloat = UI.appIcon + 6 + UI.control + UI.cardPaddingV * 2
    @EnvironmentObject private var model: AppModel
    let app: AudioApp

    private var setting: AppAudioSetting { model.setting(for: app.key) }

    var body: some View {
        Card {
            VStack(spacing: 6) {
                HStack(spacing: 8) {
                    icon
                    Text(app.name).font(.caption.weight(.semibold)).lineLimit(1).truncationMode(.tail)
                    Spacer(minLength: 6)
                    HStack(spacing: 4) {
                        Circle().fill(app.running ? Color.green : Color.secondary.opacity(0.4)).frame(width: 5, height: 5)
                        Text(statusText).font(.caption2).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    }
                }
                VolumeRow(
                    volume: setting.volume,
                    muted: setting.mute,
                    onVolume: { v in model.update(app.key) { $0.volume = v } },
                    onMute: { model.update(app.key) { $0.mute.toggle() } },
                    routeSymbol: routeSymbol,
                    routeHelp: routeDescription,
                    devices: model.outputDevices,
                    selected: Set(setting.targets),
                    allowsMultiple: true,
                    onToggle: { uid in model.toggleTarget(uid, for: app.key) },
                    onFollowDefault: { model.update(app.key) { $0.targets = [] } }
                )
            }
        }
        .opacity(app.running ? 1 : 0.75)
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

    private var statusText: String {
        app.running ? "正在播放" : "空闲"
    }

    private var routeDescription: String {
        switch setting.targets.count {
        case 0: return "跟随默认输出"
        case 1: return model.deviceName(forUID: setting.targets[0])
        default: return setting.targets.map { model.deviceName(forUID: $0) }.joined(separator: " + ")
        }
    }

    private var routeSymbol: String {
        if setting.targets.count > 1 { return "hifispeaker.2" }
        let uid = setting.targets.first ?? model.defaultTargetUID
        return model.outputDevices.first { $0.uid == uid }?.symbolName ?? "speaker.wave.2"
    }
}

// MARK: - Shared pieces

/// The one control row used by every card: speaker glyph · slider · % · mute · device button.
/// Fixed widths on the right-hand items keep all cards column-aligned.
private struct VolumeRow: View {
    let volume: Double
    let muted: Bool
    let onVolume: (Double) -> Void
    let onMute: () -> Void
    let routeSymbol: String
    let routeHelp: String
    let devices: [OutputDevice]
    let selected: Set<String>
    let allowsMultiple: Bool
    let onToggle: (String) -> Void
    let onFollowDefault: (() -> Void)?
    var showsDeviceMenu: Bool = true
    var controlLabel: String = ""
    var onEditingChanged: (Bool) -> Void = { _ in }

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: volumeSymbol(muted ? 0 : volume))
                .font(.system(size: 11)).foregroundStyle(.secondary).frame(width: 16)
            Slider(value: Binding(get: { volume }, set: onVolume), in: 0...1, onEditingChanged: onEditingChanged)
                .disabled(muted)
                .accessibilityLabel(controlLabel + "音量")
                .onDisappear { onEditingChanged(false) }
                .layoutPriority(1)
            Text("\(Int((volume * 100).rounded()))%")
                .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                .frame(width: 30, alignment: .trailing)
            MuteButton(muted: muted, action: onMute)
                .accessibilityLabel(controlLabel + (muted ? "取消静音" : "静音"))
            if showsDeviceMenu {
                DeviceMenu(symbol: routeSymbol, devices: devices, selected: selected,
                           allowsMultiple: allowsMultiple, onToggle: onToggle, onFollowDefault: onFollowDefault)
                    .frame(width: 44)
                    .help(routeHelp)
            } else {
                Color.clear.frame(width: 44, height: UI.control).accessibilityHidden(true)
            }
        }
    }
}

private struct Card<Content: View>: View {
    @ViewBuilder let content: Content
    var body: some View {
        content
            .padding(.horizontal, UI.cardPaddingH)
            .padding(.vertical, UI.cardPaddingV)
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassEffect(.regular, in: .rect(cornerRadius: UI.cardRadius, style: .continuous))
    }
}

private struct MuteButton: View {
    let muted: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            Image(systemName: muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(muted ? Color.red : Color.primary)
                .frame(width: UI.control, height: UI.control)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .circle)
        .help(muted ? "取消静音" : "静音")
    }
}

/// A compact icon-only device picker. Single-select for the default output, multi-select for
/// per-app routing.
private struct DeviceMenu: View {
    @EnvironmentObject private var model: AppModel
    let symbol: String
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
            HStack(spacing: 5) {
                Image(systemName: symbol).font(.system(size: 10, weight: .medium))
                Image(systemName: "chevron.down").font(.system(size: 7, weight: .semibold)).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity)
            .frame(height: UI.control)
            .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .glassEffect(.regular.interactive(), in: .rect(cornerRadius: UI.controlRadius, style: .continuous))
    }
}

private func volumeSymbol(_ v: Double) -> String {
    switch v {
    case ..<0.01: return "speaker.slash"
    case ..<0.34: return "speaker.wave.1"
    case ..<0.67: return "speaker.wave.2"
    default: return "speaker.wave.3"
    }
}
