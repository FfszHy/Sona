# Sona

**Sound, your way.**

Control each app's volume and audio outputs from the macOS menu bar.

[简体中文](README.zh-CN.md) · [Build & install](#build-from-source) · [Releases](https://github.com/FfszHy/Sona/releases)

https://github.com/user-attachments/assets/e753e580-f469-4e87-b770-f184ebb99b14

Launch film · 26 seconds · silent. Recorded UI with illustrative staging.

<img src="pic/sona-menu-bar.png" alt="Sona running on macOS, with a top-level sound volume control and independent volume controls for Safari and NetEase Cloud Music." width="720" />

Actual application screenshot, provided by the project author.

## Features

- **App volume.** Adjust each app's volume separately for each selected output; slide to zero to mute that route.
- **Output routing.** Apps follow the current default output, including while playing when you connect headphones or change the system output. Choose another device per app, or play through several devices at once.
- **Device controls.** Adjust output volumes and switch the default device from one panel.
- **Remembered settings.** Keep app volumes and mute settings, including volume levels for each device. Manual output choices survive pauses and reset when that app quits or Sona restarts. Login launch and automatic switching to newly connected devices are optional.

Sona uses a virtual audio output and a separate audio service. It does not request microphone or system recording permission.

## Get started

**Sona 0.6.2 · macOS 26 or later · Apple Silicon**

[Download Sona 0.6.2](https://github.com/FfszHy/Sona/releases/download/v0.6.2/Sona-0.6.2.pkg) · [Release notes](https://github.com/FfszHy/Sona/releases/tag/v0.6.2)

This release is not Developer ID signed or notarized. If macOS blocks the installer, use **Open Anyway** in System Settings → Privacy & Security. Download the [SHA-256 checksum](https://github.com/FfszHy/Sona/releases/download/v0.6.2/Sona-0.6.2.pkg.sha256) alongside the package and verify it before installing:

```sh
cd ~/Downloads
shasum -a 256 -c Sona-0.6.2.pkg.sha256
```

Continue only if verification reports `Sona-0.6.2.pkg: OK`. Installation updates the app, audio service and driver, and briefly interrupts audio.

1. Run the installer with administrator access, then open Sona.
2. Click the menu bar icon and choose your default output with the device button beside **声音** (Sound).
3. Play audio. Apps appear under **应用** (Apps). Select output devices to show their volume sliders; clearing every device silences that app until you select an output again or restart the app or Sona. Selecting only the current default output resumes following the default.

Manual choices of another output, multiple outputs or no output stay in place when the default changes.

Right-click the menu bar icon for login launch, automatic device switching and Quit. On normal exit, Sona attempts to restore the physical output.

## Build from source

Requires Xcode or Command Line Tools with the macOS 26 SDK, plus `python3`. Installing the audio service and driver requires administrator access.

```sh
git clone https://github.com/FfszHy/Sona.git
cd Sona
make
sudo make install-service
sudo make install-driver
make run
```

The app is built at `App/build/Sona.app`; `make run` opens that copy. Optionally use `make install-app` to copy it to `/Applications`. Local builds use ad-hoc signatures. Driver installation restarts `coreaudiod`, briefly interrupting audio.

## Uninstall

Quit Sona from its menu bar icon and confirm that macOS is using a physical output device. For a package installation, run:

```sh
sudo "/Library/Application Support/Sona/uninstall.sh"
```

Add `--purge` to also remove saved settings. For a source installation, run `sudo make uninstall-driver` and `sudo make uninstall-service`; remove `/Applications/Sona.app` if you used `make install-app`.

## Scope

Sona controls apps that send audio to its virtual output. It provides output routing, not microphone routing. Multi-device playback does not guarantee sample-accurate synchronization.

Hardware compatibility, sleep/wake, hot-plug behavior and end-to-end latency remain subject to on-device testing.

## Development

Run `make test-all` for automated checks or `make release` to build an installer and checksum in `build/release/`. Automated checks do not replace on-device installation, upgrade and uninstall testing. See [GitHub Releases](https://github.com/FfszHy/Sona/releases) for release history.

## License

Copyright © 2026 Haoyang Jin. Source available under [FSL-1.1-ALv2](LICENSE). See the license for permitted uses, the competing-use restriction and conversion to Apache 2.0.
