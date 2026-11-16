@main enum RendererActivationTests {
    final class Renderer {}
    static func main() {
        let first = Renderer(), second = Renderer()
        let a = ObjectIdentifier(first), b = ObjectIdentifier(second)
        var policy = KLRendererActivation()
        policy.began(a, at: 100)
        assert(!policy.takeRetry(at: 107, foreground: true, quitting: false))
        assert(!policy.takeRetry(at: 110, foreground: false, quitting: false))
        assert(!policy.takeRetry(at: 110, foreground: true, quitting: true))
        assert(!policy.retried)
        policy.began(b, at: 111)
        policy.running(a); policy.ended(a)
        assert(policy.stalled(at: 119)) // Old renderer callbacks are ignored.
        assert(policy.takeRetry(at: 119, foreground: true, quitting: false))
        policy.began(a, at: 120)
        assert(!policy.takeRetry(at: 140, foreground: true, quitting: false))
        policy.running(a)
        assert(!policy.stalled(at: 150))
        var healthy = KLRendererActivation()
        healthy.began(a, at: 0); healthy.running(a)
        assert(!healthy.takeRetry(at: 10, foreground: true, quitting: false))
        healthy.began(b, at: 20); healthy.ended(b)
        assert(!healthy.stalled(at: 40))
        print("Renderer activation: timeout, foreground/quit guards, one retry, stale callbacks and healthy startup passed")
    }
}
