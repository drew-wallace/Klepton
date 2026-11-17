#if KL_STEAM_GAME_HOST
import SwiftUI
import Foundation

@_silgen_name("kl_steam_cloud_cancel_startup") private func cloudCancelStartup(_ account: UInt64)
@_silgen_name("kl_steam_cloud_release_startup") private func cloudReleaseStartup(_ account: UInt64) -> Int32
@_silgen_name("kl_steam_cloud_cache") private func cloudCache(_ account: UInt64, _ name: UnsafePointer<CChar>, _ source: UnsafePointer<CChar>, _ size: UInt64) -> UInt64
@_silgen_name("kl_steam_cloud_account") private func cloudAccount() -> UInt64
@_silgen_name("kl_steam_cloud_submit") private func cloudSubmit(_ account: UInt64, _ method: Int32, _ bytes: UnsafeRawPointer?, _ count: UInt32) -> UInt64
@_silgen_name("kl_steam_cloud_poll") private func cloudPoll(_ serial: UInt64, _ bytes: UnsafeMutableRawPointer?, _ capacity: UInt32, _ size: UnsafeMutablePointer<UInt32>, _ result: UnsafeMutablePointer<Int32>) -> Int32

@MainActor private final class KLNativeCloud: KLCloudTransport {
    var account: UInt64 { cloudAccount() }
    func call(_ method: Int32, _ body: Data) async throws -> Data {
        let serial = body.withUnsafeBytes { cloudSubmit(account, method, $0.baseAddress, UInt32(body.count)) }
        guard serial != 0 else { throw KLCloudError("Steam Cloud is unavailable or busy. Retry sync.") }
        return try await response(serial, method: method)
    }
    private func response(_ serial: UInt64, method: Int32 = 7) async throws -> Data {
        guard serial != 0 else { throw KLCloudError("Steam save cache is unavailable. Retry sync.") }
        var response = Data(count: 8 * 1024 * 1024)
        let deadline = Date().addingTimeInterval(40)
        // Consume an in-flight response even on cancellation so the worker's
        // single-operation mailbox is reusable. No Valve call runs on the UI.
        while Date() < deadline {
            var size: UInt32 = 0, result: Int32 = 0
            let state = response.withUnsafeMutableBytes { cloudPoll(serial, $0.baseAddress, UInt32($0.count), &size, &result) }
            if state == 1 {
                try Task.checkCancellation()
                guard result == 1 else { throw KLCloudRPCError(method: method, result: result) }
                return response.prefix(Int(size))
            }
            guard state == 0 else { throw KLCloudError("Steam Cloud returned an invalid response.") }
            // This task belongs to the session rather than a disappearing view.
            await withCheckedContinuation { continuation in
                DispatchQueue.global().asyncAfter(deadline: .now() + 0.1) { continuation.resume() }
            }
        }
        throw KLCloudError("Steam Cloud timed out. Reopen Klepton and retry.")
    }
    func cache(_ name: String, bytes: Data) async throws {
        let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("SteamCloud-v1/Cache")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let source = directory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: source) }
        try bytes.write(to: source, options: .atomic)
        let serial = name.withCString { name in source.path.withCString { cloudCache(account, name, $0, UInt64(bytes.count)) } }
        _ = try await response(serial)
    }
    func download(_ response: Data, file: KLCloudFile) async throws -> Data { try await KLCloudHTTP.download(response, file: file) }
    func upload(_ response: Data, bytes: Data, check: () throws -> Void) async throws { try await KLCloudHTTP.upload(response, bytes: bytes, check: check) }
}

/// Keeps a shadow restore read-only even if its caller's settings are wrong.
@MainActor private final class KLReadOnlyCloudProbe: KLCloudTransport {
    let base: KLNativeCloud
    var calls: [Int32] = []
    var account: UInt64 { base.account }
    init(_ base: KLNativeCloud) { self.base = base }
    func call(_ method: Int32, _ body: Data) async throws -> Data {
        guard [1, 2].contains(method) else { throw KLCloudError("The download probe refused a cloud mutation.") }
        calls.append(method); return try await base.call(method, body)
    }
    func download(_ response: Data, file: KLCloudFile) async throws -> Data { try await base.download(response, file: file) }
    func upload(_ response: Data, bytes: Data, check: () throws -> Void) async throws { throw KLCloudError("The download probe cannot upload.") }
    // Shadow files are never handed to a playable SDK cache.
    func cache(_ name: String, bytes: Data) async throws {}
}

