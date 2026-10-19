# Build, install and maintain Sona

## Requirements

- macOS 26 or later, on Apple Silicon.
- Xcode or Command Line Tools with the macOS 26 SDK, and `python3`.
- Administrator access to install the audio driver and service.

The app targets arm64. The driver and service build for arm64/x86_64 with a macOS 14 deployment target; this does not establish Intel or macOS 14 support for the full product. See [compatibility records](COMPATIBILITY.md).

## Build and install the latest source

From the repository root:

```sh
make test-all
make
sudo make install-service
sudo make install-driver
make run
```

The app is built at `App/build/Sona.app`. `make run` launches that copy; optionally use `make install-app` to copy it to `/Applications`. The current build uses local ad-hoc signatures.

Installing the driver writes to `/Library/Audio/Plug-Ins/HAL` and restarts `coreaudiod`, briefly interrupting audio. The service is installed at `/Library/PrivilegedHelperTools/SonaAudioService` and registered as a system LaunchDaemon. Driver installation requires the final binary to pass `make check-driver-boundary`.

## Using the current interface

Click the menu bar icon to open the sound panel. The top device button selects the default physical output; its slider controls that output's volume. Playing apps appear below with independent volume sliders and device menus. An app can follow the default output or use one or more selected devices. Multiple routed outputs expose separate device volume controls.

Sona activates its virtual output automatically when the driver connects. There is no main on/off switch in the current interface. Right-click the menu bar icon for login launch, automatic switching to newly connected devices and Quit. Normal exit attempts to restore the selected physical output.

If the service is unavailable, audio may be silent. Select a physical device in System Settings → Sound to restore playback, then check the service or reinstall it.

## Preview installer

[Sona 0.6.1 Preview](https://github.com/FfszHy/Sona/releases/tag/v0.6.1) (build 10) includes the current panel, icon and slider updates. Download the installer and its checksum from the same release.

This preview is not Developer ID signed or notarized. If macOS blocks it, open System Settings → Privacy & Security and use **Open Anyway** for the installer. To install from Terminal:

```sh
cd ~/Downloads
curl -fLO https://github.com/FfszHy/Sona/releases/download/v0.6.1/Sona-0.6.1.pkg
curl -fLO https://github.com/FfszHy/Sona/releases/download/v0.6.1/Sona-0.6.1.pkg.sha256
shasum -a 256 -c Sona-0.6.1.pkg.sha256
```

Continue with installation only if checksum verification reports `Sona-0.6.1.pkg: OK`:

```sh
sudo installer -pkg ~/Downloads/Sona-0.6.1.pkg -target /
```

The installer places the app, driver, service, LaunchDaemon and uninstall script, then starts the service, restarts `coreaudiod` and opens Sona. Installing interrupts audio briefly.

## Uninstall

First right-click the Sona menu bar icon and choose **退出 Sona** (Quit Sona), then confirm the system output is a physical device.

For a package installation:

```sh
sudo "/Library/Application Support/Sona/uninstall.sh"
# Also remove saved settings:
sudo "/Library/Application Support/Sona/uninstall.sh" --purge
```

For a source installation:

```sh
sudo make uninstall-driver
sudo make uninstall-service
```

If you used `make install-app`, also remove `/Applications/Sona.app`. Source uninstall targets retain preferences and service state. Settings are stored in app UserDefaults and `/Library/Application Support/Sona/service-state.plist`.

## Build an installer

```sh
make release
```

This builds the native components and writes `build/release/Sona-<version>.pkg` and `.sha256`. Version values must agree across `App/Info.plist`, `Driver/Info.plist` and `Shared/SonaProtocol.h`. Upload the package and checksum to GitHub Releases after validation.

Without credentials the binaries are ad-hoc signed and the installer is unsigned. For Developer ID signing and notarization, configure `SONA_APP_IDENTITY`, `SONA_INSTALLER_IDENTITY` and `SONA_NOTARY_PROFILE` as documented in [the packaging script](../Installer/build-installer.sh). Follow the [release checklist](RELEASE-CHECKLIST.md), including installation, upgrade and uninstall verification.

## Tests and diagnostics

`make test-all` runs engine, resampler, app volume writer, transport, clock and driver boundary checks. Changes to IPC, peer identity or service lifecycle also need `make test-e2e`; it temporarily registers a service in the current user's launchd domain and may open real outputs. It does not replace system-domain acceptance testing.

```sh
make loopback-tools
Tools/build/sonactl service
log show --last 2m --predicate 'subsystem == "com.sona.driver" OR subsystem == "com.sona.audio-service"'
```

Check diagnostic output for app names, device identifiers and other personal information before sharing it. See [architecture](../ARCHITECTURE.md), [historical regression notes](DEVELOPMENT-NOTES.md) and [compatibility records](COMPATIBILITY.md) for test scope and remaining work.

## Clean generated files

```sh
make clean
```

Removes native build outputs, release staging, test binaries and Python bytecode. Source, application icon design files and local configuration remain in place.
