# Sona icons

`sona-menu-bar.png` is the latest application screenshot provided by the project author on October 19, 2026, used in both READMEs and on the website. It shows a top-level sound volume control and separate controls for Safari and NetEase Cloud Music. The original image is preserved without retouching. Other source images remain unchanged.

`SonaIcon.png` is the 1024 px App icon source used by the root Makefile to build `App/Resources/AppIcon.icns`. `SonaIcon.svg` is its editable vector artwork. `SonaMark.svg` is the shared, unstyled Flow S contour.

`App/Sources/Sona/MenuBarIcon.swift` draws the same contour with AppKit in a 22 × 16 pt template image. Its control points use the original coordinates before the master mark's 1.1× horizontal expansion; `point()` applies that expansion and the menu-bar scale/translation. Keep the App and template contours together when changing the artwork. Playback changes opacity, and mute adds a separate slash.
