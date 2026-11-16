import SwiftUI
import WebKit
import CryptoKit
import Security

// Shared Mac/headset Meta native SSO. Passwords are entered only in Meta's
// nonpersistent web view. Profile and game sessions stay in device-only Keychain.
enum KleptonMetaError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

enum KleptonMetaKeychain {
    static var service: String { (Bundle.main.bundleIdentifier ?? "dev.klepton.installer") + ".meta-login" }
    static func load(service: String = service, account: String = "profile") -> [String: String] {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
            kSecAttrAccount as String: account, kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess, let data = item as? Data,
              let value = try? JSONDecoder().decode([String: String].self, from: data) else { return [:] }
        return value
    }
    static func save(_ value: [String: String], service: String = service, account: String = "profile") throws {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: account]
        let attrs: [String: Any] = [kSecValueData as String: try JSONEncoder().encode(value),
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        var status = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if status == errSecItemNotFound { status = SecItemAdd(query.merging(attrs) { _, new in new } as CFDictionary, nil) }
        guard status == errSecSuccess else { throw KleptonMetaError.message("Could not save the Meta session in Keychain (\(status)).") }
    }
    static func remove(service: String = service, account: String = "profile") throws {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: account]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw KleptonMetaError.message("Could not remove the Meta session from Keychain (\(status)).")
        }
    }
    static var gameService: String { (Bundle.main.bundleIdentifier ?? "dev.klepton.installer") + ".meta-account" }
}

private final class KleptonMetaHTTP: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

enum KleptonMetaAPI {
    static let client = "FRL|512466987071624|01d4a1f7fd0682aea7ee8ae987704d63"
    static func form(_ values: [String: String]) -> Data {
        var parts = URLComponents()
        parts.queryItems = values.sorted { $0.key < $1.key }.map { URLQueryItem(name: $0.key, value: $0.value) }
        return Data((parts.percentEncodedQuery ?? "").replacingOccurrences(of: "+", with: "%2B").utf8)
    }
    static func call(_ endpoint: String, token: String, values: [String: String]? = nil) async throws -> [String: Any] {
        guard let url = URL(string: endpoint), url.scheme == "https", ["meta.graph.meta.com", "graph.oculus.com"].contains(url.host ?? "") else {
            throw KleptonMetaError.message("Invalid Meta service address.")
        }
        var request = URLRequest(url: url)
        request.timeoutInterval = 30
        request.setValue("Bearer " + token, forHTTPHeaderField: "Authorization")
        if let values {
            request.httpMethod = "POST"
            request.httpBody = form(values)
            request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        }
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil; config.urlCache = nil
        let session = URLSession(configuration: config, delegate: KleptonMetaHTTP(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        let data: Data; let response: URLResponse
        do { (data, response) = try await session.data(for: request) }
        catch { throw KleptonMetaError.message("Could not reach Meta. Check your connection and try again.") }
        try Task.checkCancellation()
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard data.count <= 4*1024*1024,
              let result = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw KleptonMetaError.message("Meta returned an invalid response.")
        }
        guard (200..<300).contains(status), result["error"] == nil, result["errors"] == nil else {
            throw KleptonMetaError.message("Meta rejected this session (HTTP \(status)). Please sign in again.")
        }
        return result
    }
    static func string(_ object: [String: Any], _ key: String) throws -> String {
        guard let value = object[key] as? String, !value.isEmpty, value.count < 16384 else {
            throw KleptonMetaError.message("Meta returned an incomplete sign-in response.")
        }
        return value
    }
    static func gameToken(profile: String, app: String) async throws -> String {
        guard isID(app) else { throw KleptonMetaError.message("The game has no valid Meta application ID.") }
        let result = try await call("https://graph.oculus.com/authenticate_application?app_id=" + app, token: profile, values: [:])
        return try string(result, "access_token")
    }
    static func isID(_ text: String) -> Bool {
        !text.isEmpty && text.count <= 40 && text.utf8.allSatisfy { $0 >= 48 && $0 <= 57 }
    }
}

@MainActor final class KleptonMetaSignInModel: ObservableObject {
    @Published var url: URL?
    @Published var message = "Connecting to Meta…"
    @Published var error = ""
    @Published var host = "auth.meta.com"
    private var challenge = ""
    private var handling = false
    private var task: Task<Void, Never>?
    var onSuccess: ([String: String]) -> Void = { _ in }
    typealias APICall = (String, String, [String: String]?) async throws -> [String: Any]
    private let api: APICall
    private let save: ([String: String]) throws -> Void
    init(api: @escaping APICall = { endpoint, token, values in
        try await KleptonMetaAPI.call(endpoint, token: token, values: values)
    }, save: @escaping ([String: String]) throws -> Void = { try KleptonMetaKeychain.save($0) }) {
        self.api = api; self.save = save
    }

