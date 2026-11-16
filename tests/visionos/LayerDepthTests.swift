// Host GPU regression: compile with visionos/Sources/KleptonLayerDepth.swift.
import Foundation
import Metal
import simd

@main struct LayerDepthTests {
    struct Quad {
        var rect: SIMD4<Float>
        var color: SIMD4<Float>
        var depth: Float
    }

    static func main() throws {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else {
            fatalError("A Metal device is required for the layer-depth regression")
        }
        let library = try device.makeLibrary(source: """
            #include <metal_stdlib>
            using namespace metal;
            struct Quad { float4 rect; float4 color; float depth; };
            struct Vertex { float4 position [[position]]; float4 color; };
            vertex Vertex layer_v(uint id [[vertex_id]], constant Quad &q [[buffer(0)]]) {
                float2 p = float2((id & 1) ? q.rect.z : q.rect.x,
                                 (id & 2) ? q.rect.w : q.rect.y);
                return {float4(p, q.depth, 1), q.color};
            }
            fragment float4 layer_f(Vertex v [[stage_in]]) { return v.color; }
            """, options: nil)
        let pd = MTLRenderPipelineDescriptor()
        pd.vertexFunction = library.makeFunction(name: "layer_v")
        pd.fragmentFunction = library.makeFunction(name: "layer_f")
        pd.depthAttachmentPixelFormat = .depth32Float
        let color = pd.colorAttachments[0]!
        color.pixelFormat = .rgba8Unorm
        color.isBlendingEnabled = true
        color.sourceRGBBlendFactor = .sourceAlpha
        color.destinationRGBBlendFactor = .oneMinusSourceAlpha
        color.sourceAlphaBlendFactor = .one
        color.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        let pipeline = try device.makeRenderPipelineState(descriptor: pd)
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm,
            width: 16, height: 8, mipmapped: false)
        td.usage = .renderTarget
        td.storageMode = .shared
        let output = device.makeTexture(descriptor: td)!
        td.pixelFormat = .depth32Float
        td.storageMode = .private
        let depth = device.makeTexture(descriptor: td)!
        let depthCopy = device.makeBuffer(length: 256 * 8, options: .storageModeShared)!
        let legacy = MTLDepthStencilDescriptor()
        legacy.depthCompareFunction = .greater
        legacy.isDepthWriteEnabled = true
        let legacyState = device.makeDepthStencilState(descriptor: legacy)!

        func render(state: MTLDepthStencilState, insetDepth: Float, alpha: Float,
                    seedEye: Bool) -> ([UInt8], [Float]) {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = output
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.clearDepth = 0
            let command = queue.makeCommandBuffer()!
            let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            func draw(_ rect: SIMD4<Float>, _ rgba: SIMD4<Float>, _ z: Float) {
                var quad = Quad(rect: rect, color: rgba, depth: z)
                encoder.setVertexBytes(&quad, length: MemoryLayout<Quad>.stride, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            // Separate base images for the two eyes, followed by sharp insets.
            // Seed an eye buffer to check that decorative overlays preserve it.
            encoder.setDepthStencilState(seedEye ? legacyState : state)
            draw(SIMD4(-1, -1, 0, 1), SIMD4(0, 0, 1, 1), 0.05)
            draw(SIMD4(0, -1, 1, 1), SIMD4(0, 0, 1, 1), 0.05)
            encoder.setDepthStencilState(state)
            draw(SIMD4(-0.75, -0.5, -0.25, 0.5), SIMD4(1, 0, 0, alpha), insetDepth)
            draw(SIMD4(0.25, -0.5, 0.75, 0.5), SIMD4(1, 0, 0, alpha), insetDepth)
            encoder.endEncoding()
            let blit = command.makeBlitCommandEncoder()!
            blit.copy(from: depth, sourceSlice: 0, sourceLevel: 0,
                sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: 16, height: 8, depth: 1),
                to: depthCopy, destinationOffset: 0, destinationBytesPerRow: 256,
                destinationBytesPerImage: 256 * 8)
            blit.endEncoding()
            command.commit(); command.waitUntilCompleted()
            precondition(command.status == .completed, "GPU command failed")
            var pixels = [UInt8](repeating: 0, count: 16 * 8 * 4)
            pixels.withUnsafeMutableBytes {
                output.getBytes($0.baseAddress!, bytesPerRow: 16 * 4,
                    from: MTLRegionMake2D(0, 0, 16, 8), mipmapLevel: 0)
            }
            let rawDepth = depthCopy.contents().assumingMemoryBound(to: Float.self)
            let depths = (0..<8).flatMap { y in (0..<16).map { x in rawDepth[y * 64 + x] } }
            return (pixels, depths)
        }

        // Reproduce the old detail switching as the inset crosses equal depth.
        for z: Float in [0.049999, 0.05, 0.050001] {
            let (pixels, _) = render(state: legacyState, insetDepth: z, alpha: 1, seedEye: false)
            let center = (4 * 16 + 4) * 4
            precondition(pixels[center] == (z > 0.05 ? 255 : 0),
                "The old strict-depth policy should reproduce disappearing detail")
        }
        for writes in [true, false] {
            let state = device.makeDepthStencilState(
                descriptor: KLLayerDepth.descriptor(writeDepth: writes))!
            for z: Float in [0.049999, 0.05, 0.050001] {
                for alpha: Float in [0, 0.5, 1] {
                    let (pixels, depths) = render(state: state, insetDepth: z,
                        alpha: alpha, seedEye: !writes)
                    for y in 0..<8 { for x in 0..<16 {
                        let inset = (2..<6).contains(x % 8) && (2..<6).contains(y)
                        let red = inset ? Int((alpha * 255).rounded()) : 0
                        let blue = inset ? Int(((1 - alpha) * 255).rounded()) : 255
                        let i = (y * 16 + x) * 4
                        precondition(abs(Int(pixels[i]) - red) <= 1 && pixels[i + 1] == 0
                            && abs(Int(pixels[i + 2]) - blue) <= 1 && pixels[i + 3] == 255,
                            "Both eyes must retain stable inset alpha and unchanged periphery")
                        let expectedDepth: Float = writes && inset ? z : 0.05
                        precondition(abs(depths[y * 16 + x] - expectedDepth) < 0.0000001,
                            "Layer-only frames need finite depth in both eyes; overlays retain eye depth")
                    } }
                }
            }
        }
        print("Metal layer depth: old flicker reproduced; ordered stereo insets, alpha, periphery and reprojection depth passed")
    }
}
