import AVFAudio
import Foundation

/// The audio session — the one part of playback that CoreAudio alone will not
/// give you on this platform.
///
/// `runtime/media/kl_audio.c` owns the output unit and the ring, and it is plain C
/// that compiles unchanged for macOS and visionOS. What does *not* port is the
/// session: on macOS there is none, and on visionOS an output unit will
/// initialise, start, report success and produce nothing at all unless an
/// `AVAudioSession` has been configured and activated first. That failure has
/// no error code anywhere in it, which is exactly why this file exists and why
/// it logs what it did.
///
/// The second job here is keeping the stream. visionOS inherits iPadOS's view
/// that audio is a resource the system lends out: an interruption, a route
/// change, a media-services reset or a trip through the background can all
/// take the unit away, and the unit does not always notice — it stays nominally
/// started and simply stops calling back. So there are two independent
/// recoveries, and they are meant to overlap:
///
///   * these notifications, which are precise and occasionally do not arrive;
///   * kl_audio.c's own watchdog on the producer side, which notices a render
///     callback that has stopped arriving and rebuilds the unit regardless.
///
/// The watchdog handles a dead callback without an OS interruption. Actual
/// interruptions remain under session control until ended or foreground resume.
enum KleptonAudio {
    nonisolated(unsafe) private static var started = false
    nonisolated(unsafe) private static var observers: [NSObjectProtocol] = []

    // Startup runs on the boot worker, resume on the render thread, and route
    // notifications on main. Keep category restoration and activation together.
    private static let sessionLock = NSRecursiveLock()
    nonisolated(unsafe) private static var interrupted = false // guarded by sessionLock
    nonisolated(unsafe) private static var interruptionReason: UInt? // guarded by sessionLock
    private enum SessionError: Error { case interrupted }

    /// Restore policy before activation: a scene or route transition can replace
    /// the category options even though the session object itself survives.
    private static func activate(_ session: AVAudioSession, mic: Bool,
                                 foregroundResume: Bool = false) throws {
        sessionLock.lock()
        defer { sessionLock.unlock() }
        // A route or guardian style update is not permission to override a call.
        // Only an ended notification or explicit foreground return can recover.
        guard !interrupted || foregroundResume else { throw SessionError.interrupted }
        let otherAudioBefore = session.isOtherAudioPlaying
        try setCategoryForMic(session, mic)
        // Game sound should mix alongside the user's chosen video/music app.
        // Mixing and Now Playing eligibility are separate visionOS settings.
        if session.isNowPlayingCandidate {
            try session.setIsNowPlayingCandidate(false)
        }
        let preserveOtherRoute = otherAudioBefore && !mic
        try configureStereoExperience(session)
        // Let the active call keep its hardware rate, buffer and channel route.
        // C measures the actual output format and resamples the guest itself.
        if !preserveOtherRoute && session.preferredSampleRate != 48000 {
            try session.setPreferredSampleRate(48000)
        }
        if !preserveOtherRoute && session.preferredIOBufferDuration != 0.010 {
            try session.setPreferredIOBufferDuration(0.010)
        }
        try session.setActive(true)
        if !preserveOtherRoute && session.preferredOutputNumberOfChannels != 2 {
            try session.setPreferredOutputNumberOfChannels(2)
        }
        if foregroundResume { interrupted = false }
    }

    /// Immersion-style changes may preserve the renderer. Restore mixing without
    /// rebuilding a healthy output unit every time passthrough opens or closes.
    static func refreshMixing() {
        guard started else { return }
        do {
            try activate(AVAudioSession.sharedInstance(), mic: KleptonMic.shared.settings.enabled)
        } catch {
            NSLog("[au] mixing restore failed: \(error)")
        }
    }

