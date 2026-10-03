import simd
import Foundation

// Only anchorUpdates may populate this cache. On visionOS 27, allAnchors can
// expose a different world frame. Old provider callbacks cannot enter a new one.
struct KLGuardianAnchorCache<Value> {
    private(set) var generation = 0
    private(set) var values: [UUID: Value] = [:]
    mutating func beginSession() {
        generation += 1; values = [:]
    }
    mutating func update(_ id: UUID, value: Value?, generation: Int) {
        guard generation == self.generation else { return }
        values[id] = value
    }
}

// Saved outlines are measured in ARKit's physical world. Encode against the
// first TRACKED anchor transform, which can differ from the requested pose.
enum KLGuardianCoordinates {
    static func localPoints(_ points: [SIMD2<Float>], floor: Float,
                            anchor: simd_float4x4) -> [[Float]] {
        let inverse = anchor.inverse
        return points.map {
            let p = inverse * SIMD4<Float>($0.x, floor, $0.y, 1)
            return [p.x, p.y, p.z]
        }
    }
    static func worldPoints(_ points: [[Float]], anchor: simd_float4x4) -> [SIMD3<Float>] {
        points.map {
            let p = anchor * SIMD4<Float>($0[0], $0[1], $0[2], 1)
            return SIMD3(p.x, p.y, p.z)
        }
    }
}
