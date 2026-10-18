# Sona

**Sound, your way.**

Control each app's volume and audio outputs from the macOS menu bar.

[简体中文](README.zh-CN.md) · [Build & install](docs/BUILDING.md) · [Changelog](CHANGELOG.md)

<img src="pic/sona-menu-bar.png" alt="Sona running on macOS, with output volume and independent volume controls for NetEase Cloud Music." width="720" />

Actual application screenshot, provided by the project author.

## Features

- **App volume.** Adjust each playing app independently; slide to zero to mute.
- **Output routing.** Use the default output, choose a device per app, or play through several devices at once.
- **Device controls.** Adjust output volumes and switch the default device from one panel.
- **Remembered settings.** Keep app volumes and output choices between sessions. Login launch and automatic switching to newly connected devices are optional.

Sona uses a virtual audio output and a separate audio service. It does not request microphone or system recording permission.

## Get started

**0.6.1 Preview · macOS 26 or later · Apple Silicon**

[Download Sona 0.6.1](https://github.com/FfszHy/Sona/releases/download/v0.6.1/Sona-0.6.1.pkg) · [Release notes](https://github.com/FfszHy/Sona/releases/tag/v0.6.1)

The preview is not Developer ID signed or notarized. If macOS blocks the installer, use **Open Anyway** in System Settings → Privacy & Security. See the [installation guide](docs/BUILDING.md#preview-installer) for checksum verification and details.

1. Run the installer with administrator access, then open Sona.
2. Click the menu bar icon and choose your default output with the device button beside **声音** (Sound).
3. Play audio. Apps appear under **应用** (Apps), with their own volume and output controls.

Right-click the menu bar icon for login launch, automatic device switching and Quit. On normal exit, Sona attempts to restore the physical output.

## Build from source

Requires Xcode or Command Line Tools with the macOS 26 SDK, plus `python3`.

```sh
git clone https://github.com/FfszHy/Sona.git
cd Sona
make
```

The app is built at `App/build/Sona.app`. Install the audio service and driver before launching it; follow the [build and installation guide](docs/BUILDING.md).

## Scope

Sona controls apps that send audio to its virtual output. It provides output routing, not microphone routing. Multi-device playback does not guarantee sample-accurate synchronization.

Hardware compatibility, sleep/wake, hot-plug behavior and end-to-end latency remain subject to on-device testing. See [compatibility and test records](docs/COMPATIBILITY.md) for the verified scope.

## Development

[Architecture](ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md) · [Build, diagnostics & uninstall](docs/BUILDING.md) · [Release checks](docs/RELEASE-CHECKLIST.md)

## License

Copyright © 2026 Haoyang Jin. Source available under [FSL-1.1-ALv2](LICENSE). See the license for permitted uses, the competing-use restriction and conversion to Apache 2.0.
