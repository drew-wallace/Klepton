// Steam Cloud wire messages, independent of the game's Steamworks SDK.
import Foundation
import CryptoKit
import Compression

struct KLCloudError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}
struct KLCloudRPCError: LocalizedError {
    var method: Int32
    var result: Int32
    var errorDescription: String? { "Steam Cloud operation \(method) failed (result \(result)). Retry before launching." }
}

struct KLCloudProto {
    struct Field {
        var number: Int
        var integer: UInt64 = 0
        var bytes = Data()
        var string: String { String(data: bytes, encoding: .utf8) ?? "" }
    }
    static func parse(_ data: Data) throws -> [Field] {
        let bytes = Array(data); var at = 0; var fields: [Field] = []
        func varint() throws -> UInt64 {
            var value: UInt64 = 0
            for shift in stride(from: 0, through: 63, by: 7) {
                guard at < bytes.count else { throw KLCloudError("Truncated Steam Cloud response.") }
                let byte = bytes[at]; at += 1
                guard shift != 63 || byte <= 1 else { throw KLCloudError("Invalid Steam Cloud number.") }
                value |= UInt64(byte & 127) << shift
                if byte & 128 == 0 { return value }
            }
            throw KLCloudError("Invalid Steam Cloud number.")
        }
        while at < bytes.count {
            let tag = try varint(); let number = Int(tag >> 3)
            guard number > 0, fields.count < 150_000 else { throw KLCloudError("Invalid Steam Cloud response.") }
            var field = Field(number: number)
            switch tag & 7 {
            case 0: field.integer = try varint()
            case 1, 5:
                let length = tag & 7 == 1 ? 8 : 4
                guard bytes.count - at >= length else { throw KLCloudError("Truncated Steam Cloud response.") }
                for i in 0..<length { field.integer |= UInt64(bytes[at + i]) << (i * 8) }; at += length
            case 2:
                let length = try varint()
                guard length <= UInt64(bytes.count - at) else { throw KLCloudError("Truncated Steam Cloud response.") }
                field.bytes = Data(bytes[at..<(at + Int(length))]); at += Int(length)
            default: throw KLCloudError("Unsupported Steam Cloud response.")
            }
            fields.append(field)
        }
        return fields
    }
    static func number(_ field: Int, _ value: UInt64) -> Data {
        var data = Data()
        func append(_ input: UInt64) {
            var n = input
            while n >= 128 { data.append(UInt8(n & 127) | 128); n >>= 7 }
            data.append(UInt8(n))
        }
        append(UInt64(field << 3)); append(value); return data
    }
    static func bytes(_ field: Int, _ value: Data) -> Data {
        // Reuse the varint writer and replace its tag with a length-delimited tag.
        var header = number(field, UInt64(value.count)); header[0] |= 2
        return header + value
    }
    static func string(_ field: Int, _ value: String) -> Data { bytes(field, Data(value.utf8)) }
}

struct KLCloudFile: Equatable {
    var path: String
    var sha: Data
    var size: UInt64
    var time: UInt64
    var platforms: UInt64 = 0xffff_ffff
    var supportsAndroid: Bool { platforms & 32 != 0 }
}
struct KLCloudListing: Equatable {
    var generation: UInt64
    var files: [KLCloudFile]
    static func decode(_ data: Data) throws -> Self {
        let fields = try KLCloudProto.parse(data)
        let prefixes = fields.filter { $0.number == 4 }.map(\.string)
        var files: [KLCloudFile] = []; var seen = Set<String>()
        for field in fields where field.number == 2 {
            let item = try KLCloudProto.parse(field.bytes)
            guard item.value(5) == 0 else { continue } // Never propagate deletion.
            let prefix: String
            if let index = item.first(where: { $0.number == 7 })?.integer {
                guard index < prefixes.count else { throw KLCloudError("Invalid Steam Cloud folder.") }
                prefix = prefixes[Int(index)]
            } else { prefix = "" }
            let file = KLCloudFile(path: prefix + item.text(1), sha: item.data(2), size: item.value(4), time: item.value(3), platforms: item.first { $0.number == 6 }?.integer ?? 0xffff_ffff)
            guard !file.path.isEmpty, file.sha.count == 20, file.size <= KLCloudHTTP.limit,
                  seen.insert(file.path.lowercased()).inserted, files.count < 20_000 else {
                throw KLCloudError("Invalid or oversized Steam Cloud file list.")
            }
            files.append(file)
        }
        return Self(generation: fields.value(1), files: files.sorted { $0.path < $1.path })
    }
}
extension Array where Element == KLCloudProto.Field {
    func value(_ field: Int) -> UInt64 { first { $0.number == field }?.integer ?? 0 }
    func text(_ field: Int) -> String { first { $0.number == field }?.string ?? "" }
    func data(_ field: Int) -> Data { first { $0.number == field }?.bytes ?? Data() }
}

