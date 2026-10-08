import SwiftUI
import RealityKit
import AVFAudio
import UIKit

enum ProbeAudioPolicy: String, CaseIterable, Identifiable {
    case untouched = "Untouched audio session"
    case ambient = "Ambient + mixing"
    case playback = "Playback + mixing, default spatial"
    case fixed = "Playback + mixing, fixed spatial"
    case bypassed = "Playback + mixing, bypassed spatial"
    case release = "Playback + mixing, release after entry"
    var id: String { rawValue }
}

enum ProbeImmersion: String, CaseIterable, Identifiable {
    case mixed, progressive, full
    var id: String { rawValue }
    var style: ImmersionStyle {
        switch self {
        case .mixed: .mixed
        case .progressive: .progressive
        case .full: .full
        }
    }
}

@MainActor final class ProbeState: ObservableObject {
    @Published var audio: ProbeAudioPolicy = .untouched
    @Published var immersion: ProbeImmersion = .mixed
    @Published var style: ImmersionStyle = .mixed
    @Published var spaceOpen = false
    @Published var busy = false
    @Published var showGeometry = false
    @Published var report = ""
    private var configuredSession = false
    private var events = 0
    private let logURL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("probe-events.log")

    init() {
        // Retained in the app container for devicectl, including after Home.
        try? "".write(to: logURL, atomically: true, encoding: .utf8)
    }

    func note(_ message: String) {
        events += 1
        let line = "\(events) \(Date().formatted(.iso8601)): \(message)"
        NSLog("[immersion-probe] \(line)")
        report += line + "\n"
        if report.count > 60000 { report = String(report.suffix(50000)) }
        try? report.write(to: logURL, atomically: true, encoding: .utf8)
    }

    func prepareAudio() throws {
        // No sharedInstance(), permission request, engine, player, or audio
        // component in the untouched control. Start a fresh process to restore
        // this control after any of the explicit session experiments.
        guard audio != .untouched else {
            guard !configuredSession else {
                throw NSError(domain: "ImmersiveAudioProbe", code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "Quit and relaunch for an untouched audio session."])
            }
            note("audio session untouched")
            return
        }
        configuredSession = true
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(audio == .ambient ? .ambient : .playback,
                                mode: .default, options: [.mixWithOthers])
        try session.setIsNowPlayingCandidate(false)
        if audio == .fixed {
            try session.setIntendedSpatialExperience(.fixed(soundStageSize: .small))
        } else if audio == .bypassed {
            try session.setIntendedSpatialExperience(.bypassed)
        }
        try session.setActive(true)
        snapshot("activated \(audio.rawValue)")
    }

    func releaseAudio() {
        guard configuredSession else { return }
        do {
            try AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
            snapshot("released audio session")
        } catch { note("release failed: \(error)") }
    }

    func snapshot(_ event: String) {
        guard configuredSession else { note("\(event); audio session untouched"); return }
        let session = AVAudioSession.sharedInstance()
        note("\(event); category=\(session.category.rawValue) options=\(session.categoryOptions.rawValue) "
             + "otherAudio=\(session.isOtherAudioPlaying) secondarySilenced=\(session.secondaryAudioShouldBeSilencedHint) "
             + "rate=\(session.sampleRate) buffer=\(session.ioBufferDuration) "
             + "outputs=\(session.currentRoute.outputs.map { $0.portType.rawValue })")
    }
}

@main struct ImmersiveAudioProbe: App {
    @StateObject private var state = ProbeState()
    @Environment(\.scenePhase) private var phase

    var body: some SwiftUI.Scene {
        WindowGroup {
            ProbeControls(state: state)
                .onChange(of: phase, initial: true) { _, value in
                    state.note("app phase=\(value)")
                }
        }
        .defaultSize(width: 760, height: 700)
        WindowGroup(id: "volume") {
            ProbeGeometry(immersive: false)
        }
        .windowStyle(.volumetric)
        .defaultSize(width: 0.5, height: 0.5, depth: 0.5, in: .meters)
        ImmersiveSpace(id: "probe") {
            ProbeSpace(state: state)
        }
        .immersionStyle(selection: $state.style, in: .mixed, .progressive, .full)
    }
}

