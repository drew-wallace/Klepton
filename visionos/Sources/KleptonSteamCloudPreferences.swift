import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

// Only exact-APK audited keys become cloud files. The containing PlayerPrefs
// XML can also hold Steam credentials and is never uploaded.
private final class KLPreferenceXML: NSObject, XMLParserDelegate {
    final class Node {
        var name: String
        var attributes: [String: String]
        var text = ""
        var children: [Node] = []
        init(_ name: String, _ attributes: [String: String] = [:]) { self.name = name; self.attributes = attributes }
        func xml() -> String {
            func escape(_ s: String) -> String {
                s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
                    .replacingOccurrences(of: ">", with: "&gt;").replacingOccurrences(of: "\"", with: "&quot;")
                    .replacingOccurrences(of: "\r", with: "&#13;")
            }
            let attrs = attributes.sorted { $0.key < $1.key }.map { " \($0.key)=\"\(escape($0.value))\"" }.joined()
            return "<\(name)\(attrs)>" + escape(text) + children.map { $0.xml() }.joined() + "</\(name)>"
        }
    }
    var root: Node?
    private var stack: [Node] = []
    init(_ bytes: Data?) throws {
        super.init()
        guard let bytes else { root = Node("map"); return }
        guard bytes.count <= 8 * 1024 * 1024, let text = String(data: bytes, encoding: .utf8),
              !text.contains("<!DOCTYPE"), !text.contains("<!ENTITY") else { throw KLCloudError("Unsupported Android save preferences XML.") }
        let parser = XMLParser(data: bytes)
        parser.shouldResolveExternalEntities = false; parser.delegate = self
        guard parser.parse(), root?.name == "map" else { throw KLCloudError("Could not read Android save preferences.") }
    }
    func parser(_ parser: XMLParser, didStartElement name: String, namespaceURI: String?, qualifiedName: String?, attributes: [String: String]) {
        guard stack.count < 8 else { parser.abortParsing(); return }
        let node = Node(name, attributes)
        if let parent = stack.last { parent.children.append(node) } else { root = node }
        stack.append(node)
    }
    func parser(_ parser: XMLParser, foundCharacters text: String) { stack.last?.text += text }
    func parser(_ parser: XMLParser, foundCDATA bytes: Data) {
        guard let text = String(data: bytes, encoding: .utf8) else { parser.abortParsing(); return }
        stack.last?.text += text
    }
    func parser(_ parser: XMLParser, didEndElement: String, namespaceURI: String?, qualifiedName: String?) { _ = stack.popLast() }
    func entry(_ key: String) throws -> Node? {
        let matches = root!.children.filter { $0.attributes["name"] == key }
        guard matches.count <= 1, matches.allSatisfy({ $0.name == "string" && $0.children.isEmpty }) else {
            throw KLCloudError("Android cloud save preference has an unsupported type or duplicate key.")
        }
        return matches.first
    }
    func value(_ key: String) throws -> Data? { try entry(key).map { Data($0.text.utf8) } }
    func set(_ key: String, bytes: Data) throws {
        guard bytes.count <= 8 * 1024 * 1024, let value = String(data: bytes, encoding: .utf8),
              value.unicodeScalars.allSatisfy({ [9, 10, 13].contains($0.value) || (0x20...0xd7ff).contains($0.value) ||
                  (0xe000...0xfffd).contains($0.value) || (0x10000...0x10ffff).contains($0.value) }) else {
            throw KLCloudError("Android cloud save preference is not valid text.")
        }
        let item: Node
        if let existing = try entry(key) { item = existing }
        else { item = Node("string", ["name": key]); root!.children.append(item) }
        item.text = value
    }
    var data: Data { Data(("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + root!.xml()).utf8) }
}

struct KLCloudPreferences {
    private struct Source {
        let original: Data?
        let document: KLPreferenceXML
    }
    private let paths: KLCloudPaths
    private var sources: [URL: Source] = [:]
    private var bindings: [(KLCloudConfiguration.Preference, URL)] = []
    init(paths: KLCloudPaths) throws {
        self.paths = paths
        let mappings = paths.configuration.preferences ?? []
        guard mappings.count <= 64 else { throw KLCloudError("Too many Android preference save mappings.") }
        var names = Set<String>(), keys = Set<String>()
        for mapping in mappings {
            let (root, rest) = KLCloudPaths.split(mapping.cloudPath)
            guard ["", "androidinternaldata", "androidexternaldata"].contains(root.lowercased()),
                  !mapping.key.isEmpty, mapping.key.utf8.count <= 255,
                  names.insert(mapping.cloudPath.lowercased()).inserted,
                  keys.insert(mapping.file + "\n" + mapping.key).inserted else { throw KLCloudError("Invalid Android preference save mapping.") }
            _ = try KLCloudPaths.parts(rest)
            let source = try KLCloudPaths.parts(mapping.file).reduce(paths.gameFiles) { $0.appendingPathComponent($1) }
            try paths.validate(source, under: paths.gameFiles)
            if sources[source] == nil {
                let original = FileManager.default.fileExists(atPath: source.path) ? try Data(contentsOf: source) : nil
                sources[source] = Source(original: original, document: try KLPreferenceXML(original))
            }
            bindings.append((mapping, source))
        }
        for (mapping, source) in bindings {
            let cache = try paths.location(mapping.cloudPath)
            if let value = try sources[source]!.document.value(mapping.key) {
                try FileManager.default.createDirectory(at: cache.deletingLastPathComponent(), withIntermediateDirectories: true)
                try value.write(to: cache, options: .atomic)
            } else if FileManager.default.fileExists(atPath: cache.path) { try FileManager.default.removeItem(at: cache) }
        }
    }
    func check() throws {
        for (url, source) in sources {
            try paths.validate(url, under: paths.gameFiles)
            let current = FileManager.default.fileExists(atPath: url.path) ? try Data(contentsOf: url) : nil
            guard current == source.original else { throw KLCloudError("Android save preferences changed during sync. Retry with the game closed.") }
        }
    }
    func apply(_ downloads: Set<String>, backup: URL) throws {
        try check()
        var changed = Set<URL>()
        for (mapping, source) in bindings where downloads.contains(mapping.cloudPath.lowercased()) {
            let data = try Data(contentsOf: paths.location(mapping.cloudPath))
            try sources[source]!.document.set(mapping.key, bytes: data)
            changed.insert(source)
        }
        for url in changed.sorted(by: { $0.path < $1.path }) {
            if let original = sources[url]!.original {
                let copy = backup.appendingPathComponent("preferences/" + UUID().uuidString)
                try FileManager.default.createDirectory(at: copy.deletingLastPathComponent(), withIntermediateDirectories: true)
                try original.write(to: copy, options: .atomic)
                try Data(url.path.utf8).write(to: copy.appendingPathExtension("path"), options: .atomic)
            }
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try sources[url]!.document.data.write(to: url, options: .atomic)
        }
    }
}
