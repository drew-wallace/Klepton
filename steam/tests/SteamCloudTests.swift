import Foundation
import CryptoKit

@MainActor final class ScriptedCloud: KLCloudTransport {
    var account: UInt64 = 7
    var remote: [String: Data] = [:]
    var platforms: [String: UInt64] = [:]
    var generation: UInt64 = 1
    var methods: [Int32] = []
    var fetched = 0, sent = 0
    var corrupt = false, failCommit = false
    var clientUploadFailure: Int32?
    var afterDownload: (() -> Void)?
    var beforeCommit: (() -> Void)?
    var uploadBytes = Data(), uploadPath = ""
    func call(_ method: Int32, _ body: Data) async throws -> Data {
        methods.append(method)
        let fields = try KLCloudProto.parse(body)
        switch method {
        case 1:
            var data = KLCloudProto.number(1, generation)
            for (path, bytes) in remote.sorted(by: { $0.key < $1.key }) {
                var item = KLCloudProto.string(1, path) + KLCloudProto.bytes(2, Data(Insecure.SHA1.hash(data: bytes)))
                item += KLCloudProto.number(3, 1700000000) + KLCloudProto.number(4, UInt64(bytes.count))
                if let mask = platforms[path] { item += KLCloudProto.number(6, mask) }
                data += KLCloudProto.bytes(2, item)
            }
            return data
        case 2: return KLCloudProto.string(1, fields.text(2))
        case 3: return KLCloudProto.number(1, 99)
        case 4:
            if let result = clientUploadFailure { throw KLCloudRPCError(method: 4, result: result) }
            uploadPath = fields.text(6); platforms[uploadPath] = fields.value(7); return Data()
        case 8:
            uploadPath = fields.text(3)
            assert(fields.value(9) == 99 && fields.value(5) == 0)
            assert(fields.filter { $0.number == 6 }.map(\.string) == ["Android"])
            assert(fields.text(4).count == 40)
            platforms[uploadPath] = 32
            return KLCloudProto.string(3, "cloud.steampowered.test") + KLCloudProto.string(4, "/upload") + KLCloudProto.number(5, 1)
        case 5, 9:
            if failCommit { throw KLCloudError("fixture commit failure") }
            if method == 9 { assert(fields.text(3) == KLCloudSync.hash(uploadBytes) && fields.text(4) == uploadPath) }
            remote[uploadPath] = uploadBytes; generation += 1
            return KLCloudProto.number(1, 1)
        case 6: return Data()
        default: fatalError("unexpected method")
        }
    }
    func download(_ response: Data, file: KLCloudFile) async throws -> Data {
        fetched += 1
        afterDownload?()
        return corrupt ? Data("corrupt".utf8) : remote[file.path]!
    }
    var cached: [String: Data] = [:]
    func cache(_ name: String, bytes: Data) async throws { cached[name] = bytes }
    func upload(_ response: Data, bytes: Data, check: () throws -> Void) async throws {
        try check(); sent += 1; uploadBytes = bytes; beforeCommit?()
    }
}

