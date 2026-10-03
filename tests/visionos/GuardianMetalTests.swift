// Host GPU check: swift tests/visionos/GuardianMetalTests.swift
import Foundation
import Metal
let device = MTLCreateSystemDefaultDevice()!
let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
let renderer = try String(contentsOf: root.appendingPathComponent("visionos/Sources/KleptonGuardianRenderer.swift"), encoding: .utf8)
let source = renderer.components(separatedBy: "static let shader = \"\"\"")[1].components(separatedBy: "\"\"\"")[0]
let library = try device.makeLibrary(source: source, options: nil)
let descriptor = MTLRenderPipelineDescriptor()
descriptor.vertexFunction = library.makeFunction(name: "guardian_fade_v")
descriptor.fragmentFunction = library.makeFunction(name: "guardian_fade_f")
let attachment = descriptor.colorAttachments[0]!
attachment.pixelFormat = .rgba16Float
attachment.isBlendingEnabled = true
attachment.sourceRGBBlendFactor = .zero
attachment.sourceAlphaBlendFactor = .zero
attachment.destinationRGBBlendFactor = .blendAlpha
attachment.destinationAlphaBlendFactor = .blendAlpha
let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
let queue = device.makeCommandQueue()!
let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: 8, height: 8, mipmapped: false)
textureDescriptor.usage = [.renderTarget]
textureDescriptor.storageMode = .shared
let texture = device.makeTexture(descriptor: textureDescriptor)!
for opacity: Float in [0, 0.25, 0.5, 1] {
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = texture
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0.5, blue: 0.25, alpha: 1)
    let command = queue.makeCommandBuffer()!
    let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
    encoder.setRenderPipelineState(pipeline)
    encoder.setBlendColor(red: 0, green: 0, blue: 0, alpha: opacity)
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
    precondition(command.status == .completed)
    var pixels = [UInt16](repeating: 0, count: 8 * 8 * 4)
    pixels.withUnsafeMutableBytes { texture.getBytes($0.baseAddress!, bytesPerRow: 8 * 8, from: MTLRegionMake2D(0, 0, 8, 8), mipmapLevel: 0) }
    for i in stride(from: 0, to: pixels.count, by: 4) {
        for (channel, original): (Int, Float) in [(0, 1), (1, 0.5), (2, 0.25), (3, 1)] {
            let actual = Float(Float16(bitPattern: pixels[i + channel]))
            precondition(abs(actual - original * opacity) < 0.001)
        }
    }
}
print("Metal guardian fade scales premultiplied RGB and alpha correctly at 0, 25, 50 and 100 percent")
