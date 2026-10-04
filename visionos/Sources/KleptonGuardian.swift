import SwiftUI

final class KleptonGuardian: ObservableObject {
    static let shared = KleptonGuardian()
    @Published var mode: KLGuardianMode {
        didSet {
            UserDefaults.standard.set(mode.rawValue, forKey: "klepton.guardian.mode")
            KLGuardianRuntime.shared.select(mode)
            updateImmersionStyle()
            Task { await configureTracking?() }
        }
    }
    @Published var warningDistance: Float {
        didSet {
            UserDefaults.standard.set(warningDistance, forKey: "klepton.guardian.warningDistance")
            KLGuardianRuntime.shared.setWarningDistance(warningDistance)
        }
    }
    @Published private(set) var needsPassthrough = false
    @Published var immersionStyle: ImmersionStyle = .full
    private func updateImmersionStyle() {
        let full = mode == .system && !needsPassthrough
        guard full != (immersionStyle is FullImmersionStyle) else { return }
        immersionStyle = full ? .full : .mixed
        // Style changes can keep the same compositor alive. Reapply the audio
        // policy after SwiftUI receives the new style, without rebuilding audio.
        DispatchQueue.main.async { KleptonAudio.refreshMixing() }
    }
    @MainActor func setPassthroughRequired(_ value: Bool) {
        if needsPassthrough != value {
            needsPassthrough = value
            updateImmersionStyle()
        }
    }
    @Published var status = ""
    @Published var drawing = false
    @Published var ready = false
    @Published var canDraw = false
    @Published var canAccept = false
    @Published var canReuse = false
    @Published var removingSpots = false
    @Published var canRemoveSpots = false
    @Published var canUndoRemoval = false
    @MainActor var saveBoundary: (() async -> Void)?
    @MainActor var forgetBoundaries: (() async -> Void)?
    @MainActor var updateSavedBoundary: (() -> Void)?
    @MainActor var configureTracking: (() async -> Void)?
    private init() {
        mode = KLGuardianMode(rawValue: UserDefaults.standard.string(forKey: "klepton.guardian.mode") ?? "") ?? .system
        warningDistance = UserDefaults.standard.object(forKey: "klepton.guardian.warningDistance") == nil
            ? 0.3 : min(0.8, max(0.15, UserDefaults.standard.float(forKey: "klepton.guardian.warningDistance")))
        KLGuardianRuntime.shared.select(mode)
        KLGuardianRuntime.shared.setWarningDistance(warningDistance)
        updateImmersionStyle()
    }
    func refresh() {
        let state = KLGuardianRuntime.shared.uiState()
        status = state.status; drawing = state.drawing; ready = state.ready
        canDraw = state.canDraw; canAccept = state.canAccept; canReuse = state.canReuse
        removingSpots = state.removingSpots; canRemoveSpots = state.canRemoveSpots; canUndoRemoval = state.canUndoRemoval
    }
}

struct GuardianView: View {
    @ObservedObject private var guardian = KleptonGuardian.shared
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("Guardian", selection: $guardian.mode) {
                ForEach(KLGuardianMode.allCases) { Text($0.title).tag($0) }
            }
            .pickerStyle(.segmented)
            Text(guardian.status).font(.callout)
            HStack {
                Text("Passthrough starts")
                Slider(value: $guardian.warningDistance, in: 0.15...0.8, step: 0.05)
                Text("\(Int((guardian.warningDistance * 100).rounded())) cm from edge")
                    .monospacedDigit().frame(width: 150)
            }
            Text("Checks your headset and tracked controllers or hands. The distance controls custom boundaries and the default area's controller warning; visionOS controls its own headset warning.")
                .font(.caption).foregroundStyle(.secondary)
            if guardian.mode == .manual {
                HStack {
                    if guardian.drawing {
                        Button("Undo point") { KLGuardianRuntime.shared.undo(); guardian.refresh() }
                        Button("Finish boundary") {
                            KLGuardianRuntime.shared.finishDrawing(); guardian.refresh()
                            if guardian.ready { Task { await guardian.saveBoundary?(); guardian.refresh() } }
                        }
                        Button("Cancel drawing") { KLGuardianRuntime.shared.cancelDrawing(); guardian.refresh() }
                    } else {
                        Button(guardian.ready ? "Redraw boundary" : "Start drawing") {
                            KLGuardianRuntime.shared.beginDrawing(); guardian.refresh()
                        }.disabled(!guardian.canDraw)
                    }
                }
            }
            if guardian.mode != .system, guardian.canReuse {
                Button("Reuse saved boundary") { KLGuardianRuntime.shared.reuse(); guardian.refresh() }
            }
            if guardian.mode != .system {
                Button("Forget saved boundaries", role: .destructive) { Task { await guardian.forgetBoundaries?(); guardian.refresh() } }
            }
            if guardian.mode == .automatic {
                if guardian.removingSpots {
                    HStack {
                        Button("Undo removal") { KLGuardianRuntime.shared.undoSpotRemoval(); guardian.refresh() }
                            .disabled(!guardian.canUndoRemoval)
                        Button("Done") {
                            KLGuardianRuntime.shared.finishRemovingSpots(); guardian.refresh()
                            guardian.updateSavedBoundary?()
                            if guardian.ready, KLGuardianRuntime.shared.boundaryForSaving()?.anchor == nil {
                                Task { await guardian.saveBoundary?(); guardian.refresh() }
                            }
                        }
                            .disabled(!guardian.canDraw)
                        Button("Cancel") {
                            KLGuardianRuntime.shared.cancelRemovingSpots(); guardian.refresh()
                            if guardian.ready, KLGuardianRuntime.shared.boundaryForSaving()?.anchor == nil {
                                Task { await guardian.saveBoundary?(); guardian.refresh() }
                            }
                        }
                    }
                    Text("Look inside an unwanted outlined spot on the floor and pinch once. Only remove spots where the real space is clear.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Button("Remove unwanted spots") { KLGuardianRuntime.shared.beginRemovingSpots(); guardian.refresh() }
                        .disabled(!guardian.canRemoveSpots)
                    Button("Remap area") { KLGuardianRuntime.shared.remap(); guardian.refresh() }
                    Button("Use mapped area") {
                        KLGuardianRuntime.shared.acceptMap(); guardian.refresh()
                        if guardian.ready { Task { await guardian.saveBoundary?(); guardian.refresh() } }
                    }
                        .disabled(!guardian.canAccept)
                    Text("Remove enclosed obstacle outlines while keeping the room's outer boundary. Removed spots stay ignored until you remap the area.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text("Check the outline in passthrough. Mapping can miss objects; leave space for your arms and controllers.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if guardian.mode != .system {
                Text(guardian.mode == .automatic
                     ? "Your saved auto boundary applies automatically when this space is recognised and tracking is ready. Use Remap area or Remove unwanted spots to fix it."
                     : "Your saved drawn boundary applies automatically when this space is recognised and tracking is ready. Use Redraw boundary to change it.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding()
        .task {
            while !Task.isCancelled {
                guardian.refresh()
                try? await Task.sleep(for: .milliseconds(250))
            }
        }
    }
}
