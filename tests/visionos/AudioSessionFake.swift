// Host-only AVFAudio substitute. Compile the actual KleptonAudio.swift against
// this module to exercise activation order and recovery notifications.
import Foundation

public let AVAudioSessionInterruptionTypeKey = "type"
public let AVAudioSessionInterruptionReasonKey = "interruptionReason"
public let AVAudioSessionRouteChangeReasonKey = "reason"
public protocol AVAudioSessionSpatialExperience {}
public extension AVAudioSessionSpatialExperience where Self == AVAudioSession.FixedSpatialExperience {
    static func fixed(soundStageSize: AVAudioSession.SoundStageSize) -> Self { Self(soundStageSize: soundStageSize) }
}
public final class AVAudioSession: NSObject {
    public struct Category: Equatable {
        public let rawValue: String
        public static let playback = Self(rawValue: "playback")
        public static let playAndRecord = Self(rawValue: "playAndRecord")
    }
    public enum Mode { case `default`, voiceChat }
    public struct CategoryOptions: OptionSet {
        public let rawValue: Int
        public init(rawValue: Int) { self.rawValue = rawValue }
        public static let mixWithOthers = Self(rawValue: 1)
        public static let allowBluetooth = Self(rawValue: 2)
        public static let defaultToSpeaker = Self(rawValue: 4)
    }
    public enum SoundStageSize { case small, medium, large }
    public struct FixedSpatialExperience: AVAudioSessionSpatialExperience {
        public let soundStageSize: SoundStageSize
        public init(soundStageSize: SoundStageSize) { self.soundStageSize = soundStageSize }
    }
    private struct SpatialExperience: AVAudioSessionSpatialExperience {}
    public enum InterruptionType: UInt { case ended = 0, began = 1 }
    public enum RouteChangeReason: UInt {
        case newDeviceAvailable = 1, oldDeviceUnavailable = 2, categoryChange = 3
        case override = 4, routeConfigurationChange = 8
    }
    public struct Port { public let portType: Category }
    public struct Route { public let inputs: [Port] = [] }
    public static let interruptionNotification = Notification.Name("interruption")
    public static let routeChangeNotification = Notification.Name("route")
    public static let mediaServicesWereResetNotification = Notification.Name("reset")
    private static let session = AVAudioSession()
    public static func sharedInstance() -> AVAudioSession { session }
    public var category: Category = .playback
    public var mode: Mode = .default
    public var categoryOptions: CategoryOptions = []
    public var isNowPlayingCandidate = true
    public var isOtherAudioPlaying = true
    public var preferredSampleRate: Double = 0
    public var preferredIOBufferDuration: Double = 0
    public var preferredOutputNumberOfChannels = 0
    public var intendedSpatialExperience: any AVAudioSessionSpatialExperience = SpatialExperience()
    public var sampleRate: Double { 48000 }
    public var outputNumberOfChannels: Int { 2 }
    public var ioBufferDuration: Double { preferredIOBufferDuration }
    public var currentRoute: Route { Route() }
    public var inputNumberOfChannels: Int { 0 }
    public var availableInputs: [Port]? { [] }
    public var categoryChanges = 0
    public var activations = 0
    public var rejectActivation = false
    public var rateRequests = 0
    public var bufferRequests = 0
    public var channelRequests = 0
    public func setCategory(_ category: Category, mode: Mode, options: CategoryOptions) throws {
        categoryChanges += 1
        precondition(categoryChanges < 50, "category notification loop")
        self.category = category; self.mode = mode
        categoryOptions = options
        NotificationCenter.default.post(name: Self.routeChangeNotification, object: self,
            userInfo: [AVAudioSessionRouteChangeReasonKey: RouteChangeReason.categoryChange.rawValue])
    }
    public func setIsNowPlayingCandidate(_ candidate: Bool) throws { isNowPlayingCandidate = candidate }
    public func setPreferredSampleRate(_ rate: Double) throws { rateRequests += 1; preferredSampleRate = rate }
    public func setPreferredIOBufferDuration(_ duration: Double) throws { bufferRequests += 1; preferredIOBufferDuration = duration }
    public func setPreferredOutputNumberOfChannels(_ channels: Int) throws { channelRequests += 1; preferredOutputNumberOfChannels = channels }
    public func setIntendedSpatialExperience(_ experience: any AVAudioSessionSpatialExperience) throws {
        intendedSpatialExperience = experience
    }
    public func setActive(_ active: Bool) throws {
        precondition(categoryOptions.contains(.mixWithOthers), "activation would interrupt other audio")
        precondition(!isNowPlayingCandidate, "game remains eligible to replace the selected media app")
        precondition((intendedSpatialExperience as? FixedSpatialExperience)?.soundStageSize == .small,
                     "the fixed spatial experience must be configured before activation")
        if rejectActivation { throw NSError(domain: "test", code: 1) }
        activations += 1
    }
    public func losePolicy() {
        categoryOptions = []; mode = .voiceChat
        isNowPlayingCandidate = true
        preferredSampleRate = 0; preferredIOBufferDuration = 0
        preferredOutputNumberOfChannels = 0
        intendedSpatialExperience = SpatialExperience()
    }
}
public enum AVAudioApplication {
    public static func requestRecordPermission(_ completion: @escaping (Bool) -> Void) {
        completion(true)
    }
}
