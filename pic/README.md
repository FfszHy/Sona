# Sona icons

`sona-menu-bar.png` is the latest application screenshot provided by the project author on October 19, 2026, used in both READMEs. It shows a top-level sound volume control and separate controls for Safari and NetEase Cloud Music. The original image is preserved without retouching.

`SonaIcon.png` is the 1024 px App icon source used by the root Makefile to build `App/Resources/AppIcon.icns`. `SonaIcon.svg` is its editable vector artwork. `SonaMark.svg` is the shared, unstyled Flow S contour.

`App/Sources/Sona/MenuBarIcon.swift` draws the same contour with AppKit in a 22 × 16 pt template image. Its control points use the original coordinates before the master mark's 1.1× horizontal expansion; `point()` applies that expansion and the menu-bar scale/translation. Keep the App and template contours together when changing the artwork. Playback changes opacity, and mute adds a separate slash.

The shared geometry and export script live in `Tools/icons/mark.json` and `Tools/icons/render.cjs`. To regenerate the App SVG/PNG, iconset, menu-bar state samples and contour verification report, run from the repository root with Node.js and a `node_modules` directory containing `sharp`:

```sh
NODE_PATH="/path/to/node_modules" node Tools/icons/render.cjs
iconutil -c icns Tools/build/icons/app/Sona.iconset -o Tools/build/icons/app/Sona-AppIcon.icns
```

Exports go to the ignored `Tools/build/icons/` directory and are removed by `make clean`. They do not replace the production resources automatically. Review the exports before copying any approved changes into `pic/` or `App/Resources/`. SVG generation is deterministic; PNG output can vary with the Sharp/libvips version.