struct ProbeGeometry: View {
    var immersive = true
    var body: some View {
        RealityView { content in
            let entity = ModelEntity(mesh: .generateSphere(radius: 0.1),
                                     materials: [SimpleMaterial(color: .cyan, isMetallic: false)])
            entity.position = immersive ? [0, 1.3, -1.5] : .zero
            content.add(entity)
        }
    }
}

struct ProbeSpace: View {
    @ObservedObject var state: ProbeState
    @Environment(\.scenePhase) private var phase
    var body: some View {
        Group {
            if state.showGeometry { ProbeGeometry() }
            else { RealityView { _ in } }
        }
        .onAppear { state.note("immersive appeared") }
        .onDisappear { state.spaceOpen = false; state.note("immersive disappeared") }
        .onChange(of: phase, initial: true) { _, value in
            state.note("immersive scene phase=\(value)")
        }
    }
}

struct ProbeControls: View {
    @ObservedObject var state: ProbeState
    @Environment(\.openImmersiveSpace) private var openSpace
    @Environment(\.dismissImmersiveSpace) private var dismissSpace
    @Environment(\.openWindow) private var openWindow
    @Environment(\.scenePhase) private var phase

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Immersive audio probe").font(.title)
            Text("Start your call before entering. This app runs no game, compositor, audio output, or microphone capture. Relaunch between audio policies.")
                .font(.callout)
            Picker("Immersion", selection: $state.immersion) {
                ForEach(ProbeImmersion.allCases) { Text($0.rawValue).tag($0) }
            }
            .disabled(state.spaceOpen || state.busy)
            Picker("Audio", selection: $state.audio) {
                ForEach(ProbeAudioPolicy.allCases) { Text($0.rawValue).tag($0) }
            }
            .disabled(state.spaceOpen || state.busy)
            Toggle("Render one sphere (off = empty space)", isOn: $state.showGeometry)
            HStack {
                Button(state.spaceOpen ? "Leave immersion" : "Enter immersion") {
                    Task { await transition() }
                }
                .accessibilityIdentifier("toggleImmersion")
                Button("Open shared-space volume") { openWindow(id: "volume") }
                Button("Release audio session") { state.releaseAudio() }
            }
            .disabled(state.busy)
            HStack {
                Button("Mark: call audible") { state.snapshot("USER: call audible") }
                Button("Mark: call stopped") { state.snapshot("USER: call stopped") }
                ShareLink("Share log", item: state.report)
            }
            ScrollView {
                Text(state.report).font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(24)
        .onChange(of: phase, initial: true) { _, value in state.note("controls scene phase=\(value)") }
        .onReceive(NotificationCenter.default.publisher(for: AVAudioSession.interruptionNotification)) { note in
            state.note("audio interruption: \(note.userInfo ?? [:])")
        }
        .onReceive(NotificationCenter.default.publisher(for: AVAudioSession.routeChangeNotification)) { note in
            state.note("audio route change: \(note.userInfo ?? [:])")
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
            state.note("own-process memory warning")
        }
        .onReceive(NotificationCenter.default.publisher(for: ProcessInfo.thermalStateDidChangeNotification)) { _ in
            state.note("thermal state=\(ProcessInfo.processInfo.thermalState.rawValue)")
        }
        .task {
            state.note("OS=\(ProcessInfo.processInfo.operatingSystemVersionString), policy=\(state.audio.rawValue)")
            // For simulator smoke tests only; it never touches the audio session.
            if let name = ProcessInfo.processInfo.environment["KL_PROBE_AUTO_IMMERSION"],
               let immersion = ProbeImmersion(rawValue: name) {
                state.immersion = immersion
                await transition()
            }
        }
    }

    @MainActor private func transition() async {
        guard !state.busy else { return }
        state.busy = true
        defer { state.busy = false }
        if state.spaceOpen {
            state.note("before dismiss")
            await dismissSpace()
            state.spaceOpen = false
            state.snapshot("after dismiss")
            return
        }
        do { try state.prepareAudio() }
        catch { state.note("prepare failed: \(error.localizedDescription)"); return }
        state.style = state.immersion.style
        state.note("before open: \(state.immersion.rawValue), geometry=\(state.showGeometry)")
        let result = await openSpace(id: "probe")
        state.snapshot("open result=\(result)")
        if case .opened = result {
            state.spaceOpen = true
            if state.audio == .release { state.releaseAudio() }
        }
    }
}
