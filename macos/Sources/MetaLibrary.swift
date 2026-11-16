import SwiftUI

struct MetaBuild: Decodable {
    let buildId: String
    let version: String
    let versionCode: Int
}
struct MetaDownload: Decodable {
    let buildId: String
    let version: String
    let versionCode: Int
    let downloadedAt: String
    let valid: Bool
}
struct MetaLibraryGame: Decodable, Identifiable {
    var id: String { appId }
    let appId: String
    let name: String
    let latest: MetaBuild?
    let downloaded: MetaDownload?
    var remoteKnown: Bool
    let owned: Bool?
}

struct MetaLibrarySection: View {
    @ObservedObject var model: InstallerModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label(model.metaAccount.isEmpty ? "Your Meta Quest library" : model.metaAccount, systemImage: "person.crop.circle")
                Spacer()
                if !model.metaAccount.isEmpty {
                    Button("Refresh") { model.metaAction("refresh") }.disabled(model.busy)
                    Button("Sign Out") { model.signOutMeta() }.disabled(model.busy)
                }
                Button(model.metaAccount.isEmpty ? "Sign in to Meta…" : "Sign in again…") { model.showMetaLogin = true }
                    .disabled(model.busy).accessibilityIdentifier("metaLibrarySignIn")
            }
            TextField("Search your Quest games", text: $model.metaSearch).textFieldStyle(.roundedBorder)
            if model.metaGames.isEmpty {
                Text(model.metaAccount.isEmpty ? "Sign in to see the Quest games on your account." : "Refresh to fetch your Meta Quest library.")
                    .foregroundStyle(.secondary).padding(.vertical, 8)
            } else {
                List(selection: $model.metaGameID) {
                    ForEach(model.filteredMetaLibrary) { game in
                        HStack {
                            Text(game.name)
                            Spacer()
                            if let local = game.downloaded {
                                Text(!local.valid ? "Needs repair" : game.remoteKnown && game.latest?.buildId != local.buildId ? "Update available" : "Downloaded")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }.tag(game.id)
                    }
                }.frame(minHeight: 120, maxHeight: 220).accessibilityIdentifier("metaLibraryGames")
            }
            if let game = model.selectedMetaGame {
                VStack(alignment: .leading, spacing: 6) {
                    Text(game.name).font(.headline)
                    if let latest = game.latest {
                        Text("\(game.remoteKnown ? "Latest available" : "Last known"): \(latest.version) (\(latest.versionCode))")
                    } else { Text("Latest version unavailable").foregroundStyle(.secondary) }
                    if let local = game.downloaded {
                        Text("Downloaded: \(local.version) (\(local.versionCode))" + (local.valid ? "" : " · Missing or damaged files"))
                        if let date = ISO8601DateFormatter().date(from: local.downloadedAt) {
                            Text(date.formatted(date: .abbreviated, time: .shortened)).foregroundStyle(.secondary)
                        }
                    }
                    if game.owned == false { Text("This account no longer reports an active entitlement.").foregroundStyle(.secondary) }
                    HStack {
                        Button(model.metaDownloadAction) {
                            model.metaAction(model.metaDownloadAction == "Use Downloaded" ? "use" : "download")
                        }.buttonStyle(.borderedProminent)
                            .disabled(model.busy || (model.metaDownloadAction != "Use Downloaded" && (!game.remoteKnown || game.owned == false)))
                        if game.downloaded?.valid == true && model.metaDownloadAction == "Update" {
                            Button("Use Downloaded") { model.metaAction("use") }.disabled(model.busy)
                        }
                    }
                }.font(.callout)
            }
            if let checked = model.metaCheckedAt {
                Text("\(model.metaRemoteKnown ? "Checked" : "Saved library from") \(checked)\(model.metaRemoteKnown ? "" : " · Refresh to check for updates")")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Text("Downloads include the APK, OBB and listed game assets. Game compatibility depends on Klepton’s runtime support.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}
