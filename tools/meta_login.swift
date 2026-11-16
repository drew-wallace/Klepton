import AppKit
import WebKit
import CryptoKit
import Security

// Experimental interoperability probe for Meta native SSO. Endpoints were
// checked against quest-downloader/src/meta_auth.rs; no passwords are handled.
// Live game-scoped identity and proof still require account validation.
private func option(_ key: String) -> String? {
    guard let i=CommandLine.arguments.firstIndex(of:key), i+1<CommandLine.arguments.count else { return nil }
    return CommandLine.arguments[i+1]
}

@MainActor final class MetaSignIn: NSObject, NSApplicationDelegate, WKNavigationDelegate, WKUIDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var web: WKWebView!
    private var label: NSTextField!
    private var requestToken = ""
    private var completed = false
    private let game = option("--app-id") ?? "2370815932930055"
    private let client = "FRL|512466987071624|01d4a1f7fd0682aea7ee8ae987704d63"
    private let session = URLSession(configuration: .ephemeral)
    private let resultURL = URL(fileURLWithPath: option("--result") ?? "/tmp/klepton-meta-sign-in-result.json")

    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 940, height: 790), styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = "Klepton — Meta sign-in for Vegas Infinite"
        window.delegate = self
        let root = NSView(); window.contentView = root
        label = NSTextField(wrappingLabelWithString: "Connecting to Meta…")
        label.font = .systemFont(ofSize: 14); label.translatesAutoresizingMaskIntoConstraints = false
        let config = WKWebViewConfiguration(); config.websiteDataStore = .nonPersistent()
        web = WKWebView(frame: .zero, configuration: config)
        web.navigationDelegate = self; web.uiDelegate = self; web.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(label); root.addSubview(web)
        NSLayoutConstraint.activate([
            label.topAnchor.constraint(equalTo: root.topAnchor, constant: 12), label.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 16), label.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -16),
            web.topAnchor.constraint(equalTo: label.bottomAnchor, constant: 12), web.leadingAnchor.constraint(equalTo: root.leadingAnchor), web.trailingAnchor.constraint(equalTo: root.trailingAnchor), web.bottomAnchor.constraint(equalTo: root.bottomAnchor)
        ])
        window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        Task { await begin() }
    }
    private func form(_ values: [String:String]) -> Data {
        var c = URLComponents(); c.queryItems = values.sorted { $0.key < $1.key }.map { URLQueryItem(name:$0.key,value:$0.value) }
        return Data((c.percentEncodedQuery ?? "").replacingOccurrences(of:"+",with:"%2B").utf8)
    }
    private func post(_ endpoint: String, _ values: [String:String], step: String) async throws -> [String:Any] {
        var r = URLRequest(url: URL(string:endpoint)!); r.httpMethod = "POST"; r.timeoutInterval = 30
        r.setValue("application/x-www-form-urlencoded", forHTTPHeaderField:"Content-Type"); r.httpBody = form(values)
        let (data,response) = try await session.data(for:r)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        let parsed = (try? JSONSerialization.jsonObject(with:data)) as? [String:Any] ?? [:]
        if !(200..<300).contains(status) || parsed["error"] != nil || parsed["errors"] != nil {
            let code = (parsed["error"] as? [String:Any])?["code"] as? Int ?? 0
            throw NSError(domain:"MetaSignIn",code:status,userInfo:[NSLocalizedDescriptionKey:"\(step) was rejected (HTTP \(status), code \(code))."])
        }
        return parsed
    }
    private func string(_ json: [String:Any], _ key: String) throws -> String {
        guard let s = json[key] as? String, !s.isEmpty else { throw NSError(domain:"MetaSignIn",code:1,userInfo:[NSLocalizedDescriptionKey:"Meta returned no \(key)."])}
        return s
    }
    private func begin() async {
        do {
            let challenge = try await post("https://meta.graph.meta.com/webview_tokens_query",["access_token":client],step:"Sign-in challenge")
            requestToken = try string(challenge,"native_sso_token")
            let etoken = try string(challenge,"native_sso_etoken")
            var u = URLComponents(string:"https://auth.meta.com/native_sso/confirm")!
            u.queryItems = [URLQueryItem(name:"native_app_id",value:"512466987071624"),URLQueryItem(name:"source_app_id",value:"512466987071624"),URLQueryItem(name:"native_sso_etoken",value:etoken)]
            label.stringValue = "Sign in on Meta’s website below. Your password stays with Meta. Klepton will test access for Vegas Infinite."
            web.load(URLRequest(url:u.url!))
            report(["stage":"awaiting_meta_sign_in"])
        } catch { fail(error) }
    }
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy)->Void) {
        guard let url = action.request.url else { decisionHandler(.cancel);return }
        if ["oculus","oculus-client"].contains(url.scheme ?? "") {
            decisionHandler(.cancel)
            if !completed { completed = true; Task { await finish(url) } }
        } else if url.scheme == "https" || url.absoluteString == "about:blank" { decisionHandler(.allow) }
        else { decisionHandler(.cancel) }
    }
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if action.targetFrame == nil { webView.load(action.request) }; return nil
    }
    private func finish(_ callback: URL) async {
        do {
            let parts = URLComponents(url:callback,resolvingAgainstBaseURL:false)?.queryItems ?? []
            func item(_ key:String)->String { parts.first{$0.name==key}?.value ?? "" }
            let expected = SHA256.hash(data:Data(requestToken.utf8)).map{String(format:"%02x",$0)}.joined().prefix(16)
            guard item("token") == expected, !item("blob").isEmpty else { throw NSError(domain:"MetaSignIn",code:2,userInfo:[NSLocalizedDescriptionKey:"Meta sign-in callback did not match this session."]) }
            label.stringValue = "Signed in. Checking Vegas Infinite’s platform access…"
            web.loadHTMLString("<p style='font:18px system-ui;margin:40px'>Checking the game’s Meta account connection…</p>",baseURL:nil)
            let decrypted = try await post("https://meta.graph.meta.com/webview_blobs_decrypt",["access_token":client,"blob":item("blob"),"request_token":requestToken],step:"Sign-in completion")
            let account = try string(decrypted,"access_token")
            let vars = String(data:try JSONSerialization.data(withJSONObject:["app_id":"1582076955407037"]),encoding:.utf8)!
            let profile = try await post("https://meta.graph.meta.com/graphql",["access_token":account,"doc_id":"24112177345042346","variables":vars],step:"Quest profile")
            guard let data=profile["data"] as? [String:Any], let create=data["xfr_create_profile_token"] as? [String:Any], let tokens=create["profile_tokens"] as? [[String:Any]], let first=tokens.first else { throw NSError(domain:"MetaSignIn",code:3,userInfo:[NSLocalizedDescriptionKey:"Meta returned no Quest profile token."]) }
            let profileToken=try string(first,"access_token")
            let scoped=try await post("https://graph.oculus.com/authenticate_application?app_id="+game,["access_token":profileToken],step:"Vegas application access")
            let gameToken=try string(scoped,"access_token")
            var record:[String:Any] = ["app_id":game,"access_token":gameToken]
            if let expires=scoped["expires_in"] { record["expires_in"]=expires }
            let stored=try JSONSerialization.data(withJSONObject:record)
            let query:[String:Any]=[kSecClass as String:kSecClassGenericPassword,kSecAttrService as String:"dev.klepton.meta.vegas",kSecAttrAccount as String:game]
            let attrs:[String:Any]=[kSecValueData as String:stored,kSecAttrAccessible as String:kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
            var status=SecItemUpdate(query as CFDictionary,attrs as CFDictionary)
            if status == errSecItemNotFound { status=SecItemAdd(query.merging(attrs){_,new in new} as CFDictionary,nil) }
            guard status == errSecSuccess else { throw NSError(domain:"MetaSignIn",code:Int(status),userInfo:[NSLocalizedDescriptionKey:"Could not store the game session in Keychain."]) }
            label.stringValue="Meta granted a Vegas-scoped session. Stored in this Mac’s Keychain."
            report(["stage":"vegas_token_received","keychain_saved":true])
        } catch { fail(error) }
    }
    private func report(_ values:[String:Any]) {
        if let data=try? JSONSerialization.data(withJSONObject:values,options:[.prettyPrinted,.sortedKeys]) { try? data.write(to:resultURL,options:.atomic) }
    }
    private func fail(_ error:Error) { label.stringValue=error.localizedDescription; report(["stage":"error","message":error.localizedDescription]) }
    func windowWillClose(_ notification:Notification) { if !completed {report(["stage":"cancelled"])}; NSApp.terminate(nil) }
}
@main struct Entry {
    @MainActor static func main() {
        if let path=option("--export") {
            let appID=option("--app-id") ?? "2370815932930055"
            let query:[String:Any]=[kSecClass as String:kSecClassGenericPassword,
                kSecAttrService as String:"dev.klepton.meta.vegas",kSecAttrAccount as String:appID,
                kSecReturnData as String:true,kSecMatchLimit as String:kSecMatchLimitOne]
            var value:CFTypeRef?
            guard SecItemCopyMatching(query as CFDictionary,&value)==errSecSuccess,
                  let data=value as? Data,
                  let session=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any],
                  session["app_id"] as? String==appID,
                  let token=session["access_token"] as? String,!token.isEmpty else {
                fputs("No game session is stored in Keychain. Complete Meta sign-in first.\n",stderr);exit(1)
            }
            do {
                let out=try JSONSerialization.data(withJSONObject:["app_id":appID,"access_token":token])
                let url=URL(fileURLWithPath:path)
                guard FileManager.default.createFile(atPath:url.path,contents:nil,attributes:[.posixPermissions:0o600]) else {throw CocoaError(.fileWriteUnknown)}
                try out.write(to:url)
                try FileManager.default.setAttributes([.posixPermissions:0o600],ofItemAtPath:url.path)
            } catch { fputs("Could not export the session into a private transfer file.\n",stderr);exit(1) }
            return
        }
        let app = NSApplication.shared
        let delegate = MetaSignIn()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.run()
        withExtendedLifetime(delegate) {}
    }
}
