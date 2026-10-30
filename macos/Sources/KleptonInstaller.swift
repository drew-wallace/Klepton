import SwiftUI
import AppKit
import UniformTypeIdentifiers
import Security
import CoreImage.CIFilterBuiltins

struct SteamDownloadInfo: Decodable {
    let buildId: String?
    let downloadedAt: String?
    let revision: String?
    let valid: Bool?
}

struct SteamLibraryBranch: Decodable, Identifiable {
    var id: String { name }
    let name: String
    let buildId: String
    let updatedAt: String?
    let locked: Bool
    let available: Bool
    let revision: String?
    let remoteKnown: Bool?
    let downloaded: [String: SteamDownloadInfo]?
}

struct SteamLibraryLaunch: Decodable, Identifiable {
    let id: String
    let name: String
    let executable: String
}

struct SteamLibraryGame: Decodable, Identifiable {
    var id: String { appId }
    let appId: String
    let name: String
    let vr: Bool
    let androidAvailable: Bool
    let branches: [SteamLibraryBranch]
    let launches: [SteamLibraryLaunch]
}

private enum SteamKeychain {
    static let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
        kSecAttrService as String: "dev.klepton.installer.steam-library", kSecAttrAccount as String: "active-session"]
    static func load() -> [String: String] {
        var lookup = query
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(lookup as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data,
              let session = try? JSONSerialization.jsonObject(with: data) as? [String: String] else { return [:] }
        return session
    }
    static func save(_ session: [String: String]) throws {
        let data = try JSONSerialization.data(withJSONObject: session)
        let status = SecItemUpdate(query as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        if status == errSecItemNotFound {
            var record = query
            record[kSecValueData as String] = data
            record[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            guard SecItemAdd(record as CFDictionary, nil) == errSecSuccess else {
                throw NSError(domain: "SteamKeychain", code: 1, userInfo: [NSLocalizedDescriptionKey: "Could not remember the Steam session in Keychain."])
            }
        } else if status != errSecSuccess {
            throw NSError(domain: "SteamKeychain", code: Int(status), userInfo: [NSLocalizedDescriptionKey: "Could not update the Steam session in Keychain."])
        }
    }
    static func remove() -> Bool {
        let status = SecItemDelete(query as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }
}

private func steamDate(_ value: String?) -> String {
    guard let value else { return "Unavailable" }
    let formatter = ISO8601DateFormatter()
    formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    let date = formatter.date(from: value) ?? ISO8601DateFormatter().date(from: value)
    return date?.formatted(date: .abbreviated, time: .shortened) ?? "Unavailable"
}

private func steamQR(_ challenge: String) -> NSImage? {
    guard !challenge.isEmpty else { return nil }
    let filter = CIFilter.qrCodeGenerator()
    filter.message = Data(challenge.utf8)
    guard let output = filter.outputImage?.transformed(by: CGAffineTransform(scaleX: 8, y: 8)),
          let cgImage = CIContext().createCGImage(output, from: output.extent) else { return nil }
    return NSImage(cgImage: cgImage, size: NSSize(width: 230, height: 230))
}

// Keep synchronous directory enumeration outside an asynchronous context.
private func selectedDataSize(_ urls: [URL]) -> Int64 {
    var total: Int64 = 0
    for url in urls {
        var directory: ObjCBool = false
        if FileManager.default.fileExists(atPath: url.path, isDirectory: &directory), directory.boolValue {
            if let enumerator = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
                for case let file as URL in enumerator {
                    if let values = try? file.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey]), values.isRegularFile == true {
                        total += Int64(values.fileSize ?? 0)
                    }
                }
            }
        } else if let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize {
            total += Int64(size)
        }
    }
    return total
}

struct Profile: Identifiable {
    let id: String
    let name: String
    let kind: String
    let entry: String
    let bundle: String
    let obb: String
    let dataPath: String
    init(_ d: [String: Any]) {
        id = d["id"] as? String ?? ""
        name = d["name"] as? String ?? id
        kind = d["kind"] as? String ?? ""
        entry = d["entry"] as? String ?? ""
        bundle = d["bundle"] as? String ?? ""
        obb = d["obb"] as? String ?? "obb"
        dataPath = d["data_path"] as? String ?? ""
    }
}

struct Choice: Identifiable {
    let id: String
    let name: String
}

struct SteamChannelInfo {
    var current = "Not installed"
    var remote = "Checking…"
    var currentBuild = ""
    var remoteBuild = ""
    var error = ""
    var compatible = true
    var remoteAvailable = false
    var isUpToDate: Bool {
        compatible && !currentBuild.isEmpty && currentBuild == remoteBuild && current == remote
    }
    init(_ d: [String: Any] = [:]) {
        if let installed = d["current"] as? [String: Any] {
            current = installed["version"] as? String ?? "Unknown"
            currentBuild = installed["build"] as? String ?? ""
            compatible = installed["compatible"] as? Bool ?? false
            if !compatible { error = installed["compatibility_error"] as? String ?? "Requires a login bridge update." }
        }
        if let latest = d["remote"] as? [String: Any] {
            remote = latest["version"] as? String ?? "Unknown"
            remoteBuild = latest["build"] as? String ?? ""
            remoteAvailable = true
        } else if !d.isEmpty { remote = "Unavailable" }
        if let failure = d["error"] as? String, !failure.isEmpty { error = failure }
    }
}

struct SetupCheck: Identifiable {
    var id: String { name }
    let name: String
    let ready: Bool
    let fix: String
    let required: Bool
}

struct APKInfo {
    let importID: String
    let name: String
    let package: String
    let version: String
    let architecture: String
    let kind: String
    let entry: String
    let libraries: [String]
    let reason: String
    let warning: String
    let steam: Bool
    let audited: [String]
    let graphics: String
    let suggestions: [String]
    let suggestedDataPath: String
    let bytes: Int64
    let requirements: [String: [String: Any]]
    let vulkanRequired: Bool
    let suggestedGLESVersion: String
    init(_ d: [String: Any]) {
        importID = d["import_id"] as? String ?? ""
        name = d["name"] as? String ?? "APK"
        package = d["package"] as? String ?? ""
        version = [d["version_name"] as? String, d["version_code"] as? String]
            .compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · ")
        architecture = d["architecture"] as? String ?? "ARM64"
        kind = d["kind"] as? String ?? ""
        entry = d["entry"] as? String ?? ""
        libraries = d["entry_libraries"] as? [String] ?? d["libraries"] as? [String] ?? []
        reason = d["reason"] as? String ?? ""
        warning = d["warning"] as? String ?? ""
        steam = d["steam"] as? Bool ?? false
        audited = d["steam_audited_profiles"] as? [String] ?? []
        graphics = d["graphics"] as? String ?? "Automatic"
        suggestions = d["suggested_data"] as? [String] ?? []
        suggestedDataPath = d["suggested_data_path"] as? String ?? ""
        bytes = d["transfer_bytes"] as? Int64 ?? 0
        requirements = d["data_requirements"] as? [String: [String: Any]] ?? [:]
        vulkanRequired = d["vulkan_required"] as? Bool ?? false
        suggestedGLESVersion = d["suggested_gles_version"] as? String ?? ""
    }
}

