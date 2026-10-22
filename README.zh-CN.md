# Sona

**Sound, your way.**

在 macOS 菜单栏，独立控制每个应用的音量与音频输出。

[English](README.md) · [构建与安装](#从源码构建) · [版本发布](https://github.com/FfszHy/Sona/releases)

https://github.com/user-attachments/assets/e753e580-f469-4e87-b770-f184ebb99b14

发布片 · 26 秒 · 无声。原生界面录屏与创意场景合成。

<img src="pic/sona-menu-bar.png" alt="Sona 在 macOS 上的实际运行界面，顶部为声音音量控制，下方为 Safari 浏览器与网易云音乐的独立音量控制。" width="720" />

实际应用截图，由项目作者提供。

## 功能

- **应用音量**：为每个应用的各个输出设备独立调节音量，音量归零即可静音该路输出。
- **输出选择**：应用默认跟随当前默认输出，播放中连接耳机或更改系统输出也会随之切换。需要时可为应用另选设备，或同时输出到多个设备。
- **设备控制**：在同一面板调节输出音量、切换默认设备。
- **记住偏好**：保留应用音量、静音与各设备的音量值；手动输出选择在暂停后保留，退出该应用或重启 Sona 后重新跟随默认输出。可选登录启动和新设备接入时自动切换。

Sona 通过虚拟音频输出与独立音频服务处理声音，不申请麦克风或系统录音权限。

## 开始使用

**Sona 0.6.2 · macOS 26 或更高版本 · Apple Silicon**

[下载 Sona 0.6.2](https://github.com/FfszHy/Sona/releases/download/v0.6.2/Sona-0.6.2.pkg) · [版本说明](https://github.com/FfszHy/Sona/releases/tag/v0.6.2)

此版本未使用 Developer ID 签名，也未经过 Apple 公证。如果 macOS 拦截安装器，请在「系统设置 → 隐私与安全性」中选择「仍要打开」。将 [SHA-256 校验文件](https://github.com/FfszHy/Sona/releases/download/v0.6.2/Sona-0.6.2.pkg.sha256)与安装包下载到同一目录，安装前执行校验：

```sh
cd ~/Downloads
shasum -a 256 -c Sona-0.6.2.pkg.sha256
```

仅在显示 `Sona-0.6.2.pkg: OK` 后继续安装。安装包会更新 App、音频服务与驱动，并短暂中断音频。

1. 使用管理员权限运行安装包，然后打开 Sona。
2. 点击菜单栏图标，再点击「声音」右侧的设备按钮，选择默认输出。
3. 播放音频后，应用会显示在「应用」列表中。选中输出设备后可分别调节其音量；取消全部设备即让该应用无输出，直到重新选择输出、重启该应用或重启 Sona。仅选中当前默认设备时，恢复跟随默认输出。

手动选择其他设备、多选或取消全部设备后，更改默认输出会保留这些选择。

右键点击菜单栏图标，可设置登录启动、新设备自动切换或退出。正常退出时，Sona 会尝试恢复物理输出设备。

## 从源码构建

需要支持 macOS 26 SDK 的 Xcode 或命令行工具，以及 `python3`。安装音频服务和驱动需要管理员权限。

```sh
git clone https://github.com/FfszHy/Sona.git
cd Sona
make
sudo make install-service
sudo make install-driver
make run
```

App 生成于 `App/build/Sona.app`，`make run` 会打开该副本；也可执行 `make install-app` 将其复制到 `/Applications`。本地构建使用 ad-hoc 签名。安装驱动会重启 `coreaudiod`，短暂中断音频。

## 卸载

从菜单栏图标退出 Sona，并确认 macOS 已切换到物理输出设备。使用安装包安装的版本可执行：

```sh
sudo "/Library/Application Support/Sona/uninstall.sh"
```

添加 `--purge` 可一并删除已保存的设置。源码安装的版本需执行 `sudo make uninstall-driver` 和 `sudo make uninstall-service`；如果使用过 `make install-app`，还需删除 `/Applications/Sona.app`。

## 使用范围

Sona 仅处理发送到其虚拟输出的应用音频，不提供麦克风路由。多设备播放不保证采样级同步。

设备兼容性、睡眠唤醒、热插拔和端到端延迟仍需实机验证。

## 开发

执行 `make test-all` 运行自动检查，或执行 `make release` 在 `build/release/` 生成安装包与校验文件。自动检查不能代替实机安装、升级和卸载验证。历史版本见 [GitHub Releases](https://github.com/FfszHy/Sona/releases)。

## 许可证

Copyright © 2026 Haoyang Jin。源码采用 [FSL-1.1-ALv2](LICENSE) 授权。允许的使用范围、竞争使用限制和转为 Apache 2.0 的条款以许可证原文为准。
