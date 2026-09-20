//
//  MenuBarIcon.swift
//  Renders the ((•)) status item icon as template images: idle, playing (3 animation frames), muted.
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
        let image = NSImage(size: size, flipped: false) { _ in
            let center = NSPoint(x: 11, y: 8)
            let radii: [CGFloat] = [3.6, 6.1, 8.6]
            let halfSpan: CGFloat = 42
            let lineWidth: CGFloat = 1.6

            func arcAlpha(_ index: Int) -> CGFloat {
                switch state {
                case .idle:
                    return index == 0 ? 1.0 : (index == 1 ? 0.35 : 0.0)
                case .playing(let frame):
                    return index <= frame ? 1.0 : 0.3
                case .muted:
                    return index == 0 ? 0.45 : (index == 1 ? 0.2 : 0.0)
                }
            }

            for (i, r) in radii.enumerated() {
                let alpha = arcAlpha(i)
                guard alpha > 0 else { continue }
                NSColor.black.withAlphaComponent(alpha).setStroke()
                for side: CGFloat in [0, 180] {
                    let path = NSBezierPath()
                    path.lineWidth = lineWidth
                    path.lineCapStyle = .round
                    path.appendArc(withCenter: center, radius: r, startAngle: side - halfSpan, endAngle: side + halfSpan)
                    path.stroke()
                }
            }

            let dotAlpha: CGFloat = (state == .muted) ? 0.45 : 1.0
            NSColor.black.withAlphaComponent(dotAlpha).setFill()
            let dotR: CGFloat = 1.7
            NSBezierPath(ovalIn: NSRect(x: center.x - dotR, y: center.y - dotR, width: dotR * 2, height: dotR * 2)).fill()

            if state == .muted {
                NSColor.black.setStroke()
                let slash = NSBezierPath()
                slash.lineWidth = 1.8
                slash.lineCapStyle = .round
                slash.move(to: NSPoint(x: 3.5, y: 1.5))
                slash.line(to: NSPoint(x: 18.5, y: 14.5))
                slash.stroke()
            }
            return true
        }
        image.isTemplate = true
        return image
    }
}
