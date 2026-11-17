import Foundation
import CryptoKit
import Darwin

@MainActor protocol KLCloudTransport: AnyObject {
    var account: UInt64 { get }
    func call(_ method: Int32, _ body: Data) async throws -> Data
    func download(_ response: Data, file: KLCloudFile) async throws -> Data
    func upload(_ response: Data, bytes: Data, check: () throws -> Void) async throws
    func cache(_ name: String, bytes: Data) async throws
}

struct KLCloudConfiguration: Codable {
    struct Rule: Codable {
        var root: String
        var path: String
        var pattern: String
        var recursive: Bool
        var platforms: [String]
    }
    struct Override: Codable {
        var root: String
        var useInstead: String
        var addPath: String
        var replacePath: Bool
    }
    struct Preference: Codable {
        var file: String
        var key: String
        var cloudPath: String
    }
    var enabled: Bool = true
    var rules: [Rule] = []
    var overrides: [Override] = []
    var preferences: [Preference]?
}

struct KLCloudPaths {
    var documents: URL
    var steam: URL
    var account: UInt64
    var app: UInt32
    var configuration: KLCloudConfiguration
    var package: String
    init(documents: URL, steam: URL, account: UInt64, app: UInt32, configuration: KLCloudConfiguration, package: String = "") {
        self.documents = Self.canonical(documents); self.steam = Self.canonical(steam)
        self.account = account; self.app = app; self.configuration = configuration; self.package = package
    }
    static func canonical(_ url: URL) -> URL {
        var ancestor = url; var missing: [String] = []
        while !FileManager.default.fileExists(atPath: ancestor.path), ancestor.path != "/" {
            missing.append(ancestor.lastPathComponent); ancestor.deleteLastPathComponent()
        }
        return missing.reversed().reduce(ancestor.resolvingSymlinksInPath()) { $0.appendingPathComponent($1) }
    }
    var remote: URL { steam.appendingPathComponent("userdata/\(account & 0xffff_ffff)/\(app)/remote") }
    var gameFiles: URL { documents.appendingPathComponent("android-files") }
    static func parts(_ path: String, empty: Bool = false) throws -> [String] {
        let name = path.replacingOccurrences(of: "\\", with: "/")
        let pieces = name.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        if empty && (name.isEmpty || name == ".") { return [] }
        guard !name.isEmpty, pieces.count <= 32, pieces.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." && !$0.contains(":") && $0.utf8.count <= 255 }),
              !name.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else { throw KLCloudError("Unsafe Steam Cloud save path.") }
        return pieces
    }
    static func split(_ path: String) -> (String, String) {
        guard path.hasPrefix("%"), let end = path.dropFirst().firstIndex(of: "%") else { return ("", path) }
        return (String(path[path.index(after: path.startIndex)..<end]), String(path[path.index(after: end)...]).trimmingCharacters(in: CharacterSet(charactersIn: "/\\")))
    }
    func expanded(_ path: String) -> String {
        path.replacingOccurrences(of: "{64BitSteamID}", with: String(account))
            .replacingOccurrences(of: "{Steam3AccountID}", with: String(account & 0xffff_ffff))
    }
    func folder(_ root: String) throws -> URL {
        switch root.lowercased() {
        case "": return remote
        case "androidinternaldata", "androidexternaldata": return gameFiles
        default: throw KLCloudError("This game's Steam Cloud folder (\(root)) has no Android mapping. Launch paused.")
        }
    }
    func location(_ path: String) throws -> URL {
        if (configuration.preferences ?? []).contains(where: { $0.cloudPath.caseInsensitiveCompare(path) == .orderedSame }) {
            let digest = Insecure.SHA1.hash(data: Data(path.lowercased().utf8)).map { String(format: "%02x", $0) }.joined()
            let target = remote.deletingLastPathComponent().appendingPathComponent("klepton-preferences/" + digest)
            try validate(target, under: steam)
            return target
        }
        var (root, rest) = Self.split(path)
        if let override = configuration.overrides.first(where: { $0.root.caseInsensitiveCompare(root) == .orderedSame }) {
            root = override.useInstead.isEmpty ? root : override.useInstead
            if override.replacePath {
                // Replace only the rule's directory; retain the file's relative suffix.
                guard let rule = configuration.rules.first(where: {
                    $0.root.caseInsensitiveCompare(Self.split(path).0) == .orderedSame &&
                    (expanded($0.path) == "." || expanded($0.path).isEmpty || rest.lowercased().hasPrefix(expanded($0.path).trimmingCharacters(in: CharacterSet(charactersIn: "/")).lowercased() + "/"))
                }) else { throw KLCloudError("Steam Cloud root override does not match a save rule.") }
                let prefix = expanded(rule.path)
                if prefix != "." && !prefix.isEmpty { rest = String(rest.dropFirst(prefix.count + 1)) }
            }
            if !override.addPath.isEmpty && override.addPath != "." { rest = override.addPath + "/" + rest }
        }
        let base = try folder(root)
        var pieces = try Self.parts(expanded(rest))
        // AndroidExternalData is /data/media/0. Context paths in Klepton use
        // /sdcard/files rather than /sdcard/Android/data/<this package>/files.
        if !package.isEmpty, root.lowercased() == "androidexternaldata", pieces.count > 3,
           pieces[0].caseInsensitiveCompare("Android") == .orderedSame,
           pieces[1].caseInsensitiveCompare("data") == .orderedSame,
           pieces[2].caseInsensitiveCompare(package) == .orderedSame {
            pieces.removeFirst(3)
        } else if !package.isEmpty, root.lowercased() == "androidinternaldata", pieces.first?.caseInsensitiveCompare(package) == .orderedSame {
            pieces.removeFirst()
        }
        let url = try pieces.reduce(base) { parent, part in
            let names = (try? FileManager.default.contentsOfDirectory(atPath: parent.path)) ?? []
            let matches = names.filter { $0.caseInsensitiveCompare(part) == .orderedSame }
            guard matches.count <= 1 else { throw KLCloudError("Ambiguous Steam save filename.") }
            return parent.appendingPathComponent(matches.first ?? part)
        }
        try validate(url, under: base)
        return url
    }
    func isPreference(_ path: String) -> Bool {
        (configuration.preferences ?? []).contains { $0.cloudPath.caseInsensitiveCompare(path) == .orderedSame }
    }
    func validate(_ url: URL, under base: URL, directory: Bool = false) throws {
        let target = Self.canonical(url).path, root = Self.canonical(base).path
        guard target.hasPrefix(root + "/") || (directory && target == root) else { throw KLCloudError("Unsafe cloud save destination.") }
        // Reject every symbolic-link component, including dangling links and
        // ancestors of yet-to-be-created files. No lexical-only containment.
        let boundary: String
        let documentsRoot = Self.canonical(documents).path, steamRoot = Self.canonical(steam).path
        if root == documentsRoot || root.hasPrefix(documentsRoot + "/") { boundary = documentsRoot }
        else if root == steamRoot || root.hasPrefix(steamRoot + "/") { boundary = steamRoot }
        else { throw KLCloudError("Steam save folder is outside this app's storage.") }
        var cursor = url
        while true {
            if let attrs = try? FileManager.default.attributesOfItem(atPath: cursor.path), attrs[.type] as? FileAttributeType == .typeSymbolicLink {
                throw KLCloudError("A Steam save path contains a symbolic link.")
            }
            if Self.canonical(cursor).path == boundary { break }
            guard cursor.path != "/" else { throw KLCloudError("Unsafe Steam save folder.") }
            cursor.deleteLastPathComponent()
        }
        if !directory, let attrs = try? FileManager.default.attributesOfItem(atPath: url.path), attrs[.type] as? FileAttributeType != .typeRegular {
            throw KLCloudError("A Steam save path is not a regular file.")
        }
    }
    func localFiles() throws -> [String: URL] {
        let fm = FileManager.default
        var result: [String: URL] = [:]; var count = 0
        func scan(_ folder: URL, prefix: String, pattern: String, recursive: Bool, depth: Int = 0) throws {
            guard depth <= 32 else { throw KLCloudError("Steam save folders are too deeply nested.") }
            // Check the directory before enumerating its children.
            try validate(folder, under: folder, directory: true)
            guard fm.fileExists(atPath: folder.path) else { return }
            for url in try fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey]) {
                count += 1
                guard count <= 100_000 else { throw KLCloudError("Too many local save files.") }
                let type = try fm.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType
                if type == .typeDirectory { try validate(url, under: folder, directory: true) }
                else { try validate(url, under: folder) }
                let path = prefix + url.lastPathComponent
                if url.lastPathComponent.lowercased() == "steam_autocloud.vdf" { continue }
                if type == .typeDirectory {
                    if recursive { try scan(url, prefix: path + "/", pattern: pattern, recursive: true, depth: depth + 1) }
                } else if type == .typeRegular && fnmatch(pattern, url.lastPathComponent, 0) == 0 {
                    if let old = result[path.lowercased()], old != url { throw KLCloudError("Colliding local Steam save paths.") }
                    result[path.lowercased()] = url
                }
            }
        }
        try scan(remote, prefix: "", pattern: "*", recursive: true)
        for rule in configuration.rules where rule.platforms.isEmpty || rule.platforms.contains("android") {
            let path = rule.path == "." || rule.path.isEmpty ? "" : expanded(rule.path).trimmingCharacters(in: CharacterSet(charactersIn: "/")) + "/"
            let prefix = "%\(rule.root)%" + path
            let directory = try location(prefix + ".klepton-check").deletingLastPathComponent()
            try scan(directory, prefix: prefix, pattern: rule.pattern.isEmpty ? "*" : rule.pattern, recursive: rule.recursive)
        }
        for preference in configuration.preferences ?? [] {
            let target = try location(preference.cloudPath)
            if fm.fileExists(atPath: target.path) {
                guard result[preference.cloudPath.lowercased()] == nil else { throw KLCloudError("Duplicate Android preference save mapping.") }
                result[preference.cloudPath.lowercased()] = target
            }
        }
        return result
    }
    func uploadName(_ key: String, url: URL) throws -> String {
        let local = Self.canonical(url).path
        if let preference = (configuration.preferences ?? []).first(where: { $0.cloudPath.lowercased() == key }) {
            return preference.cloudPath
        }
        let api = Self.canonical(remote).path
        if KLCloudPaths.split(key).0.isEmpty, local.hasPrefix(api + "/") {
            return String(local.dropFirst(api.count + 1))
        }
        for rule in configuration.rules where rule.platforms.isEmpty || rule.platforms.contains("android") {
            let path = rule.path == "." || rule.path.isEmpty ? "" : expanded(rule.path).trimmingCharacters(in: CharacterSet(charactersIn: "/")) + "/"
            let prefix = "%\(rule.root)%" + path
            let directory = Self.canonical(try location(prefix + ".klepton-check").deletingLastPathComponent()).path
            if local.hasPrefix(directory + "/") {
                let name = prefix + local.dropFirst(directory.count + 1)
                if name.lowercased() == key { return name }
            }
        }
        throw KLCloudError("A local Steam save has no matching upload mapping.")
    }
}