    /// A foreground transition can deactivate the session as well as its
    /// output unit. Reactivate first, then rebuild C's output against that route.
    static func resume() {
        guard started else { return }
        let session = AVAudioSession.sharedInstance()
        do {
            try activate(session, mic: KleptonMic.shared.settings.enabled, foregroundResume: true)
            kl_audio_resume()
        } catch {
            NSLog("[au] session resume failed: \(error)")
        }
    }

    /// Closing the launcher can interrupt the shared session even though the
    /// compositor keeps running. Treat that confirmed scene transition like
    /// foreground entry, rather than making the watchdog override every call.
    static func settingsWindowClosed(immersiveActive: Bool) {
        guard immersiveActive, started else { return }
        sessionLock.lock()
        defer { sessionLock.unlock() }
        // Default / scene-backgrounded interruptions can accompany window
        // closure. A disconnected route or muted microphone must still wait.
        guard !interrupted || interruptionReason == 0 || interruptionReason == 3 else { return }
        NSLog("[au] settings window closed with active immersion — restoring game audio")
        resume()
    }

    /// Configure and activate the session, then tell the C side what the
    /// hardware rate turned out to be. Safe to call more than once.
    static func start() {
        guard !started else { return }
        started = true
        // The C watchdog must not guess that an OS interruption has ended.
        kl_audio_set_session_managed(1)

        let session = AVAudioSession.sharedInstance()
        do {
            try activate(session, mic: KleptonMic.shared.settings.enabled)
        } catch {
            NSLog("[au] AVAudioSession setup failed: \(error) — expect silence")
        }

        NSLog("[au] session active: \(session.sampleRate) Hz, "
              + "\(session.outputNumberOfChannels) ch out, "
              + "IO buffer \(String(format: "%.1f", session.ioBufferDuration * 1000)) ms")
        kl_audio_session_ready(session.sampleRate)

        observe(AVAudioSession.interruptionNotification) { note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt ?? 0
            let began = AVAudioSession.InterruptionType(rawValue: raw) == .began
            let reason = note.userInfo?[AVAudioSessionInterruptionReasonKey] as? UInt
            sessionLock.lock()
            interrupted = began
            interruptionReason = began ? reason : nil
            sessionLock.unlock()
            if !began {
                // The session is deactivated for us on the way in but NOT
                // reactivated on the way out — an interruption that ends leaves
                // an inactive session, and a unit started against one produces
                // silence with no error. Reactivate before telling C to rebuild.
                do {
                    try activate(session, mic: KleptonMic.shared.settings.enabled)
                } catch {
                    NSLog("[au] interruption recovery failed: \(error)")
                    return
                }
            }
            kl_audio_interrupted(began ? 1 : 0)
        }

        // A route change is the headphones going in or out, and on this device
        // also the transition into and out of an immersive space. The hardware
        // sample rate can change with it, which is why the C side rebuilds the
        // unit rather than restarting it: a unit initialised against the old
        // rate starts happily and never calls back.
        observe(AVAudioSession.routeChangeNotification) { note in
            let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt ?? 0
            let reason = AVAudioSession.RouteChangeReason(rawValue: raw)
            NSLog("[au] route change (reason \(raw)), now \(session.sampleRate) Hz")
            switch reason {
            case .oldDeviceUnavailable, .newDeviceAvailable, .override,
                 .routeConfigurationChange, .categoryChange:
                do {
                    try activate(session, mic: KleptonMic.shared.settings.enabled)
                    _ = kl_audio_restart()
                } catch {
                    NSLog("[au] route recovery failed: \(error)")
                }
            default:
                break
            }
        }

        // Rare, and total: every audio object in the process is invalid. Nothing
        // survives it except a full rebuild, and the session has to be
        // configured again from scratch first.
        observe(AVAudioSession.mediaServicesWereResetNotification) { _ in
            NSLog("[au] media services were reset — reconfiguring from scratch")
            do {
                try activate(session, mic: KleptonMic.shared.settings.enabled)
                kl_audio_session_ready(session.sampleRate)
                _ = kl_audio_restart()
            } catch {
                NSLog("[au] media services recovery failed: \(error)")
            }
        }
    }

