import QuartzCore
import SwiftUI

/// A perceptible, non-bouncing rhythm for changes to the sound panel's contents.
enum SonaMotion {
    static let contentDuration = 0.36
    static let feedbackDuration = 0.12
    static let reducedDuration = 0.14

    static var contentTimingFunction: CAMediaTimingFunction {
        CAMediaTimingFunction(controlPoints: 0.25, 0.1, 0.25, 1)
    }

    static func content(reduceMotion: Bool) -> Animation? {
        // Layout changes are immediate with Reduce Motion; controls still provide
        // their native feedback, without moving whole rows or cards.
        reduceMotion ? nil : .timingCurve(0.25, 0.1, 0.25, 1, duration: contentDuration)
    }

    static func feedback(reduceMotion: Bool) -> Animation {
        .easeOut(duration: reduceMotion ? reducedDuration : feedbackDuration)
    }

    static func transition(reduceMotion: Bool) -> AnyTransition {
        reduceMotion ? .opacity : .opacity.combined(with: .offset(y: -4))
    }
}