@MainActor
final class InstallerModel: ObservableObject {
    @Published var checkout: String
    @Published var info: APKInfo?
    @Published var filename = ""
    @Published var profiles: [Profile] = []
    @Published var devices: [Choice] = []
    @Published var teams: [Choice] = []
    @Published var checks: [SetupCheck] = []
    @Published var profile = ""
    @Published var steam = "auto"
    @Published var steamChannel = "stable" {
        didSet { UserDefaults.standard.set(steamChannel, forKey: "KleptonSteamChannel") }
    }
    @Published var steamVersions: [String: SteamChannelInfo] = [:]
    @Published var source = "steam"
    @Published var libraryGames: [SteamLibraryGame] = []
    @Published var librarySearch = ""
    @Published var libraryVROnly = true
    @Published var libraryAndroidOnly = true
    @Published var libraryGameID = ""
    @Published var libraryBranch = "public"
    @Published var libraryLaunch = ""
    @Published var steamAccount = ""
    @Published var showSteamLogin = false
    @Published var steamAuthKind = ""
    @Published var steamQRChallenge = ""
    @Published var unmappedSteamData: [String] = []
    @Published var steamDownloadFolder = ""
    private var steamSession: [String: String] = [:]
    private var inputPipe: Pipe?
    private var pendingInput: [String: Any]?
    private var loadedCachedLibrary = false
    private var attemptedLibraryRefresh = false
    private var expandedLibrary = false
    @Published var kind = ""
    @Published var entry = ""
    @Published var glesVersion = ""
    @Published var bundle = ""
    @Published var dataPath = ""
    @Published var device = ""
    @Published var team = ""
    @Published var data: [URL] = []
    @Published var dataBytes: Int64 = 0
    @Published var verbose = false
    @Published var busy = false
    @Published var cancelling = false
    @Published var message = "Sign in to your Steam library or choose a local APK."
    @Published var error = ""
    @Published var logs = ""
    @Published var logPath = ""
    @Published var appPath = ""
    @Published var started = Date()
    @Published var installed = false
    @Published var diagnosticsPath = ""
    @Published var diagnosticsJob = ""
    @Published var diagnosticsApp = ""
    @Published var recoveryJob = ""
    private var process: Process?
    private var gotResult = false
    private var operation = ""

    init(steamSessionOverride: [String: String]? = nil) {
        let remembered = UserDefaults.standard.string(forKey: "KleptonCheckout")
        let embedded = Bundle.main.object(forInfoDictionaryKey: "KleptonCheckout") as? String
        var inferred: String?
        var parent = Bundle.main.bundleURL
        for _ in 0..<8 {
            parent.deleteLastPathComponent()
            if FileManager.default.fileExists(atPath: parent.appendingPathComponent("tools/utility.py").path) {
                inferred = parent.path
                break
            }
        }
        checkout = [remembered, embedded, inferred].compactMap { $0 }.first {
            FileManager.default.fileExists(atPath: URL(fileURLWithPath: $0).appendingPathComponent("tools/utility.py").path)
        } ?? ""
        let channel = UserDefaults.standard.string(forKey: "KleptonSteamChannel") ?? "stable"
        steamChannel = ["stable", "beta"].contains(channel) ? channel : "stable"
        steamSession = steamSessionOverride ?? SteamKeychain.load()
        steamAccount = steamSession["account_name"] ?? ""
        restoreDiagnosticsJob()
    }

    var filteredLibrary: [SteamLibraryGame] {
        libraryGames.filter { (!libraryVROnly || $0.vr) && (!libraryAndroidOnly || $0.androidAvailable) &&
            (librarySearch.isEmpty || $0.name.localizedCaseInsensitiveContains(librarySearch)) }
    }
    var libraryRefreshScope: String { expandedLibrary || !libraryVROnly || !libraryAndroidOnly ? "all" : "vr_android" }

    func libraryFiltersChanged() {
        expandedLibrary = true
        if !steamSession.isEmpty { libraryAction("refresh") }
    }
    var selectedGame: SteamLibraryGame? { libraryGames.first { $0.id == libraryGameID } }
    var selectedBranch: SteamLibraryBranch? { selectedGame?.branches.first { $0.name == libraryBranch } }
    var downloadedGame: SteamDownloadInfo? { selectedBranch?.downloaded?[libraryLaunch] }
    var downloadAction: String {
        guard let local = downloadedGame else { return "Download" }
        if local.valid != true { return "Repair" }
        guard let branch = selectedBranch, branch.remoteKnown == true else { return "Use Downloaded" }
        return branch.revision == local.revision ? "Use Downloaded" : "Update"
    }
    var canDownloadGame: Bool {
        !busy && !libraryLaunch.isEmpty && (downloadAction == "Use Downloaded" ||
            (selectedBranch?.available == true && selectedBranch?.remoteKnown == true && !steamSession.isEmpty))
    }

    func reconcileLibrarySelection() {
        if !libraryGameID.isEmpty && !filteredLibrary.contains(where: { $0.id == libraryGameID }) {
            libraryGameID = ""
            selectLibraryGame()
        }
    }

    func selectLibraryGame() {
        libraryBranch = selectedGame?.branches.first(where: { $0.name == "public" })?.name ?? selectedGame?.branches.first?.name ?? "public"
        libraryLaunch = selectedGame?.launches.first?.id ?? ""
        if source == "steam" { info = nil; filename = ""; data = []; dataBytes = 0; unmappedSteamData = [] }
    }

    func libraryAction(_ action: String, input: [String: Any] = [:]) {
        guard !busy else { return }
        var payload: [String: Any] = action == "login" ? [:] : steamSession
        if action == "refresh" || action == "login" { payload["scope"] = libraryRefreshScope }
        input.forEach { payload[$0.key] = $0.value }
        if action == "download" || action == "use" {
            payload["app_id"] = libraryGameID
            payload["branch"] = libraryBranch
            payload["launch_id"] = libraryLaunch
        }
        pendingInput = payload
        execute(["steam-library", "--action", action], operation: "library-" + action)
        pendingInput = nil
    }

    func libraryDownload() {
        guard canDownloadGame else { return }
        installed = false
        info = nil
        data = []
        dataBytes = 0
        unmappedSteamData = []
        libraryAction(downloadAction == "Use Downloaded" ? "use" : "download")
    }

