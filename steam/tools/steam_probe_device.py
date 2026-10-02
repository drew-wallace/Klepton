#!/usr/bin/env python3
"""Build a separate visionOS device/simulator probe with no Unity or game assets.

Generated projects, Valve binaries, translations and logs stay in ignored build/.
This experiment is never counted as authenticated client readiness.
"""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time
import uuid
import steam_runtime
import steam_auth_ui
import steam_sdk_compat

REPO = Path(__file__).resolve().parents[2]
ROOT = REPO / 'build/steam-runtime'
NAME = 'KleptonSteamRuntimeProbe'
BUNDLE = 'com.noosphere.klepton.' + re.sub(r'[^A-Za-z0-9-]', '-', getpass.getuser()) + '.steam-runtime-probe'

APP = '''import SwiftUI
import Foundation
@_silgen_name("kl_steam_probe_run")
func probe(_ library: UnsafePointer<CChar>, _ interfaces: Int32) -> Int32
final class ProbeWebSocketDelegate: NSObject, URLSessionWebSocketDelegate, @unchecked Sendable {
    let completed = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var didOpen = false
    private var errorCode = 0
    private var httpStatus = 0
    var diagnostics: (Int, Int) { lock.lock(); defer { lock.unlock() }; return (errorCode, httpStatus) }
    var opened: Bool { lock.lock(); defer { lock.unlock() }; return didOpen }
    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol negotiatedProtocol: String?) {
        lock.lock(); didOpen = true; lock.unlock()
        completed.signal()
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        lock.lock(); errorCode = (error as NSError?)?.code ?? 0
        httpStatus = (task.response as? HTTPURLResponse)?.statusCode ?? 0; lock.unlock()
        completed.signal()
    }
}
@_cdecl("kl_steam_probe_websocket")
func openProbeWebSocket(_ port: UInt32) -> Int32 {
    guard port > 0 && port <= 65535,
          let url = URL(string: "ws://localhost:\\(port)/transportsocket/") else { return 0 }
    let delegate = ProbeWebSocketDelegate()
    let session = URLSession(configuration: .ephemeral, delegate: delegate, delegateQueue: nil)
    // Valve's client checks its local UI origin before WebSocket upgrade.
    // This value is embedded in the pinned backend and used by its UI.
    var request = URLRequest(url: url)
    request.setValue("https://steamloopback.host", forHTTPHeaderField: "Origin")
    let socket = session.webSocketTask(with: request)
    socket.resume()
    let timedOut = delegate.completed.wait(timeout: .now() + 3) == .timedOut
    let diagnostics = delegate.diagnostics
    print("[steam-probe] WebSocket HTTP status=\\(diagnostics.1) error_code=\\(diagnostics.0) timeout=\\(timedOut)")
    let opened = delegate.opened
    socket.cancel(with: .goingAway, reason: nil)
    session.invalidateAndCancel()
    return opened ? 1 : 0
}
@main struct RuntimeProbeApp: App {
    var body: some Scene { WindowGroup { ProbeView() } }
}
struct ProbeView: View {
    @State private var status = "Starting client probe…"
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Steam runtime probe").font(.title)
            Text(status)
            Text("This tests signed loading and client initialization. Login and ticket issuance remain unverified.").foregroundStyle(.secondary)
        }.padding(32).frame(width: 600).task {
            status = await Task.detached(priority: .userInitiated) {
                let data = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                try? FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)
                let log = data.appendingPathComponent("steam-probe.log").path
                freopen(log, "w", stdout)
                dup2(fileno(stdout), STDERR_FILENO)
                setbuf(stdout, nil)
                setbuf(stderr, nil)
                setenv("KL_DYLIB_DIR", Bundle.main.privateFrameworksPath!, 1)
                setenv("KL_STEAM_PROBE_RUN_ID", "@RUN_ID@", 1)
                setenv("KL_STEAM_PROBE_DATA", data.path, 1)
                setenv("KL_STEAM_DATA_ROOT", data.path, 1)
                chdir(data.path)
                let result = "libsteamclient.so".withCString { probe($0, 1) }
                return "Probe finished with result \\(result). See steam-probe.log. No authenticated session has been established."
            }.value
        }
    }
}
'''

AUTH_UI_SWIFT = r'''
import WebKit
struct AuthTransportProbe: UIViewRepresentable {
    final class Coordinator: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
        func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
            guard let data = message.body as? [String: Any],
                  let stage = data["stage"] as? String,
                  ["modules_ready", "qr_begin_returned", "probe_failed"].contains(stage),
                  let result = data["result"] as? Int,
                  let present = data["hasChallenge"] as? Bool,
                  let transportError = data["transportError"] as? Int else { return }
            print("[steam-probe] Valve auth UI stage=\(stage) result=\(result) challenge_present=\(present ? 1 : 0) transport_error=\(transportError)")
        }
        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            guard let root = Bundle.main.url(forResource: "SteamAuthAssets", withExtension: nil) else { return }
            Task { @MainActor in
                do {
                    for name in ["libraries/libraries~00299a408.js", "chunk~2dcc5aaf7.js", "chunk~1a96cdf59.js", "library.js", "probe.js"] {
                        let source = try String(contentsOf: root.appendingPathComponent(name), encoding: .utf8)
                        _ = try await webView.evaluateJavaScript(source)
                    }
                } catch {
                    // Never print a JavaScript exception that could contain a
                    // request, challenge URL or authentication material.
                    print("[steam-probe] Valve auth UI script loading failed")
                }
            }
        }
    }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        config.userContentController.add(context.coordinator, name: "steamAuthProbe")
        let view = WKWebView(frame: .zero, configuration: config)
        view.navigationDelegate = context.coordinator
        // Use the origin of Valve's packaged UI while loading attested scripts
        // from signed app resources. No page or script is fetched from this URL.
        view.loadHTMLString("<!doctype html><meta charset=utf-8><p id=status>Loading Valve authentication modules…</p>", baseURL: URL(string: "https://steamloopback.host/"))
        return view
    }
    func updateUIView(_ view: WKWebView, context: Context) {}
    static func dismantleUIView(_ view: WKWebView, coordinator: Coordinator) {
        view.stopLoading()
        view.configuration.userContentController.removeScriptMessageHandler(forName: "steamAuthProbe")
    }
}
'''


