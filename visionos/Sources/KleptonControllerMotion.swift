import simd

/// Convert motion at the tracked accessory origin to motion at the reported
/// grip. ARKit velocities use the anchor's axes, which may differ from .grip.
enum KLControllerMotion {
    static func atGrip(anchor: simd_float4x4, position: SIMD3<Float>,
                       linear: SIMD3<Float>, angular: SIMD3<Float>,
                       local: Bool = true) -> (SIMD3<Float>, SIMD3<Float>) {
        let orientation = simd_quatf(anchor)
        let v = local ? orientation.act(linear) : linear
        let w = local ? orientation.act(angular) : angular
        let origin = SIMD3<Float>(anchor.columns.3.x, anchor.columns.3.y, anchor.columns.3.z)
        // A wrist rotation also moves any point offset from the tracked origin.
        return (v + simd_cross(w, position - origin), w)
    }
}
