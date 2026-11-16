// Startup-only recovery policy. No renderer is reopened while the app/window
// is inactive, and stale callbacks cannot affect a replacement renderer.
struct KLRendererActivation {
    private var renderer: ObjectIdentifier?
    private var waitingSince: Double?
    private(set) var retried = false

    mutating func began(_ id: ObjectIdentifier, at time: Double) {
        renderer = id
        waitingSince = time
    }
    mutating func running(_ id: ObjectIdentifier) {
        guard renderer == id else { return }
        waitingSince = nil
    }
    mutating func ended(_ id: ObjectIdentifier) {
        guard renderer == id else { return }
        renderer = nil
        waitingSince = nil
    }
    func stalled(at time: Double) -> Bool {
        guard let waitingSince else { return false }
        return time - waitingSince >= 8
    }
    mutating func takeRetry(at time: Double, foreground: Bool, quitting: Bool) -> Bool {
        guard !retried, foreground, !quitting, stalled(at: time) else { return false }
        retried = true
        return true
    }
}
