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

**Preview · macOS 26 or later · Apple Silicon**

For the interface shown above, [build and install the latest source](docs/BUILDING.md). The existing [0.6.0 preview installer](https://github.com/FfszHy/Sona/raw/refs/heads/main/website/public/downloads/Sona-0.6.0.pkg) was built on September 20, 2026 and predates these interface updates. It is not Developer ID signed or notarized; see the [installation guide](docs/BUILDING.md#preview-installer).

1. Open Sona and click its menu bar icon.
2. Choose your default output with the device button beside **声音** (Sound).
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
