import WebKit
import Security
import CoreImage.CIFilterBuiltins
@_silgen_name("kl_steam_session_status")
func steamSessionStatus(_ result: UnsafeMutablePointer<Int32>) -> Int32
@_silgen_name("kl_steam_session_submit")
func steamSessionSubmit(_ token: UnsafePointer<CChar>, _ account: UnsafePointer<CChar>) -> Int32
@_silgen_name("kl_steam_ticket_status")
func steamTicketStatus(_ result: UnsafeMutablePointer<Int32>) -> Int32
@_silgen_name("kl_steam_ticket_retry")
func steamTicketRetry() -> Int32
@_silgen_name("kl_steam_session_cancel")
func steamSessionCancel()

// Only refresh token + account name are reusable. Passwords, Guard codes,
// challenge URLs and access tokens are never persisted by this controller.
private enum SteamLoginKeychain {
    static var query: [String: Any] { [kSecClass as String: kSecClassGenericPassword,
        kSecAttrService as String: Bundle.main.bundleIdentifier! + ".steam-login",
        kSecAttrAccount as String: "local-steam-session"] }
    static func load() -> [String: String]? {
        var request = query; request[kSecReturnData as String] = true
        request[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(request as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return try? JSONDecoder().decode([String: String].self, from: data)
    }
    static func save(_ credentials: [String: String]) -> Bool {
        guard let data = try? JSONEncoder().encode(credentials) else { return false }
        let attributes: [String: Any] = [kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        let result = SecItemUpdate(query as CFDictionary, attributes as CFDictionary)
        if result == errSecItemNotFound {
            return SecItemAdd(query.merging(attributes) { _, new in new } as CFDictionary, nil) == errSecSuccess
        }
        return result == errSecSuccess
    }
    static func delete() { SecItemDelete(query as CFDictionary) }
}
@MainActor final class SteamLoginModel: ObservableObject {
    @Published var ticketStatus = ""
    @Published var codeLogin = false
    @Published var accountName = ""
    @Published var password = ""
    @Published var guardCode = ""
    @Published var guardRequired = false
    @Published var guardDeviceAvailable = false
    @Published var guardEmailAvailable = false
    @Published var guardType = 3
    @Published var submittingCode = false
    @Published var ticketStage: Int32 = 0
    @Published var nativeState: Int32 = 0
    @Published var status = "Starting local Steam backend…"
    @Published var qrImage: UIImage?
    @Published var modulesReady = false
    @Published var waiting = false
    @Published var savedAvailable = SteamLoginKeychain.load() != nil
    weak var webView: WKWebView?
    private var pendingCredentials: [String: String]?
    private var stopped = false
    private var automaticStarted = false
    private var smokeStopAt: Date?
    func startQR() {
        guard nativeState == 1, modulesReady, !waiting else { return }
        waiting = true
        webView?.evaluateJavaScript("void KleptonSteamLogin.start()", completionHandler: { [weak self] _, error in
            if error != nil { self?.status = "Steam login could not start."; self?.waiting = false }
        })
    }
    func selectLoginMode(_ codes: Bool) {
        guard nativeState == 1 else { return }
        webView?.evaluateJavaScript("void KleptonSteamLogin.cancel()", completionHandler: nil)
        codeLogin = codes; waiting = false; qrImage = nil
        guardRequired = false; password = ""; guardCode = ""; submittingCode = false
        status = codes ? "Sign in with your Steam account, then enter a Steam Guard code." : "Sign in by scanning the QR code with Steam Guard."
    }
    func startCodeLogin() {
        guard nativeState == 1, modulesReady, !waiting, !accountName.isEmpty, !password.isEmpty else { return }
        waiting = true; guardRequired = false
        let arguments = ["account": accountName, "password": password]
        password = ""
        webView?.callAsyncJavaScript("void KleptonSteamLogin.startCredentials(account, password);", arguments: arguments,
            in: nil, in: .page, completionHandler: { [weak self] result in
                if case .failure = result { self?.waiting = false; self?.status = "Steam sign-in could not start." }
            })
    }
    func submitCode() {
        let code = guardCode.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        guard guardRequired, !submittingCode, code.count == 5,
              code.unicodeScalars.allSatisfy({ CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").contains($0) }) else { return }
        submittingCode = true; guardCode = ""
        webView?.callAsyncJavaScript("void KleptonSteamLogin.submitGuardCode(code, type);",
            arguments: ["code": code, "type": guardType], in: nil, in: .page,
            completionHandler: { [weak self] result in
                if case .failure = result { self?.submittingCode = false; self?.status = "Code could not be sent. Try again." }
            })
    }
    func retryTickets() {
        if steamTicketRetry() == 1 { ticketStatus = "Retrying Steam startup and ticket requests…" }
    }
    func useSaved() {
        guard nativeState == 1, let saved = SteamLoginKeychain.load(),
              let token = saved["refreshToken"], let account = saved["accountName"] else { return }
        print("[steam-probe] saved Steam login selected=1")
        accept(token: token, account: account)
    }
    func accept(token: String, account: String) {
        guard nativeState == 1, token.utf8.count >= 20, token.utf8.count <= 16384,
              !account.isEmpty, account.utf8.count <= 64,
              !token.contains("\0"), !account.contains("\0") else { return }
        let accepted = token.withCString { value in account.withCString { steamSessionSubmit(value, $0) } }
        print("[steam-probe] native token mailbox accepted=\(accepted)")
        qrImage = nil; waiting = false; guardRequired = false; password = ""; guardCode = ""; accountName = ""
        guard accepted == 1 else { status = "Native Steam backend rejected the token handoff."; return }
        pendingCredentials = ["refreshToken": token, "accountName": account]
        status = "Connecting the approved session to the local Steam backend…"
    }
    func refresh() {
#if KL_STEAM_GAME_HOST
        if !automaticStarted, nativeState == 1 {
            if savedAvailable { automaticStarted = true; useSaved() }
            else if modulesReady, !codeLogin { automaticStarted = true; startQR() }
        }
#endif
        if ProcessInfo.processInfo.environment["KL_STEAM_AUTH_SAVED"] == "1", !automaticStarted,
           nativeState == 1 {
            automaticStarted = true
            useSaved()
        }
        if ProcessInfo.processInfo.environment["KL_STEAM_AUTH_SMOKE"] == "1", !automaticStarted,
           nativeState == 1, modulesReady {
            automaticStarted = true; smokeStopAt = Date().addingTimeInterval(20)
            startQR()
        }
        if let deadline = smokeStopAt, Date() >= deadline { smokeStopAt = nil; stop() }
        var ticketResult: Int32 = 0
        ticketStage = steamTicketStatus(&ticketResult)
        switch ticketStage {
        case 1: ticketStatus = "Walkabout Steam API loaded."
        case 2: ticketStatus = "Walkabout Steam API initialization failed (result \(ticketResult)); waiting for Steam account data…"
        case 3: ticketStatus = "Walkabout Steam API interface or app context mismatch."
        case 4: ticketStatus = "Walkabout Steam API initialized. Tickets require a logged-on owning account."
        case 5: ticketStatus = "Waiting for Steam to confirm Walkabout ownership…"
        case 6: ticketStatus = "Waiting for Steam tickets; temporary failures retry automatically for up to five minutes…"
        case 7: ticketStatus = "Both tickets delivered by Steam. Server verification and PlayFab remain untested."
        case 8: ticketStatus = "Steam startup could not finish. Check your connection, then retry tickets here."
        case 9: ticketStatus = "Steam is ready for Walkabout."
        default: break
        }
        var result: Int32 = 0
        let next = steamSessionStatus(&result)
        if next != nativeState {
            nativeState = next
            switch next {
            case 1: status = "Local backend ready. Sign in with Steam Guard on your phone."
            case 2: status = "Steam backend connecting…"
            case 3:
                status = "Signed in to Steam. Preparing Walkabout…"
                if let credentials = pendingCredentials {
                    let saved = SteamLoginKeychain.save(credentials)
                    print("[steam-probe] authenticated refresh token Keychain saved=\(saved ? 1 : 0)")
                    savedAvailable = saved
                }
                pendingCredentials = nil; qrImage = nil; waiting = false
            case 4: status = "Native Steam login failed or disconnected (result \(result))."; pendingCredentials = nil
            case 5: status = "Local Steam backend stopped."; pendingCredentials = nil; qrImage = nil; waiting = false
            default: break
            }
        }
    }
    func stop(logout: Bool = false) {
        if logout { SteamLoginKeychain.delete(); savedAvailable = false }
        if stopped { return }; stopped = true
        webView?.evaluateJavaScript("void globalThis.KleptonSteamLogin?.cancel()", completionHandler: nil)
        steamSessionCancel(); pendingCredentials = nil; qrImage = nil; waiting = false
        password = ""; guardCode = ""; accountName = ""; guardRequired = false; submittingCode = false
    }
    func challenge(_ text: String) {
        guard !stopped, waiting, let url = URL(string: text), url.scheme == "https",
              url.host == "s.team", url.path.hasPrefix("/q/"), text.utf8.count <= 4096 else {
            status = "Steam returned an unsupported QR challenge."; return
        }
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(text.utf8); filter.correctionLevel = "M"
        guard let output = filter.outputImage?.transformed(by: CGAffineTransform(scaleX: 8, y: 8)),
              let image = CIContext().createCGImage(output, from: output.extent) else { return }
        qrImage = UIImage(cgImage: image)
        print("[steam-probe] interactive QR rendered=1")
    }
}
struct SteamLoginWebView: UIViewRepresentable {
    @ObservedObject var model: SteamLoginModel
    @MainActor final class Coordinator: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
        let model: SteamLoginModel
        private var initialNavigation = true
        init(_ model: SteamLoginModel) { self.model = model }
        func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
            guard message.frameInfo.isMainFrame, message.frameInfo.securityOrigin.host == "steamloopback.host",
                  let data = message.body as? [String: Any], let kind = data["kind"] as? String else { return }
            switch kind {
            case "status":
                guard let stage = data["stage"] as? String, let result = data["result"] as? Int,
                      ["modules_ready", "starting", "approval_required", "poll_waiting", "token_delivered",
                       "agreement_required", "expired", "failure", "cancelled", "code_required",
                       "code_accepted", "code_rejected", "confirmation_required", "unsupported_guard"].contains(stage) else { return }
                // Whitelisted labels and integers only. Never print message.body.
                print("[steam-probe] interactive Valve auth stage=\(stage) result=\(result)")
                if stage == "modules_ready" { model.modulesReady = true }
                if stage == "approval_required" { model.status = "Scan this QR code with Steam Guard and approve Klepton on Vision Pro." }
                if stage == "code_required" { model.guardRequired = true; model.status = "Enter the Steam Guard code from your phone or email." }
                if stage == "code_accepted" { model.guardRequired = false; model.submittingCode = false; model.status = "Code accepted. Waiting for Steam to finish sign-in…" }
                if stage == "code_rejected" { model.submittingCode = false; model.status = "Steam could not accept that code (result \(result)). Enter a fresh code and try again." }
                if stage == "confirmation_required" { model.status = "Approve this sign-in in Steam Guard or the email Steam sent you." }
                if ["expired", "failure", "agreement_required", "unsupported_guard"].contains(stage) {
                    model.status = "Steam authentication \(stage) (result \(result))."; model.waiting = false; model.qrImage = nil; model.guardRequired = false; model.submittingCode = false
                }
            case "guard_options":
                guard model.waiting, model.codeLogin, let device = data["device"] as? Bool,
                      let email = data["email"] as? Bool, device || email else { return }
                model.guardDeviceAvailable = device; model.guardEmailAvailable = email
                model.guardType = device ? 3 : 2
            case "challenge": if let url = data["url"] as? String { model.challenge(url) }
            case "credential":
                if let token = data["refreshToken"] as? String, let account = data["accountName"] as? String { model.accept(token: token, account: account) }
            default: break
            }
        }
        func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            // This view runs local attested scripts only; the QR approval occurs
            // in Steam Guard. Reject links, redirects, popups and remote pages.
            let allowed = initialNavigation && action.navigationType == .other
            initialNavigation = false
            decisionHandler(allowed ? .allow : .cancel)
        }
        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            guard let root = Bundle.main.url(forResource: "SteamAuthAssets", withExtension: nil) else { return }
            Task { @MainActor in
                do {
                    for name in ["libraries/libraries~00299a408.js", "chunk~2dcc5aaf7.js", "chunk~1a96cdf59.js", "library.js", "login.js"] {
                        _ = try await webView.evaluateJavaScript(String(contentsOf: root.appendingPathComponent(name), encoding: .utf8))
                    }
                } catch { model.status = "Valve login modules could not load." }
            }
        }
    }
    func makeCoordinator() -> Coordinator { Coordinator(model) }
    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration(); config.websiteDataStore = .nonPersistent()
        config.userContentController.add(context.coordinator, name: "steamLogin")
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = context.coordinator; model.webView = view
        view.loadHTMLString("<!doctype html><meta charset=utf-8>", baseURL: URL(string: "https://steamloopback.host/"))
        return view
    }
    func updateUIView(_ view: WKWebView, context: Context) {}
    static func dismantleUIView(_ view: WKWebView, coordinator: Coordinator) {
#if !KL_STEAM_GAME_HOST
        coordinator.model.stop(); view.stopLoading()
#else
        // Closing the login window must not tear down the game's backend.
        view.stopLoading()
#endif
        view.configuration.userContentController.removeScriptMessageHandler(forName: "steamLogin")
    }
}
struct SteamLoginProbeView: View {
#if KL_STEAM_GAME_HOST
    @StateObject private var model = SteamHostLifecycle.model
    var onReady: () -> Void = {}
    @State private var readyReported = false
#else
    @StateObject private var model = SteamLoginModel()
#endif
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Standalone Steam login").font(.title)
            Text(model.status)
            if !model.ticketStatus.isEmpty { Text(model.ticketStatus).font(.callout) }
            if model.nativeState == 1 {
                Picker("Sign-in method", selection: Binding(get: { model.codeLogin }, set: { model.selectLoginMode($0) })) {
                    Text("QR code").tag(false)
                    Text("Steam Guard code").tag(true)
                }.pickerStyle(.segmented)
                if model.codeLogin {
                    if !model.waiting {
                        TextField("Steam account name", text: $model.accountName).textInputAutocapitalization(.never).autocorrectionDisabled()
                        SecureField("Steam password", text: $model.password)
                        Button("Sign in with account") { model.startCodeLogin() }.disabled(!model.modulesReady || model.accountName.isEmpty || model.password.isEmpty)
                    }
                    if model.guardRequired {
                        if model.guardDeviceAvailable && model.guardEmailAvailable {
                            Picker("Code source", selection: $model.guardType) {
                                Text("Steam Guard app").tag(3); Text("Email").tag(2)
                            }
                        }
                        SecureField(model.guardType == 3 ? "Steam Guard app code" : "Email code", text: $model.guardCode)
                            .textInputAutocapitalization(.characters).autocorrectionDisabled()
                        Button("Submit code") { model.submitCode() }.disabled(model.submittingCode || model.guardCode.count != 5)
                    }
                    Text("Your password and Steam Guard code are used only for this sign-in and are never saved.").font(.caption)
                }
            }
            if let qr = model.qrImage, !model.codeLogin {
                Image(uiImage: qr).interpolation(.none).resizable().scaledToFit()
                    .frame(width: 360, height: 360).padding(20).background(.white)
            }
            if model.nativeState == 3, model.ticketStage == 8 {
                Button("Retry Steam tickets") { model.retryTickets() }
            }
            HStack {
                if !model.codeLogin { Button("Sign in with QR code") { model.startQR() }.disabled(model.nativeState != 1 || !model.modulesReady || model.waiting) }
                if model.savedAvailable { Button("Use saved Steam login") { model.useSaved() }.disabled(model.nativeState != 1 || model.waiting) }
                Button("Stop") { model.stop() }
                Button("Log out and forget login") { model.stop(logout: true) }
            }
            Text("Experimental client. Steam login, ownership, signed tickets and Walkabout authentication require verification.").font(.caption)
            SteamLoginWebView(model: model).frame(width: 1, height: 1).opacity(0).accessibilityHidden(true)
        }.padding(32).frame(width: 650).task {
#if KL_STEAM_GAME_HOST
            SteamHostLifecycle.start()
            while !Task.isCancelled {
                model.refresh()
                if model.ticketStage == 9, model.nativeState == 3, !readyReported {
                    readyReported = true; onReady()
                }
                if model.nativeState == 5 { break }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
#else
            let worker = Task.detached(priority: .userInitiated) {
                let data = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                try? FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)
                freopen(data.appendingPathComponent("steam-probe.log").path, "w", stdout)
                dup2(fileno(stdout), STDERR_FILENO); setbuf(stdout, nil); setbuf(stderr, nil)
                setenv("KL_DYLIB_DIR", Bundle.main.privateFrameworksPath!, 1)
                setenv("KL_STEAM_PROBE_RUN_ID", "@RUN_ID@", 1)
                setenv("KL_STEAM_PROBE_DATA", data.path, 1); setenv("KL_STEAM_DATA_ROOT", data.path, 1)
                @WALKABOUT_API_ENV@
                chdir(data.path)
                return "libsteamclient.so".withCString { probe($0, 4) }
            }
            while !Task.isCancelled {
                model.refresh()
                if model.nativeState == 5 { break }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            model.stop(); _ = await worker.value; model.refresh()
#endif
        }.onDisappear {
#if !KL_STEAM_GAME_HOST
            model.stop()
#endif
        }
    }
}