@MainActor final class KleptonSteamCloud: ObservableObject {
    static let shared = KleptonSteamCloud()
    @Published private(set) var permissions = KLCloudSync.Permissions()
    @Published private(set) var status = "Sign in to configure Steam Cloud"
    @Published private(set) var syncing = false
    @Published private(set) var account: UInt64 = 0
    private var service: KLCloudSync?
    private var configurationError: String?
    private let transport = KLNativeCloud()
    private var probeStarted = false
    /// Explicit device-test opt-in. Listing and shadow downloads are read-only.
    /// sync-roundtrip first runs the user's permitted sync, then verifies a
    /// download into separate test storage. No mode releases the startup SDK.
    func runDeviceProbeIfRequested() {
        let mode = ProcessInfo.processInfo.environment["KL_STEAM_CLOUD_PROBE"] ?? ""
        guard !probeStarted, !syncing, account != 0,
              ["list", "download", "sync-roundtrip", "quota"].contains(mode) else { return }
        var result: Int32 = 0
        guard [7, 9].contains(steamTicketStatus(&result)),
              let app = UInt32(ProcessInfo.processInfo.environment["KL_STEAM_APP_ID"] ?? "") else { return }
        probeStarted = true
        syncing = true
        Task { @MainActor in
            defer { syncing = false }
            let destination = URL(fileURLWithPath: Paths.container).appendingPathComponent("klepton-steam-cloud-probe.json")
            do {
                if mode == "quota" {
                    let fields = try KLCloudProto.parse(await transport.call(10, KLCloudProto.number(1, UInt64(app))))
                    let report: [String: Any] = ["mode": mode, "status": "quota read", "appID": app,
                        "files": String(fields.value(1)), "bytes": String(fields.value(2)),
                        "maxFiles": String(fields.value(3)), "maxBytes": String(fields.value(4))]
                    try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys]).write(to: destination, options: .atomic)
                    return
                }
                if mode == "sync-roundtrip" {
                    guard let service else { throw KLCloudError("The Steam save service is unavailable.") }
                    try await service.sync()
                }
                let readOnly = KLReadOnlyCloudProbe(transport)
                let raw = try await readOnly.call(1, KLCloudProto.number(1, UInt64(app)) + KLCloudProto.number(2, 0))
                let fields = try KLCloudProto.parse(raw)
                let prefixes = fields.filter { $0.number == 4 }.map(\.string)
                var files: [[String: Any]] = []
                for field in fields where field.number == 2 {
                    let item = try KLCloudProto.parse(field.bytes)
                    files.append(["name": item.text(1), "sha1": item.data(2).map { String(format: "%02x", $0) }.joined(),
                        "size": String(item.value(4)), "persistState": item.value(5),
                        "platforms": item.first { $0.number == 6 }.map { String($0.integer) } ?? "unspecified",
                        "prefixIndex": item.value(7)])
                }
                var report: [String: Any] = ["appID": app, "status": "listed", "mode": mode, "generation": String(fields.value(1)),
                    "prefixes": prefixes, "files": files, "rawProtobuf": raw.base64EncodedString()]
                if mode != "list" {
                    let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                        .appendingPathComponent("SteamCloudProbe/" + UUID().uuidString)
                    let json = Bundle.main.object(forInfoDictionaryKey: "KleptonSteamCloudConfiguration") as? String ?? "{}"
                    let config = try JSONDecoder().decode(KLCloudConfiguration.self, from: Data(json.utf8))
                    let shadow = KLCloudPaths(documents: root.appendingPathComponent("Documents"), steam: root.appendingPathComponent("Steam"),
                        account: account, app: app, configuration: config,
                        package: Bundle.main.object(forInfoDictionaryKey: "KleptonGamePackage") as? String ?? "")
                    let sync = KLCloudSync(transport: readOnly, paths: shadow, support: root.appendingPathComponent("Records"))
                    try sync.setPermissions(.init(download: true))
                    try await sync.sync()
                    let downloaded = try shadow.localFiles().sorted { $0.key < $1.key }.map { path, url in
                        let data = try Data(contentsOf: url)
                        return ["path": path, "sha1": KLCloudSync.hash(data), "size": String(data.count)]
                    }
                    report["downloaded"] = downloaded
                    report["shadowRoot"] = root.path
                    report["status"] = downloaded.isEmpty ? "no Android save files to verify" : "download verified"
                }
                report["readOnlyMethods"] = readOnly.calls
                try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted, .sortedKeys]).write(to: destination, options: .atomic)
            } catch {
                let report = ["status": "failed", "error": error.localizedDescription]
                try? JSONSerialization.data(withJSONObject: report).write(to: destination, options: .atomic)
            }
        }
    }
    var locked: Bool { syncing || KleptonSession.shared.running || KleptonSession.shared.finished || KleptonSession.shared.quitting }
    func refresh() {
        let id = transport.account
        guard id != account else { return }
        account = id; service = nil; permissions = .init(); configurationError = nil
        guard id != 0, let app = UInt32(ProcessInfo.processInfo.environment["KL_STEAM_APP_ID"] ?? ""), app != 0 else {
            status = "Sign in to configure Steam Cloud"; return
        }
        do {
            let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            var configuration = KLCloudConfiguration()
            if let json = Bundle.main.object(forInfoDictionaryKey: "KleptonSteamCloudConfiguration") as? String {
                configuration = try JSONDecoder().decode(KLCloudConfiguration.self, from: Data(json.utf8))
            }
            let paths = KLCloudPaths(documents: URL(fileURLWithPath: Paths.container),
                                     steam: support.appendingPathComponent("SteamRuntime"), account: id, app: app,
                                     configuration: configuration,
                                     package: Bundle.main.object(forInfoDictionaryKey: "KleptonGamePackage") as? String ?? "")
            let sync = KLCloudSync(transport: transport, paths: paths, support: support.appendingPathComponent("SteamCloud-v1"))
            sync.progress = { [weak self] message in
                self?.status = message; print("[steam-cloud] \(message)")
            }
            permissions = try sync.record().permissions; service = sync
            status = configuration.enabled ? (permissions.enabled ? "Ready to sync saves" : "Save uploads and downloads are off") : "This game does not use Steam Cloud"
        } catch { configurationError = error.localizedDescription; status = "Could not read Steam Cloud settings: \(error.localizedDescription)" }
    }
    func set(upload: Bool? = nil, download: Bool? = nil) {
        guard !locked, let service else { return }
        var next = permissions
        if let upload { next.upload = upload }; if let download { next.download = download }
        do {
            try service.setPermissions(next); permissions = next
            status = next.enabled ? "Saves will sync before launch" : "Save uploads and downloads are off"
        } catch { status = error.localizedDescription }
    }
    @discardableResult func sync(preferCloud: Bool = false, launching: Bool = false) async -> Bool {
        guard !syncing else { return false }
        refresh()
        if let configurationError { status = configurationError; return false }
        guard let service else { status = "Sign in to Steam before syncing saves."; return false }
        let expectedAccount = account
        syncing = true; defer { syncing = false }
        do {
            try await service.sync(preferCloud: preferCloud, launching: launching)
            if launching {
                guard transport.account == expectedAccount, cloudReleaseStartup(expectedAccount) != 0 else { throw KLCloudError("Steam startup is busy. Retry launch.") }
                defer { cloudCancelStartup(expectedAccount) }
                status = "Preparing the Steam game session…"
                let deadline = Date().addingTimeInterval(90)
                var result: Int32 = 0, previous: Int32 = -1
                var stage = steamTicketStatus(&result)
                while stage != 9 {
                    if stage != previous {
                        print("[steam-cloud] waiting for startup SDK stage=\(stage) result=\(result)")
                        previous = stage
                    }
                    guard Date() < deadline, transport.account == expectedAccount else { throw KLCloudError("Steam startup could not hand off to the game. Retry launch.") }
                    try await Task.sleep(for: .milliseconds(100))
                    stage = steamTicketStatus(&result)
                }
            }
            guard transport.account == expectedAccount else { throw KLCloudError("Steam changed accounts. Retry launch.") }
            return true
        } catch {
            status = "Launch paused: \(error.localizedDescription)"
            print("[steam-cloud] \(status)"); return false
        }
    }
    func uploadAfterStop() async {
        guard !syncing, let service, permissions.upload else { return }
        syncing = true; defer { syncing = false }
        do { try await service.sync(uploadOnly: true) }
        catch {
            status = "Save upload pending; it will retry next launch. \(error.localizedDescription)"
            print("[steam-cloud] \(status)")
        }
    }
}

