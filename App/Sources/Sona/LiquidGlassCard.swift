import AppKit
import SwiftUI

/// The panel's existing native material, scoped to one module. Keeping the
/// controls in contentView preserves the glass/content rendering relationship.
struct LiquidGlassCard<Content: View>: NSViewControllerRepresentable {
    var cornerRadius: CGFloat = 26
    @ViewBuilder let content: Content

    func makeNSViewController(context: Context) -> LiquidGlassCardController {
        LiquidGlassCardController(content: hostedContent(context), cornerRadius: cornerRadius)
    }

    func updateNSViewController(_ controller: LiquidGlassCardController, context: Context) {
        if let glass = controller.view as? NSGlassEffectView, glass.cornerRadius != cornerRadius {
            glass.cornerRadius = cornerRadius
        }
        // The parent animates card geometry, not every hosted control. Lists
        // inside this stable hosting root opt in using their own structure IDs.
        // Do not disable animations altogether: a changed local list still
        // needs its insertion/removal transition.
        var transaction = context.transaction
        transaction.animation = nil
        withTransaction(transaction) {
            controller.hostingController.rootView = hostedContent(context)
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsViewController controller: LiquidGlassCardController,
                      context: Context) -> CGSize? {
        let size = controller.hostingController.sizeThatFits(in: CGSize(
            width: proposal.width ?? .greatestFiniteMagnitude,
            height: proposal.height ?? .greatestFiniteMagnitude
        ))
        controller.contentContainer.setNaturalSize(size)
        return size
    }

    private func hostedContent(_ context: Context) -> AnyView {
        // A nested hosting controller starts a new SwiftUI environment. Forward
        // the model, control size and other inherited values to the controls.
        AnyView(content.environment(\.self, context.environment))
    }
}

final class LiquidGlassCardController: NSViewController {
    let hostingController: NSHostingController<AnyView>
    let contentContainer: TopAlignedModuleContentView

    init(content: AnyView, cornerRadius: CGFloat) {
        hostingController = NSHostingController(rootView: content)
        contentContainer = TopAlignedModuleContentView(content: hostingController.view)
        super.init(nibName: nil, bundle: nil)

        let glass = ModuleGlassView()
        glass.style = .clear
        glass.cornerRadius = cornerRadius
        glass.contentView = contentContainer
        addChild(hostingController)
        view = glass
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
}

/// The glass edge changes height, while its controls retain their natural height
/// and top edge. Resizing the hosting view on every animation frame would make
/// SwiftUI center the final contents inside an intermediate-height card, moving
/// existing rows out and back even though their identities never changed.
final class TopAlignedModuleContentView: NSView {
    private let content: NSView
    private var naturalSize = NSSize.zero

    init(content: NSView) {
        self.content = content
        super.init(frame: .zero)
        content.autoresizingMask = []
        content.clipsToBounds = false
        clipsToBounds = false
        addSubview(content)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override var isFlipped: Bool { true }

    func setNaturalSize(_ size: NSSize) {
        guard naturalSize != size else { return }
        naturalSize = size
        positionContent()
    }

    override func layout() {
        super.layout()
        positionContent()
    }

    private func positionContent() {
        let frame = NSRect(x: 0, y: 0,
                           width: bounds.width > 0 ? bounds.width : naturalSize.width,
                           height: naturalSize.height)
        if content.frame != frame { content.frame = frame }
    }
}

private final class ModuleGlassView: NSGlassEffectView {
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        updateTint()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        updateTint()
    }

    private func updateTint() {
        // Preserve the existing appearance-resolved system tint. Resolving it
        // again avoids retaining an Aqua tint after switching to Dark Aqua.
        effectiveAppearance.performAsCurrentDrawingAppearance {
            tintColor = NSColor.windowBackgroundColor.withAlphaComponent(0.35)
        }
    }
}
