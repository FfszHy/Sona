//
//  MenuBarIcon.swift
//  Renders the shared Flow S mark as template images: idle, playing (3 opacity frames), muted.
//

import AppKit

enum MenuBarIcon {
    enum State: Equatable {
        case idle
        case playing(frame: Int)   // 0...2
        case muted
    }

    private static var cache: [String: NSImage] = [:]

    static func image(for state: State) -> NSImage {
        let key: String
        switch state {
        case .idle: key = "idle"
        case .playing(let f): key = "playing\(f)"
        case .muted: key = "muted"
        }
        if let img = cache[key] { return img }
        let img = render(state)
        cache[key] = img
        return img
    }

    private static func render(_ state: State) -> NSImage {
        let size = NSSize(width: 22, height: 16)
        let image = NSImage(size: size, flipped: true) { _ in
            let opacity: CGFloat
            switch state {
            case .idle: opacity = 0.82
            case .playing(let frame): opacity = [0.90, 1.00, 0.94][max(0, min(frame, 2))]
            case .muted: opacity = 0.42
            }
            NSColor.black.withAlphaComponent(opacity).setFill()
            markPath().fill()

            if state == .muted {
                NSColor.black.setStroke()
                let slash = NSBezierPath()
                slash.lineWidth = 1.5
                slash.lineCapStyle = .round
                slash.move(to: NSPoint(x: 4.1, y: 1.6))
                slash.line(to: NSPoint(x: 18, y: 14.2))
                slash.stroke()
            }
            return true
        }
        image.isTemplate = true
        return image
    }

    /// The same filled contour as pic/SonaMark.svg and pic/SonaIcon.svg.
    /// Apply the master mark's x expansion, then scale uniformly into 22 × 16 pt.
    private static func markPath() -> NSBezierPath {
        func point(_ x: CGFloat, _ y: CGFloat) -> NSPoint {
            NSPoint(x: 2.35 + x * 1.1 * 0.154, y: -0.05 + y * 0.154)
        }
        let path = NSBezierPath()
        path.move(to: point(78, 18))
        path.curve(to: point(24, 17), controlPoint1: point(63, 5), controlPoint2: point(38, 6))
        path.curve(to: point(20, 43), controlPoint1: point(14, 25), controlPoint2: point(13, 35))
        path.curve(to: point(56, 57), controlPoint1: point(27, 51), controlPoint2: point(43, 53))
        path.curve(to: point(70, 70), controlPoint1: point(68, 61), controlPoint2: point(73, 64))
        path.curve(to: point(31, 72), controlPoint1: point(65, 81), controlPoint2: point(43, 81))
        path.curve(to: point(17, 71), controlPoint1: point(27, 68), controlPoint2: point(21, 67))
        path.curve(to: point(18, 85), controlPoint1: point(13, 75), controlPoint2: point(14, 81))
        path.curve(to: point(83, 80), controlPoint1: point(37, 101), controlPoint2: point(70, 99))
        path.curve(to: point(78, 47), controlPoint1: point(94, 65), controlPoint2: point(90, 54))
        path.curve(to: point(42, 36), controlPoint1: point(69, 42), controlPoint2: point(55, 40))
        path.curve(to: point(34, 28), controlPoint1: point(34, 34), controlPoint2: point(31, 32))
        path.curve(to: point(66, 29), controlPoint1: point(39, 21), controlPoint2: point(56, 21))
        path.curve(to: point(81, 27), controlPoint1: point(71, 33), controlPoint2: point(77, 32))
        path.curve(to: point(78, 18), controlPoint1: point(84, 24), controlPoint2: point(82, 21))
        path.close()
        return path
    }
}