def run(command, log=None):
    print('Running:', ' '.join(map(str, command)), flush=True)
    if log:
        with log.open('wb') as output:
            subprocess.run(list(map(str, command)), cwd=REPO, stdout=output, stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run(list(map(str, command)), cwd=REPO, check=True)


def project(out, framework, team, backend=False, simulator=False, login_probe=False, run_id=None, auth_ui_probe=False, auth_ui_login=False, game_frameworks=(), separate_game_client=False, service_probe=False):
    """Small standalone Xcode app: probe C source, Swift view, runtime archive."""
    app_source = APP.replace('@RUN_ID@', run_id or str(uuid.uuid4())).replace('probe($0, 1)', 'probe($0, 3)' if login_probe else 'probe($0, 2)' if backend else 'probe($0, 1)')
    if auth_ui_probe:
        # Run the isolated anonymous UI request after the bounded backend probe
        # has finished. This mode does not claim a native authenticated session.
        app_source = app_source.replace('@State private var status =', '@State private var authStarted = false\n    @State private var status =')
        app_source = app_source.replace('Text(status)', 'Text(status)\n            if authStarted { AuthTransportProbe().frame(height: 180) }')
        app_source = app_source.replace('}.value\n        }', '}.value\n            authStarted = true\n        }')
        app_source += AUTH_UI_SWIFT
    if auth_ui_login:
        app_source = APP[:APP.index('struct ProbeView: View')]
        app_source = app_source.replace('ProbeView()', 'SteamLoginProbeView()')
        app_source += (REPO/'steam/tools/steam_auth_ui_login.swift').read_text()
        app_source = app_source.replace('@RUN_ID@', run_id or str(uuid.uuid4()))
        app_source = app_source.replace('@WALKABOUT_API_ENV@', 'setenv("KL_STEAM_WALKABOUT_API", "1", 1)' if game_frameworks else 'unsetenv("KL_STEAM_WALKABOUT_API")')
    if separate_game_client:
        app_source = app_source.replace('"libsteamclient.so".withCString', '"libsteamclient_backend.so".withCString')
    if service_probe:
        app_source = app_source.replace('kl_steam_probe_run', 'kl_steam_service_probe_run')
        app_source = app_source.replace('"libsteamclient.so".withCString', '"steamservice.so".withCString')
    (out / 'App.swift').write_text(app_source)
    plistlib.dump({'CFBundleExecutable': '$(EXECUTABLE_NAME)', 'CFBundleIdentifier': '$(PRODUCT_BUNDLE_IDENTIFIER)',
                  'CFBundleName': NAME, 'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '1.0',
                  'CFBundleVersion': '1', 'UIApplicationSceneManifest': {'UIApplicationSupportsMultipleScenes': False},
                  'UILaunchScreen': {}, 'NSAppTransportSecurity': {'NSAllowsLocalNetworking': True}}, (out / 'Info.plist').open('wb'))
    runtime_dir = REPO / ('build/xrsim' if simulator else 'build/xros')
    objects = {}
    def obj(key, **fields):
        objects[key] = fields
        return key
    swift = obj('SWIFT', isa='PBXFileReference', lastKnownFileType='sourcecode.swift', path=str(out/'App.swift'), sourceTree='<absolute>')
    c = obj('CSOURCE', isa='PBXFileReference', lastKnownFileType='sourcecode.c.c', path=str(REPO/'steam/tools/steam_probe.c'), sourceTree='<absolute>')
    archive = obj('ARCHIVE', isa='PBXFileReference', lastKnownFileType='archive.ar', path=str(runtime_dir/'libklepton.a'), sourceTree='<absolute>')
    fw = obj('FRAMEWORK', isa='PBXFileReference', lastKnownFileType='wrapper.framework', path=str(framework), sourceTree='<absolute>')
    product = obj('PRODUCT', isa='PBXFileReference', explicitFileType='wrapper.application', path=NAME+'.app', sourceTree='BUILT_PRODUCTS_DIR')
    sb = obj('SWIFTBUILD', isa='PBXBuildFile', fileRef=swift)
    cb = obj('CBUILD', isa='PBXBuildFile', fileRef=c)
    ab = obj('ABUILD', isa='PBXBuildFile', fileRef=archive)
    fb = obj('FBUILD', isa='PBXBuildFile', fileRef=fw, settings={'ATTRIBUTES':['CodeSignOnCopy','RemoveHeadersOnCopy']})
    sources = obj('SOURCES', isa='PBXSourcesBuildPhase', buildActionMask=2147483647, files=[sb,cb], runOnlyForDeploymentPostprocessing=0)
    libs = obj('LIBRARIES', isa='PBXFrameworksBuildPhase', buildActionMask=2147483647, files=[], runOnlyForDeploymentPostprocessing=0)
    embeds = obj('EMBEDS', isa='PBXCopyFilesBuildPhase', buildActionMask=2147483647, dstPath='', dstSubfolderSpec=10, files=[fb], name='Embed Client', runOnlyForDeploymentPostprocessing=0)
    extra_refs = []
    for index, extra in enumerate(game_frameworks):
        reference = obj(f'GAMEFW{index}', isa='PBXFileReference', lastKnownFileType='wrapper.framework', path=str(extra), sourceTree='<absolute>')
        build = obj(f'GAMEBUILD{index}', isa='PBXBuildFile', fileRef=reference, settings={'ATTRIBUTES':['CodeSignOnCopy','RemoveHeadersOnCopy']})
        objects[embeds]['files'].append(build)
        extra_refs.append(reference)
    phases = [sources,libs,embeds]
    if auth_ui_probe or auth_ui_login:
        assets = obj('AUTHA', isa='PBXFileReference', lastKnownFileType='folder', path=str(out/'SteamAuthAssets'), sourceTree='<absolute>')
        assets_build = obj('AUTHB', isa='PBXBuildFile', fileRef=assets)
        phases.append(obj('RESOURCES', isa='PBXResourcesBuildPhase', buildActionMask=2147483647, files=[assets_build], runOnlyForDeploymentPostprocessing=0))
    group = obj('GROUP', isa='PBXGroup', children=[swift,c,archive,fw,product]+extra_refs, sourceTree='<group>')
    settings = {'PRODUCT_NAME':NAME, 'PRODUCT_BUNDLE_IDENTIFIER': BUNDLE, 'DEVELOPMENT_TEAM':team,
        'CODE_SIGN_STYLE':'Automatic', 'INFOPLIST_FILE':str(out/'Info.plist'), 'SDKROOT':'xrsimulator' if simulator else 'xros',
        'SUPPORTED_PLATFORMS':'xrsimulator' if simulator else 'xros', 'XROS_DEPLOYMENT_TARGET':'26.0', 'TARGETED_DEVICE_FAMILY':'7',
        'ARCHS':'arm64', 'SWIFT_VERSION':'5.0', 'SWIFT_OPTIMIZATION_LEVEL':'-Onone', 'CLANG_ENABLE_MODULES':'YES',
        'GCC_PREPROCESSOR_DEFINITIONS':['KL_STEAM_PROBE_LIBRARY=1'], 'HEADER_SEARCH_PATHS':[str(REPO/'runtime')],
        'OTHER_LDFLAGS':[str(runtime_dir/'libklepton.a'),'-lz','-framework','AudioToolbox','-framework','VideoToolbox','-framework','CoreMedia',
                         '-framework','CoreVideo','-framework','IOSurface','-framework','AVFoundation'],
        'LIBRARY_SEARCH_PATHS':[str(runtime_dir)],
        'LD_RUNPATH_SEARCH_PATHS':['$(inherited)','@executable_path/Frameworks']}
    if game_frameworks:
        settings['GCC_PREPROCESSOR_DEFINITIONS'].append('KL_WALKABOUT_API_PINNED=1')
    if login_probe:
        settings['GCC_PREPROCESSOR_DEFINITIONS'].append('KL_STEAM_LOGIN_ABI_PINNED=1')
    if service_probe:
        settings['GCC_PREPROCESSOR_DEFINITIONS'].append('KL_STEAM_SERVICE_ABI_PINNED=1')
    if simulator:
        settings['CODE_SIGN_IDENTITY'] = '-'
        settings['DEVELOPMENT_TEAM'] = ''
    config = obj('CONFIG', isa='XCBuildConfiguration', buildSettings=settings, name='Debug')
    cfg = obj('CFGLIST', isa='XCConfigurationList', buildConfigurations=[config], defaultConfigurationIsVisible=0, defaultConfigurationName='Debug')
    pc = obj('PROJECTCONFIG', isa='XCBuildConfiguration', buildSettings={}, name='Debug')
    pcl = obj('PROJECTCFGLIST', isa='XCConfigurationList', buildConfigurations=[pc], defaultConfigurationIsVisible=0, defaultConfigurationName='Debug')
    target = obj('TARGET', isa='PBXNativeTarget', buildConfigurationList=cfg, buildPhases=phases, buildRules=[], dependencies=[], name=NAME, productName=NAME, productReference=product, productType='com.apple.product-type.application')
    obj('PROJECT', isa='PBXProject', attributes={'LastUpgradeCheck':'2600'}, buildConfigurationList=pcl, compatibilityVersion='Xcode 14.0', developmentRegion='en', knownRegions=['en','Base'], mainGroup=group, productRefGroup=group, projectDirPath='', projectRoot='', targets=[target])
    ids = {key: hashlib.sha256(key.encode()).hexdigest()[:24].upper() for key in objects}
    def encode(value):
        if isinstance(value, dict): return '{ ' + ' '.join(f'{json.dumps(k)} = {encode(v)};' for k,v in value.items()) + ' }'
        if isinstance(value, list): return '( ' + ', '.join(map(encode,value)) + ', )' if value else '( )'
        if isinstance(value, str) and value in ids: return ids[value]
        return json.dumps(value)
    text = '// !$*UTF8*$!\n{ archiveVersion = 1; classes = {}; objectVersion = 56; objects = {\n'
    text += '\n'.join(f'{ids[key]} = {encode(fields)};' for key,fields in objects.items())
    text += '\n}; rootObject = '+ids['PROJECT']+'; }\n'
    path = out/(NAME+'.xcodeproj')
    path.mkdir(exist_ok=True)
    (path/'project.pbxproj').write_text(text)
    return path


def simulator_observation(simulator, out, run_id, auth_ui_probe=False, interactive=False, smoke=False, wait_seconds=None):
    """Collect the bounded no-credential probe, keeping readiness unproven."""
    container = Path(subprocess.check_output(
        ['xcrun', 'simctl', 'get_app_container', simulator, BUNDLE, 'data'], text=True).strip())
    log = container/'Library/Application Support/steam-probe.log'
    deadline = time.monotonic() + (wait_seconds if wait_seconds is not None else 45 if smoke else 20)
    text = ''
    while time.monotonic() < deadline:
        if log.exists():
            candidate = log.read_text(errors='replace')
            text = candidate if f'[steam-probe] run id: {run_id}\n' in candidate else ''
            if smoke and 'interactive session LogOff returned' in text and 'runtime gate UNPROVEN;' in text: break
            if interactive and not smoke and 'interactive native session ready;' in text and 'interactive Valve auth stage=modules_ready' in text: break
            if '[steam-probe] runtime gate UNPROVEN;' in text and (not auth_ui_probe or 'Valve auth UI stage=qr_begin_returned' in text or 'Valve auth UI stage=probe_failed' in text): break
        time.sleep(0.2)
    (out/'steam-probe.log').write_text(text)
    sdk_attempts = [int(value) for value in re.findall(r'Walkabout SteamAPI_InitFlat result=(\d+)', text)]
    native_logon = re.findall(r'interactive native BLoggedOn=(\d+)', text)
    pump = re.search(r'frame pump returned; callbacks=(\d+) IPC calls=(\d+)', text)
    service_start = re.search(r'service StartThread returned; server_present=(\d+) getter_matches=(\d+) threads_created=(\d+)', text)
    return {
        'run_id_verified': bool(text),
        'service_constructors_returned': '[steam-probe] service constructors returned' in text,
        'service_server_present': bool(service_start and service_start[1] == '1'),
        'service_server_getter_matches': bool(service_start and service_start[2] == '1'),
        'service_threads_created': int(service_start[3]) if service_start else None,
        'service_stop_returned': '[steam-probe] service Stop returned' in text,
        'service_shutdown_returned': '[steam-probe] service Shutdown returned' in text,
        'service_ipc_request_tested': False,
        'walkabout_sdk_loaded': 'Walkabout genuine SDK loaded;' in text,
        'walkabout_sdk_init_result': sdk_attempts[-1] if sdk_attempts else None,
        'walkabout_sdk_init_attempts': sdk_attempts,
        'walkabout_client_instance_distinct': bool(int(m[1])) if (m := re.search(r'Walkabout client instance distinct=(\d+)', text)) else None,
        'walkabout_sdk_connect_global_failure_observed': 'reason_connect_global=1' in text,
        'walkabout_sdk_app_context_failure_observed': 'reason_app_context=1' in text,
        'walkabout_sdk_app_id': int(m[1]) if (m := re.search(r'Walkabout SDK interfaces present; app_id=(\d+)', text)) else None,
        'walkabout_subscribed': bool(int(m[1])) if (m := re.search(r'Walkabout ownership subscribed=(\d+)', text)) else None,
        'walkabout_session_ticket_requests': [{'handle_present': bool(int(a)), 'bytes': int(b), 'bounds_valid': bool(int(c)), 'recipient_set': bool(int(d))} for a, b, c, d in re.findall(r'Walkabout session ticket requested handle_present=(\d+) bytes=(\d+) bounds_valid=(\d+) recipient_set=(\d+)', text)],
        'walkabout_ticket_callbacks': [{'id': int(a), 'result': int(b), 'both_delivered': bool(int(c))} for a, b, c in re.findall(r'Walkabout ticket callback id=(\d+) result=(\d+) delivered=(\d+)', text)],
        'walkabout_dlc': [{'app_id': int(a), 'store_available': bool(int(b)), 'subscribed': bool(int(c)), 'installed': bool(int(d))} for a, b, c, d in re.findall(r'Walkabout DLC app_id=(\d+) store_available=(\d+) subscribed=(\d+) installed=(\d+)', text)],
        'walkabout_ticket_delivery_observed': bool(re.search(r'Walkabout ticket callback id=\d+ result=1 delivered=1', text)),
        'walkabout_sdk_shutdown_returned': 'Walkabout SteamAPI_Shutdown returned' in text,
        'interactive_backend_ready': 'interactive native session ready;' in text,
        'saved_login_selected': 'saved Steam login selected=1' in text,
        'interactive_ui_ready': 'interactive Valve auth stage=modules_ready' in text,
        'interactive_poll_returned_success': 'interactive Valve auth stage=poll_waiting result=1' in text,
        'interactive_qr_rendered': 'interactive QR rendered=1' in text,
        'interactive_cancelled': 'interactive Valve auth stage=cancelled' in text,
        'interactive_logoff_returned': 'interactive session LogOff returned' in text,
        'interactive_native_logged_on': bool(native_logon and native_logon[-1] == '1'),
        'probe_returned': '[steam-probe] probe returned result=' in text or '[steam-probe] runtime gate UNPROVEN;' in text,
        'local_backend_created': bool(re.search(r'legacy CreateGlobalUser: user=[1-9]\d* pipe=[1-9]\d*', text)),
        'public_pipe_created': bool(re.search(r'\[steam-probe\] CreateSteamPipe: [1-9]\d*', text)),
        'genuine_user_interface': '[steam-probe] SteamUser023: present' in text,
        'logged_on': native_logon[-1] == '1' if native_logon else 'BLoggedOn: 1' in text,
        'callback_count': int(pump[1]) if pump else None,
        'ipc_calls': int(pump[2]) if pump else None,
        'shutdown_returned_true': '[steam-probe] BShutdownIfAllPipesClosed: 1' in text,
        'assertions_observed': 'Assertion Failed:' in text,
        'native_login_abi_verified': '[steam-probe] native login ABI verified;' in text,
        'credential_free_login_result': int(m[1]) if (m := re.search(r'credential-free Steam_LogOn result=(\d+)', text)) else None,
        'webui_transport_info_returned': bool(re.search(r'WebUI transport info: result=1 port=[1-9]\d* auth_key_present=1', text)),
        'webui_websocket_upgraded': '[steam-probe] WebUI WebSocket upgraded=1' in text,
        'webui_websocket_http_status': int(m[1]) if (m := re.search(r'\[steam-probe\] WebSocket HTTP status=(\d+)', text)) else None,
        'webui_minimal_http_status': int(m[1]) if (m := re.search(r'minimal WebSocket HTTP status=(\d+)', text)) else None,
        'webui_loopback_reachable': '[steam-probe] WebUI clientdll loopback reachable=1' in text,
        'auth_ui_modules_ready': 'Valve auth UI stage=modules_ready' in text,
        'auth_ui_qr_result': int(m[1]) if (m := re.search(r'Valve auth UI stage=qr_begin_returned result=(\d+)', text)) else None,
        'auth_ui_transport_code': int(m[1]) if (m := re.search(r'Valve auth UI stage=qr_begin_returned result=\d+ challenge_present=\d+ transport_error=(\d+)', text)) else None,
        'auth_ui_challenge_present': 'Valve auth UI stage=qr_begin_returned result=1 challenge_present=1' in text,
        'native_logoff_returned': '[steam-probe] native LogOff returned' in text,
        'log': str(out/'steam-probe.log'),
    }


def probe_process_identity(pid):
    """Identify the exact live simulator process; the app can outlive its worker."""
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise ValueError('invalid recorded simulator PID')
    fields = {}
    for key, column in [('executable', 'comm='), ('started', 'lstart=')]:
        result = subprocess.run(['ps', '-p', str(pid), '-o', column], text=True, capture_output=True)
        if result.returncode == 1 and not result.stdout.strip(): return None
        if result.returncode or not result.stdout.strip():
            raise ValueError('could not verify simulator process identity')
        fields[key] = result.stdout.strip()
    return dict(pid=pid, **fields)


def verify_installed_probe(simulator, receipt, identity):
    app = Path(subprocess.check_output(['xcrun', 'simctl', 'get_app_container', simulator, BUNDLE, 'app'], text=True).strip())
    if Path(identity['executable']).resolve() != (app/NAME).resolve():
        raise ValueError('recorded process is not the installed probe')
    binary = app/(NAME+'.debug.dylib')
    if not binary.exists(): binary = app/NAME
    if hashlib.sha256(binary.read_bytes()).hexdigest() != receipt['app_binary_sha256']:
        raise ValueError('installed probe differs from build receipt')
    client_name = 'steamservice' if receipt.get('probe_kind') == 'service_helper' else 'libsteamclient_backend' if receipt.get('separate_game_client') else 'libsteamclient'
    images = [(client_name, receipt['embedded_client_sha256'])]
    if receipt.get('separate_game_client'):
        images.append(('libsteamclient', receipt['separate_game_client']['embedded_sha256']))
    for item in receipt.get('walkabout_sdk_inputs', []):
        name = Path(item['path']).stem.replace('+', 'x')
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name): raise ValueError('invalid SDK framework name')
        images.append((name, item['embedded_sha256']))
    for name, expected in images:
        if hashlib.sha256((app/'Frameworks'/(name+'.framework')/name).read_bytes()).hexdigest() != expected:
            raise ValueError('installed framework differs from build receipt')


