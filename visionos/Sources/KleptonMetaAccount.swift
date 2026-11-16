#if os(visionOS)
import Foundation
import SwiftUI

@MainActor final class KleptonMetaGameSession: ObservableObject {
    let appID = Bundle.main.object(forInfoDictionaryKey: "KleptonMetaAppID") as? String ?? ""
    @Published var ready = false
    @Published var checking = false
    @Published var error = ""
    @Published var showSignIn = false
    @Published var accountName = "Meta account"
    var required: Bool {
#if KL_STEAM_GAME_HOST || KL_STEAM_DIAGNOSTICS
        return false
#else
        return KleptonMetaAPI.isID(appID)
#endif
    }
    private var attempted = false

    func prepare() async {
        guard required, !checking else { return }
        checking = true; error = ""
        defer { checking = false }
        // Import any legacy game session before checking for it. Only this
        // game's token is exposed to the C provider, never the profile token.
        kl_meta_account_install()
        do {
            let profile = KleptonMetaKeychain.load()
            let token: String
            if let profileToken = profile["profile_token"] {
                token = try await KleptonMetaAPI.gameToken(profile: profileToken, app: appID)
                accountName = profile["account_name"] ?? "Meta account"
            } else {
                let game = KleptonMetaKeychain.load(service: KleptonMetaKeychain.gameService, account: appID)
                guard let saved = game["access_token"] else {
                    ready = false
                    if !attempted { showSignIn = true }
                    attempted = true
                    return
                }
                token = saved
            }
            // A cached token alone is not proof of a valid session. Confirm it
            // with Meta, renewing from the profile when one is available.
            _ = try await KleptonMetaAPI.call("https://graph.oculus.com/me?fields=id", token: token)
            try KleptonMetaKeychain.save(["app_id": appID, "access_token": token],
                service: KleptonMetaKeychain.gameService, account: appID)
            kl_meta_account_install()
            ready = true; attempted = true
        } catch {
            ready = false; attempted = true; self.error = error.localizedDescription
        }
    }
    func signedIn(_ profile: [String: String]) async {
        showSignIn = false
        // Never leave a previous user's game session usable after switching.
        ready = false
        do { try KleptonMetaKeychain.remove(service: KleptonMetaKeychain.gameService, account: appID) }
        catch { self.error = error.localizedDescription; return }
        await prepare()
    }
    func signOut() {
        do {
            try KleptonMetaKeychain.remove()
            try KleptonMetaKeychain.remove(service: KleptonMetaKeychain.gameService, account: appID)
            ready = false; error = ""; accountName = "Meta account"
        } catch { self.error = error.localizedDescription }
    }
}
#endif