    func submitGuard(_ code: String) {
        guard let inputPipe, let encoded = try? JSONSerialization.data(withJSONObject: ["guard_code": code]) else { return }
        try? inputPipe.fileHandleForWriting.write(contentsOf: encoded + Data([10]))
    }

    func signOutSteam() {
        guard !busy else { return }
        guard SteamKeychain.remove() else { error = "Could not remove the Steam session from Keychain. Try again."; return }
        steamSession = [:]
        steamAccount = ""
        expandedLibrary = false
        libraryGames = []
        libraryGameID = ""
        selectLibraryGame()
    }

    var canInstall: Bool {
        !busy && info != nil && !kind.isEmpty && !entry.isEmpty &&
        !team.isEmpty && !device.isEmpty && !checks.isEmpty && requiredChecks.allSatisfy(\.ready) &&
        (info?.libraries.contains(entry) ?? false) && !(requiresData && data.isEmpty)
    }

    var currentProfile: Profile? { profiles.first { $0.id == profile } }
    var requiredChecks: [SetupCheck] {
        checks.filter { $0.required || ($0.name == "MoltenVK" && ((info?.vulkanRequired ?? false) || ["eleven", "walkabout-57013", "bonelab", "openbrush", "re4"].contains(profile))) }
    }
    var requiresData: Bool { info?.requirements[profile]?["required"] as? Bool ?? false }
    var requiredFiles: [String] { info?.requirements[profile]?["files"] as? [String] ?? [] }
    var dataDescription: String { info?.requirements[profile]?["description"] as? String ?? "" }
    var dataDestination: String { dataPath.isEmpty ? (currentProfile?.dataPath.isEmpty == false ? currentProfile!.dataPath : currentProfile?.obb ?? (kind == "ue4" ? "Android/obb/\(info?.package ?? "package")" : "obb")) : dataPath }
    var formattedBytes: String {
        ByteCountFormatter.string(fromByteCount: dataBytes + (info?.bytes ?? 0), countStyle: .file)
    }