def observe_existing_probe(receipt_path, simulator, wait_seconds=20):
    """Read a verified existing launch. Never build, install, terminate or launch."""
    receipt_path = Path(receipt_path).resolve()
    if not receipt_path.is_relative_to(ROOT.resolve()):
        raise ValueError('observation receipt must be inside build/steam-runtime')
    receipt = json.loads(receipt_path.read_text())
    recorded = receipt.get('simulator_process')
    if not recorded:
        raise ValueError('receipt has no launch identity; inspect the existing process before choosing a new run')
    current = probe_process_identity(recorded['pid'])
    if current is None:
        raise ValueError('recorded simulator process is no longer present; no restart performed')
    if current != recorded:
        raise ValueError('simulator PID identity changed; no restart performed')
    verify_installed_probe(simulator, receipt, current)
    out = receipt_path.parent/'observations'/str(uuid.uuid4())
    out.mkdir(parents=True)
    observed = simulator_observation(simulator, out, receipt['probe_run_id'],
        receipt.get('auth_ui_probe', False), False,
        receipt.get('anonymous_interactive_smoke', False), wait_seconds)
    if not observed['run_id_verified']:
        raise ValueError('no matching launch log observed; no restart performed')
    after = probe_process_identity(recorded['pid'])
    if after is not None and after != recorded:
        raise ValueError('simulator PID changed during observation; no restart performed')
    snapshot = dict(receipt, observed=observed, build_receipt=str(receipt_path),
        observation_source='existing_verified_launch', process_live_after=after is not None,
        observation_timed_out=not observed['probe_returned'])
    (out/'receipt.json').write_text(json.dumps(snapshot, indent=2)+'\n')
    return out/'receipt.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--team', default=os.environ.get('KLEPTON_TEAM'))
    parser.add_argument('--observe', type=Path, help='collect a later snapshot from the same verified simulator launch; no rebuild or restart')
    parser.add_argument('--observe-seconds', type=int, default=20, help='bounded existing-launch observation window, 1–60 seconds')
    parser.add_argument('--service-probe', action='store_true', help='isolated recovery helper StartThread/GetIPCServer/Stop/Shutdown experiment; no service commands or account backend')
    parser.add_argument('--sdk-bootstrap-compat', action='store_true', help='audited one-instruction SDK user/pipe ordering correction in the translation input; preserves original artifact')
    parser.add_argument('--separate-game-client', action='store_true', help='experimental separate client globals for backend and game, using two signed translations of the same pinned library')
    parser.add_argument('--walkabout-api', action='store_true', help='package Walkabout SDK, initialize actual app context, check ownership and both ticket APIs after login')
    parser.add_argument('--auth-ui-smoke', action='store_true', help='simulator-only anonymous QR polling and cancellation; no account approval')
    parser.add_argument('--auth-ui-login', action='store_true', help='interactive QR approval, native token handoff and Keychain storage; recovery source only')
    parser.add_argument('--use-saved-login', action='store_true', help='simulator interactive probe: reconnect the previously approved Keychain login once, without a new QR challenge')
    parser.add_argument('--auth-ui-probe', action='store_true', help='anonymous QR request using bundled Valve HTTPS authentication modules')
    destination = parser.add_mutually_exclusive_group()
    destination.add_argument('--device', help='physical Vision Pro UDID; installs and launches when supplied')
    destination.add_argument('--simulator', nargs='?', const='booted', help='simulator UDID (default: booted); builds, installs and launches')
    parser.add_argument('--source',choices=['steam-client','steamframe-recovery'],default='steam-client',help='pinned client artifact source')
    parser.add_argument('--backend', action='store_true', help='experimental local backend, five-second frame/callback pump and shutdown; never logs in')
    parser.add_argument('--login-probe', action='store_true', help='recovery-only native login ABI and credential-free rejection probe')
    args = parser.parse_args()
    if args.observe:
        if not args.simulator or args.device: parser.error('--observe requires --simulator')
        if not 1 <= args.observe_seconds <= 60: parser.error('--observe-seconds must be 1–60')
        if any((args.service_probe, args.backend, args.login_probe, args.auth_ui_probe, args.auth_ui_login, args.auth_ui_smoke, args.walkabout_api, args.sdk_bootstrap_compat, args.separate_game_client, args.use_saved_login)):
            parser.error('--observe reads its mode from the build receipt; omit build experiment flags')
        print('Observation receipt:', observe_existing_probe(args.observe, args.simulator, args.observe_seconds))
        return 0
    if args.service_probe:
        if args.source != 'steamframe-recovery': parser.error('--service-probe requires --source steamframe-recovery')
        if any((args.backend, args.login_probe, args.auth_ui_probe, args.auth_ui_login, args.auth_ui_smoke, args.walkabout_api, args.sdk_bootstrap_compat, args.separate_game_client)):
            parser.error('--service-probe is isolated; choose it without backend, login or game experiments')
    if args.auth_ui_smoke:
        if not args.simulator: parser.error('--auth-ui-smoke requires a simulator')
        args.auth_ui_login = True
    if args.use_saved_login and (not args.simulator or not args.auth_ui_login or args.auth_ui_smoke):
        parser.error('--use-saved-login requires --simulator --auth-ui-login without --auth-ui-smoke')
    if args.auth_ui_login:
        if args.auth_ui_probe: parser.error('choose anonymous probe or interactive login')
        args.login_probe = True
    if args.login_probe:
        if args.source != 'steamframe-recovery': parser.error('--login-probe requires --source steamframe-recovery')
        args.backend = True
    if args.sdk_bootstrap_compat and not args.walkabout_api: parser.error('--sdk-bootstrap-compat requires --walkabout-api')
    if args.separate_game_client and not args.walkabout_api: parser.error('--separate-game-client requires --walkabout-api')
    if args.walkabout_api and not args.auth_ui_login: parser.error('--walkabout-api requires --auth-ui-login or --auth-ui-smoke')
    if not args.team and not args.simulator: parser.error('--team or KLEPTON_TEAM is required for device builds')
    simulator = bool(args.simulator)
    runtime_dir = REPO / ('build/xrsim' if simulator else 'build/xros')
    sdk = 'xrsimulator' if simulator else 'xros'
    if args.source == 'steamframe-recovery':
        # The hash-pinned recovery client's four legacy startup/release exports
        # were disassembled independently. Their ARM64 forwarding signatures
        # match the older pinned client used by steam_probe.c. This authorizes
        # only the disposable local-backend experiment, not private login calls.
        lock = json.loads((REPO/'steam/tools/steamframe_recovery.lock.json').read_text())
        record = next(a for a in lock['artifacts'] if a['path']==('androidarm64/steamservice.so' if args.service_probe else 'androidarm64/libsteamclient.so'))
        source = ROOT/'steamframe-0.3.0/steam'/record['path']
        client_version = None
        provenance = {'kind':args.source,'release':lock['release'],'build':lock['build'],
                      'archive_sha256':lock['sha256'],'client_builddate':lock['client_builddate']}
    else:
        lock = json.loads(steam_runtime.DEFAULT_LOCK.read_text())
        steam_runtime.manifest_snapshot(lock, steam_runtime.DEFAULT_LOCK.parent)
        inventory = steam_runtime.inventory(lock, ROOT, None)
        record = next(a for a in inventory['artifacts'] if a['package']=='bins_androidarm64_linuxarm64' and a['path']=='androidarm64/libsteamclient.so')
        source = ROOT/'artifacts'/record['package']/record['path']
        client_version = lock['client_version']
        provenance = {'kind':args.source,'manifest_sha256':lock['manifest_sha256']}
    steam_runtime.verify(source.read_bytes(), record['size'], record['sha256'])
    if (args.auth_ui_probe or args.auth_ui_login) and args.source != 'steamframe-recovery':
        raise ValueError('auth UI probe requires the pinned Steam Frame recovery source')
    if args.login_probe and record['sha256'] != '4b1318ea53168ecbf74318ef5b330eb0a41a3e2d335c14f3827504bebba4b40d':
        raise ValueError('private login ABI is not audited for this client hash')
    if args.service_probe and record['sha256'] != '5625b2a0fe98b4cfb4af52266550e74d25d7e283b0503c319bdadfc51070b1b6':
        raise ValueError('service export ABI is not audited for this helper hash')
    name = ('simulator-backend' if args.backend else 'simulator') if simulator else 'device'
    if args.use_saved_login: name += '-saved'
    if args.service_probe: name += '-service'
    if args.login_probe: name += '-login'
    if args.auth_ui_probe: name += '-auth-ui'
    if args.auth_ui_login: name += '-interactive'
    if args.walkabout_api: name += '-walkabout-api'
    if args.sdk_bootstrap_compat: name += '-sdk-compat'
    if args.separate_game_client: name += '-separate-client'
    if args.source == 'steamframe-recovery': name += '-steamframe'
    out = ROOT/name
    out.mkdir(exist_ok=True)
    ui_records = steam_auth_ui.package(ROOT/'steamframe-0.3.0/steam/steamui', out/'SteamAuthAssets', interactive=args.auth_ui_login) if (args.auth_ui_probe or args.auth_ui_login) else None
    run(['make','build/klepton-ld',runtime_dir.relative_to(REPO)/'libklepton.a'], out/'runtime-build.log')
    client_name = 'steamservice' if args.service_probe else 'libsteamclient_backend' if args.separate_game_client else 'libsteamclient'
    framework = out/(client_name+'.framework'); framework.mkdir(exist_ok=True)
    client_command = [REPO/'build/klepton-ld',source,'-o',framework/client_name,'--platform','visionossim' if simulator else 'visionos']
    if args.separate_game_client:
        client_command += ['--install-name', '@rpath/'+framework.name+'/'+client_name]
    run(client_command,out/'translate.log')
    translation = (out/'translate.log').read_text()
    if not re.search(r'TLS rewrites\s+\d+\s+refused 0',translation) or not re.search(r'x18 sites\s+\d+\s+veneered\s+\d+\s+refused 0',translation):
        raise ValueError('translation refused sites; deployment stopped')
    plistlib.dump({'CFBundleIdentifier':BUNDLE+'.client', 'CFBundleExecutable':client_name,
        'CFBundleName':client_name,'CFBundlePackageType':'FMWK','CFBundleVersion':'1',
        'CFBundleShortVersionString':'1.0','MinimumOSVersion':'26.0','CFBundleSupportedPlatforms':['XRSimulator' if simulator else 'XROS']},(framework/'Info.plist').open('wb'))
    run_id = str(uuid.uuid4())
    game_records, game_frameworks, sdk_correction = [], [], None
    separate_client_record = None
    if args.separate_game_client:
        extra = out/'libsteamclient.framework'; extra.mkdir(exist_ok=True)
        run([REPO/'build/klepton-ld',source,'-o',extra/'libsteamclient','--platform','visionossim' if simulator else 'visionos'],out/'game-client-translate.log')
        translated = (out/'game-client-translate.log').read_text()
        if not re.search(r'TLS rewrites\s+\d+\s+refused 0',translated) or not re.search(r'x18 sites\s+\d+\s+veneered\s+\d+\s+refused 0',translated):
            raise ValueError('separate game client translation refused sites')
        plistlib.dump({'CFBundleIdentifier':BUNDLE+'.gameclient','CFBundleExecutable':'libsteamclient',
            'CFBundleName':'libsteamclient','CFBundlePackageType':'FMWK','CFBundleVersion':'1',
            'CFBundleShortVersionString':'1.0','MinimumOSVersion':'26.0',
            'CFBundleSupportedPlatforms':['XRSimulator' if simulator else 'XROS']},(extra/'Info.plist').open('wb'))
        separate_client_record = {'input_sha256':record['sha256'],'translated_sha256':hashlib.sha256((extra/'libsteamclient').read_bytes()).hexdigest()}
    if args.walkabout_api:
        game_lock = json.loads((REPO/'games/walkabout/steam/walkabout_steam_api.lock.json').read_text())
        for item in game_lock['files']:
            steam_runtime.verify((REPO/item['path']).read_bytes(), item['size'], item['sha256'])
        for item in game_lock['files']:
            fwname = Path(item['path']).stem.replace('+', 'x')
            extra = out/(fwname+'.framework'); extra.mkdir(exist_ok=True)
            translation_source = REPO/item['path']
            if args.sdk_bootstrap_compat and fwname == 'libsteam_api':
                corrected, sdk_correction = steam_sdk_compat.correct_bootstrap(translation_source.read_bytes())
                translation_source = out/'sdk-compat-source/libsteam_api.so'
                translation_source.parent.mkdir(exist_ok=True)
                translation_source.write_bytes(corrected)
            run([REPO/'build/klepton-ld', translation_source, '-o', extra/fwname, '--platform', 'visionossim' if simulator else 'visionos'], out/(fwname+'-translate.log'))
            translated = (out/(fwname+'-translate.log')).read_text()
            if not re.search(r'TLS rewrites\s+\d+\s+refused 0', translated) or not re.search(r'x18 sites\s+\d+\s+veneered\s+\d+\s+refused 0', translated):
                raise ValueError('Walkabout SDK dependency translation refused sites')
            plistlib.dump({'CFBundleIdentifier': BUNDLE+'.'+re.sub(r'[^A-Za-z0-9.-]', '-', fwname),
                'CFBundleExecutable': fwname, 'CFBundleName': fwname, 'CFBundlePackageType':'FMWK',
                'CFBundleVersion':'1', 'CFBundleShortVersionString':'1.0', 'MinimumOSVersion':'26.0',
                'CFBundleSupportedPlatforms':['XRSimulator' if simulator else 'XROS']}, (extra/'Info.plist').open('wb'))
            game_records.append(dict(item, translated_sha256=hashlib.sha256((extra/fwname).read_bytes()).hexdigest()))
            game_frameworks.append(extra)
    embedded_frameworks = game_frameworks + ([out/'libsteamclient.framework'] if args.separate_game_client else [])
    path = project(out,framework,args.team or '',args.backend,simulator,args.login_probe,run_id,args.auth_ui_probe,args.auth_ui_login,embedded_frameworks,args.separate_game_client,args.service_probe)
    command = ['xcodebuild','-project',path,'-scheme',NAME,'-configuration','Debug','-sdk',sdk,
        '-destination','generic/platform=visionOS Simulator' if simulator else 'generic/platform=visionOS',
        '-derivedDataPath',out/'dd']
    if not simulator: command.append('-allowProvisioningUpdates')
    run(command + ['build'],out/'xcodebuild.log')
    app = out/('dd/Build/Products/Debug-'+sdk)/(NAME+'.app')
    run(['codesign','--verify','--deep','--strict',app])
    binary = app/(NAME+'.debug.dylib')
    if not binary.exists(): binary = app/NAME
    linked = subprocess.check_output(['otool','-L',str(binary)], text=True)
    (out/'linked-libraries.txt').write_text(linked)
    if 'libklepton.dylib' in linked: raise ValueError('probe accidentally linked a dynamic runtime')
    for item, extra in zip(game_records, game_frameworks):
        item['embedded_sha256'] = hashlib.sha256((app/'Frameworks'/extra.name/extra.stem).read_bytes()).hexdigest()
    if separate_client_record:
        separate_client_record['embedded_sha256'] = hashlib.sha256((app/'Frameworks/libsteamclient.framework/libsteamclient').read_bytes()).hexdigest()
    receipt = {'probe_run_id':run_id,'client_version':client_version,'source':provenance,'input_sha256':record['sha256'],
               'probe_kind':'service_helper' if args.service_probe else 'account_backend', 'artifact_path':record['path'],
               'translated_sha256':hashlib.sha256((framework/client_name).read_bytes()).hexdigest(),
               'app_binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
               'embedded_client_sha256':hashlib.sha256((app/'Frameworks'/framework.name/client_name).read_bytes()).hexdigest(),
               'separate_game_client':separate_client_record,
               'app':str(app), 'device':args.device, 'simulator':args.simulator, 'backend_experiment':args.backend, 'credential_free_login_probe':args.login_probe, 'auth_ui_probe':args.auth_ui_probe, 'interactive_login':args.auth_ui_login, 'saved_login_requested':args.use_saved_login, 'anonymous_interactive_smoke':args.auth_ui_smoke, 'auth_ui_inputs':ui_records, 'walkabout_sdk_inputs':game_records, 'sdk_bootstrap_correction':sdk_correction, 'walkabout_app_id':1408230 if args.walkabout_api else None, 'gate':'unproven','login':'credential_free_rejection_only' if args.login_probe else 'not_run','tickets':'not_run','playfab':'not_run'}
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    if args.simulator:
        run(['xcrun','simctl','install',args.simulator,app])
        previous_smoke = os.environ.get('SIMCTL_CHILD_KL_STEAM_AUTH_SMOKE')
        previous_saved = os.environ.get('SIMCTL_CHILD_KL_STEAM_AUTH_SAVED')
        try:
            if args.use_saved_login: os.environ['SIMCTL_CHILD_KL_STEAM_AUTH_SAVED'] = '1'
            else: os.environ.pop('SIMCTL_CHILD_KL_STEAM_AUTH_SAVED', None)
            if args.auth_ui_smoke: os.environ['SIMCTL_CHILD_KL_STEAM_AUTH_SMOKE'] = '1'
            else: os.environ.pop('SIMCTL_CHILD_KL_STEAM_AUTH_SMOKE', None)
            run(['xcrun','simctl','launch','--terminate-running-process',args.simulator,BUNDLE],out/'launch.log')
        finally:
            if previous_saved is None: os.environ.pop('SIMCTL_CHILD_KL_STEAM_AUTH_SAVED', None)
            else: os.environ['SIMCTL_CHILD_KL_STEAM_AUTH_SAVED'] = previous_saved
            if previous_smoke is None: os.environ.pop('SIMCTL_CHILD_KL_STEAM_AUTH_SMOKE', None)
            else: os.environ['SIMCTL_CHILD_KL_STEAM_AUTH_SMOKE'] = previous_smoke
        launched = re.search(re.escape(BUNDLE)+r':\s*(\d+)', (out/'launch.log').read_text())
        if not launched: raise ValueError('simulator launch did not return a process handle')
        receipt['simulator_process'] = probe_process_identity(int(launched[1]))
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
        receipt['observed'] = simulator_observation(args.simulator, out, run_id,args.auth_ui_probe,args.auth_ui_login,args.auth_ui_smoke)
    if args.device:
        run(['xcrun','devicectl','device','install','app','--device',args.device,app])
        run(['xcrun','devicectl','device','process','launch','--device',args.device,'--terminate-existing','--timeout','30',BUNDLE])
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Build receipt:',out/'receipt.json')
    return 0

if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'probe build failed: {error}',file=sys.stderr); sys.exit(1)
