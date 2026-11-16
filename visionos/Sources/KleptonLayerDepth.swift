import Metal

enum KLLayerDepth {
    // Composition layers follow submission order, including coplanar layers.
    // Keep finite depth for visionOS reprojection when layers are the entire
    // picture; overlays above an existing eye image retain that image's depth.
    static func descriptor(writeDepth: Bool) -> MTLDepthStencilDescriptor {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = .always
        descriptor.isDepthWriteEnabled = writeDepth
        return descriptor
    }
}
