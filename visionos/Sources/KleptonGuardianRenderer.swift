import Metal
import CompositorServices
import simd

// A final pass covers every guest projection/skybox/UI path. Multiplying both
// premultiplied RGB and alpha reveals system passthrough in mixed immersion.
final class KLGuardianRenderer {
    private let fade: MTLRenderPipelineState
    private let lines: MTLRenderPipelineState
    private let noDepthWrite: MTLDepthStencilState
    private let lineDepth: MTLDepthStencilState
    private let device: MTLDevice
    private let amplification: Bool
    private var cachedEdges: [KLGuardianGeometry.Segment] = []
    private var cachedFloor: Float = .nan
    private var lineBuffer: MTLBuffer?
    private var lineVertexCount = 0

    init(device: MTLDevice, color: MTLPixelFormat) throws {
        self.device = device
        let supportsAmplification = device.supportsVertexAmplificationCount(2)
        amplification = supportsAmplification
        let library = try device.makeLibrary(source: Self.shader, options: nil)
        func pipeline(vertex: String, fragment: String, line: Bool) throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: vertex)
            d.fragmentFunction = library.makeFunction(name: fragment)
            d.maxVertexAmplificationCount = supportsAmplification ? 2 : 1
            d.depthAttachmentPixelFormat = .depth32Float
            let a = d.colorAttachments[0]!
            a.pixelFormat = color; a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = line ? .one : .zero
            a.sourceAlphaBlendFactor = line ? .one : .zero
            a.destinationRGBBlendFactor = line ? .oneMinusSourceAlpha : .blendAlpha
            a.destinationAlphaBlendFactor = line ? .oneMinusSourceAlpha : .blendAlpha
            return try device.makeRenderPipelineState(descriptor: d)
        }
        fade = try pipeline(vertex: "guardian_fade_v", fragment: "guardian_fade_f", line: false)
        lines = try pipeline(vertex: "guardian_line_v", fragment: "guardian_line_f", line: true)
        let depth = MTLDepthStencilDescriptor()
        depth.depthCompareFunction = .always; depth.isDepthWriteEnabled = false
        noDepthWrite = device.makeDepthStencilState(descriptor: depth)!
        depth.isDepthWriteEnabled = true
        lineDepth = device.makeDepthStencilState(descriptor: depth)!
    }
    func encode(_ drawable: LayerRenderer.Drawable, cmd: MTLCommandBuffer,
                originFromDevice: simd_float4x4, state: KLGuardianRuntime.Frame) {
        guard state.reveal > 0 || state.showLines else { return }
        if cachedEdges != state.edges || cachedFloor != state.floor {
            cachedEdges = state.edges; cachedFloor = state.floor
            var vertices: [SIMD4<Float>] = []
            for edge in state.edges {
                let delta = edge.b - edge.a, length = KLGuardianGeometry.length(delta)
                guard length > 0 else { continue }
                let offset = SIMD2<Float>(-delta.y, delta.x) / length * 0.015
                let a = edge.a - offset, b = edge.a + offset, c = edge.b - offset, d = edge.b + offset
                for p in [a, b, c, b, d, c] { vertices.append(SIMD4(p.x, state.floor + 0.015, p.y, 1)) }
            }
            lineVertexCount = vertices.count
            lineBuffer = vertices.isEmpty ? nil : vertices.withUnsafeBytes {
                device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
            }
        }
        let layered = amplification && drawable.views.count == 2 && drawable.rasterizationRateMaps.count == 1 && drawable.colorTextures.count == 1
        let groups = layered ? [Array(drawable.views.indices)] : drawable.views.indices.map { [$0] }
        for indices in groups {
            let map = drawable.views[indices[0]].textureMap
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawable.colorTextures[map.textureIndex]
            pass.colorAttachments[0].loadAction = .load; pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = drawable.depthTextures[map.textureIndex]
            pass.depthAttachment.loadAction = .load; pass.depthAttachment.storeAction = .store
            if layered {
                pass.renderTargetArrayLength = indices.count
                pass.rasterizationRateMap = drawable.rasterizationRateMaps.first
            } else {
                pass.colorAttachments[0].slice = map.sliceIndex
                pass.depthAttachment.slice = map.sliceIndex
                if drawable.rasterizationRateMaps.count == drawable.views.count {
                    pass.rasterizationRateMap = drawable.rasterizationRateMaps[indices[0]]
                }
            }
            guard let encoder = cmd.makeRenderCommandEncoder(descriptor: pass) else { continue }
            encoder.label = "guardian passthrough and floor outline"
            encoder.setViewports(indices.map { drawable.views[$0].textureMap.viewport })
            if layered {
                var mappings = indices.enumerated().map { n, vi in
                    MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32(n),
                        renderTargetArrayIndexOffset: UInt32(drawable.views[vi].textureMap.sliceIndex))
                }
                encoder.setVertexAmplificationCount(indices.count, viewMappings: &mappings)
            }
            encoder.setCullMode(.none)
            encoder.setDepthStencilState(noDepthWrite)
            encoder.setRenderPipelineState(fade)
            encoder.setBlendColor(red: 0, green: 0, blue: 0, alpha: 1 - state.reveal)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            if state.showLines, let lineBuffer {
                var matrices = indices.map { vi in
                    drawable.computeProjection(viewIndex: vi) * (originFromDevice * drawable.views[vi].transform).inverse
                }
                encoder.setRenderPipelineState(lines)
                encoder.setDepthStencilState(lineDepth)
                encoder.setVertexBuffer(lineBuffer, offset: 0, index: 0)
                matrices.withUnsafeMutableBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: lineVertexCount)
            }
            encoder.endEncoding()
        }
    }
    static let shader = """
    #include <metal_stdlib>
    using namespace metal;
    struct GuardianVertex { float4 position [[position]]; };
    vertex GuardianVertex guardian_fade_v(uint id [[vertex_id]], uint eye [[amplification_id]]) {
        float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(3, -1) : float2(-1, 3));
        return { float4(p, 0, 1) };
    }
    fragment float4 guardian_fade_f() { return float4(0); }
    vertex GuardianVertex guardian_line_v(uint id [[vertex_id]], uint eye [[amplification_id]],
                                         device const float4 *vertices [[buffer(0)]],
                                         constant float4x4 *matrices [[buffer(1)]]) {
        return { matrices[eye] * vertices[id] };
    }
    fragment float4 guardian_line_f() { return float4(0.05, 0.65, 0.8, 0.9); }
    """
}
