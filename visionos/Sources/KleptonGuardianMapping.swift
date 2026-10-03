import ARKit
import simd

final class KLGuardianMapping {
    private var planes = PlaneDetectionProvider(alignments: [.horizontal, .vertical])
    private var rooms = RoomTrackingProvider()
    private var meshes = SceneReconstructionProvider(modes: [.classification])
    private var tasks: [Task<Void, Never>] = []
    @MainActor private var planeAnchors: [UUID: PlaneAnchor] = [:]
    @MainActor private var roomAnchors: [UUID: RoomAnchor] = [:]
    @MainActor private var meshAnchors: [UUID: MeshAnchor] = [:]
    @MainActor private var currentRoom: RoomAnchor? { roomAnchors.values.first { $0.isCurrentRoom } }
    private var generation = 0
    private var lastMapDiagnostic = Date.distantPast
    private(set) var enabled = false
    @MainActor var healthy: Bool {
        enabled && planes.state == .running &&
        (KleptonGuardian.shared.mode != .automatic || (rooms.state == .running && meshes.state == .running && currentRoom != nil))
    }

    @MainActor func reset(resetBoundary: Bool = true) {
        generation += 1; tasks.forEach { $0.cancel() }; tasks = []
        enabled = false
        planeAnchors = [:]; roomAnchors = [:]; meshAnchors = [:]
        lastMapDiagnostic = .distantPast
        planes = PlaneDetectionProvider(alignments: [.horizontal, .vertical])
        rooms = RoomTrackingProvider(); meshes = SceneReconstructionProvider(modes: [.classification])
        if resetBoundary { KLGuardianRuntime.shared.resetForRenderer() }
    }
    @MainActor func providers(session: ARKitSession) async -> [any DataProvider] {
        guard KleptonGuardian.shared.mode != .system else { enabled = false; return [] }
        guard PlaneDetectionProvider.isSupported else {
            KLGuardianRuntime.shared.unavailable("Floor mapping is unavailable on this device. Choose the default visionOS area.")
            return []
        }
        let authorization = await session.requestAuthorization(for: [.worldSensing])
        KLGuardianRuntime.shared.setAuthorization(authorization[.worldSensing] == .allowed)
        guard authorization[.worldSensing] == .allowed else {
            KLGuardianRuntime.shared.unavailable("Allow World Sensing in Settings, or choose the default visionOS area.")
            return []
        }
        if planes.state == .stopped || rooms.state == .stopped || meshes.state == .stopped { reset() }
        guard KleptonGuardian.shared.mode != .system else { enabled = false; return [] }
        enabled = true
        var providers: [any DataProvider] = [planes]
        if RoomTrackingProvider.isSupported && SceneReconstructionProvider.isSupported {
            providers += [rooms, meshes]
        } else if KleptonGuardian.shared.mode == .automatic {
            KLGuardianRuntime.shared.unavailable("Room reconstruction is unavailable. Choose a drawn boundary or the default visionOS area.")
            enabled = false
        }
        return providers
    }
    @MainActor func consume() {
        generation += 1; tasks.forEach { $0.cancel() }; tasks = []
        guard enabled else { return }
        let currentGeneration = generation
        let planes = planes, rooms = rooms, meshes = meshes
        tasks.append(Task { @MainActor in
            for await update in planes.anchorUpdates {
                guard !Task.isCancelled, currentGeneration == self.generation else { return }
                // Use only classified floors; a tabletop must not become the ground.
                self.planeAnchors[update.anchor.id] = update.event == .removed ? nil : update.anchor
                let floor = Self.selectFloor(Array(self.planeAnchors.values), head: KLGuardianRuntime.shared.referenceHead)
                if let floor {
                    let transform = floor.originFromAnchorTransform * floor.geometry.extent.anchorFromExtentTransform
                    KLGuardianRuntime.shared.setFloor(transform.columns.3.y)
                } else if update.event == .removed {
                    KLGuardianRuntime.shared.unavailable("The mapped floor was lost. Keep the room visible until tracking returns.")
                }
            }
        })
        tasks.append(Task { @MainActor in
            for await update in rooms.anchorUpdates {
                guard !Task.isCancelled, currentGeneration == self.generation else { return }
                self.roomAnchors[update.anchor.id] = update.event == .removed ? nil : update.anchor
            }
        })
        tasks.append(Task { @MainActor in
            for await update in meshes.anchorUpdates {
                guard !Task.isCancelled, currentGeneration == self.generation else { return }
                self.meshAnchors[update.anchor.id] = update.event == .removed ? nil : update.anchor
            }
        })
        tasks.append(Task { @MainActor in
            while !Task.isCancelled, currentGeneration == self.generation {
                if !self.healthy {
                    KLGuardianRuntime.shared.unavailable("Room tracking is unavailable. Keep passthrough visible or choose the default visionOS area.")
                }
                if self.healthy && KleptonGuardian.shared.mode == .automatic {
                    if let room = self.currentRoom {
                        let roomFloors = room.geometries(classifiedAs: .floor).flatMap {
                            Self.triangles($0, transform: room.originFromAnchorTransform)
                        }
                        // RoomAnchor is a coarse room envelope. The live floor
                        // planes and classified reconstruction carry coverage
                        // that may not yet appear in that envelope.
                        let roomPlaneIDs = Set(room.planeAnchorIDs), roomMeshIDs = Set(room.meshAnchorIDs)
                        let roomPlanes = self.planeAnchors.values.filter { roomPlaneIDs.isEmpty || roomPlaneIDs.contains($0.id) }
                        let roomMeshes = self.meshAnchors.values.filter { roomMeshIDs.isEmpty || roomMeshIDs.contains($0.id) }
                        let floorPlanes = roomPlanes.filter { $0.surfaceClassification == .floor && $0.alignment == .horizontal }
                        let planeFloors = floorPlanes.flatMap { Self.planeTriangles($0) }
                        let reconstructedFloors = roomMeshes.flatMap {
                            Self.triangles($0.geometry, transform: $0.originFromAnchorTransform, only: .floor)
                        }
                        let head = KLGuardianRuntime.shared.referenceHead
                        let detectedFloor = Self.selectFloor(floorPlanes, head: head).map {
                            ($0.originFromAnchorTransform * $0.geometry.extent.anchorFromExtentTransform).columns.3.y
                        }
                        let floorTriangles = roomFloors + planeFloors + reconstructedFloors
                        let detectedWalls = roomPlanes.filter { $0.surfaceClassification == .wall }
                            .flatMap { Self.planeTriangles($0) }
                        let roomWalls = room.geometries(classifiedAs: .wall).flatMap {
                            Self.triangles($0, transform: room.originFromAnchorTransform)
                        }
                        let walls = detectedWalls.isEmpty ? roomWalls : detectedWalls
                        let obstacles = walls + roomMeshes.flatMap {
                            Self.triangles($0.geometry, transform: $0.originFromAnchorTransform,
                                           excluding: walls.isEmpty ? [.floor] : [.floor, .wall])
                        }
                        // Mesh conversion above occurs on the provider's actor;
                        // rasterization runs off the UI actor and only publishes a value.
                        let map = await Task.detached(priority: .utility) {
                            KLGuardianGeometry.mappedCells(floors: floorTriangles, obstacles: obstacles, detectedFloor: detectedFloor)
                        }.value
                        guard !Task.isCancelled, currentGeneration == self.generation else { return }
                        guard self.healthy, let currentRoom = self.currentRoom, currentRoom.id == room.id,
                              simd_distance(currentRoom.originFromAnchorTransform.columns.3, room.originFromAnchorTransform.columns.3) < 0.02,
                              simd_dot(currentRoom.originFromAnchorTransform.columns.2, room.originFromAnchorTransform.columns.2) > 0.999 else { continue }
                        if let map {
                            KLGuardianRuntime.shared.mapped(map.cells, floor: map.floor, room: room.id, field: map.field)
                            KleptonGuardian.shared.updateSavedBoundary?()
                            if Date().timeIntervalSince(self.lastMapDiagnostic) >= 5 {
                                self.lastMapDiagnostic = Date()
                                let connectedCount = head.map { KLGuardianGeometry.connected(map.cells, from: KLGuardianGeometry.cell(at: SIMD2($0.x, $0.z))).count } ?? 0
                                let cellArea = KLGuardianGeometry.cellSize * KLGuardianGeometry.cellSize
                                NSLog("[guardian] auto map: floor faces room %d plane %d mesh %d, walls %d, obstacles %d, floor Y %.3f, covered %.2f m2, clear %.2f m2, connected %.2f m2",
                                      roomFloors.count, planeFloors.count, reconstructedFloors.count, walls.count, obstacles.count,
                                      map.floor, Float(map.field.count) * cellArea, Float(map.cells.count) * cellArea, Float(connectedCount) * cellArea)
                            }
                        } else {
                            KLGuardianRuntime.shared.unavailable("No mapped floor yet. Look around the room or draw a boundary.")
                        }
                    } else {
                        KLGuardianRuntime.shared.unavailable("Looking for the current room. Keep the room visible while mapping.")
                    }
                }
                try? await Task.sleep(for: .milliseconds(750))
            }
        })
    }

