import simd

@main struct ControllerMotionTests {
    static func near(_ actual: SIMD3<Float>, _ expected: SIMD3<Float>,
                     _ message: String, tolerance: Float = 0.0001) {
        precondition(simd_distance(actual, expected) < tolerance, message)
    }

    static func main() {
        // A body turned 90 degrees around Z: local X translation and rotation
        // must arrive along world Y, regardless of the grip's own orientation.
        var anchor = simd_float4x4(simd_quatf(angle: .pi / 2, axis: SIMD3(0, 0, 1)))
        anchor.columns.3 = SIMD4(1, 2, 3, 1)
        let origin = SIMD3<Float>(1, 2, 3)
        let (v, w) = KLControllerMotion.atGrip(anchor: anchor, position: origin,
            linear: SIMD3(2, 0, 0), angular: SIMD3(3, 0, 0))
        near(v, SIMD3(0, 2, 0), "Linear velocity must use anchor axes")
        near(w, SIMD3(0, 3, 0), "Angular velocity must use anchor axes")

        // A pure wrist rotation about Z moves a grip 5cm along X towards Y.
        let (flick, spin) = KLControllerMotion.atGrip(anchor: matrix_identity_float4x4,
            position: SIMD3(0.05, 0, 0), linear: .zero, angular: SIMD3(0, 0, 10))
        near(flick, SIMD3(0, 0.5, 0), "Grip velocity must include rotational motion")
        near(spin, SIMD3(0, 0, 10), "Translation must not change angular velocity")

        // Match the headset's 34.81-degree anchor/grip disagreement. Compare
        // against a central difference of a moving, rotating rigid body, rather
        // than calculating the expected velocity using the production formula.
        let body = simd_quatf(angle: -34.81 * .pi / 180, axis: SIMD3(1, 0, 0))
        var base = simd_float4x4(body)
        base.columns.3 = SIMD4(1, 2, 3, 1)
        let localLever = SIMD3<Float>(0.012, -0.021, 0.035)
        let localV = SIMD3<Float>(0.4, 0.7, -0.2)
        let localW = SIMD3<Float>(2, -3, 4)
        let worldW = body.act(localW)
        let worldV = body.act(localV)
        let grip = origin + body.act(localLever)
        let (computed, computedW) = KLControllerMotion.atGrip(anchor: base,
            position: grip, linear: localV, angular: localW)
        func gripAt(_ dt: Float) -> SIMD3<Float> {
            let turn = simd_quatf(angle: simd_length(worldW) * dt,
                                 axis: simd_normalize(worldW))
            return origin + worldV * dt + (turn * body).act(localLever)
        }
        let dt: Float = 0.001
        near(computed, (gripAt(dt) - gripAt(-dt)) / (2 * dt),
             "Reported velocity must follow reported grip position", tolerance: 0.0003)
        near(computedW, worldW, "Motion rotation must ignore grip tilt")
        let (worldInput, worldSpin) = KLControllerMotion.atGrip(anchor: base,
            position: grip, linear: worldV, angular: worldW, local: false)
        near(worldInput, computed, "World diagnostic must transfer origin to grip too")
        near(worldSpin, computedW, "World diagnostic must avoid a second rotation")
        print("Controller motion: anchor axes, wrist lever arm and pose derivative passed")
    }
}