    /// Keep the game unanchored to its settings window while retaining the
    /// system spatial mixer. Fixed is non-head-tracked.
    private static func configureStereoExperience(_ session: AVAudioSession) throws {
        let env = ProcessInfo.processInfo.environment
        guard env["KL_AUDIO_SPATIAL"] != "1" else { return }
        if (session.intendedSpatialExperience as? AVAudioSession.FixedSpatialExperience)?.soundStageSize != .small {
            try session.setIntendedSpatialExperience(.fixed(soundStageSize: .small))
        }
    }

    /// Keep game audio mixable with other apps, including Discord calls.
    /// OFF → playback with mixing, leaving microphone capture to the calling app.
    /// ON → playAndRecord for guest voice chat; mixing does not guarantee that
    /// two apps can capture the microphone at the same time. Use .default mode
    /// because the C capture unit provides its own voice processing.
    static func setCategoryForMic(_ session: AVAudioSession, _ mic: Bool) throws {
        let category: AVAudioSession.Category = mic ? .playAndRecord : .playback
        let options: AVAudioSession.CategoryOptions = mic
            ? [.mixWithOthers, .allowBluetooth, .defaultToSpeaker] : [.mixWithOthers]
        // Changing category emits another route notification. Do not turn that
        // notification into an endless reconfiguration/restart loop.
        guard session.category != category || session.mode != .default
                || session.categoryOptions != options else { return }
        if mic {
            // mode .default, NOT .voiceChat — and this is deliberate even though the
            // capture unit is now VoiceProcessingIO. .voiceChat makes the SYSTEM
            // insert its own voice-processing I/O; with our VPIO that is redundant,
            // and if VPIO ever fails and kl_audio.c falls back to a plain RemoteIO
            // capture unit, .voiceChat's system unit STARVES it → silence (the
            // recurring "peak 0" / no-sound regression). .default has no system
            // voice unit to compete, so BOTH capture units stay audible: VPIO still
            // applies its own noise-suppression/AGC (unit-level, not mode-gated) for
            // clean voice, and a RemoteIO fallback is raw but working — never silent.
            try session.setCategory(.playAndRecord, mode: .default,
                                    options: [.mixWithOthers, .allowBluetooth, .defaultToSpeaker])
        } else {
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        }
    }

    /// Called by KleptonMic when the toggle changes at runtime. Reconfigures the
    /// live session and, when turning the mic ON, asks for record permission so
    /// the visionOS prompt appears the moment the user opts in (and never
    /// before). The C capture side is armed separately via kl_audio_mic_set_enabled.
    static func applyMicCategory(_ on: Bool) {
        let session = AVAudioSession.sharedInstance()
        do {
            try activate(session, mic: on)
            NSLog("[au] microphone \(on ? "enabled — category .playAndRecord" : "disabled — category .playback + mixWithOthers")")
            if on {
                // The input route the capture unit will actually read. If inputs is
                // empty or inputChannels is 0, the mic is not on the route and the
                // capture renders silence no matter what — this line says which.
                let r = session.currentRoute
                NSLog("[au] mic route: inputs=\(r.inputs.map { $0.portType.rawValue }) "
                      + "inputChannels=\(session.inputNumberOfChannels) "
                      + "available=\(session.availableInputs?.map { $0.portType.rawValue } ?? [])")
            }
        } catch {
            NSLog("[au] microphone category change failed: \(error)")
        }
        if on {
            AVAudioApplication.requestRecordPermission { granted in
                NSLog("[au] microphone permission \(granted ? "granted" : "denied")")
            }
        }
    }

    private static func observe(_ name: Notification.Name,
                                _ body: @escaping (Notification) -> Void) {
        observers.append(NotificationCenter.default.addObserver(
            forName: name, object: AVAudioSession.sharedInstance(),
            queue: .main, using: body))
    }
}
