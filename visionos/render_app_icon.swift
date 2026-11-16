// Host-side icon rasterizer. No Python imaging packages or game art in Git.
import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

struct Paint: Decodable {
    var path: String?
    var color: [CGFloat]?
}
struct Icon: Decodable {
    var background: Paint
    var foreground: Paint
    var front_output: String
    var back_output: String
}
struct IconError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

func render(_ paint: Paint, opaque: Bool, to output: String) throws {
    let size = 1024
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let context = CGContext(data: nil, width: size, height: size,
                                  bitsPerComponent: 8, bytesPerRow: size * 4,
                                  space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        throw IconError("Cannot create icon canvas")
    }
    let bounds = CGRect(x: 0, y: 0, width: size, height: size)
    if opaque {
        context.setFillColor(CGColor(colorSpace: space, components: [0.08, 0.10, 0.14, 1])!)
        context.fill(bounds)
    }
    if let components = paint.color {
        guard components.count == 4,
              let color = CGColor(colorSpace: space, components: components) else {
            throw IconError("Invalid launcher color")
        }
        context.setFillColor(color)
        context.fill(bounds)
    }
    if let path = paint.path {
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil),
              image.width > 0, image.height > 0 else {
            throw IconError("Cannot decode launcher image: \(path)")
        }
        let scale = max(CGFloat(size) / CGFloat(image.width), CGFloat(size) / CGFloat(image.height))
        let width = CGFloat(image.width) * scale, height = CGFloat(image.height) * scale
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: (CGFloat(size) - width) / 2,
                                      y: (CGFloat(size) - height) / 2, width: width, height: height))
    }
    guard let image = context.makeImage(),
          let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: output) as CFURL,
                                                            UTType.png.identifier as CFString, 1, nil) else {
        throw IconError("Cannot write icon image")
    }
    CGImageDestinationAddImage(destination, image, nil)
    guard CGImageDestinationFinalize(destination) else { throw IconError("Cannot finish icon PNG") }
}

do {
    guard CommandLine.arguments.count == 2 else { throw IconError("Expected icon JSON path") }
    let spec = try JSONDecoder().decode(Icon.self, from: Data(contentsOf:
        URL(fileURLWithPath: CommandLine.arguments[1])))
    try render(spec.background, opaque: true, to: spec.back_output)
    try render(spec.foreground, opaque: false, to: spec.front_output)
} catch {
    FileHandle.standardError.write(Data("\(error)\n".utf8))
    exit(1)
}