    func start() {
        task?.cancel(); challenge = ""; handling = false; url = nil; error = ""
        message = "Connecting to Meta…"; host = "auth.meta.com"
        task = Task {
            do {
                let value = try await api("https://meta.graph.meta.com/webview_tokens_query", KleptonMetaAPI.client, [:])
                try Task.checkCancellation()
                challenge = try KleptonMetaAPI.string(value, "native_sso_token")
                let etoken = try KleptonMetaAPI.string(value, "native_sso_etoken")
                var parts = URLComponents(string: "https://auth.meta.com/native_sso/confirm")!
                parts.queryItems = [URLQueryItem(name: "native_app_id", value: "512466987071624"),
                    URLQueryItem(name: "source_app_id", value: "512466987071624"), URLQueryItem(name: "native_sso_etoken", value: etoken)]
                url = parts.url
                message = "Sign in on Meta’s page. Your password stays with Meta."
            } catch { if !Task.isCancelled { self.error = error.localizedDescription } }
        }
    }
    func cancel() { task?.cancel(); challenge = ""; url = nil }
    func callback(_ url: URL) {
        guard !handling, !challenge.isEmpty else { return }
        let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
        let expected = SHA256.hash(data: Data(challenge.utf8)).map { String(format: "%02x", $0) }.joined().prefix(16)
        guard items.first(where: { $0.name == "token" })?.value == String(expected),
              let blob = items.first(where: { $0.name == "blob" })?.value, !blob.isEmpty else {
            error = "The Meta callback did not match this sign-in. Start again."; return
        }
        handling = true; error = ""; self.url = nil; message = "Finishing Meta sign-in…"
        let requestToken = challenge
        task = Task {
            do {
                let decrypted = try await api("https://meta.graph.meta.com/webview_blobs_decrypt", KleptonMetaAPI.client, ["blob": blob, "request_token": requestToken])
                let accountToken = try KleptonMetaAPI.string(decrypted, "access_token")
                let profile = try await api("https://meta.graph.meta.com/graphql", accountToken, ["doc_id": "24112177345042346", "variables": "{\"app_id\":\"1582076955407037\"}"])
                guard let data = profile["data"] as? [String: Any], let created = data["xfr_create_profile_token"] as? [String: Any],
                      let profiles = created["profile_tokens"] as? [[String: Any]], let first = profiles.first else {
                    throw KleptonMetaError.message("Meta returned no Quest profile. Check that this account has a Quest profile.")
                }
                let profileToken = try KleptonMetaAPI.string(first, "access_token")
                let me = try await api("https://graph.oculus.com/me?fields=id,alias", profileToken, nil)
                let accountID = try KleptonMetaAPI.string(me, "id")
                guard KleptonMetaAPI.isID(accountID) else { throw KleptonMetaError.message("Meta returned an invalid account ID.") }
                try Task.checkCancellation()
                let session = ["profile_token": profileToken, "account_id": accountID, "account_name": me["alias"] as? String ?? "Meta account"]
                try save(session)
                challenge = ""; message = "Signed in to Meta."
                onSuccess(session)
            } catch { if !Task.isCancelled { self.error = error.localizedDescription } }
        }
    }
}

private final class KleptonMetaWebDelegate: NSObject, WKNavigationDelegate, WKUIDelegate {
    let model: KleptonMetaSignInModel
    init(_ model: KleptonMetaSignInModel) { self.model = model }
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        guard let url = action.request.url else { decisionHandler(.cancel); return }
        if ["oculus", "oculus-client"].contains(url.scheme ?? "") {
            decisionHandler(.cancel); model.callback(url)
        } else if url.scheme == "https" || url.absoluteString == "about:blank" {
            if action.targetFrame?.isMainFrame != false { model.host = url.host ?? "Meta" }
            decisionHandler(.allow)
        } else { decisionHandler(.cancel) }
    }
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if action.targetFrame == nil { webView.load(action.request) }; return nil
    }
}

@MainActor private struct KleptonMetaWebView {
    let model: KleptonMetaSignInModel
    func makeCoordinator() -> KleptonMetaWebDelegate { KleptonMetaWebDelegate(model) }
    func makeWeb(_ delegate: KleptonMetaWebDelegate) -> WKWebView {
        let config = WKWebViewConfiguration(); config.websiteDataStore = .nonPersistent()
        let web = WKWebView(frame: .zero, configuration: config)
        web.navigationDelegate = delegate; web.uiDelegate = delegate
        if let url = model.url { web.load(URLRequest(url: url)) }
        return web
    }
}
#if os(macOS)
extension KleptonMetaWebView: NSViewRepresentable {
    func makeNSView(context: Context) -> WKWebView { makeWeb(context.coordinator) }
    func updateNSView(_ view: WKWebView, context: Context) {}
}
#else
extension KleptonMetaWebView: UIViewRepresentable {
    func makeUIView(context: Context) -> WKWebView { makeWeb(context.coordinator) }
    func updateUIView(_ view: WKWebView, context: Context) {}
}
#endif

struct KleptonMetaSignInView: View {
    @StateObject private var model = KleptonMetaSignInModel()
    let onSuccess: ([String: String]) -> Void
    let onCancel: () -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack { Text("Sign in to Meta").font(.title2); Spacer(); Button("Cancel") { model.cancel(); onCancel() } }
            Text(model.message)
            if !model.error.isEmpty {
                Text(model.error).foregroundStyle(.red)
                Button("Try Again") { model.start() }
            }
            if model.url != nil {
                Label(model.host, systemImage: "lock").font(.caption)
                KleptonMetaWebView(model: model).id(model.url).frame(minHeight: 480)
            } else if model.error.isEmpty { ProgressView() }
        }.padding(24).frame(minWidth: 650, minHeight: 620)
        .onAppear { model.onSuccess = onSuccess; model.start() }
        .onDisappear { model.cancel() }
    }
}