@main struct SteamCloudTests {
    @MainActor static func main() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("klepton-cloud-tests-" + UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        func fixture(_ name: String, configuration: KLCloudConfiguration = .init()) -> (ScriptedCloud, KLCloudSync) {
            let transport = ScriptedCloud(), folder = root.appendingPathComponent(name)
            let paths = KLCloudPaths(documents: folder.appendingPathComponent("Documents"), steam: folder.appendingPathComponent("Steam"),
                                     account: 7, app: 42, configuration: configuration)
            let service = KLCloudSync(transport: transport, paths: paths, support: folder.appendingPathComponent("Services"))
            return (transport, service)
        }
        func write(_ bytes: String, _ url: URL) throws {
            try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(bytes.utf8).write(to: url)
        }
        func read(_ url: URL) throws -> String { try String(contentsOf: url, encoding: .utf8) }
        func expectFailure(_ operation: () async throws -> Void) async {
            do { try await operation(); fatalError("expected failure") } catch { }
        }
        // Defaults and both-off do no cloud service work.
        let (off, disabled) = fixture("off")
        assert(try! disabled.record().permissions == .init())
        try await disabled.sync(launching: true); assert(off.methods.isEmpty)
        let (noCloud, unsupported) = fixture("unsupported", configuration: .init(enabled: false))
        try unsupported.setPermissions(.init(upload: true, download: true))
        try await unsupported.sync(); assert(noCloud.methods.isEmpty)

        // Pistol Whip's published UFS rules are Windows-only. An empty cloud
        // must not hide an unsupported mapping and allow a misleading launch.
        let pistolRules = KLCloudConfiguration(rules: [
            .init(root: "WinAppDataLocalLow", path: "Cloudhead Games, Ltd_/Pistol Whip", pattern: "*.dat", recursive: false, platforms: ["windows"]),
            .init(root: "WinAppDataLocalLow", path: "Cloudhead Games LTD/Pistol Whip", pattern: "*.dat", recursive: false, platforms: ["windows"])])
        let (pistol, pistolSync) = fixture("pistolwhip", configuration: pistolRules)
        try await pistolSync.sync(launching: true); assert(pistol.methods.isEmpty)
        try pistolSync.setPermissions(.init(download: true))
        await expectFailure { try await pistolSync.sync(launching: true) }
        assert(pistol.methods.allSatisfy { $0 == 1 } && pistol.sent == 0 && pistol.fetched == 0)
        try pistolSync.setPermissions(.init(upload: true))
        await expectFailure { try await pistolSync.sync(launching: true) }
        assert(pistol.methods.allSatisfy { $0 == 1 } && pistol.sent == 0 && pistol.fetched == 0)

        // SDK/API files remain usable even when Auto-Cloud rules only list Windows.
        pistol.remote["api.sav"] = Data("api progress".utf8)
        try pistolSync.setPermissions(.init(download: true))
        try await pistolSync.sync()
        assert(pistol.fetched == 1 && pistol.cached["api.sav"] == Data("api progress".utf8))

        var androidPistolRules = pistolRules
        androidPistolRules.rules.append(.init(root: "AndroidExternalData", path: "Android/data/com.cloudheadgames.pistolwhip/files", pattern: "*.dat", recursive: false, platforms: ["android"]))
        let (pistolAndroid, pistolAndroidSync) = fixture("pistol-android", configuration: androidPistolRules)
        // Match the verified package alias used by Context.getExternalFilesDir.
        let scopedPaths = KLCloudPaths(documents: pistolAndroidSync.paths.documents, steam: pistolAndroidSync.paths.steam,
            account: 7, app: 42, configuration: androidPistolRules, package: "com.cloudheadgames.pistolwhip")
        let scopedSync = KLCloudSync(transport: pistolAndroid, paths: scopedPaths, support: pistolAndroidSync.support)
        let scopedName = "%AndroidExternalData%Android/data/com.cloudheadgames.pistolwhip/files/gsdata.dat"
        try write("android progress", scopedPaths.gameFiles.appendingPathComponent("files/gsdata.dat"))
        // A Windows-only cloud file must stay untouched and must not block Android saves.
        let desktopName = "%WinAppDataLocalLow%Cloudhead Games LTD/Pistol Whip/gsdata.dat"
        pistolAndroid.remote[desktopName] = Data("desktop progress".utf8); pistolAndroid.platforms[desktopName] = 1
        try scopedSync.setPermissions(.init(upload: true))
        try await scopedSync.sync()
        assert(pistolAndroid.platforms[scopedName] == 32)
        assert(pistolAndroid.remote[desktopName] == Data("desktop progress".utf8))
        assert(pistolAndroid.remote[scopedName] == Data("android progress".utf8))
        try FileManager.default.removeItem(at: scopedPaths.gameFiles.appendingPathComponent("files/gsdata.dat"))
        try scopedSync.setPermissions(.init(download: true))
        try await scopedSync.sync()
        assert(try! read(scopedPaths.gameFiles.appendingPathComponent("files/gsdata.dat")) == "android progress")

        // Android Pistol Whip stores gsdata.dat inside PlayerPrefs. Project only
        // that entry, then merge a restore without replacing local credentials.
        let preferencePath = "klepton-android/playerprefs/gsdata.dat"
        let preferenceRule = KLCloudConfiguration.Preference(file: "shared_prefs/pistol.xml", key: "gsdata.dat", cloudPath: preferencePath)
        let preferenceConfig = KLCloudConfiguration(preferences: [preferenceRule])
        let (prefCloud, prefSync) = fixture("preferences", configuration: preferenceConfig)
        prefCloud.clientUploadFailure = 42
        let prefURL = prefSync.paths.gameFiles.appendingPathComponent(preferenceRule.file)
        let originalPrefs = "<map><string name=\"STEAM_AUTH_KEY\">local-only &amp; secret</string><int name=\"device\" value=\"42\"/><string name=\"gsdata.dat\">%7B%22gun%22%3A1%7D</string></map>"
        try write(originalPrefs, prefURL)
        try prefSync.setPermissions(.init(upload: true))
        try await prefSync.sync()
        let prefName = preferencePath
        assert(prefCloud.remote == [prefName: Data("%7B%22gun%22%3A1%7D".utf8)])
        assert(prefCloud.platforms[prefName] == 32 && prefCloud.cached.isEmpty)
        assert(prefCloud.methods.contains(8) && prefCloud.methods.contains(9))
        assert(try! read(prefURL) == originalPrefs) // Upload never rewrites XML.
        prefCloud.remote[prefName] = Data("%7B%22gun%22%3A2%7D".utf8); prefCloud.generation += 1
        try prefSync.setPermissions(.init(download: true))
        try await prefSync.sync()
        let merged = try read(prefURL)
        assert(merged.contains("local-only &amp; secret") && merged.contains("value=\"42\""))
        assert(merged.contains("%7B%22gun%22%3A2%7D") && !merged.contains("%7B%22gun%22%3A1%7D"))
        // Concurrent preference edits block a restore rather than discarding it.
        prefCloud.remote[prefName] = Data("new progress".utf8); prefCloud.generation += 1
        prefCloud.afterDownload = { try! write("<map><string name=\"gsdata.dat\">concurrent</string></map>", prefURL) }
        await expectFailure { try await prefSync.sync() }
        assert(try! read(prefURL).contains("concurrent"))
        prefCloud.afterDownload = nil
        // Malformed XML fails before any upload.
        try write("<map><string name=\"gsdata.dat\">broken", prefURL)
        let sentBeforeBadXML = prefCloud.sent
        try prefSync.setPermissions(.init(upload: true))
        await expectFailure { try await prefSync.sync() }
        assert(prefCloud.sent == sentBeforeBadXML)
        let (deniedCloud, deniedSync) = fixture("upload-denied")
        deniedCloud.clientUploadFailure = 15
        try write("local progress", deniedSync.paths.remote.appendingPathComponent("save.dat"))
        try deniedSync.setPermissions(.init(upload: true))
        await expectFailure { try await deniedSync.sync() }
        assert(!deniedCloud.methods.contains(8) && deniedCloud.sent == 0)

        // Restore API saves, verify checksums, then record only equal files.
        let (steam, sync) = fixture("api")
        steam.remote["save.bin"] = Data("cloud progress".utf8)
        try sync.setPermissions(.init(upload: false, download: true))
        try await sync.sync(launching: true)
        let target = try sync.paths.location("save.bin")
        assert(try! read(target) == "cloud progress" && steam.fetched == 1 && steam.sent == 0)
        assert(try! sync.record().baseline["save.bin"] != nil)
        assert(steam.cached["save.bin"] == Data("cloud progress".utf8))
        try write("local progress", target)
        steam.methods = []; try await sync.sync()
        assert(try! read(target) == "local progress" && steam.sent == 0 && steam.fetched == 1)
        // Cloud and local both changed: block, then explicitly restore with backup.
        steam.remote["save.bin"] = Data("other device".utf8); steam.generation += 1
        await expectFailure { try await sync.sync() }
        assert(try! read(target) == "local progress")
        try await sync.sync(preferCloud: true)
        assert(try! read(target) == "other device" && steam.sent == 0)
        let backups = fm.enumerator(at: sync.support.appendingPathComponent("Backups"), includingPropertiesForKeys: nil)!
        assert(backups.compactMap { $0 as? URL }.contains { (try? read($0)) == "local progress" })

        // Upload-only sends no cloud bytes down, waits for commit and verification.
        try sync.setPermissions(.init(upload: true, download: false))
        try write("new progress", target)
        let fetched = steam.fetched
        try await sync.sync(launching: true)
        assert(steam.remote["save.bin"] == Data("new progress".utf8) && steam.fetched == fetched)
        assert(try! sync.record().pendingUpload)
        try write("after gameplay", target); steam.failCommit = true
        await expectFailure { try await sync.sync(uploadOnly: true) }
        assert(try! sync.record().pendingUpload)
        steam.failCommit = false; try await sync.sync(uploadOnly: true)
        assert(steam.remote["save.bin"] == Data("after gameplay".utf8))
        assert(try! !sync.record().pendingUpload)
        assert(steam.methods.contains(6))
        // Remote deletion must not resurrect an old local copy.
        steam.remote = [:]; steam.generation += 1; let sent = steam.sent
        try await sync.sync(); assert(steam.sent == sent)
        try write("brand new", sync.paths.remote.appendingPathComponent("new.bin"))
        try await sync.sync(); assert(steam.remote["new.bin"] == Data("brand new".utf8))

        // Bad downloads and account changes leave local saves unchanged.
        let (bad, badSync) = fixture("bad")
        bad.remote["save.bin"] = Data("remote".utf8)
        try badSync.setPermissions(.init(download: true))
        bad.corrupt = true
        await expectFailure { try await badSync.sync() }
        assert(!fm.fileExists(atPath: badSync.paths.remote.appendingPathComponent("save.bin").path))
        bad.corrupt = false; bad.afterDownload = { bad.account = 8 }
        await expectFailure { try await badSync.sync() }
        assert(!fm.fileExists(atPath: badSync.paths.remote.appendingPathComponent("save.bin").path))
        assert(!fm.fileExists(atPath: badSync.support.appendingPathComponent("8/42.json").path))

        // Permissions are rechecked before committing, not captured at task start.
        let (revoked, revokeSync) = fixture("revoked")
        try revokeSync.setPermissions(.init(upload: true))
        try write("local", revokeSync.paths.remote.appendingPathComponent("save.bin"))
        revoked.beforeCommit = {
            var record = try! revokeSync.record(); record.permissions.upload = false; try! revokeSync.store(record)
        }
        await expectFailure { try await revokeSync.sync() }
        assert(!revoked.methods.contains(5) && revoked.remote.isEmpty)

        // Publisher Android UFS rules and Windows-to-Android root overrides.
        let config = KLCloudConfiguration(rules: [.init(root: "WinAppDataLocalLow", path: "Company/Game", pattern: "*.sav", recursive: true, platforms: [])],
            overrides: [.init(root: "WinAppDataLocalLow", useInstead: "AndroidInternalData", addPath: "", replacePath: true)])
        let (android, androidSync) = fixture("android", configuration: config)
        try write("android progress", androidSync.paths.gameFiles.appendingPathComponent("Other.SAV"))
        try write("not a save", androidSync.paths.gameFiles.appendingPathComponent("textures.bin"))
        // Pattern matching follows Android's case-sensitive filesystem.
        try write("android save", androidSync.paths.gameFiles.appendingPathComponent("slot.sav"))
        let locals = try androidSync.paths.localFiles()
        assert(locals.count == 1 && locals["%winappdatalocallow%company/game/slot.sav"] != nil)
        try androidSync.setPermissions(.init(upload: true, download: true))
        try await androidSync.sync()
        assert(android.remote["%WinAppDataLocalLow%Company/Game/slot.sav"] == Data("android save".utf8))

        // External Android rules use /sdcard, including Klepton's Context alias.
        let external = KLCloudPaths(documents: androidSync.paths.documents, steam: androidSync.paths.steam,
            account: 7, app: 42, configuration: .init(), package: "com.example.game")
        assert(try! external.location("%AndroidExternalData%Android/data/com.example.game/files/save.bin") == external.gameFiles.appendingPathComponent("files/save.bin"))
        assert(try! external.location("%androidexternaldata%android/data/com.example.game/files/save.bin") == external.gameFiles.appendingPathComponent("files/save.bin"))
        assert(try! external.location("%AndroidExternalData%UE4Game/Game/Saved/save.sav") == external.gameFiles.appendingPathComponent("UE4Game/Game/Saved/save.sav"))
        let otherAccount = ScriptedCloud(); otherAccount.account = 8
        let otherPaths = KLCloudPaths(documents: androidSync.paths.documents, steam: androidSync.paths.steam,
            account: 8, app: 42, configuration: config)
        let otherSync = KLCloudSync(transport: otherAccount, paths: otherPaths, support: androidSync.support)
        assert(try! otherSync.record().permissions == .init())
        try otherSync.setPermissions(.init(upload: true))
        await expectFailure { try await otherSync.sync() }
        assert(otherAccount.sent == 0)

        // Reject traversal, unknown roots, collisions and symbolic links.
        for path in ["../outside", "/outside", "a//b", "a/./b", "a\\..\\b", "%WinSavedGames%save.bin"] {
            do { _ = try sync.paths.location(path); fatalError("unsafe path accepted") } catch { }
        }
        let outside = root.appendingPathComponent("outside")
        try fm.createDirectory(at: outside, withIntermediateDirectories: true)
        let link = sync.paths.remote.appendingPathComponent("link")
        try fm.createSymbolicLink(at: link, withDestinationURL: outside)
        do { _ = try sync.paths.location("link/save.bin"); fatalError("symlink accepted") } catch { }
        // Malformed protobuf and listing sizes are rejected, never treated as empty cloud.
        for bytes in [Data([0x80]), Data([0x0a, 0x7f]), Data([0x0f])] {
            do { _ = try KLCloudProto.parse(bytes); fatalError("bad protobuf accepted") } catch { }
        }
        print("Steam Cloud: defaults, directions, staged restore, backups, conflicts, upload commit, retry, account isolation, UFS and path safety passed")
    }
}
