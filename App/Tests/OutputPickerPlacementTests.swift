import Foundation
import CoreGraphics

@main
struct OutputPickerPlacementTests {
    static func main() {
        let screen = CGRect(x: 0, y: 0, width: 1440, height: 900)
        let panelWidth: CGFloat = 336
        let pickerSize = CGSize(width: OutputPickerPlacement.width, height: 180)

        func assertVisible(_ popup: CGRect, in visible: CGRect) {
            assert(popup.minX >= visible.minX && popup.maxX <= visible.maxX)
            assert(popup.minY >= visible.minY && popup.maxY <= visible.maxY)
        }

        func rightPopup(anchorMidX: CGFloat, in visible: CGRect) -> CGRect {
            let x = OutputPickerPlacement.mainPanelOriginX(anchorMidX: anchorMidX,
                                                           panelWidth: panelWidth,
                                                           visibleFrame: visible)
            let parent = CGRect(x: x, y: 200, width: panelWidth, height: 500)
            let anchor = CGRect(x: parent.maxX - 40, y: 400, width: 28, height: 28)
            let popup = OutputPickerPlacement.popupFrame(parentFrame: parent, anchorFrame: anchor,
                                                         desiredSize: pickerSize, visibleFrame: visible)
            assert(popup.minX == parent.maxX + OutputPickerPlacement.gap)
            assert(popup.width == pickerSize.width)
            assert(popup.midY == anchor.midY)
            assertVisible(popup, in: visible)
            return popup
        }

        // A status item at the right edge reserves room before the picker opens.
        let right = rightPopup(anchorMidX: 1430, in: screen)
        assert(right.maxX == screen.maxX)
        let centeredX = OutputPickerPlacement.mainPanelOriginX(anchorMidX: 720,
                                                               panelWidth: panelWidth,
                                                               visibleFrame: screen)
        assert(centeredX == 552)

        // Left-edge clamping must not push the parent or picker offscreen.
        let left = rightPopup(anchorMidX: 10, in: screen)
        assert(left.minX == panelWidth + OutputPickerPlacement.gap)

        // Displays left of the main display have negative global X coordinates.
        let leftDisplay = CGRect(x: -1920, y: 0, width: 1920, height: 1080)
        let external = rightPopup(anchorMidX: -10, in: leftDisplay)
        assert(external.maxX == 0)

        // Clamp the complete popup height, including displays below the main one.
        let lowerDisplay = CGRect(x: -1600, y: -900, width: 1600, height: 860)
        let parent = CGRect(x: -604, y: -850, width: panelWidth, height: 500)
        let topAnchor = CGRect(x: -300, y: -50, width: 28, height: 28)
        let top = OutputPickerPlacement.popupFrame(parentFrame: parent, anchorFrame: topAnchor,
                                                   desiredSize: pickerSize, visibleFrame: lowerDisplay)
        assert(top.maxY == lowerDisplay.maxY)
        assertVisible(top, in: lowerDisplay)
        let bottomAnchor = CGRect(x: -300, y: -910, width: 28, height: 28)
        let bottom = OutputPickerPlacement.popupFrame(parentFrame: parent, anchorFrame: bottomAnchor,
                                                      desiredSize: pickerSize, visibleFrame: lowerDisplay)
        assert(bottom.minY == lowerDisplay.minY)
        assertVisible(bottom, in: lowerDisplay)
        let tall = OutputPickerPlacement.popupFrame(parentFrame: parent, anchorFrame: topAnchor,
                                                    desiredSize: CGSize(width: 260, height: 1200),
                                                    visibleFrame: lowerDisplay)
        assert(tall.height == lowerDisplay.height && tall.minY == lowerDisplay.minY)

        // A parent without reserved right space can still fit a full picker left.
        let unreservedParent = CGRect(x: 1104, y: 200, width: panelWidth, height: 500)
        let fallbackLeft = OutputPickerPlacement.popupFrame(parentFrame: unreservedParent,
                                                            anchorFrame: topAnchor,
                                                            desiredSize: pickerSize, visibleFrame: screen)
        assert(fallbackLeft.maxX + OutputPickerPlacement.gap == unreservedParent.minX)
        assert(fallbackLeft.width == pickerSize.width)
        assertVisible(fallbackLeft, in: screen)

        // On small displays, use the available side space before overlapping.
        let narrowScreen = CGRect(x: 100, y: 0, width: 500, height: 600)
        let narrowX = OutputPickerPlacement.mainPanelOriginX(anchorMidX: 590, panelWidth: panelWidth,
                                                             visibleFrame: narrowScreen)
        assert(narrowX == 264)
        let narrowParent = CGRect(x: narrowX, y: 100, width: panelWidth, height: 400)
        let narrow = OutputPickerPlacement.popupFrame(parentFrame: narrowParent,
                                                      anchorFrame: CGRect(x: 560, y: 250, width: 28, height: 28),
                                                      desiredSize: pickerSize, visibleFrame: narrowScreen)
        assert(narrow.width == 156)
        assert(narrow.maxX + OutputPickerPlacement.gap == narrowParent.minX)
        assertVisible(narrow, in: narrowScreen)

        // A full-width parent leaves no non-overlapping placement on either side.
        let fullParent = CGRect(x: 100, y: 100, width: 500, height: 400)
        let unavoidable = OutputPickerPlacement.popupFrame(parentFrame: fullParent,
                                                           anchorFrame: fullParent,
                                                           desiredSize: pickerSize, visibleFrame: narrowScreen)
        assert(unavoidable.intersects(fullParent))
        assertVisible(unavoidable, in: narrowScreen)

        // A display change may briefly leave a stale parent frame outside its screen.
        let staleParent = CGRect(x: -500, y: 200, width: panelWidth, height: 500)
        let clamped = OutputPickerPlacement.popupFrame(parentFrame: staleParent,
                                                       anchorFrame: staleParent,
                                                       desiredSize: pickerSize, visibleFrame: screen)
        assert(clamped.minX == screen.minX)
        assertVisible(clamped, in: screen)

        print("Output picker placement tests passed: edge reservation, alignment, multiple displays, height limits, narrow-screen fallback")
    }
}
