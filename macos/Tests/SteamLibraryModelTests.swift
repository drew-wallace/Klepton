import Foundation

// Exercise the real UI model against the backend's JSON contract without
// authenticating, starting subprocesses, or accessing the user's Keychain.
@main
struct SteamLibraryModelTests {
    @MainActor
    static func main() throws {
        let model = InstallerModel(steamSessionOverride: ["steam_id": "76561198000000001", "refresh_token": "fixture"])
        // Diagnostics survive reopening and ignore incomplete installs.
        let diagnosticsRoot = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: diagnosticsRoot) }
        let originalCheckout = model.checkout
        for (name, date, complete) in [("older", 100.0, true), ("latest", 200.0, true), ("unfinished", 300.0, false)] {
            let job = diagnosticsRoot.appendingPathComponent("build/apk-installer/jobs/" + name)
            try FileManager.default.createDirectory(at: job, withIntermediateDirectories: true)
            try JSONSerialization.data(withJSONObject: ["display": name, "device": "fixture", "bundle": "test.fixture"])
                .write(to: job.appendingPathComponent("manifest.json"))
            if complete { try Data("{}".utf8).write(to: job.appendingPathComponent("result.json")) }
            try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: date)], ofItemAtPath: job.path)
        }
        model.checkout = diagnosticsRoot.path
        model.restoreDiagnosticsJob()
        precondition(model.diagnosticsApp == "latest" && model.diagnosticsJob.hasSuffix("/latest"))
        model.checkout = diagnosticsRoot.appendingPathComponent("no-installs").path
        model.restoreDiagnosticsJob()
        precondition(model.diagnosticsJob.isEmpty && model.diagnosticsApp.isEmpty)
        model.checkout = originalCheckout
        model.info = APKInfo(["package": "com.bigscreenvr.bigscreen", "kind": "unity", "entry": "libmain", "suggested_gles_version": "3.0"])
        model.profile = ""
        model.selectProfile()
        precondition(model.glesVersion == "3.0", "Measured graphics workaround must be suggested for the inspected APK")
        model.info = APKInfo(["package": "com.MightyCoconut.WalkaboutMiniGolf", "version_code": "59349",
            "kind": "unity", "entry": "libmain", "suggested_data_path": "obb"])
        model.profile = ""
        model.selectProfile()
        precondition(model.glesVersion.isEmpty, "A new APK must not inherit another game’s GLES workaround")
        precondition(model.dataPath == "obb", "Steam data defaults must apply even when the APK has no pinned compatibility profile")
        model.dataPath = "manual-custom-path"
        model.info = APKInfo(["package": "test.manual", "kind": "unity", "entry": "libmain"])
        model.selectProfile()
        precondition(model.dataPath.isEmpty, "A different manual APK must not inherit Steam's data path")
        model.info = APKInfo(["package": "com.LuckyVR.PokerStarsVR", "kind": "unity", "entry": "libmain",
            "suggested_data_path": "Android/obb/com.LuckyVR.PokerStarsVR",
            "catalog_data": ["files": ["main.42.com.LuckyVR.PokerStarsVR.obb", "required.bundle"]],
            "data_requirements": ["": ["required": true, "files": ["main.42.com.LuckyVR.PokerStarsVR.obb"],
                "description": "1 catalog-referenced bundles"]]])
        model.selectProfile()
        precondition(model.dataDestination == "Android/obb/com.LuckyVR.PokerStarsVR")
        precondition(model.info?.dataInclude.count == 2 && model.requiresData)
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        func game(_ local: [String: Any]? = nil, revision: String = "first", remoteKnown: Bool = true,
                  build: String = "100", vr: Bool = true, android: Bool = true) throws -> SteamLibraryGame {
            let branch: [String: Any] = ["name": "public", "build_id": build, "updated_at": NSNull(),
                "locked": false, "available": android, "revision": revision, "remote_known": remoteKnown,
                "downloaded": local.map { ["0": $0] } ?? [:]]
            let value: [String: Any] = ["app_id": "10", "name": "Example VR", "vr": vr, "android_available": android,
                "branches": [branch], "launches": [["id": "0", "name": "Game", "executable": "game.apk"],
                    ["id": "1", "name": "Alternative", "executable": "alternative.apk"]]]
            return try decoder.decode(SteamLibraryGame.self, from: JSONSerialization.data(withJSONObject: value))
        }
        model.libraryGames = [try game(), try game(vr: false), try game(android: false)]
        precondition(!model.libraryVROnly, "VR filtering must default to off")
        precondition(model.filteredLibrary.count == 2, "Default library must show all Android games")
        model.libraryVROnly = true
        precondition(model.filteredLibrary.count == 1, "VR toggle must limit the Android library to VR games")
        model.libraryVROnly = false
        precondition(model.filteredLibrary.count == 2, "Disabling VR must still exclude desktop-only games")
        model.librarySearch = "EXAMPLE"
        precondition(model.filteredLibrary.count == 2, "Search must ignore case")
        model.librarySearch = "not present"
        precondition(model.filteredLibrary.isEmpty)

        model.libraryVROnly = true

        model.libraryGames = [try game()]
        model.libraryGameID = "10"
        model.selectLibraryGame()
        precondition(model.libraryBranch == "public" && model.libraryLaunch == "0")
        precondition(model.downloadAction == "Download" && model.canDownloadGame)
        model.source = "meta"
        model.updateSteamLibrary([try game(remoteKnown: false)], afterDownload: true)
        model.source = "steam"
        precondition(model.canDownloadGame, "A partial download catalog must preserve other games' completed version checks across tabs")
        model.updateSteamLibrary([try game(revision: "changed", remoteKnown: false)], afterDownload: true)
        precondition(!model.canDownloadGame, "Changed remote content must not inherit a previous version check")
        model.libraryGames = [try game()]
        model.updateSteamLibrary([try game(remoteKnown: false)])
        precondition(!model.canDownloadGame, "An offline catalog must not inherit freshness from a failed refresh")
        model.libraryGames = [try game()]
        model.libraryBranch = "missing"; model.libraryLaunch = "missing"
        model.updateSteamLibrary([try game()])
        precondition(model.libraryBranch == "public" && model.libraryLaunch == "0" && model.canDownloadGame)
        let local: [String: Any] = ["build_id": "100", "downloaded_at": "2026-10-27T12:00:00+00:00", "revision": "first", "valid": true]
        model.libraryGames = [try game(local)]
        precondition(model.downloadAction == "Use Downloaded")
        model.libraryGames = [try game(local, build: "200")]
        precondition(model.downloadAction == "Use Downloaded", "Desktop-only build changes must not trigger Update")
        model.libraryGames = [try game(local, revision: "second")]
        precondition(model.downloadAction == "Update" && model.canDownloadGame)
        model.libraryGames = [try game(local, revision: "second", remoteKnown: false)]
        precondition(model.downloadAction == "Use Downloaded" && model.canDownloadGame, "Offline cached games must remain usable")
        var damaged = local
        damaged["valid"] = false
        model.libraryGames = [try game(damaged)]
        precondition(model.downloadAction == "Repair" && model.canDownloadGame)
        model.libraryGames = [try game(damaged, remoteKnown: false)]
        precondition(!model.canDownloadGame, "Repair requires known remote content")
        model.libraryGames = [try game(local)]
        model.libraryLaunch = "1"
        precondition(model.downloadAction == "Download", "Each Android APK selection needs its own receipt")
        model.busy = true
        precondition(!model.canDownloadGame)
        model.librarySearch = "not present"
        model.reconcileLibrarySelection()
        precondition(model.libraryGameID.isEmpty, "Filtering must not leave a hidden game selected")
        // Meta JSON uses a single build/receipt rather than Steam's branches.
        func metaGame(_ build: String = "100", remote: Bool = true, valid: Bool = true, downloaded: Bool = true) throws -> MetaLibraryGame {
            var fixture: [String: Any] = ["app_id": "2370815932930055", "name": "Example Quest Game", "owned": true,
                "remote_known": remote, "latest": ["build_id": build, "version": "1.0", "version_code": 12]]
            if downloaded { fixture["downloaded"] = ["build_id": "100", "version": "1.0", "version_code": 12,
                "downloaded_at": "2026-11-08T12:00:00Z", "valid": valid] }
            let decoder = JSONDecoder(); decoder.keyDecodingStrategy = .convertFromSnakeCase
            return try decoder.decode(MetaLibraryGame.self, from: JSONSerialization.data(withJSONObject: fixture))
        }
        model.source = "meta"; model.metaGameID = "2370815932930055"
        model.metaGames = [try metaGame(downloaded: false)]
        precondition(model.metaDownloadAction == "Download")
        model.metaGames = [try metaGame()]
        precondition(model.metaDownloadAction == "Use Downloaded")
        model.metaGames = [try metaGame("200")]
        precondition(model.metaDownloadAction == "Update")
        model.metaGames = [try metaGame("200", remote: false)]
        precondition(model.metaDownloadAction == "Use Downloaded", "Stale Meta metadata cannot claim an update")
        model.metaGames = [try metaGame(valid: false)]
        precondition(model.metaDownloadAction == "Repair")
        model.info = APKInfo(["package": "example.test"]); model.filename = "old"
        model.clearMetaSelection()
        precondition(model.info == nil && model.filename.isEmpty)
        precondition(model.metaGameID == "2370815932930055", "Using a download must retain the selected Meta game")
        model.metaSearch = "hidden selection"
        model.reconcileMetaSelection()
        precondition(model.metaGameID.isEmpty, "Searching Meta must not retain a hidden selection")
        print("Steam library model checks passed (filtering, JSON decoding, download/update/repair, offline reuse, APK isolation).")
    }
}