struct KleptonSteamCloudPanel: View {
    @ObservedObject private var cloud = KleptonSteamCloud.shared
    @ObservedObject private var session = KleptonSession.shared
    @State private var confirmCloud = false
    var body: some View {
        GroupBox("Steam Cloud saves") {
            VStack(alignment: .leading, spacing: 10) {
                Toggle("Upload saves", isOn: Binding(get: { cloud.permissions.upload }, set: { cloud.set(upload: $0) }))
                Toggle("Download saves", isOn: Binding(get: { cloud.permissions.download }, set: { cloud.set(download: $0) }))
                Text("Both are off by default. Enabled saves sync before the game starts. Uploads also retry after an interrupted session.")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button("Sync saves") { Task { await cloud.sync() } }.disabled(!cloud.permissions.enabled)
                    Button("Use cloud saves") { confirmCloud = true }.disabled(!cloud.permissions.download)
                    if cloud.syncing { ProgressView() }
                }
                Text(cloud.status).font(.footnote).textSelection(.enabled)
            }
            .disabled(cloud.locked || cloud.account == 0)
        }
        .confirmationDialog("Replace conflicting local saves with Steam Cloud copies?", isPresented: $confirmCloud, titleVisibility: .visible) {
            Button("Use cloud saves") { Task { await cloud.sync(preferCloud: true) } }
        } message: { Text("Replaced local saves are backed up. This action only downloads saves.") }
    }
}
#endif
