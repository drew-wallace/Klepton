import ARKit
import Foundation
import simd

// ARKit owns anchor persistence/relocalization. The app stores only the outline
// in each anchor's local frame, never a previous session's world coordinates.
final class KLGuardianPersistence {
    struct Record: Codable {
        var anchorID: UUID
        var points: [[Float]]
        // Older records contain manual polygons. Automatic records contain
        // endpoint pairs so interior obstacle holes survive anchor rotation.
        var mode: String? = nil
        var frameVersion: Int? = nil
        // Removed obstacle polygons share the boundary's anchor-local frame.
        var removedSpots: [[[Float]]]? = nil
    }
    private static let key = "klepton.guardian.boundaries.v1"
    private var records: [Record] = []
    private var tasks: [Task<Void, Never>] = []
    private var world: WorldTrackingProvider?
    private var anchorCache = KLGuardianAnchorCache<WorldAnchor>()
    private var syncedRevision: Int?
    private var loggedRestores: Set<UUID> = []
    init() {
        if let data = UserDefaults.standard.data(forKey: Self.key),
           let saved = try? JSONDecoder().decode([Record].self, from: data) {
            records = saved.filter {
                let automatic = $0.mode == KLGuardianMode.automatic.rawValue
                return (automatic && $0.points.isEmpty || $0.points.count >= (automatic ? 6 : 3)) && $0.points.count <= (automatic ? 100_000 : 256) &&
                    (!automatic || $0.points.count.isMultiple(of: 2)) &&
                    $0.points.allSatisfy { $0.count == 3 && $0.allSatisfy(\.isFinite) } &&
                    ($0.removedSpots?.count ?? 0) <= 256 &&
                    ($0.removedSpots ?? []).allSatisfy { polygon in
                        polygon.count >= 3 && polygon.count <= 4096 &&
                            polygon.allSatisfy { $0.count == 3 && $0.allSatisfy(\.isFinite) }
                    }
            }
        }
    }
    @MainActor func consume(_ world: WorldTrackingProvider) {
        if self.world === world, !tasks.isEmpty { return }
        stop(); self.world = world
        let generation = anchorCache.generation
        KleptonGuardian.shared.saveBoundary = { [weak self] in await self?.saveCurrent() }
        KleptonGuardian.shared.forgetBoundaries = { [weak self] in await self?.forget() }
        KleptonGuardian.shared.updateSavedBoundary = { [weak self] in self?.syncCurrent() }
        tasks.append(Task { @MainActor in
            for await update in world.anchorUpdates {
                guard !Task.isCancelled, generation == self.anchorCache.generation else { return }
                self.syncCurrent()
                if update.event == .removed {
                    self.anchorCache.update(update.anchor.id, value: nil, generation: generation)
                    KLGuardianRuntime.shared.anchorLost(update.anchor.id)
                } else {
                    self.anchorCache.update(update.anchor.id, value: update.anchor, generation: generation)
                    self.restore(update.anchor)
                }
            }
        })
        tasks.append(Task { @MainActor in
            while !Task.isCancelled, generation == self.anchorCache.generation {
                // Also handles anchor updates received before the first head pose.
                guard world.state == .running else {
                    KLGuardianRuntime.shared.unavailable("World tracking is unavailable. Passthrough stays visible.")
                    try? await Task.sleep(for: .milliseconds(500))
                    continue
                }
                // Retry streamed poses after the first head/floor pose arrives.
                // Never replace them with an allAnchors snapshot.
                for anchor in self.anchorCache.values.values { self.restore(anchor) }
                try? await Task.sleep(for: .milliseconds(500))
            }
        })
    }
    @MainActor func stop() {
        tasks.forEach { $0.cancel() }; tasks = []
        anchorCache.beginSession(); world = nil
        loggedRestores = []
    }
    @MainActor private func restore(_ anchor: WorldAnchor) {
        guard world?.state == .running else { return }
        guard let record = records.first(where: { $0.anchorID == anchor.id }) else { return }
        guard record.frameVersion == 2 else {
            KLGuardianRuntime.shared.legacyBoundaryFound(); return
        }
        guard anchor.isTracked else { KLGuardianRuntime.shared.anchorLost(anchor.id); return }
        let transform = anchor.originFromAnchorTransform
        // A floor guardian cannot follow a tilted or implausibly scaled anchor.
        guard transform.columns.1.y > 0.995 else { KLGuardianRuntime.shared.anchorLost(anchor.id); return }
        if loggedRestores.insert(anchor.id).inserted {
            NSLog("[guardian] restore from anchorUpdates: generation %d, yaw %.1f, anchor Y %.3f",
                  anchorCache.generation, atan2f(transform.columns.2.x, transform.columns.2.z) * 180 / .pi, transform.columns.3.y)
        }
        let points = KLGuardianCoordinates.worldPoints(record.points, anchor: transform)
        let polygon = points.map { KLGuardianGeometry.Point($0.x, $0.z) }
        let y = points.isEmpty ? transform.columns.3.y : points.reduce(Float(0)) { $0 + $1.y } / Float(points.count)
        if record.mode == KLGuardianMode.automatic.rawValue {
            let edges = stride(from: 0, to: polygon.count, by: 2).map {
                KLGuardianGeometry.Segment(a: polygon[$0], b: polygon[$0 + 1])
            }
            let removedSpots = (record.removedSpots ?? []).map {
                KLGuardianCoordinates.worldPoints($0, anchor: transform).map { KLGuardianGeometry.Point($0.x, $0.z) }
            }
            KLGuardianRuntime.shared.restoreMap(anchor: anchor.id, edges: edges, floor: y, removedSpots: removedSpots)
        } else {
            guard KLGuardianGeometry.validPolygon(polygon) else { return }
            KLGuardianRuntime.shared.restore(anchor: anchor.id, polygon: polygon, floor: y)
        }
    }
    @MainActor private func saveCurrent() async {
        guard let world, world.state == .running,
              let boundary = KLGuardianRuntime.shared.boundaryForSaving(), !boundary.points.isEmpty else { return }
        let center = boundary.points.reduce(SIMD2<Float>.zero, +) / Float(boundary.points.count)
        var transform = matrix_identity_float4x4
        transform.columns.3 = SIMD4(center.x, boundary.floor, center.y, 1)
        let anchor = WorldAnchor(originFromAnchorTransform: transform)
        let generation = anchorCache.generation
        do {
            try await world.addAnchor(anchor)
            guard generation == anchorCache.generation, KLGuardianRuntime.shared.isCurrent(boundary.setupRevision) else {
                try? await world.removeAnchor(forID: anchor.id)
                return
            }
            // Wait for the actual tracked pose; storing relative to the
            // requested pose can shift the drawing when its first update arrives.
            var trackedAnchor: WorldAnchor?
            for _ in 0..<40 {
                if let tracked = anchorCache.values[anchor.id], tracked.isTracked { trackedAnchor = tracked; break }
                guard KLGuardianRuntime.shared.isCurrent(boundary.setupRevision), !Task.isCancelled,
                      generation == anchorCache.generation else {
                    try? await world.removeAnchor(forID: anchor.id); return
                }
                try? await Task.sleep(for: .milliseconds(100))
            }
            guard let trackedAnchor, generation == anchorCache.generation, KLGuardianRuntime.shared.isCurrent(boundary.setupRevision),
                  let current = KLGuardianRuntime.shared.boundaryForSaving(), !current.points.isEmpty else {
                try? await world.removeAnchor(forID: anchor.id)
                KLGuardianRuntime.shared.saveFailed(); return
            }
            let record = Record(anchorID: anchor.id, points: KLGuardianCoordinates.localPoints(
                current.points, floor: current.floor, anchor: trackedAnchor.originFromAnchorTransform), mode: current.mode.rawValue, frameVersion: 2,
                                removedSpots: current.removedSpots.map { KLGuardianCoordinates.localPoints($0, floor: current.floor, anchor: trackedAnchor.originFromAnchorTransform) })
            NSLog("[guardian] saved from anchorUpdates: floor %.3f, tracked anchor Y %.3f, %d points",
                  current.floor, trackedAnchor.originFromAnchorTransform.columns.3.y, current.points.count)
            // Replace only the boundary being redrawn, retaining other rooms.
            if let oldID = boundary.replacing {
                records.removeAll { $0.anchorID == oldID }
            }
            records.append(record); persist()
            syncedRevision = current.revision
            KLGuardianRuntime.shared.didSave(anchor.id, revision: current.revision)
            restore(trackedAnchor)
            if let oldID = boundary.replacing { try? await world.removeAnchor(forID: oldID) }
        } catch {
            KLGuardianRuntime.shared.saveFailed()
        }
    }
    // Commit live obstacle reductions in the current anchor frame before an
    // anchor update changes the world origin. Restoration cannot reopen them.
    @MainActor private func syncCurrent() {
        guard let boundary = KLGuardianRuntime.shared.boundaryForSaving(),
              syncedRevision != boundary.revision, let id = boundary.anchor,
              let anchor = anchorCache.values[id], anchor.isTracked,
              let index = records.firstIndex(where: { $0.anchorID == id }) else { return }
        records[index].points = KLGuardianCoordinates.localPoints(boundary.points,
            floor: boundary.floor, anchor: anchor.originFromAnchorTransform)
        records[index].removedSpots = boundary.removedSpots.map {
            KLGuardianCoordinates.localPoints($0, floor: boundary.floor, anchor: anchor.originFromAnchorTransform)
        }
        syncedRevision = boundary.revision
        persist()
    }
    private func persist() {
        if let data = try? JSONEncoder().encode(records) { UserDefaults.standard.set(data, forKey: Self.key) }
    }
    @MainActor private func forget() async {
        let ids = records.map(\.anchorID)
        records = []; persist(); KLGuardianRuntime.shared.reset()
        for id in ids { try? await world?.removeAnchor(forID: id) }
    }
}
