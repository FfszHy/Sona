import Foundation
import CoreGraphics

/// Screen-space placement shared by the sound panel and its output picker.
enum OutputPickerPlacement {
    static let width: CGFloat = 220
    static let gap: CGFloat = 8
    static let reservedWidth: CGFloat = width + gap

    static func mainPanelOriginX(anchorMidX: CGFloat, panelWidth: CGFloat,
                                 visibleFrame: CGRect) -> CGFloat {
        let reservation = visibleFrame.width >= panelWidth + reservedWidth ? reservedWidth : 0
        let maximumX = max(visibleFrame.minX, visibleFrame.maxX - panelWidth - reservation)
        return min(max(anchorMidX - panelWidth / 2, visibleFrame.minX), maximumX)
    }

    static func popupFrame(parentFrame: CGRect, anchorFrame: CGRect,
                           desiredSize: CGSize, visibleFrame: CGRect) -> CGRect {
        var popupWidth = min(max(1, desiredSize.width), max(1, visibleFrame.width))
        let popupHeight = min(max(1, desiredSize.height), max(1, visibleFrame.height))
        let rightX = max(parentFrame.maxX + gap, visibleFrame.minX)
        let leftEdge = min(parentFrame.minX - gap, visibleFrame.maxX)
        let roomOnRight = max(0, visibleFrame.maxX - rightX)
        let roomOnLeft = max(0, leftEdge - visibleFrame.minX)
        let popupX: CGFloat

        if roomOnRight >= popupWidth {
            popupX = rightX
        } else if roomOnLeft >= popupWidth {
            // On a narrow screen the parent cannot reserve the normal right-hand
            // width. Prefer the opposite side to covering any volume controls.
            popupX = leftEdge - popupWidth
        } else if max(roomOnRight, roomOnLeft) > 0 {
            // An unusually narrow display can fit only a smaller picker. Preserve
            // the gap and keep it outside the parent on the roomier side.
            if roomOnRight >= roomOnLeft {
                popupWidth = roomOnRight
                popupX = rightX
            } else {
                popupWidth = roomOnLeft
                popupX = visibleFrame.minX
            }
        } else {
            // If the parent occupies the full visible width, overlap is physically
            // unavoidable. Keep the picker on screen so its controls stay reachable.
            popupX = min(max(rightX, visibleFrame.minX), visibleFrame.maxX - popupWidth)
        }

        let popupY = min(max(anchorFrame.midY - popupHeight / 2, visibleFrame.minY),
                         visibleFrame.maxY - popupHeight)
        return CGRect(x: popupX, y: popupY, width: popupWidth, height: popupHeight)
    }
}
