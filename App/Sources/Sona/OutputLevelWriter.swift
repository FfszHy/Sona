import Foundation
import CoreAudio

/// One latest-value mailbox per UID. HAL calls never run on the UI thread.
@MainActor
final class OutputLevelWriter {
    struct Change {
        var volume: Double?
        var mute: Bool?
    }
    private var pending: [String: Change] = [:]
    private var task: Task<Void, Never>?
    private let send: (String, Change) async -> Void

    init(send: @escaping (String, Change) async -> Void) { self.send = send }

    func submit(uid: String, volume: Double? = nil, mute: Bool? = nil) {
        var change = pending[uid] ?? Change()
        if let volume { change.volume = volume }
        if let mute { change.mute = mute }
        pending[uid] = change
        guard task == nil else { return }
        task = Task { [weak self] in
            guard let self else { return }
            while !self.pending.isEmpty {
                // Throttle, not debounce: sound follows a continuous drag at up to 30 Hz.
                try? await Task.sleep(nanoseconds: 33_000_000)
                let batch = self.pending
                self.pending.removeAll()
                for (uid, change) in batch { await self.send(uid, change) }
            }
            self.task = nil
        }
    }
}
