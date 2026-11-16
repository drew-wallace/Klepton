import Foundation
import CryptoKit

@main struct MetaSignInTests {
    @MainActor static func main() async throws {
        var calls: [String] = []
        var saved: [String: String] = [:]
        var completed = false
        let challenge = "test-challenge"
        let model = KleptonMetaSignInModel(api: { endpoint, token, values in
            calls.append(endpoint)
            if endpoint.hasSuffix("webview_tokens_query") { return ["native_sso_token": challenge, "native_sso_etoken": "opaque"] }
            if endpoint.hasSuffix("webview_blobs_decrypt") {
                precondition(values?["request_token"] == challenge && values?["blob"] == "encrypted")
                return ["access_token": "account-fixture"]
            }
            if endpoint.hasSuffix("graphql") {
                precondition(token == "account-fixture" && values?["doc_id"] == "24112177345042346")
                return ["data": ["xfr_create_profile_token": ["profile_tokens": [["access_token": "profile-fixture"]]]]]
            }
            precondition(endpoint == "https://graph.oculus.com/me?fields=id,alias" && token == "profile-fixture")
            return ["id": "123456", "alias": "Example"]
        }, save: { saved = $0 })
        model.onSuccess = { session in precondition(session["account_id"] == "123456"); completed = true }
        model.start()
        for _ in 0..<100 where model.url == nil { try await Task.sleep(for: .milliseconds(5)) }
        precondition(model.url?.host == "auth.meta.com")
        model.callback(URL(string: "oculus://callback?token=wrong&blob=encrypted")!)
        precondition(calls.count == 1 && saved.isEmpty && !completed, "Unmatched callback must not exchange or save credentials")
        let state = SHA256.hash(data: Data(challenge.utf8)).map { String(format: "%02x", $0) }.joined().prefix(16)
        model.callback(URL(string: "oculus://callback?token=\(state)&blob=encrypted")!)
        for _ in 0..<100 where !completed { try await Task.sleep(for: .milliseconds(5)) }
        precondition(completed && calls.count == 4)
        precondition(saved == ["profile_token": "profile-fixture", "account_id": "123456", "account_name": "Example"], "Only the Quest profile belongs in the persistent session")
        model.callback(URL(string: "oculus://callback?token=\(state)&blob=encrypted")!)
        precondition(calls.count == 4, "A callback cannot be replayed")

        var cancelledSaved = false
        let cancelled = KleptonMetaSignInModel(api: { _, _, _ in
            try? await Task.sleep(for: .milliseconds(20))
            return ["native_sso_token": challenge, "native_sso_etoken": "opaque"]
        }, save: { _ in cancelledSaved = true })
        cancelled.start(); cancelled.cancel()
        try await Task.sleep(for: .milliseconds(40))
        precondition(cancelled.url == nil && !cancelledSaved, "A cancelled challenge must not reopen or save a session")
        precondition(String(data: KleptonMetaAPI.form(["blob": "a+b&c=d"]), encoding: .utf8) == "blob=a%2Bb%26c%3Dd")
        print("Meta sign-in checks passed (callback binding, profile isolation, replay rejection, cancellation, form encoding).")
    }
}