private final class KLCloudRedirectPolicy: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(request.url?.scheme == "https" && request.url?.host == response.url?.host
                          && request.url?.port == response.url?.port && request.url?.user == nil && request.url?.password == nil ? request : nil)
    }
}
enum KLCloudHTTP {
    static let limit: UInt64 = 256 * 1024 * 1024
    static func uploadBlocks(_ reply: Data, size: UInt64) throws -> Data {
        let fields = try KLCloudProto.parse(reply)
        var block = KLCloudProto.string(1, fields.text(3)) + KLCloudProto.string(2, fields.text(4))
        block += KLCloudProto.number(3, fields.value(5)) + KLCloudProto.number(4, 4)
        for field in fields where field.number == 6 { block += KLCloudProto.bytes(5, field.bytes) }
        block += KLCloudProto.number(6, 0) + KLCloudProto.number(7, size)
        return KLCloudProto.bytes(2, block)
    }
    static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 60; config.timeoutIntervalForResource = 180
        config.urlCache = nil; config.httpCookieStorage = nil; config.urlCredentialStorage = nil
        return URLSession(configuration: config, delegate: KLCloudRedirectPolicy(), delegateQueue: nil)
    }()
    static func request(host: String, path: String, https: Bool, headers: [KLCloudProto.Field]) throws -> URLRequest {
        guard https, !host.isEmpty, host.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "." || $0 == "-") }),
              path.hasPrefix("/"), !path.contains("\\"), !path.contains("\r"), !path.contains("\n"),
              let url = URL(string: "https://" + host + path), url.host == host, url.user == nil else {
            throw KLCloudError("Steam returned an unsupported cloud transfer address.")
        }
        var request = URLRequest(url: url)
        for field in headers {
            let header = try KLCloudProto.parse(field.bytes)
            let name = header.text(1), value = header.text(2)
            guard !name.isEmpty, !name.contains(":"), !name.contains("\r"), !name.contains("\n"),
                  !value.contains("\r"), !value.contains("\n") else { throw KLCloudError("Invalid cloud transfer header.") }
            request.setValue(value, forHTTPHeaderField: name)
        }
        return request
    }
    static func download(_ response: Data, file: KLCloudFile) async throws -> Data {
        let fields = try KLCloudProto.parse(response)
        guard fields.value(11) == 0, fields.value(2) <= limit, fields.value(3) == file.size else {
            throw KLCloudError("Unsupported encrypted or oversized cloud save.")
        }
        let request = try request(host: fields.text(7), path: fields.text(8), https: fields.value(9) != 0,
                                  headers: fields.filter { $0.number == 10 })
        let (location, response) = try await session.download(for: request)
        defer { try? FileManager.default.removeItem(at: location) }
        let size = try location.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        guard (response as? HTTPURLResponse)?.statusCode == 200, size <= limit else { throw KLCloudError("Steam Cloud download failed.") }
        var data = try Data(contentsOf: location)
        if UInt64(data.count) != file.size || fields.value(2) != fields.value(3) {
            data = try unpack(data, size: Int(file.size))
        }
        guard UInt64(data.count) == file.size, Data(Insecure.SHA1.hash(data: data)) == file.sha else {
            throw KLCloudError("Steam Cloud save checksum mismatch.")
        }
        return data
    }
    static func unpack(_ data: Data, size: Int) throws -> Data {
        let b = Array(data)
        func u16(_ i: Int) -> Int { Int(b[i]) | Int(b[i + 1]) << 8 }
        func u32(_ i: Int) -> Int { u16(i) | u16(i + 2) << 16 }
        guard b.count >= 30, u32(0) == 0x04034b50, size >= 0, size <= limit else { throw KLCloudError("Invalid compressed cloud save.") }
        let start = 30 + u16(26) + u16(28)
        guard start <= b.count else { throw KLCloudError("Invalid compressed cloud save.") }
        var packed = u32(18)
        if packed == 0, b.count >= 46 {
            for i in stride(from: b.count - 46, through: start, by: -1) where u32(i) == 0x02014b50 { packed = u32(i + 20); break }
        }
        guard packed <= b.count - start else { throw KLCloudError("Invalid compressed cloud save.") }
        let payload = Array(b[start..<(start + packed)])
        if u16(8) == 0, payload.count == size { return Data(payload) }
        guard u16(8) == 8 else { throw KLCloudError("Unsupported cloud save compression.") }
        if size == 0 { return Data() }
        var output = [UInt8](repeating: 0, count: size + 1)
        let count = compression_decode_buffer(&output, output.count, payload, payload.count, nil, COMPRESSION_ZLIB)
        guard count == size else { throw KLCloudError("Cloud save decompression failed.") }
        return Data(output.prefix(size))
    }
    static func upload(_ response: Data, bytes: Data, check: () throws -> Void) async throws {
        let fields = try KLCloudProto.parse(response)
        let blocks = fields.filter { $0.number == 2 }
        guard fields.value(1) == 0, !blocks.isEmpty, blocks.count <= 4096 else { throw KLCloudError("Unsupported cloud upload.") }
        for block in blocks {
            try check()
            let fields = try KLCloudProto.parse(block.bytes)
            var request = try request(host: fields.text(1), path: fields.text(2), https: fields.value(3) != 0,
                                      headers: fields.filter { $0.number == 5 })
            guard [3, 4].contains(fields.value(4)) else { throw KLCloudError("Unsupported cloud upload method.") }
            request.httpMethod = fields.value(4) == 3 ? "POST" : "PUT"
            var body = fields.data(8)
            if body.isEmpty {
                let offset = fields.value(6), length = fields.value(7)
                guard offset <= bytes.count, length <= UInt64(bytes.count) - offset else { throw KLCloudError("Invalid cloud upload block.") }
                body = bytes.subdata(in: Int(offset)..<Int(offset + length))
            }
            let (_, reply) = try await session.upload(for: request, from: body)
            guard let status = (reply as? HTTPURLResponse)?.statusCode, (200..<300).contains(status) else { throw KLCloudError("Steam Cloud upload failed.") }
        }
    }
}
