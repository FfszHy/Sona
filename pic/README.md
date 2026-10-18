# Sona icons

`SonaIcon.png` is the 1024 px App icon source used by the root Makefile to build `App/Resources/AppIcon.icns`. `SonaIcon.svg` is its editable vector artwork. `SonaMark.svg` is the shared, unstyled Flow S contour.

`App/Sources/Sona/MenuBarIcon.swift` draws the same contour with AppKit in a 22 × 16 pt template image. Its control points use the original coordinates before the master mark's 1.1× horizontal expansion; `point()` applies that expansion and the menu-bar scale/translation. Keep the App and template contours together when changing the artwork. Playback changes opacity, and mute adds a separate slash.
