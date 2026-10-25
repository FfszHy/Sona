import Foundation

/// Coalesce slider preferences; gesture completion and termination can save synchronously.
@MainActor
final class PreferenceSaver {
    private let delay: Duration
    private let save: () -> Void
    private var task: Task<Void, Never>?

    init(delay: Duration = .milliseconds(250), save: @escaping () -> Void) {
        self.delay = delay
        self.save = save
    }

    func request(immediately: Bool = false) {
        task?.cancel()
        task = nil
        if immediately {
            save()
            return
        }
        let delay = delay
        task = Task { [weak self] in
            do { try await Task.sleep(for: delay) }
            catch { return }
            guard !Task.isCancelled else { return }
            self?.flush()
        }
    }

    func flush() {
        guard task != nil else { return }
        task?.cancel()
        task = nil
        save()
    }
}