    func chooseCheckout() {
        let panel = NSOpenPanel()
        panel.title = "Choose the Klepton checkout"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            checkout = url.path
            UserDefaults.standard.set(checkout, forKey: "KleptonCheckout")
            restoreDiagnosticsJob()
            info = nil
            libraryGames = []
            libraryGameID = ""
            loadedCachedLibrary = false
            attemptedLibraryRefresh = false
            expandedLibrary = false
            refresh()
        }
    }

    func chooseAPK() {
        let panel = NSOpenPanel()
        panel.title = "Choose an ARM64 APK"
        panel.allowedContentTypes = [UTType(filenameExtension: "apk") ?? .data]
        panel.canChooseDirectories = false
        if panel.runModal() == .OK, let url = panel.url { inspect(url) }
    }

    func inspect(_ url: URL) {
        guard !busy else { return }
        guard url.pathExtension.lowercased() == "apk" else {
            error = "Choose a standalone .apk file. Split packages and AAB files are not supported."
            return
        }
        info = nil
        source = "apk"
        unmappedSteamData = []
        installed = false
        filename = url.lastPathComponent
        data = []
        dataBytes = 0
        appPath = ""
        steam = "auto"
        execute(["inspect", "--apk", url.path], operation: "inspect")
    }

    func selectProfile() {
        glesVersion = profile.isEmpty ? (info?.suggestedGLESVersion ?? "") : ""
        if let p = currentProfile {
            kind = p.kind
            entry = p.entry
            bundle = p.bundle
            dataPath = p.dataPath
        } else if let info {
            kind = info.kind
            entry = info.entry
            bundle = ""
            dataPath = info.suggestedDataPath
        }
        if steam == "runtime", !(info?.audited.contains(profile) ?? false) { steam = "auto" }
    }

    func refresh() { execute(["doctor"], operation: "doctor") }
    func refreshSteam() { execute(["steam-status"], operation: "steam-status") }
    func updateSteam(_ channel: String) {
        execute(["steam-update", "--channel", channel], operation: "steam-update")
    }

    func addData(folders: Bool) {
        let panel = NSOpenPanel()
        panel.title = folders ? "Choose the folder containing game data" : "Choose matching OBB files"
        panel.canChooseDirectories = folders
        panel.canChooseFiles = !folders
        panel.allowsMultipleSelection = !folders
        if !folders { panel.allowedContentTypes = [UTType(filenameExtension: "obb") ?? .data] }
        if panel.runModal() == .OK {
            for url in panel.urls { addData(url) }
        }
    }

    func addData(_ url: URL) {
        if !data.contains(url) { data.append(url) }
        countData()
    }

    func countData() {
        let selected = data
        Task.detached {
            let size = selectedDataSize(selected)
            await MainActor.run { if self.data == selected { self.dataBytes = size } }
        }
    }

    func install() {
        guard let info, canInstall else { return }
        let request: [String: Any] = ["schema_version": 1, "import_id": info.importID,
            "profile": profile, "steam_mode": steam, "steam_channel": steamChannel, "kind": kind, "entry": entry,
            "gles_version": glesVersion,
            "bundle": bundle, "data_path": dataPath, "team": team, "device": device,
            "data": data.map(\.path), "verbose": verbose]
        do {
            let directory = URL(fileURLWithPath: checkout).appendingPathComponent("build/apk-installer/requests")
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let path = directory.appendingPathComponent(UUID().uuidString + ".json")
            try JSONSerialization.data(withJSONObject: request).write(to: path, options: .atomic)
            installed = false
            execute(["run", "--request", path.path], operation: "run")
        } catch { self.error = error.localizedDescription }
    }

    func cancel() {
        cancelling = true
        message = "Cancelling — retaining logs and backups…"
        process?.terminate()
    }

    func recover() {
        guard !recoveryJob.isEmpty else { return }
        execute(["recover", "--job", recoveryJob], operation: "recover")
    }

    func restoreDiagnosticsJob() {
        // Also find successful installs made before this UI had diagnostics.
        let jobs = URL(fileURLWithPath: checkout).appendingPathComponent("build/apk-installer/jobs")
        let candidates = (try? FileManager.default.contentsOfDirectory(at: jobs, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        let latest = candidates.filter { FileManager.default.fileExists(atPath: $0.appendingPathComponent("result.json").path) }
            .sorted { (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate ?? .distantPast) ?? .distantPast >
                      (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate ?? .distantPast) ?? .distantPast }.first
        diagnosticsJob = latest?.path ?? ""
        diagnosticsApp = ""
        diagnosticsPath = ""
        if let latest, let data = try? Data(contentsOf: latest.appendingPathComponent("manifest.json")),
           let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            diagnosticsApp = manifest["display"] as? String ?? "Last installed app"
        }
    }

    func collectHeadsetLogs() {
        guard !diagnosticsJob.isEmpty else { return }
        diagnosticsPath = ""
        execute(["diagnose", "--job", diagnosticsJob], operation: "diagnose")
    }

    func chooseDiagnosticsJob() {
        let panel = NSOpenPanel()
        panel.title = "Choose the install job for the app that crashed"
        panel.directoryURL = URL(fileURLWithPath: checkout).appendingPathComponent("build/apk-installer/jobs")
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        guard panel.runModal() == .OK, let url = panel.url,
              let data = try? Data(contentsOf: url.appendingPathComponent("manifest.json")),
              let manifest = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              manifest["bundle"] is String, manifest["device"] is String else { return }
        diagnosticsJob = url.path
        diagnosticsApp = manifest["display"] as? String ?? "Selected app"
        diagnosticsPath = ""
    }

    func execute(_ arguments: [String], operation: String) {
        guard !busy else { return }
        guard FileManager.default.fileExists(atPath: URL(fileURLWithPath: checkout).appendingPathComponent("tools/utility.py").path) else {
            error = "Choose a prepared Klepton checkout containing tools/utility.py."
            return
        }
        self.operation = operation
        busy = true
        cancelling = false
        error = ""
        logs = ""
        gotResult = false
        started = Date()
        switch operation {
        case "doctor": message = "Checking local tools, signing, and paired devices…"
        case "steam-status": message = "Checking Valve’s stable and beta releases…"
        case "steam-update": message = "Preparing the Steam update…"
        case "diagnose": message = "Collecting headset logs…"
        case let action where action.hasPrefix("library-"): message = "Connecting to your Steam library…"
        default: message = "Inspecting APK…"
        }
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        task.arguments = ["python3", "tools/utility.py"] + arguments
        task.currentDirectoryURL = URL(fileURLWithPath: checkout)
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:" + (env["PATH"] ?? "")
        env["PYTHONUNBUFFERED"] = "1"
        if let node = Bundle.main.object(forInfoDictionaryKey: "KleptonNodeExecutable") as? String, !node.isEmpty {
            env["KLEPTON_NODE"] = node
        }
        task.environment = env
        if let pendingInput {
            let input = Pipe()
            task.standardInput = input
            inputPipe = input
            // Initial messages are small; Guard replies are written while the operation is running.
            if let encoded = try? JSONSerialization.data(withJSONObject: pendingInput) {
                try? input.fileHandleForWriting.write(contentsOf: encoded + Data([10]))
            }
        }
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        process = task
        do {
            try task.run()
            Task.detached {
                var pending = Data()
                while true {
                    // read(upToCount:) can wait to fill a pipe read on macOS. Auth
                    // prompts and progress must be delivered before the child exits.
                    let chunk = pipe.fileHandleForReading.availableData
                    if chunk.isEmpty { break }
                    pending.append(chunk)
                    while let end = pending.firstIndex(of: 10) {
                        let line = Data(pending[..<end])
                        pending.removeSubrange(...end)
                        let text = String(decoding: line, as: UTF8.self)
                        await MainActor.run { self.receive(text) }
                    }
                }
                if !pending.isEmpty {
                    let text = String(decoding: pending, as: UTF8.self)
                    await MainActor.run { self.receive(text) }
                }
                task.waitUntilExit()
                let status = task.terminationStatus
                await MainActor.run { self.finished(status) }
            }
        } catch {
            busy = false
            process = nil
            inputPipe = nil
            self.error = "Could not start the backend: \(error.localizedDescription)"
        }
    }

    private func receive(_ line: String) {
        guard let bytes = line.data(using: .utf8), let d = (try? JSONSerialization.jsonObject(with: bytes)) as? [String: Any],
              d["schema_version"] as? Int == 1 else {
            if !operation.hasPrefix("library-") { appendLog(line) }
            return
        }
        let status = d["status"] as? String ?? ""
        let stage = d["stage"] as? String ?? ""
        // Private auth events must be handled before any generic logging.
        if stage == "steam-auth" {
            if status == "credential" {
                let session = ["refresh_token": d["refresh_token"] as? String ?? "",
                    "steam_id": d["steam_id"] as? String ?? "", "account_name": d["account_name"] as? String ?? "Steam"]
                if steamSession["steam_id"] != session["steam_id"] {
                    libraryGames = []
                    libraryGameID = ""
                    selectLibraryGame()
                }
                steamSession = session
                steamAccount = session["account_name"] ?? "Steam"
                do { try SteamKeychain.save(session) } catch { self.error = error.localizedDescription }
                steamQRChallenge = ""
                steamAuthKind = ""
            } else if status == "auth" {
                guard !cancelling else { return }
                steamAuthKind = d["kind"] as? String ?? ""
                steamQRChallenge = d["challenge_url"] as? String ?? ""
                showSteamLogin = true
            }
            return
        }
        let text = d["message"] as? String ?? ""
        if let path = d["log_path"] as? String, !path.isEmpty { logPath = path }
        if status == "log" { appendLog(text); return }
        if !text.isEmpty { message = text; appendLog("\(stage): \(text)") }
        if status == "error" || status == "cancelled" {
            error = text
            recoveryJob = d["recovery_job"] as? String ?? ""
            return
        }
        guard status == "result" else { return }
        gotResult = true
        if stage == "doctor" {
            profiles = (d["profiles"] as? [[String: Any]] ?? []).map(Profile.init)
            devices = (d["devices"] as? [[String: Any]] ?? []).filter { $0["paired"] as? Bool == true }
                .map { Choice(id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "Vision Pro") }
            teams = (d["teams"] as? [[String: Any]] ?? []).map { Choice(id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "") }
            checks = (d["checks"] as? [[String: Any]] ?? []).map { SetupCheck(name: $0["name"] as? String ?? "Tool", ready: $0["ready"] as? Bool ?? false, fix: $0["fix"] as? String ?? "", required: $0["required"] as? Bool ?? true) }
            if !devices.contains(where: { $0.id == device }) { device = devices.first?.id ?? "" }
            if !teams.contains(where: { $0.id == team }) { team = teams.first?.id ?? "" }
            message = d["ready"] as? Bool == true ? "Local build tools are ready." : "Complete the setup steps below."
        } else if stage == "steam-status" {
            let channels = d["channels"] as? [String: [String: Any]] ?? [:]
            steamVersions = channels.mapValues(SteamChannelInfo.init)
            if text.isEmpty { message = "Steam versions refreshed." }
        } else if stage == "steam-library" {
            let decoder = JSONDecoder()
            decoder.keyDecodingStrategy = .convertFromSnakeCase
            if let games = d["games"], let encoded = try? JSONSerialization.data(withJSONObject: games),
               let decoded = try? decoder.decode([SteamLibraryGame].self, from: encoded) {
                libraryGames = decoded
                if !libraryGameID.isEmpty && selectedGame == nil { libraryGameID = ""; selectLibraryGame() }
                if let game = selectedGame {
                    if !game.branches.contains(where: { $0.name == libraryBranch }) || !game.launches.contains(where: { $0.id == libraryLaunch }) {
                        selectLibraryGame()
                    }
                }
            }
            showSteamLogin = false
        } else if stage == "inspect" || stage == "steam-game" {
            info = APKInfo(d)
            if stage == "steam-game" {
                filename = selectedGame?.name ?? "Steam APK"
                steamDownloadFolder = d["download_folder"] as? String ?? ""
                data = (d["downloaded_data"] as? [String] ?? []).map { URL(fileURLWithPath: $0) }
                unmappedSteamData = d["unmapped_data"] as? [String] ?? []
                countData()
            }
            profile = d["profile"] as? String ?? ""
            selectProfile()
            message = "APK inspected. Review settings and add any required game data."
        } else if stage == "complete" {
            installed = true
            appPath = d["app_path"] as? String ?? ""
            recoveryJob = ""
            diagnosticsJob = d["job_path"] as? String ?? ""
            diagnosticsApp = info?.name ?? "Last installed app"
            diagnosticsPath = ""
        } else if stage == "diagnose" {
            diagnosticsPath = d["diagnostics_path"] as? String ?? ""
        } else if stage == "recover" {
            recoveryJob = ""
        }
    }

    private func appendLog(_ line: String) {
        logs += line + "\n"
        if logs.count > 60000 { logs = String(logs.suffix(50000)) }
    }

    private func finished(_ status: Int32) {
        busy = false
        cancelling = false
        process = nil
        try? inputPipe?.fileHandleForWriting.close()
        inputPipe = nil
        if (!gotResult || status != 0) && error.isEmpty {
            error = "The backend stopped (exit \(status)). \(logs.suffix(1000))"
        }
        if operation.hasPrefix("library-") && status != 0 {
            steamAuthKind = ""
            steamQRChallenge = ""
        }
        if operation == "doctor", status == 0 { refreshSteam() }
        else if operation == "steam-status" && !steamSession.isEmpty && !loadedCachedLibrary {
            loadedCachedLibrary = true
            libraryAction("cached")
        } else if operation == "library-cached" && status == 0 && !attemptedLibraryRefresh {
            attemptedLibraryRefresh = true
            libraryAction("refresh")
        }
    }

    func reveal(_ path: String) {
        guard !path.isEmpty else { return }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
    }
}