@MainActor final class KLCloudSync {
    struct Permissions: Codable, Equatable {
        var upload = false
        var download = false
        var enabled: Bool { upload || download }
    }
    struct Record: Codable {
        var permissions = Permissions()
        var baseline: [String: String] = [:]
        var pendingUpload = false
    }
    let transport: any KLCloudTransport
    let paths: KLCloudPaths
    let support: URL
    private(set) var busy = false
    var progress: (String) -> Void = { _ in }
    init(transport: any KLCloudTransport, paths: KLCloudPaths, support: URL) {
        self.transport = transport; self.paths = paths; self.support = support
    }
    private var ownerURL: URL { support.appendingPathComponent("LocalOwner-\(paths.app).json") }
    private func localOwner() throws -> UInt64? {
        guard FileManager.default.fileExists(atPath: ownerURL.path) else { return nil }
        return try JSONDecoder().decode(UInt64.self, from: Data(contentsOf: ownerURL))
    }
    private func rememberOwner() throws {
        guard try localOwner() == nil else { return }
        try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        try JSONEncoder().encode(paths.account).write(to: ownerURL, options: .atomic)
    }
    var recordURL: URL { support.appendingPathComponent("\(paths.account)/\(paths.app).json") }
    func record() throws -> Record {
        if !FileManager.default.fileExists(atPath: recordURL.path) { return Record() }
        return try JSONDecoder().decode(Record.self, from: Data(contentsOf: recordURL))
    }
    func store(_ record: Record) throws {
        try FileManager.default.createDirectory(at: recordURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder().encode(record).write(to: recordURL, options: .atomic)
    }
    func setPermissions(_ permissions: Permissions) throws {
        guard !busy else { throw KLCloudError("Wait for Steam save synchronization to finish.") }
        var saved = try record(); saved.permissions = permissions; try store(saved)
    }
    private func check() throws {
        try Task.checkCancellation()
        guard transport.account == paths.account, paths.account != 0 else { throw KLCloudError("Steam disconnected or changed accounts. Retry save sync.") }
    }
    private func direction(_ upload: Bool) throws {
        try check()
        let permissions = try record().permissions
        guard upload ? permissions.upload : permissions.download else { throw KLCloudError(upload ? "Save uploads are off." : "Save downloads are off.") }
    }
    private func call(_ method: Int32, _ body: Data) async throws -> Data {
        try check(); let reply = try await transport.call(method, body); try check(); return reply
    }
    private func listing() async throws -> KLCloudListing {
        try KLCloudListing.decode(await call(1, KLCloudProto.number(1, UInt64(paths.app)) + KLCloudProto.number(2, 0)))
    }
    static func hash(_ data: Data) -> String { Insecure.SHA1.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    private func bytes(_ url: URL) throws -> Data {
        let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        guard size >= 0, size <= KLCloudHTTP.limit else { throw KLCloudError("A local Steam save is too large.") }
        return try Data(contentsOf: url)
    }
    private func download(_ file: KLCloudFile) async throws -> Data {
        try direction(false)
        let reply = try await call(2, KLCloudProto.number(1, UInt64(paths.app)) + KLCloudProto.string(2, file.path))
        let bytes = try await transport.download(reply, file: file)
        try direction(false)
        guard UInt64(bytes.count) == file.size, Data(Insecure.SHA1.hash(data: bytes)) == file.sha else { throw KLCloudError("Steam save checksum mismatch.") }
        return bytes
    }
    /// Run with no guest writers. A failed sync never grants launch permission.
    /// Use cloud resolves conflicts explicitly and never sends an upload.
    func sync(preferCloud: Bool = false, launching: Bool = false, uploadOnly: Bool = false) async throws {
        guard !busy else { throw KLCloudError("Steam saves are already syncing.") }
        busy = true; defer { busy = false }
        var saved = try record()
        var allowed = saved.permissions
        if uploadOnly { allowed.download = false }
        guard allowed.enabled else {
            if launching { try check(); try rememberOwner() }
            progress("Save uploads and downloads are off"); return
        }
        try check()
        if !paths.configuration.enabled { progress("This game does not use Steam Cloud"); return }
        if preferCloud && !allowed.download { throw KLCloudError("Turn on Download saves to use cloud saves.") }
        progress("Checking Steam Cloud…")
        let preferences = try KLCloudPreferences(paths: paths)
        let remote = try await listing()
        var locals = try paths.localFiles()
        // Auto-Cloud metadata does not describe files written via the SDK API.
        // Inspect the actual listing/local API cache before rejecting a mapping.
        let applicable = remote.files.filter(\.supportsAndroid)
        let hasAPISaves = applicable.contains { KLCloudPaths.split($0.path).0.isEmpty }
            || locals.keys.contains { KLCloudPaths.split($0).0.isEmpty }
        if !paths.configuration.rules.isEmpty,
           !paths.configuration.rules.contains(where: { $0.platforms.isEmpty || $0.platforms.contains("android") }),
           paths.configuration.overrides.isEmpty, (paths.configuration.preferences ?? []).isEmpty, !hasAPISaves {
            throw KLCloudError("This game's Steam Cloud save rules do not include Android. Launch paused until a compatible save mapping is available.")
        }
        if allowed.upload, let owner = try localOwner(), owner != paths.account,
           (locals.values.contains(where: { KLCloudPaths.canonical($0).path.hasPrefix(paths.gameFiles.path + "/") }) ||
            (paths.configuration.preferences ?? []).contains(where: { locals[$0.cloudPath.lowercased()] != nil })) {
            throw KLCloudError("Local Android saves belong to another Steam account. Keep uploads off for this account.")
        }
        var cloud: [String: KLCloudFile] = [:]
        for file in applicable {
            let key = file.path.lowercased(); let target = try paths.location(file.path)
            if let alias = locals.first(where: { $0.value.standardizedFileURL == target.standardizedFileURL }), alias.key != key {
                throw KLCloudError("Multiple Steam Cloud paths map to one save.")
            }
            cloud[key] = file
            if FileManager.default.fileExists(atPath: target.path) { locals[key] = target }
        }
        var hashes: [String: String] = [:]
        for (key, url) in locals { hashes[key] = Self.hash(try bytes(url)) }
        var downloads: [KLCloudFile] = []; var uploads: [String] = []
        for key in Set(locals.keys).union(cloud.keys).sorted() {
            let local = hashes[key], file = cloud[key], old = saved.baseline[key]
            let remoteHash = file.map { $0.sha.map { String(format: "%02x", $0) }.joined() }
            if local == remoteHash { continue }
            if let file, local == nil {
                if allowed.download { downloads.append(file) }
            } else if file == nil {
                // A previously synced file deleted remotely is never resurrected.
                if allowed.upload, old == nil, local != nil,
                   !remote.files.contains(where: { $0.path.lowercased() == key }) { uploads.append(key) }
            } else if preferCloud {
                if allowed.download, let file { downloads.append(file) }
            } else if let old, old == remoteHash {
                if allowed.upload { uploads.append(key) }
            } else if let old, old == local {
                if allowed.download, let file { downloads.append(file) }
            } else {
                // First use and concurrent edits require a deliberate choice.
                throw KLCloudError("Local and Steam Cloud saves conflict. Turn on Download saves and choose Use cloud saves.")
            }
        }
        // Keep staged downloads out of the active save tree until all are verified.
        let staging = support.appendingPathComponent("Staging/" + UUID().uuidString)
        let backup = support.appendingPathComponent("Backups/\(paths.account)/\(paths.app)/" + UUID().uuidString)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: staging) }
        for (index, file) in downloads.enumerated() {
            progress("Downloading saves (\(index + 1)/\(downloads.count))…")
            let data = try await download(file)
            try data.write(to: staging.appendingPathComponent(String(index)), options: .atomic)
        }
        // Detect remote/local edits during network waits, before replacing anything.
        guard try await listing() == remote else { throw KLCloudError("Steam Cloud changed on another device. Retry sync.") }
        try preferences.check()
        for (key, url) in locals where hashes[key] != Self.hash(try bytes(url)) { throw KLCloudError("Local saves changed during sync. Retry with the game closed.") }
        for (index, file) in downloads.enumerated() {
            try direction(false)
            let target = try paths.location(file.path)
            if FileManager.default.fileExists(atPath: target.path) {
                let copy = backup.appendingPathComponent("local/" + String(index))
                try FileManager.default.createDirectory(at: copy.deletingLastPathComponent(), withIntermediateDirectories: true)
                try FileManager.default.copyItem(at: target, to: copy)
                try Data(file.path.utf8).write(to: copy.appendingPathExtension("path"))
            }
            let data = try Data(contentsOf: staging.appendingPathComponent(String(index)))
            try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: target, options: .atomic)
            if KLCloudPaths.split(file.path).0.isEmpty && !paths.isPreference(file.path) {
                try await transport.cache(file.path, bytes: data)
                try direction(false)
            }
            try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: Double(file.time))], ofItemAtPath: target.path)
            locals[file.path.lowercased()] = target
        }
        if !preferCloud, !uploads.isEmpty {
            // Snapshot local bytes before opening a server batch. Download-only
            // mode never enters here; upload-only never downloads backup bytes.
            var payloads: [String: Data] = [:]
            var names: [String: String] = [:]
            for key in uploads {
                guard let url = locals[key] else { throw KLCloudError("Missing local Steam save.") }
                payloads[key] = try bytes(url)
                names[key] = try cloud[key]?.path ?? paths.uploadName(key, url: url)
                if allowed.download, let file = cloud[key] {
                    let data = try await download(file)
                    let copy = backup.appendingPathComponent("cloud/" + String(payloads.count))
                    try FileManager.default.createDirectory(at: copy.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try data.write(to: copy); try Data(file.path.utf8).write(to: copy.appendingPathExtension("path"))
                }
            }
            guard try await listing() == remote else { throw KLCloudError("Steam Cloud changed before upload. Retry sync.") }
            try direction(true)
            var begin = KLCloudProto.number(1, UInt64(paths.app)) + KLCloudProto.string(2, "Klepton on Vision Pro")
            for key in uploads { begin += KLCloudProto.string(3, names[key]!) }
            let batch = try KLCloudProto.parse(await call(3, begin)).value(1)
            guard batch != 0 else { throw KLCloudError("Steam refused the save upload batch.") }
            func complete(_ success: Bool) async throws {
                _ = try await call(6, KLCloudProto.number(1, UInt64(paths.app)) + KLCloudProto.number(2, batch) + KLCloudProto.number(3, success ? 1 : 2))
            }
            do {
                for (index, key) in uploads.enumerated() {
                    try direction(true)
                    try preferences.check()
                    progress("Uploading saves (\(index + 1)/\(uploads.count))…")
                    let data = payloads[key]!, name = names[key]!
                    guard let url = locals[key], Self.hash(try bytes(url)) == Self.hash(data) else { throw KLCloudError("Local save changed before upload.") }
                    let time = (try url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate)?.timeIntervalSince1970 ?? 0
                    let sha = Data(Insecure.SHA1.hash(data: data))
                    var request = KLCloudProto.number(1, UInt64(paths.app))
                    request += KLCloudProto.number(2, UInt64(data.count)) + KLCloudProto.number(3, UInt64(data.count))
                    request += KLCloudProto.bytes(4, sha) + KLCloudProto.number(5, UInt64(max(0, time)))
                    let androidRoot = KLCloudPaths.split(name).0.lowercased().hasPrefix("android") || paths.isPreference(name)
                    let platforms = cloud[key]?.platforms ?? (androidRoot ? 32 : 0xffff_ffff)
                    request += KLCloudProto.string(6, name) + KLCloudProto.number(7, platforms) + KLCloudProto.number(13, batch)
                    let reply: Data
                    var publicHTTP = false
                    do { reply = try await call(4, request) }
                    catch let error as KLCloudRPCError where error.method == 4 && error.result == 42 {
                        // Retry NoMatch through Valve's public upload API in the same
                        // account/app batch, retaining the exact platform mask.
                        publicHTTP = true
                        var http = KLCloudProto.number(1, UInt64(paths.app)) + KLCloudProto.number(2, UInt64(data.count))
                        http += KLCloudProto.string(3, name) + KLCloudProto.string(4, Self.hash(data))
                        http += KLCloudProto.number(5, 0) + KLCloudProto.number(9, batch)
                        if platforms == 0xffff_ffff { http += KLCloudProto.string(6, "All") }
                        else {
                            guard platforms != 0, platforms & ~UInt64(123) == 0 else { throw KLCloudError("Unsupported Steam Cloud platform flags.") }
                            for (mask, platform) in [(1, "Windows"), (2, "MacOS"), (8, "linux"), (16, "Switch"), (32, "Android"), (64, "iPhoneOS")] where platforms & UInt64(mask) != 0 {
                                http += KLCloudProto.string(6, platform)
                            }
                        }
                        reply = try KLCloudHTTP.uploadBlocks(await call(8, http), size: UInt64(data.count))
                    }
                    try await transport.upload(reply, bytes: data) { try self.direction(true) }
                    try direction(true)
                    var commit = KLCloudProto.number(1, 1) + KLCloudProto.number(2, UInt64(paths.app))
                    commit += publicHTTP ? KLCloudProto.string(3, Self.hash(data)) : KLCloudProto.bytes(3, sha)
                    commit += KLCloudProto.string(4, name)
                    guard try KLCloudProto.parse(await call(publicHTTP ? 9 : 5, commit)).value(1) == 1 else { throw KLCloudError("Steam did not commit the uploaded save.") }
                }
                try direction(true); try await complete(true)
            } catch { try? await complete(false); throw error }
        }
        let after = try await listing()
        var verified: [String: String] = [:]
        let transferred = Set(downloads.map { $0.path.lowercased() } + uploads)
        for file in after.files where file.supportsAndroid {
            let key = file.path.lowercased(), target = try paths.location(file.path)
            let hash = file.sha.map { String(format: "%02x", $0) }.joined()
            let equal = try FileManager.default.fileExists(atPath: target.path) && Self.hash(bytes(target)) == hash
            if transferred.contains(key), !equal { throw KLCloudError("Steam saves changed during verification. Retry sync.") }
            if equal { verified[key] = hash }
        }
        guard transferred.isSubset(of: Set(verified.keys)) else { throw KLCloudError("Steam did not confirm all synchronized saves.") }
        try preferences.apply(Set(downloads.map { $0.path.lowercased() }), backup: backup)
        saved.baseline.merge(verified) { _, new in new }
        saved.permissions = try record().permissions
        saved.pendingUpload = launching && saved.permissions.upload
        try check(); try store(saved); try rememberOwner()
        progress("Steam Cloud saves synced")
    }
}
