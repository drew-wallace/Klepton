import Foundation
import AVFAudio

final class KleptonMic {
    static let shared = KleptonMic()
    struct Settings { var enabled = false }
    var settings = Settings()
}
private var resumes = 0
private var restarts = 0
private var interruptions: [Int32] = []
private var sessionManaged = false
func kl_audio_set_session_managed(_ managed: Int32) { sessionManaged = managed != 0 }
func kl_audio_resume() { resumes += 1; checkPolicy() }
func kl_audio_restart() -> Int32 { restarts += 1; checkPolicy(); return 0 }
func kl_audio_session_ready(_ rate: Double) { checkPolicy() }
func kl_audio_interrupted(_ began: Int32) {
    interruptions.append(began)
    if began == 0 { checkPolicy() }
}
private func checkPolicy() {
    let s = AVAudioSession.sharedInstance()
    precondition(s.category == (KleptonMic.shared.settings.enabled ? .playAndRecord : .playback))
    precondition(s.mode == .default && s.categoryOptions.contains(.mixWithOthers))
    precondition(!s.isNowPlayingCandidate)
    if KleptonMic.shared.settings.enabled || !s.isOtherAudioPlaying {
        precondition(s.preferredSampleRate == 48000 && s.preferredIOBufferDuration == 0.010)
        precondition(s.preferredOutputNumberOfChannels == 2)
    }
    precondition((s.intendedSpatialExperience as? AVAudioSession.FixedSpatialExperience)?.soundStageSize == .small)
}
@main struct AudioSessionTests {
    static func main() {
        let s = AVAudioSession.sharedInstance()
        s.preferredSampleRate = 44100
        s.preferredIOBufferDuration = 0.020
        s.preferredOutputNumberOfChannels = 1
        KleptonAudio.resume()
        KleptonAudio.refreshMixing()
        precondition(s.activations == 0, "must not activate before boot")
        KleptonAudio.start()
        precondition(sessionManaged)
        checkPolicy()
        precondition(s.preferredSampleRate == 44100 && s.preferredIOBufferDuration == 0.020
                     && s.preferredOutputNumberOfChannels == 1, "existing call route was changed")
        let initialChanges = s.categoryChanges
        KleptonAudio.start()
        KleptonAudio.refreshMixing()
        precondition(s.categoryChanges == initialChanges, "healthy session reconfigured")

        // Home/immersive entry: restore lost mixing before activating or rebuilding.
        s.losePolicy()
        KleptonAudio.resume()
        checkPolicy()
        precondition(resumes == 1)
        s.losePolicy()
        let beforeStyle = restarts
        KleptonAudio.refreshMixing()
        checkPolicy()
        precondition(resumes == 1 && restarts <= beforeStyle + 1)

        // Every route recovery, plus notifications emitted by our category repair.
        for reason: AVAudioSession.RouteChangeReason in [
            .newDeviceAvailable, .oldDeviceUnavailable, .override,
            .routeConfigurationChange, .categoryChange
        ] {
            s.losePolicy()
            let changes = s.categoryChanges
            NotificationCenter.default.post(name: AVAudioSession.routeChangeNotification,
                object: s, userInfo: [AVAudioSessionRouteChangeReasonKey: reason.rawValue])
            checkPolicy()
            precondition(s.categoryChanges == changes + 1, "category feedback loop")
        }
        NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
            object: s, userInfo: [AVAudioSessionInterruptionTypeKey: UInt(1)])
        let interruptedActivations = s.activations
        let interruptedRestarts = restarts
        KleptonAudio.refreshMixing()
        NotificationCenter.default.post(name: AVAudioSession.routeChangeNotification,
            object: s, userInfo: [AVAudioSessionRouteChangeReasonKey: UInt(8)])
        precondition(s.activations == interruptedActivations && restarts == interruptedRestarts,
                     "route/style updates must not fight an active interruption")
        s.losePolicy()
        NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
            object: s, userInfo: [AVAudioSessionInterruptionTypeKey: UInt(0)])
        precondition(interruptions == [1, 0])
        checkPolicy()
        s.losePolicy()
        NotificationCenter.default.post(name: AVAudioSession.mediaServicesWereResetNotification, object: s)
        checkPolicy()

        // A foreground return replaces the old timeout recovery. Failed session
        // activation must leave the interruption intact; successful entry clears it.
        NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
            object: s, userInfo: [AVAudioSessionInterruptionTypeKey: UInt(1)])
        s.rejectActivation = true
        let beforeInterruptedResume = resumes
        KleptonAudio.resume()
        precondition(resumes == beforeInterruptedResume)
        s.rejectActivation = false
        let beforeDeferredStyle = s.activations
        KleptonAudio.refreshMixing()
        precondition(s.activations == beforeDeferredStyle)
        KleptonAudio.resume()
        precondition(resumes == beforeInterruptedResume + 1)
        let beforeRestoredStyle = s.activations
        KleptonAudio.refreshMixing()
        precondition(s.activations == beforeRestoredStyle + 1)
        precondition(s.rateRequests == 0 && s.bufferRequests == 0 && s.channelRequests == 0,
                     "recovery must not reconfigure a microphone-off call route")

        // A closed settings window is an explicit scene transition, but only
        // while the game is still foreground. Home must not restart playback.
        for reason: UInt in [0, 3] {
            NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
                object: s, userInfo: [AVAudioSessionInterruptionTypeKey: UInt(1),
                                     AVAudioSessionInterruptionReasonKey: reason])
            let beforeClose = resumes
            KleptonAudio.settingsWindowClosed(immersiveActive: false)
            precondition(resumes == beforeClose)
            s.rejectActivation = true
            KleptonAudio.settingsWindowClosed(immersiveActive: true)
            precondition(resumes == beforeClose)
            s.rejectActivation = false
            KleptonAudio.refreshMixing()
            precondition(resumes == beforeClose)
            KleptonAudio.settingsWindowClosed(immersiveActive: true)
            precondition(resumes == beforeClose + 1)
            checkPolicy()
        }
        for reason: UInt in [1, 2, 4] {
            NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
                object: s, userInfo: [AVAudioSessionInterruptionTypeKey: UInt(1),
                                     AVAudioSessionInterruptionReasonKey: reason])
            let beforeClose = resumes
            let beforeActivation = s.activations
            KleptonAudio.settingsWindowClosed(immersiveActive: true)
            precondition(resumes == beforeClose && s.activations == beforeActivation,
                         "window closure must not override a route/microphone interruption")
        }
        NotificationCenter.default.post(name: AVAudioSession.interruptionNotification,
            object: s, userInfo: [AVAudioSessionInterruptionTypeKey: UInt(0)])

        // Solo playback can request the guest's preferred format. A later call
        // route must survive all subsequent foreground and style updates.
        s.isOtherAudioPlaying = false
        KleptonAudio.resume()
        checkPolicy()
        s.isOtherAudioPlaying = true
        s.preferredSampleRate = 44100
        s.preferredIOBufferDuration = 0.020
        s.preferredOutputNumberOfChannels = 1
        KleptonAudio.resume()
        KleptonAudio.refreshMixing()
        precondition(s.preferredSampleRate == 44100 && s.preferredIOBufferDuration == 0.020
                     && s.preferredOutputNumberOfChannels == 1)

        KleptonMic.shared.settings.enabled = true
        KleptonAudio.applyMicCategory(true)
        checkPolicy()
        precondition(s.categoryOptions.contains([.allowBluetooth, .defaultToSpeaker]))
        s.losePolicy()
        KleptonAudio.resume()
        checkPolicy()
        KleptonMic.shared.settings.enabled = false
        KleptonAudio.applyMicCategory(false)
        checkPolicy()
        precondition(s.categoryOptions == [.mixWithOthers])

        // A failed activation must not falsely tell the output unit to resume.
        s.rejectActivation = true
        let beforeFailure = resumes
        KleptonAudio.resume()
        precondition(resumes == beforeFailure)
        print("Audio mixing lifecycle tests passed")
    }
}