struct InstallerView: View {
    @StateObject private var model = InstallerModel()
    @State private var targeted = false
    @State private var advanced = false
    @State private var showLogs = false
    private let bootTypes: [(String, String)] = [("unity", "Unity / IL2CPP"), ("ue4", "Unreal Engine 4"), ("native", "NativeActivity"), ("steamlink", "Steam Link"), ("jkxr", "JKXR"), ("sdl2", "SDL2"), ("gles3jni", "GLES3 JNI")]

    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .top) {
                Image(systemName: "vision.pro").font(.system(size: 30)).foregroundStyle(.tint).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 5) {
                    Text("Klepton APK Installer").font(.title2.bold())
                    Text("Convert an Android VR APK and install it on Vision Pro.").foregroundStyle(.secondary)
                }
                Spacer()
                Button("Checkout…") { model.chooseCheckout() }.disabled(model.busy)
                    .help(model.checkout.isEmpty ? "Choose your prepared Klepton checkout" : model.checkout)
            }.padding(22)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    apkSection
                    if let info = model.info {
                        compatibilitySection(info)
                        dataSection(info)
                    }
                    destinationSection
                    if model.info != nil { advancedSection }
                }.padding(22)
                .disabled(model.busy)
            }
            Divider()
            footer.padding(18)
        }
        .frame(minWidth: 680, minHeight: 640)
        .task { if !model.checkout.isEmpty { model.refresh() } }
        .onChange(of: model.profile) { _, _ in model.selectProfile() }
        .onChange(of: model.kind) { _, kind in
            if kind != "unity" { model.glesVersion = "" }
        }
        .onChange(of: model.info?.importID) { _, _ in
            if model.info != nil && model.kind.isEmpty { advanced = true }
        }
        .onChange(of: model.libraryGameID) { _, _ in model.selectLibraryGame() }
        .onChange(of: model.libraryVROnly) { _, _ in model.libraryFiltersChanged() }
        .onChange(of: model.libraryAndroidOnly) { _, _ in model.libraryFiltersChanged() }
        .onChange(of: model.filteredLibrary.map(\.id)) { _, _ in model.reconcileLibrarySelection() }
        .onChange(of: model.source) { _, _ in
            model.info = nil
            model.filename = ""
            model.data = []
            model.dataBytes = 0
            model.unmappedSteamData = []
        }
        .onChange(of: model.libraryBranch) { _, _ in
            if model.source == "steam" { model.info = nil; model.data = []; model.dataBytes = 0; model.unmappedSteamData = [] }
        }
        .onChange(of: model.libraryLaunch) { _, _ in
            if model.source == "steam" { model.info = nil; model.data = []; model.dataBytes = 0; model.unmappedSteamData = [] }
        }
        .sheet(isPresented: $model.showSteamLogin) { SteamSignInView(model: model) }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
            if model.busy { model.cancel() }
        }
    }

    private var apkSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 12) {
                Picker("Game source", selection: $model.source) {
                    Text("Steam Library").tag("steam")
                    Text("Local APK").tag("apk")
                }.pickerStyle(.segmented).accessibilityIdentifier("gameSource")
                if model.source == "steam" {
                    steamLibrarySection
                } else {
                HStack(spacing: 14) {
                    Image(systemName: "shippingbox").font(.system(size: 26)).foregroundStyle(.secondary).accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 5) {
                        Text(model.filename.isEmpty ? "Drop an APK here" : model.filename).font(.headline).lineLimit(2)
                        Text("Standalone ARM64 .apk").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Choose APK…") { model.chooseAPK() }.keyboardShortcut("o", modifiers: .command)
                        .accessibilityIdentifier("chooseAPK")
                }
                .padding(18).frame(maxWidth: .infinity, minHeight: 85)
                .background(targeted ? Color.accentColor.opacity(0.12) : Color.secondary.opacity(0.04))
                .clipShape(RoundedRectangle(cornerRadius: 8))
                .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(targeted ? Color.accentColor : Color.secondary.opacity(0.25), style: StrokeStyle(lineWidth: 1, dash: [5])))
                .onDrop(of: [.fileURL], isTargeted: $targeted) { providers in
                    guard !model.busy, providers.count == 1, let provider = providers.first else { return false }
                    _ = provider.loadObject(ofClass: URL.self) { url, _ in
                        if let url { Task { @MainActor in model.inspect(url) } }
                    }
                    return true
                }
                .accessibilityIdentifier("apkDropArea")
                }
                if let info = model.info {
                    HStack(alignment: .top) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(info.name).font(.headline)
                            Text(info.package).font(.caption.monospaced()).textSelection(.enabled)
                            Text("Version \(info.version) · \(info.architecture)").font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Label(model.profile.isEmpty ? "Experimental" : "Profile selected", systemImage: model.profile.isEmpty ? "flask" : "slider.horizontal.3")
                            .font(.caption).padding(6).background(.quaternary).clipShape(Capsule())
                    }
                }
            }.padding(8)
        } label: { Text("1  Game").font(.headline) }
    }

    private var steamLibrarySection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label(model.steamAccount.isEmpty ? "Your Steam library" : model.steamAccount, systemImage: "person.crop.circle")
                Spacer()
                if !model.steamAccount.isEmpty {
                    Button("Refresh") { model.libraryAction("refresh") }.accessibilityIdentifier("refreshLibrary")
                    Button("Sign Out") { model.signOutSteam() }
                }
                Button(model.steamAccount.isEmpty ? "Sign in to Steam…" : "Sign in again…") {
                    model.steamAuthKind = ""
                    model.steamQRChallenge = ""
                    model.showSteamLogin = true
                }.accessibilityIdentifier("steamLibrarySignIn")
            }
            TextField("Search your library", text: $model.librarySearch).textFieldStyle(.roundedBorder)
            HStack {
                Toggle("VR titles", isOn: $model.libraryVROnly)
                Toggle("Android bundle available", isOn: $model.libraryAndroidOnly)
            }.toggleStyle(.checkbox)
            if model.libraryGames.isEmpty {
                Text(model.steamAccount.isEmpty ? "Sign in to find the Android VR games you own." : "No cached games. Refresh your Steam library to check available releases.")
                    .font(.callout).foregroundStyle(.secondary).padding(.vertical, 10)
            } else if model.filteredLibrary.isEmpty {
                Text("No games match these filters.").foregroundStyle(.secondary).padding(.vertical, 10)
            } else {
                List(selection: $model.libraryGameID) {
                    ForEach(model.filteredLibrary) { game in
                        HStack {
                            Text(game.name)
                            Spacer()
                            if game.vr { Text("VR").font(.caption).foregroundStyle(.secondary) }
                            if game.androidAvailable { Image(systemName: "shippingbox").help("Android release available") }
                        }.tag(game.id)
                    }
                }.frame(height: 190).listStyle(.bordered).accessibilityIdentifier("steamLibraryGames")
            }
            if let game = model.selectedGame {
                Text(game.name).font(.headline)
                Picker("Release branch", selection: $model.libraryBranch) {
                    ForEach(game.branches) { branch in
                        Text(branch.name + (branch.locked ? " (password protected)" : branch.available ? "" : " (no Android release)"))
                            .tag(branch.name).disabled(branch.locked)
                    }
                }.accessibilityIdentifier("steamGameBranch")
                if game.launches.count > 1 {
                    Picker("Android APK", selection: $model.libraryLaunch) {
                        ForEach(game.launches) { Text($0.name).tag($0.id) }
                    }
                }
                if let branch = model.selectedBranch {
                    HStack(alignment: .top, spacing: 24) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Remote").font(.subheadline.bold())
                            Text(branch.remoteKnown == true ? (branch.buildId.isEmpty ? "Build unavailable" : "Build \(branch.buildId)") : "Unknown — refresh to check")
                            Text("Updated: " + (branch.remoteKnown == true ? steamDate(branch.updatedAt) : "Unavailable"))
                        }.frame(maxWidth: .infinity, alignment: .leading)
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Downloaded").font(.subheadline.bold())
                            if let local = model.downloadedGame {
                                Text("Build " + (local.buildId ?? "Unavailable"))
                                Text("Downloaded: " + steamDate(local.downloadedAt))
                                if local.valid != true { Text("Missing or damaged files").foregroundStyle(.orange) }
                            } else { Text("Not downloaded") }
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }.font(.caption).textSelection(.enabled)
                    HStack {
                        Button(model.downloadAction) { model.libraryDownload() }
                            .disabled(!model.canDownloadGame).accessibilityIdentifier("downloadSteamGame")
                        if model.downloadAction == "Update", model.downloadedGame?.valid == true {
                            Button("Use Downloaded") { model.libraryAction("use") }
                        }
                        Text("Downloads prepare the APK for Step 2.").font(.caption).foregroundStyle(.secondary)
                    }
                    if !branch.available {
                        Text("This branch has no accessible Android release. Choose another branch or import a local APK.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                if !model.unmappedSteamData.isEmpty {
                    Text("Additional downloaded files need a data mapping in Step 3: " + model.unmappedSteamData.prefix(8).joined(separator: ", "))
                        .font(.caption).foregroundStyle(.orange)
                }
                if model.info != nil && !model.steamDownloadFolder.isEmpty {
                    Button("Reveal Download") { model.reveal(model.steamDownloadFolder) }.buttonStyle(.link)
                }
            }
        }
    }

    private func compatibilitySection(_ info: APKInfo) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                LabeledContent("Compatibility profile") {
                    Picker("Compatibility profile", selection: $model.profile) {
                        Text("Experimental / autodetected").tag("")
                        ForEach(model.profiles) { Text($0.name).tag($0.id) }
                    }.labelsHidden().frame(maxWidth: 340)
                }
                Text(info.reason).font(.caption).foregroundStyle(.secondary)
                Text("Detected startup: " + (bootTypes.first { $0.0 == info.kind }?.1 ?? "Uncertain — choose under Advanced"))
                    .font(.caption).foregroundStyle(.secondary)
                if model.profile.isEmpty {
                    Text("Experimental conversion — review the detected boot type and entry library under Advanced.")
                        .font(.caption).foregroundStyle(.orange)
                }
                LabeledContent("Steam mode") {
                    Picker("Steam mode", selection: $model.steam) {
                        Text("Auto").tag("auto")
                        Text("Not Steam").tag("none").disabled(info.steam)
                        Text("Steam runtime (audited)").tag("runtime").disabled(!info.audited.contains(model.profile))
                        Text("Offline diagnostics").tag("offline")
                    }.labelsHidden().frame(maxWidth: 340)
                }
                steamVersionsSection
                if model.steam == "offline" || (model.steam == "auto" && info.steam && !info.audited.contains(model.profile)) {
                    Label("Offline diagnostics provides no authenticated Steam services.", systemImage: "info.circle").font(.caption).foregroundStyle(.orange)
                } else if model.steam == "runtime" || (model.steam == "auto" && info.audited.contains(model.profile)) {
                    Text("Uses the audited local Steam runtime. Sign in on Vision Pro when you open the app.").font(.caption).foregroundStyle(.secondary)
                }
                Text(info.graphics).font(.caption).foregroundStyle(.secondary)
                Text(info.warning).font(.caption).foregroundStyle(.secondary)
            }.padding(8)
        } label: { Text("2  Compatibility").font(.headline) }
    }

    private var steamVersionsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Steam channel").frame(width: 215, alignment: .leading)
                Picker("Steam channel", selection: $model.steamChannel) {
                    Text("Stable").tag("stable")
                    Text("Beta").tag("beta")
                }.labelsHidden().pickerStyle(.segmented).frame(width: 220)
                    .accessibilityIdentifier("steamChannel")
            }
            ForEach(["stable", "beta"], id: \.self) { channel in
                let version = model.steamVersions[channel] ?? SteamChannelInfo()
                let name = channel == "stable" ? "stable" : "beta"
                HStack {
                    Text("Current \(name) version").frame(width: 215, alignment: .leading)
                    Text(version.current).monospacedDigit().textSelection(.enabled)
                }
                HStack(spacing: 10) {
                    Text("Latest remote \(name) version").frame(width: 215, alignment: .leading)
                    Text(version.remote).monospacedDigit().textSelection(.enabled)
                    Button("Update") { model.updateSteam(channel) }
                        .disabled(model.busy || !version.remoteAvailable || version.isUpToDate)
                        .accessibilityLabel("Update Steam \(name)")
                        .accessibilityIdentifier("updateSteam_\(channel)")
                }
                if !version.error.isEmpty {
                    Text("\(name.capitalized): \(version.error)").font(.caption).foregroundStyle(.orange).textSelection(.enabled)
                }
            }
            HStack {
                Text("The selected channel supplies Steam files for the next build and install.")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Refresh versions") { model.refreshSteam() }.disabled(model.busy)
            }
        }.padding(10).background(.quaternary.opacity(0.4)).clipShape(RoundedRectangle(cornerRadius: 6))
            .accessibilityElement(children: .contain)
    }

    private func dataSection(_ info: APKInfo) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text(model.data.isEmpty ? "Add any OBBs or external game data this APK needs." : "\(model.data.count) selection(s) · Estimated transfer: \(model.formattedBytes)")
                        .font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("Add OBBs…") { model.addData(folders: false) }
                    Button("Add Folder…") { model.addData(folders: true) }
                }
                if model.requiresData {
                    Text("Required: " + (model.requiredFiles + (model.dataDescription.isEmpty ? [] : [model.dataDescription])).joined(separator: ", "))
                        .font(.caption).foregroundStyle(model.data.isEmpty ? .orange : .secondary).textSelection(.enabled)
                }
                Text("Estimated transfer: \(model.formattedBytes)").font(.caption).foregroundStyle(.secondary)
                ForEach(model.data, id: \.self) { url in
                    HStack {
                        Image(systemName: url.pathExtension.lowercased() == "obb" ? "doc.zipper" : "folder").accessibilityHidden(true)
                        Text(url.lastPathComponent).lineLimit(1).help(url.path)
                        Spacer()
                        Button { model.data.removeAll { $0 == url }; model.countData() } label: { Image(systemName: "xmark.circle.fill") }
                            .buttonStyle(.plain).accessibilityLabel("Remove \(url.lastPathComponent)")
                    }
                }
                ForEach(info.suggestions.filter { candidate in !model.data.contains(URL(fileURLWithPath: candidate)) }, id: \.self) { candidate in
                    HStack {
                        Text("Nearby: \(URL(fileURLWithPath: candidate).lastPathComponent)").font(.caption).lineLimit(1)
                        Spacer()
                        Button("Add") { model.addData(URL(fileURLWithPath: candidate)) }
                            .accessibilityLabel("Add suggested \(URL(fileURLWithPath: candidate).lastPathComponent)")
                    }
                }
                Text("Game data → android-files/\(model.dataDestination)/").font(.caption.monospaced()).foregroundStyle(.secondary)
                Text("Existing writable data is backed up before an update. Saved files take precedence over supplied seed data.").font(.caption).foregroundStyle(.secondary)
            }.padding(8)
        } label: { Text("3  Game data").font(.headline) }
    }

    private var destinationSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("Paired Vision Pro and development signing").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button("Refresh") { model.refresh() }.accessibilityIdentifier("refreshReadiness")
                }
                LabeledContent("Vision Pro") {
                    Picker("Vision Pro", selection: $model.device) {
                        Text("Select a paired Vision Pro").tag("")
                        ForEach(model.devices) { Text($0.name).tag($0.id) }
                    }.labelsHidden().frame(maxWidth: 400)
                }
                if model.devices.isEmpty {
                    Text("Pair your Vision Pro in Xcode → Window → Devices and Simulators, then unlock it and refresh.").font(.caption).foregroundStyle(.secondary)
                }
                LabeledContent("Signing team") {
                    Picker("Signing team", selection: $model.team) {
                        Text("Select a development team").tag("")
                        ForEach(model.teams) { Text($0.name).tag($0.id) }
                    }.labelsHidden().frame(maxWidth: 400)
                }
                ForEach(model.requiredChecks.filter { !$0.ready }) { check in
                    VStack(alignment: .leading, spacing: 4) {
                        Label(check.name, systemImage: "exclamationmark.triangle").font(.caption.bold()).foregroundStyle(.orange)
                        Text(check.fix).font(.caption.monospaced()).textSelection(.enabled)
                    }
                }
                if !model.checks.isEmpty && model.requiredChecks.allSatisfy(\.ready) {
                    Label("Local tools and signing certificate are ready. Xcode also needs an account or valid provisioning profile.", systemImage: "checkmark.circle").font(.caption).foregroundStyle(.secondary)
                }
            }.padding(8)
        } label: { Text("4  Destination").font(.headline) }
    }

    private var advancedSection: some View {
        DisclosureGroup("Advanced", isExpanded: $advanced) {
            VStack(alignment: .leading, spacing: 12) {
                LabeledContent("Boot type") {
                    Picker("Boot type", selection: $model.kind) {
                        Text("Choose boot type").tag("")
                        ForEach(bootTypes, id: \.0) { id, title in
                            Text(title).tag(id).disabled(model.profile.isEmpty && !["unity", "ue4", "native"].contains(id))
                        }
                    }.labelsHidden().frame(maxWidth: 340)
                }
                LabeledContent("Entry library") {
                    Picker("Entry library", selection: $model.entry) {
                        Text("Choose entry library").tag("")
                        ForEach(model.info?.libraries ?? [], id: \.self) { Text($0).tag($0) }
                    }.labelsHidden().frame(maxWidth: 340)
                }
                LabeledContent("Bundle identifier") {
                    TextField("Automatic, stable for this game", text: $model.bundle).textFieldStyle(.roundedBorder).frame(maxWidth: 400)
                        .accessibilityLabel("Bundle identifier")
                }
                LabeledContent("GLES compatibility") {
                    Picker("GLES compatibility", selection: $model.glesVersion) {
                        Text("Target default").tag("")
                        Text("GLES 3.0").tag("3.0")
                        Text("GLES 3.1").tag("3.1")
                    }.labelsHidden().frame(maxWidth: 340).disabled(model.kind != "unity")
                }
                Text("GLES 3.1 is recommended for Bigscreen avatars. GLES 3.0 can avoid shader failures in other Unity apps. Changes require rebuilding.")
                    .font(.caption).foregroundStyle(.secondary)
                LabeledContent("Data folder destination") {
                    TextField("Automatic for this profile", text: $model.dataPath).textFieldStyle(.roundedBorder).frame(maxWidth: 400)
                        .accessibilityLabel("Data folder destination")
                }
                Text("A folder inside the game’s Android storage, not a folder on your Mac. For example, obb places loose files in android-files/obb. Downloaded Steam data supplies this automatically.").font(.caption).foregroundStyle(.secondary)
                Toggle("Stream verbose build output", isOn: $model.verbose)
                Text("Complete build logs are retained regardless of this setting.").font(.caption).foregroundStyle(.secondary)
            }.padding(.top, 12)
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 10) {
            if !model.error.isEmpty {
                Label { Text(model.error).lineLimit(5).textSelection(.enabled) } icon: { Image(systemName: "exclamationmark.triangle.fill") }
                    .font(.callout).foregroundStyle(.red)
            }
            HStack(spacing: 12) {
                if model.busy { ProgressView().controlSize(.small) }
                VStack(alignment: .leading, spacing: 4) {
                    Text(model.message).font(.callout).textSelection(.enabled)
                    if model.busy {
                        TimelineView(.periodic(from: model.started, by: 1)) { context in
                            let seconds = max(0, Int(context.date.timeIntervalSince(model.started)))
                            Text("\(seconds / 60)m \(seconds % 60)s elapsed").font(.caption).foregroundStyle(.secondary).monospacedDigit()
                        }
                    }
                }
                Spacer()
                if model.busy {
                    Button(model.cancelling ? "Cancelling…" : "Cancel") { model.cancel() }
                        .disabled(model.cancelling).keyboardShortcut(.cancelAction)
                } else {
                    if !model.recoveryJob.isEmpty {
                        Button("Restore Saved Data") { model.recover() }
                            .help("Restore the previous update’s backup. Current writable files are backed up separately first.")
                    }
                    if model.installed && !model.appPath.isEmpty { Button("Reveal App") { model.reveal(model.appPath) } }
                    Button("Build & Install") { model.install() }.buttonStyle(.borderedProminent)
                        .disabled(!model.canInstall).keyboardShortcut(.defaultAction).accessibilityIdentifier("buildAndInstall")
                }
            }
            HStack {
                Button(showLogs ? "Hide Logs" : "Show Logs") { showLogs.toggle() }.buttonStyle(.link)
                Spacer()
                if !model.logPath.isEmpty { Button("Reveal Logs") { model.reveal(model.logPath) }.buttonStyle(.link) }
            }
            VStack(alignment: .leading, spacing: 6) {
                Text("App closes or fails to start?").font(.callout.bold())
                Text("Reproduce the failure, then collect its headset logs. Keep Vision Pro unlocked. Installation alone does not verify compatibility.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    if !model.diagnosticsApp.isEmpty { Text(model.diagnosticsApp).font(.caption) }
                    Button("Choose Install…") { model.chooseDiagnosticsJob() }.disabled(model.busy)
                    Button("Collect Headset Logs") { model.collectHeadsetLogs() }
                        .disabled(model.busy || model.diagnosticsJob.isEmpty)
                    if !model.diagnosticsPath.isEmpty {
                        Button("Reveal Headset Logs") { model.reveal(model.diagnosticsPath) }
                    }
                }
            }
            if showLogs {
                ScrollView {
                    Text(model.logs.isEmpty ? "Build progress will appear here." : model.logs)
                        .font(.system(size: 11, design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                }.frame(height: 140).padding(8).background(.quaternary).clipShape(RoundedRectangle(cornerRadius: 6))
            }
        }
    }
}

struct SteamSignInView: View {
    @ObservedObject var model: InstallerModel
    @State private var username = ""
    @State private var password = ""
    @State private var guardCode = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Sign in to Steam").font(.title2.bold())
            Text("Connect your library to download the Android releases you own. Your remembered session is stored in macOS Keychain.")
                .font(.callout).foregroundStyle(.secondary)
            if let qr = steamQR(model.steamQRChallenge) {
                HStack {
                    Spacer()
                    Image(nsImage: qr).interpolation(.none).resizable().frame(width: 230, height: 230)
                        .padding(12).background(.white).accessibilityLabel("Steam sign-in QR code")
                    Spacer()
                }
                Text("Scan with the Steam mobile app and approve the sign-in.")
            } else if !model.busy {
                Button("Sign in with a QR code") {
                    model.libraryAction("login", input: ["auth_method": "qr"])
                }.buttonStyle(.borderedProminent)
                Divider()
                TextField("Steam account name", text: $username).textFieldStyle(.roundedBorder)
                SecureField("Password", text: $password).textFieldStyle(.roundedBorder)
                Button("Sign in with password") {
                    let secret = password
                    password = ""
                    model.libraryAction("login", input: ["auth_method": "password", "account_name": username, "password": secret])
                }.disabled(username.isEmpty || password.isEmpty)
            }
            if model.steamAuthKind == "guard" {
                Text("Approve the sign-in in Steam, or enter your email / Steam Guard code.")
                HStack {
                    TextField("Steam Guard code", text: $guardCode).textFieldStyle(.roundedBorder)
                    Button("Submit code") {
                        model.submitGuard(guardCode.trimmingCharacters(in: .whitespacesAndNewlines))
                        guardCode = ""
                    }.disabled(guardCode.isEmpty)
                }
            }
            if model.busy { ProgressView(model.message).controlSize(.small) }
            if !model.error.isEmpty { Text(model.error).foregroundStyle(.red).textSelection(.enabled) }
            HStack {
                Spacer()
                Button(model.busy ? "Cancel sign-in" : "Close") {
                    if model.busy { model.cancel() }
                    password = ""
                    guardCode = ""
                    model.showSteamLogin = false
                }
            }
        }.padding(24).frame(width: 470)
        .interactiveDismissDisabled(model.busy)
    }
}

#if !KLEPTON_MODEL_TESTS
@main
struct KleptonInstallerApp: App {
    var body: some Scene {
        WindowGroup("Klepton APK Installer") { InstallerView() }
            .defaultSize(width: 820, height: 890)
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}
#endif