    nonisolated private static func selectFloor(_ planes: [PlaneAnchor], head: SIMD3<Float>?) -> PlaneAnchor? {
        // Keep manual drawing and automatic mapping on the same measured floor.
        // A higher floor elsewhere (such as a landing) must not win merely
        // because its height is closer to the wearer's head.
        let candidates = planes.filter { plane in
            guard plane.surfaceClassification == .floor, plane.alignment == .horizontal else { return false }
            let transform = plane.originFromAnchorTransform * plane.geometry.extent.anchorFromExtentTransform
            return head.map { transform.columns.3.y < $0.y - 0.3 } ?? true
        }
        func score(_ plane: PlaneAnchor) -> Float {
            let extent = plane.geometry.extent
            let transform = plane.originFromAnchorTransform * extent.anchorFromExtentTransform
            guard let head else { return -transform.columns.3.y }
            let local = transform.inverse * SIMD4<Float>(head, 1)
            let outside = max(0, abs(local.x) - extent.width / 2) + max(0, abs(local.z) - extent.height / 2)
            return outside * 10 + abs(head.y - transform.columns.3.y)
        }
        return candidates.min { score($0) < score($1) }
    }

    private typealias Triangle = KLGuardianGeometry.Triangle
    nonisolated private static func planeTriangles(_ plane: PlaneAnchor) -> [Triangle] {
        // Use the detected polygon rather than filling its rectangular extent,
        // which can project beyond the actual wall or observed floor.
        triangles(vertices: plane.geometry.meshVertices, faces: plane.geometry.meshFaces,
                  transform: plane.originFromAnchorTransform)
    }
    nonisolated private static func triangles(_ geometry: MeshAnchor.Geometry, transform: simd_float4x4,
                                              only: SurfaceClassification? = nil,
                                              excluding: [SurfaceClassification] = []) -> [Triangle] {
        triangles(vertices: geometry.vertices, faces: geometry.faces, transform: transform,
                  classifications: geometry.classifications, only: only, excluding: excluding)
    }
    nonisolated private static func triangles(vertices: GeometrySource, faces: GeometryElement, transform: simd_float4x4,
                                              classifications: GeometrySource? = nil, only: SurfaceClassification? = nil,
                                              excluding: [SurfaceClassification] = []) -> [Triangle] {
        guard vertices.format == .float3, faces.bytesPerIndex == 2 || faces.bytesPerIndex == 4 else { return [] }
        func vertex(_ index: Int) -> SIMD3<Float> {
            let p = vertices.buffer.contents().advanced(by: vertices.offset + index * vertices.stride)
            let local = SIMD4<Float>(p.load(as: Float.self), p.advanced(by: 4).load(as: Float.self), p.advanced(by: 8).load(as: Float.self), 1)
            let world = transform * local
            return SIMD3(world.x, world.y, world.z)
        }
        func index(_ n: Int) -> Int {
            let p = faces.buffer.contents().advanced(by: n * faces.bytesPerIndex)
            return faces.bytesPerIndex == 2 ? Int(p.load(as: UInt16.self)) : Int(p.load(as: UInt32.self))
        }
        var result: [Triangle] = []
        for face in 0..<min(faces.count, 100_000) {
            var classification: SurfaceClassification?
            if let classes = classifications, classes.format == .uchar,
               face < classes.count {
                let value = classes.buffer.contents().advanced(by: classes.offset + face * classes.stride).load(as: UInt8.self)
                classification = SurfaceClassification(rawValue: Int(value))
            }
            if let only, classification != only { continue }
            if let classification, excluding.contains(classification) { continue }
            let indices = (0..<3).map { index(face * 3 + $0) }
            guard indices.allSatisfy({ $0 < vertices.count }) else { continue }
            let t = Triangle(a: vertex(indices[0]), b: vertex(indices[1]), c: vertex(indices[2]))
            if [t.a, t.b, t.c].allSatisfy({ $0.x.isFinite && $0.y.isFinite && $0.z.isFinite }) { result.append(t) }
        }
        return result
    }
}
