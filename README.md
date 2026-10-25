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

**Sona 1.1.0 · macOS 26 or later · Apple Silicon**

[Download Sona 1.1.0](https://github.com/FfszHy/Sona/releases/download/v1.1.0/Sona-1.1.0.pkg) · [Release notes](https://github.com/FfszHy/Sona/releases/tag/v1.1.0)

This release is not Developer ID signed or notarized. See the [release notes](https://github.com/FfszHy/Sona/releases/tag/v1.1.0) for installation help.

1. Run the installer with administrator access, then open Sona.
2. Click the menu bar icon, then hover over or click the output icon beside **声音** (Sound) to choose one default output from the list on the right.
3. Play audio. Apps appear in the panel. Hover over an app's output icon to open its device list. Select several outputs without closing the list, and click a selected device again to remove it. The list closes after the pointer leaves both the icon and list. Multiple output sliders show small device names. Clearing every device silences that app until you select an output again or restart the app or Sona. Selecting only the current default output resumes following the default.

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
