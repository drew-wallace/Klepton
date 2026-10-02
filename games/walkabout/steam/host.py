"""Walkabout host lifecycle for the audited local Steam integration."""
HOST_SWIFT = '''import SwiftUI
import Foundation
@_silgen_name("kl_steam_probe_run")
func probe(_ library: UnsafePointer<CChar>, _ interfaces: Int32) -> Int32
@MainActor enum SteamHostLifecycle {
    static let model = SteamLoginModel()
    private static var started = false
    private static var worker: Task<Int32, Never>?
    static func start() {
        guard !started else { return }
        started = true
        captureSteamHostContext()
        worker = Task.detached(priority: .userInitiated) {
            let data = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("SteamRuntime", isDirectory: true)
            do { try FileManager.default.createDirectory(at: data, withIntermediateDirectories: true) }
            catch { steamSessionCancel(); return 3 }
            freopen(data.appendingPathComponent("steam-runtime.log").path, "w", stdout)
            dup2(fileno(stdout), STDERR_FILENO); setbuf(stdout, nil); setbuf(stderr, nil)
            setenv("KL_DYLIB_DIR", Bundle.main.privateFrameworksPath!, 1)
            setenv("KL_STEAM_PROBE_RUN_ID", UUID().uuidString, 1)
            setenv("KL_STEAM_PROBE_DATA", data.path, 1)
            setenv("KL_STEAM_DATA_ROOT", data.path, 1)
            setenv("KL_STEAM_WALKABOUT_API", "1", 1)
            guard chdir(data.path) == 0 else { steamSessionCancel(); return 3 }
            return "libsteamclient_backend.so".withCString { probe($0, 4) }
        }
    }
}
'''
