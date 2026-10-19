import AppKit
import SwiftUI

/// The panel's existing native material, scoped to one module. Keeping the
/// controls in contentView preserves the glass/content rendering relationship.
struct LiquidGlassCard<Content: View>: NSViewControllerRepresentable {
    @ViewBuilder let content: Content

    func makeNSViewController(context: Context) -> LiquidGlassCardController {
        LiquidGlassCardController(content: hostedContent(context))
    }

    func updateNSViewController(_ controller: LiquidGlassCardController, context: Context) {
        controller.hostingController.rootView = hostedContent(context)
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsViewController controller: LiquidGlassCardController,
                      context: Context) -> CGSize? {
        controller.hostingController.sizeThatFits(in: CGSize(
            width: proposal.width ?? .greatestFiniteMagnitude,
            height: proposal.height ?? .greatestFiniteMagnitude
        ))
    }

    private func hostedContent(_ context: Context) -> AnyView {
        // A nested hosting controller starts a new SwiftUI environment. Forward
        // the model, control size and other inherited values to the controls.
        AnyView(content.environment(\.self, context.environment))
    }
}

final class LiquidGlassCardController: NSViewController {
    let hostingController: NSHostingController<AnyView>

    init(content: AnyView) {
        hostingController = NSHostingController(rootView: content)
        super.init(nibName: nil, bundle: nil)

        let glass = ModuleGlassView()
        glass.style = .clear
        glass.cornerRadius = 26
        glass.contentView = hostingController.view
        hostingController.view.autoresizingMask = [.width, .height]
        addChild(hostingController)
        view = glass
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
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
