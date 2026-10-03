// swiftc visionos/Sources/KleptonGuardianCoordinates.swift \
//   tests/visionos/GuardianCoordinatesTests.swift -o /tmp/guardian-coordinates-tests && /tmp/guardian-coordinates-tests
import simd
import Foundation

@main struct GuardianCoordinatesTests {
    static func main() {
        let points: [SIMD2<Float>] = [SIMD2(-2, -2), SIMD2(2, -2), SIMD2(2, 2), SIMD2(-2, 2)]
        // Reproduce an actual anchor whose origin is lower than the drawn floor.
        var anchor = matrix_identity_float4x4
        anchor.columns.3 = SIMD4(1, -2.8, 3, 1)
        let local = KLGuardianCoordinates.localPoints(points, floor: -1.4, anchor: anchor)
        let restored = KLGuardianCoordinates.worldPoints(local, anchor: anchor)
        for (point, original) in zip(restored, points) {
            precondition(abs(point.x - original.x) < 0.001 && abs(point.z - original.y) < 0.001)
            precondition(abs(point.y + 1.4) < 0.001, "Saving must preserve the drawn floor instead of using the anchor height")
        }
        // On a later run, ARKit may relocate and rotate the world origin.
        let rotated = simd_float4x4(simd_quatf(angle: .pi / 2, axis: SIMD3(0, 1, 0)))
        var relocated = rotated
        relocated.columns.3 = SIMD4(-4, -1.2, 5, 1)
        let moved = KLGuardianCoordinates.worldPoints(local, anchor: relocated)
        for (point, offset) in zip(moved, local) {
            let expected = relocated * SIMD4<Float>(offset[0], offset[1], offset[2], 1)
            precondition(simd_distance(point, SIMD3(expected.x, expected.y, expected.z)) < 0.001)
        }
        precondition(abs(simd_distance(moved[0], moved[1]) - 4) < 0.001)
        let id = UUID()
        var cache = KLGuardianAnchorCache<simd_float4x4>()
        cache.beginSession()
        let previousSession = cache.generation
        cache.update(id, value: anchor, generation: previousSession)
        cache.beginSession()
        cache.update(id, value: relocated, generation: cache.generation)
        // A late callback from the old provider must not replace the new pose
        // with a transform that would follow the entry head position/yaw.
        cache.update(id, value: anchor, generation: previousSession)
        let current = KLGuardianCoordinates.worldPoints(local, anchor: cache.values[id]!)
        precondition(zip(current, moved).allSatisfy { simd_distance($0, $1) < 0.001 })
        print("Guardian anchor-coordinate tests passed")
    }
}
