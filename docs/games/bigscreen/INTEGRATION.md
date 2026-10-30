# Bigscreen Quest startup and WebKit integration

Current status (October 30, 2026): the simulator reaches My Room using Bigscreen's
own email login. The user has confirmed browser interaction and audible playback;
the WebKit bridge supplies real browser textures and input. Joining an online
room exposed a native SCTP socket crash. After its errno fix, the next attempt
reached an unimplemented video SurfaceTexture constructor. That consumer now
connects to the real decoder and passes a local decode-to-GPU test. The user
confirmed that joining an online room now works. Meta platform authentication
remains unavailable. Saved-session startup uses Bigscreen's own refresh-token
flow. Earlier sections retain the sequence of observed blockers.

Tested October 28, 2026 with the local standalone Quest APK:

- Package: `com.bigscreenvr.bigscreen`
- Version: `0.950.2.a276a2-beta-perma-rooms-beta`, code `4688857`
- APK SHA-256: `6cae96955397be3522c238196635a061c5f9da465f032cbe34b817dddbb4a18a`
- Unity `2020.3.33f1`, ARM64 IL2CPP, Oculus XR Plugin

The first physical Vision Pro boot reached Unity's native startup and aborted
with `fatal: called unresolved import 'putenv'`. This is a missing Klepton libc
binding, rather than evidence that the APK needs Android headset patching.
`runtime/libc/kl_shim.c` now serves `putenv`, preserving caller-owned string
storage and the bare-name removal behavior. `tests/t_putenv.c` checks lookup,
subsequent mutation of the supplied storage, and removal through the guest shim.

After that fix, the Mac lifecycle probe reached a graphics initialization wait.
This APK's `OculusUnity.initOculus()` obtains the Unity view's Surface and calls
the native `initComplete(Surface)`. Klepton only called the newer plugin's
`surfaceCreated(Surface)`. Both native signatures are `(JNIEnv*, jobject,
Surface)`; Klepton now resolves the newer export first, then the older export.
`tests/t_oculus_init.c` exercises both generations with real JNI Surface objects
and checks callback caching. No proprietary APK is needed for these tests.

The unmodified APK then completes 3,000 Mac frame-pump iterations with the null
graphics driver. With actual ANGLE/Metal enabled it submits Oculus eye layers
and reaches a later, explicit runtime stop:

```
[jni] no host implementation for com/vuplex/webview/WebView3D.setAlternativePointerInputSystemEnabled(Z)V
```

That was the next observed blocker in the first investigation. The simulator
work below implements those configuration calls and reaches the unavailable
browser itself. Browser functionality was still unimplemented at that stage. Do not label
this build compatible or interpret the earlier null-driver frame count as a
rendered, playable application.

## Why ovrport is not a required preprocessing step

The local `~/ovrport-app` checkout was checked against upstream and was already
at `1c7cb335c81369fbfd2957ad0edcb238956a8e3c`. Its library-copy and Unity/Java
patches target other Android headset runtimes. Klepton serves Oculus/VrApi
runtime library names itself. They do not implement the missing Darwin libc
binding or execute Bigscreen's Java WebView code on visionOS. No ovrport output
replaced the original APK or the imported library tree.

[AVP Play](https://github.com/myMartek/avp-play) provides useful store-ownership,
per-game recipe and error-reporting patterns. Its native installer/toolchain
workflow also translates Quest APKs with Klepton. Its reviewed installer and
toolchain code do not establish that this Bigscreen APK works or that ovrport
would supply these missing runtime methods.

## Evidence and reproduction

Local evidence is retained under `build/bigscreen-investigation/` (ignored):

- `klepton-boot.log`: original physical-device failure.
- `original-host-after.log`: libc fix alone; later startup wait.
- `original-host-metal.log`: initialization wait before callback fix.
- `original-host-oculus.log`: 60 frame-pump iterations after callback fix.
- `original-host-long.log`: 3,000 null-driver frame-pump iterations.
- `original-host-angle.log`: actual ANGLE/Metal and the Vuplex JNI stop.
- `installer-check.log`, `oculus-test-build.log`: validation evidence.

The `original` and `original.apk` entries in that evidence directory point at
the installer's immutable import. Reproduce the rendered Mac probe there with:

```sh
KL_ANGLE_DIR=/absolute/path/to/Klepton/vendor/out/Debug \
  KL_GLFB=1 KL_GLFB_MTL=1 KL_LIFECYCLE=1 KL_FRAMES=3000 KL_ALARM=90 \
  ../../build/m_boot original/lib/arm64-v8a
```

`KL_ANGLE_DIR` must be absolute because this probe runs outside the checkout
root. Both fixes build for the physical visionOS runtime and the signed
Bigscreen app. For new headset failures, use **Collect Headset Logs** in the
Mac installer; its report distinguishes missing runtime methods from missing
game content. Select the Bigscreen install job if another app was installed
more recently. Keep backups private; share diagnostic logs only after review.

The signed app with both fixes was installed on the paired physical Vision Pro
as job `bigscreen-fix-8bd562e8`. All 35 inventoried writable files were backed
up and restored, the APK/assets were staged in the new container, and the
transfer receipt was updated. There is no pending recovery. The original
backup remains in that job. Subsequent remote launch attempts timed out at
20 and 15 seconds without a new boot log, so the updated build's physical
startup was unverified at that stage. The simulator follow-up below removes
that device dependency; the initial Vuplex stop above was from the Mac with
actual ANGLE rendering.

Validation: `make installer-check` passes (51 installer Python tests, 14 Steam
update tests, 44 Node tests and Swift model checks). The model checks also
cover restoring the latest completed diagnostics job and ignoring unfinished
installs. Both new C runtime tests pass; the signed device app and Mac
installer build successfully. These checks do not establish full playability.


## visionOS simulator follow-up

The isolated **Klepton Bigscreen** visionOS 27.0 simulator
(`B7CF191F-8115-40ED-96DF-42D3155434F0`) reproduces the startup failures without
requiring the physical headset. It uses bundle `dev.klepton.bigscreen.probe`,
the original APK, a separate container and a build manifest under
`build/bigscreen-investigation/simulator/`.

The added JNI families implement the Java behavior needed for source-less
startup: Vuplex input flags, user agent, autoplay and DRM-request settings;
Bigscreen event callback registration; retained media/browser contexts;
independent idle player and web-client IDs; and external GLES texture creation.
Before a media source is opened, the APK's transport calls have no underlying
player to act on, no available frame and zero duration/position. These handlers
model that initial state. They do not open a movie, report playback success,
provision Widevine, produce browser pixels or emit invented completion events.
Actual source-opening/browser-construction operations remain unsupported.

The default GLES 3.2 description also led to an `Internal-Skinning.main` shader
failure followed by a native crash at `libunity.so+0x565930`. A diagnostic
injected setter first confirmed that a GLES 3.0 description passed that point.
The supported `KL_GLES_VERSION=3.0` switch now makes that choice without injection.
Version strings, GLSL strings and numeric version queries agree, while unset
and invalid values preserve existing target defaults. The Mac installer offers
**Advanced → GLES compatibility → GLES 3.0** for Unity imports, automatically
suggested only for this exact tested APK fingerprint. The selected setting is
validated and baked into the generated app; the final simulator run uses that
manifest setting rather than a launch override.

With strict JNI handling and GLES 3.0, the simulator renders Bigscreen's
apartment environment and remains alive. The retained initial final report
records 5,507 compositor iterations, 5,402 with a picture, no unresolved calls
and no native faults. This establishes rendered startup, not a working login,
network session or movie player. The visible browser-backed panel is unavailable:

```
[jni] Vuplex browser unavailable: Klepton needs a WebView browser/texture/JavaScript bridge; ovrport preprocessing cannot provide it
E/Unity: WebViewUnavailableException: The Android System WebView package is currently unavailable.
```

`isWebViewAvailable()` now returns false deliberately because no browser bridge
exists. Bigscreen takes its own unavailable-WebView error path. A separate Mac
probe also encountered a later null IL2CPP thread-local-state read at
`libil2cpp.so+0xa570e8`; that fault was not observed in the final simulator run
and has not been fixed. One earlier isolated Mac permissive diagnostic was used
to enumerate the remaining Java calls (`surface-diagnostic.log`); its results
were not used as compatibility evidence. Final simulator tests use strict JNI.

The visionOS status window now continues reading a bounded log tail after
native initialization. The browser failure changes the green initialization
badge to **Runtime support needed**, with a visible explanation and log-export
instructions. The warning remains visible while the room renders. Export log
shares the complete retained log file even though the live display uses a bounded
tail. The Mac
inspector warns about this tested APK's browser dependency before building,
and the log collector recognizes the unavailable-browser exception.

Reproduction helpers and evidence:

- `simulator/manifest.json`, `simulator/rebuild.py`: isolated simulator build,
  install, staging and launch (no physical-device mutation).
- `simulator/final-simulator-boot.log`, `simulator/final-report.json`: strict
  simulator startup evidence with the selected GLES setting.
- `simulator/host-availability.log`: default GLES shader failure.
- `simulator/host-gles30.log`: original GLES 3.0 diagnostic.
- `simulator/host-idle-players.log`: separate Mac IL2CPP TLS failure.
- `simulator/jni-tests.log`, `simulator/gles-version-tests.log`: new runtime
  regressions; `simulator/final-installer-check.log`: installer validation.

The physical headset retains the earlier two-fix build; this follow-up uses the
simulator. Rebuild through the updated installer to include these later changes.
The WebKit follow-up below supersedes the unavailable-browser result. Login and
actual movie playback remain development work.


## System WebKit follow-up

`runtime/media/kl_webview.m` embeds Apple's `WKWebView` on macOS and visionOS.
The C API in `kl_webview.h` queues browser commands onto Apple's main thread.
A view stays attached outside the host window's visible area: the game consumes
snapshots, without an extra browser panel covering its native status window.
Each browser owns its navigation delegate, script-message handler, snapshot
buffer and timer. Closing invalidates the timer, removes the handler and view,
and rejects pending events. Both ARC and the runtime's non-ARC builds are used.

`runtime/jni/kl_jni_vuplex.c` supplies the APK's WebView3D constructors, HTML and
URL loading, JavaScript execution/results, page messages, resize, basic DOM
pointer/scroll/key input, user agent and navigation operations. Asset URLs under
`file:///android_asset/` resolve into the imported APK's real asset directory,
including relative stylesheets. The page message adapter is an independent small
implementation of the public `window.vuplex` API and `_vuplexBridge.postMessage`.
TLS validation and WebKit's normal browser restrictions remain in force.

The browser never calls guest code from Apple's main thread. It queues bounded
events, then `kl_driver.c` drains them between Unity frames on the attached guest
thread. `UnityPlayer.nativeUnitySendMessage` receives real UTF-8 byte arrays;
JavaScript result consumers use Unity's proxy callback. Both observed
`ReflectionHelper.newProxyInstance` signatures are supported. Destroyed views
lose their queued destinations and pending consumer references.

Snapshots run at up to 10 Hz, with one request in flight per view, and produce
BGRA pixels at the requested game viewport size. Vuplex's real render call runs
on Unity's graphics thread. `kl_glfb_upload_webview_bgra` allocates storage for
the external texture name on its first frame or resize, then updates it while
restoring the previous texture/PBO/unpack state. The existing decoded-video
upload continues to use its original preallocated-storage behavior.

Bigscreen's `SetupClient` retains that same native view and its pointer/scroll
entry points route input to it. Android custom full-screen video views, Widevine
and Bigscreen's source-opening media player are not implemented by this browser
adapter. DOM-generated input also does not create trusted hardware gestures.
Do not interpret an initialized browser or its initial about:blank texture as a
working Bigscreen page, authenticated session or movie player.

The strict original-APK simulator run records three `WKWebView` initializations
and first texture uploads (1400×788, 1400×788 and 1152×788), without unresolved
JNI calls or the previous stream of native GL errors. The apartment renders.
The application remains on its initial data-loading panel and logs
`An unknown error occurred. Request failed.` No application page load is observed
in this run. A separate `KL_TRACE_NET=1` run reaches
`main-shark-cloud-api.bigscreencloud.com` and
`main-shark-api.bigscreencloud.com`; those connections alone do not establish
which request failed or its cause. No response, entitlement or login was forged.

Validation:

- `make build/t_webview && build/t_webview`: real WebKit page and pixels, DOM
  click, JavaScript result, messages in both directions, independent views,
  resize, APK asset URL/relative CSS, navigation failure and shutdown.
- `make build/t_bigscreen_jni && build/t_bigscreen_jni`: deferred cross-thread
  events into a registered Unity callback, UTF-8 payloads, invalid initialization,
  destroyed-view event rejection, both proxy generations and the Java
  UnitySendMessage wrapper, in addition to the earlier startup contracts.
- `make build/t_webview_texture` then
  `KL_GLFB=1 KL_ANGLE_DIR="$PWD/vendor/out/Debug" build/t_webview_texture`:
  actual ANGLE framebuffer readback checks BGRA conversion, first allocation,
  updates, resize, texture/PBO/unpack state restoration and the existing video
  upload path. Requires the prepared ANGLE checkout.
- `make installer-check`: Python, Node and Swift installer regressions, including
  actionable WebKit load/render diagnostics.
- Device/simulator runtime compilation and full-archive link gates, followed by
  the isolated simulator app build/install/launch.

The installer now describes this as an experimental WebKit integration, while
retaining the exact-APK GLES 3.0 suggestion. Browser navigation/content-process
errors point to connectivity and collected logs; persistent texture failures
point to a runtime development report. Early transient snapshots during WebKit
startup do not trigger that warning. Rebuilding through the installer includes
the bridge; the physical headset has not been updated by this simulator work.

Evidence is retained locally under `build/bigscreen-investigation/` in
`webview-test.log`, `webview-jni-test.log`, `webview-installer-check.log`,
`webview-texture-test.log`, `webview-final-build.log` and the isolated simulator
`webview-final-simulator-boot.log` / `webview-final-report.json`. These logs may contain
local paths or application details; review before sharing.

## Initial loading, graphics and account-service follow-up

The initial `An unknown error occurred. Request failed.` was traced to the
Oculus SDK callback chain, not to a failed Bigscreen HTTP response. Returning
request ID zero from `ovr_User_GetLoggedInUser` left this APK's Unity request
awaiter registered indefinitely. A nonzero request with a correctly typed
error completion lets Bigscreen take its own offline path and open the menu.
It then displays `Couldn't reach Oculus. You won't be able to use Bigscreen's
online features.` This is an accurate missing-service result, not a successful
Meta login. No HTTP request to Meta was made by that local handler.

Opening the menu exposed a second crash: Unity's clipboard native code called
`strlen(NULL)` at `libunity.so+0x692338`. The original APK's Java implementation
returns a non-null empty string when no clipboard text exists. The JNI adapter
now does the same; the soft-input regression covers both empty and populated
clipboards.

The unreadable rectangular glyphs were R8 font atlas alpha swizzles ignored by
ANGLE's Metal simulator backend. Native texture swizzling is now enabled on
Apple Silicon simulators. The bundled simulator renderer also predated the
tracked ASTC mip-copy fixes. Rebuilding it from the reconciled patch restored
the room's wood floor and other sampled textures. The existing GPU copy
self-test now checks four R8 alpha samples in addition to 23 ASTC samples and
five invalid-copy cases. Both pass in the simulator without a feature override.
Both device and simulator ANGLE frameworks have been rebuilt; physical-headset
rendering has not been verified in this follow-up.

### What automatic Oculus login needs

Read-only inspection of this exact APK's metadata and native method tables
establishes this sequence:

| Stage | Guest method / native offset | Result needed |
| --- | --- | --- |
| Startup identity | `BIG_APP_LOADER.GetOculusUser`, `0xb89150` | `Users.GetLoggedInUser` |
| Offline retry | `SettingsScreen_Account.ReturnToLogin_Offline`, `0xdce0e8` | Restarts `StartConnectionFlow` |
| Automatic login | `Accounts.OculusLogin` continuation, `0x18c5e70` | `Utils.GetOculusUserData` |
| Account data | User-data callback, `0x1ead79c` | `Users.GetOrgScopedID` after the real user |
| Account proof | Organization callback, `0x1eae1fc` | `Utils.GetOculusUserProof` |
| Proof request | Proof continuation, `0x22f4510` | `Users.GetUserProof` |

`Bigscreen.Cloud.OculusUserData` contains the Oculus app ID, user ID, alias,
image URLs, organization-scoped ID and nonce. The startup log supplies app ID
`2497738113633933`. A synthetic local profile cannot provide that authenticated
identity. Meta documents a proof nonce as single-use and verified by the app's
backend: [User proof API](https://developers.meta.com/vr/reference/platform/v207/function_o_v_r_requests_user_8h_ovr_user_get_user_proof/).
Meta's standalone development initialization requires an app ID and suitable
credentials; the documented SSO linking flow requires the app owner's
registered redirect and server credentials. Neither is supplied by adding a
WebView. See [native setup](https://developers.meta.com/vr/documentation/native/ps-setup/)
and [account linking](https://developers.meta.com/vr/documentation/unity/ps-account-linking/).

The user confirmed that Settings → Account & Prefs → Go Online returns to Home
and shows the same Oculus notice. That matches the retry method above.
`Accounts.Login` (`0xe09a54`, continuation `0x18c51c4`) contains a separate
email/password HTTP route, but reaching that form and signing in through the
unmodified UI remain unverified. Its existence does not establish an available
fallback from this APK's offline state.

The platform adapter now also completes missing user-proof and organization-ID
requests with typed errors (`0x22810483`, `0x18f0b01b`) instead of request zero.
Online access-token requests fail explicitly; the existing synthetic token is
confined to explicit `KL_NET_OFFLINE=1` diagnostics. The installer and visionOS
status window distinguish missing Meta account support from connectivity,
browser or password errors. These changes improve failure/recovery behavior;
they do not implement a Meta account provider or establish online playability.

[AVP Play's Meta login](https://github.com/myMartek/avp-play/blob/main/core/Sources/AVPPlayCore/MetaLogin.swift)
uses Meta's platform utility to obtain a token for store/library operations.
That source contains no running-game user-proof bridge; its token must not be
treated as Bigscreen's fresh SDK proof nonce.

Validation and investigation evidence under `build/bigscreen-investigation/`:

- `auth-types.json`, `managed-methods.json`, `auth-disassembly.txt`,
  `login-continuations.txt`, `offline-ui-disassembly.txt`: local read-only
  inspection of the user's APK (not committed or redistributed).
- `auth-errors-test.log`: real request queue, unique request IDs, typed account
  errors and no fabricated online access token.
- `auth-installer-check.log`: installer diagnostics/model regressions.
- `simulator/fresh-angle-regressions.log`: R8 and ASTC GPU readback checks.
- `touch-pose-test.log`: exact Touch pose through render/physics node reads,
  both hands, rotated rays and physical-frame correction on/off.

Simulator input uses the system selection ray when no tracked hands exist.
It applies the same standing-height lift as the simulated head and inverts the
physical controller grip correction before publishing the aimed Touch pose.
The prior implementation applied that correction twice, pitching selections
below the simulator pointer. Physical controller publishing is unchanged.
The input regression checks the pose seam; visual alignment still needs
confirmation in Bigscreen after rebuilding the simulator app.

The remaining online blocker is a genuine Meta account provider. A signed-in
Quest is the next reference environment to compare the real platform flow and
investigate whether an authorized bridge is practical. Merely connecting it
does not automatically provide the simulator with a valid session. Login and
playback have not yet been demonstrated.

## Quest reference and direct account-screen experiment

The user's USB-connected reference headset is an original Quest (`monterey`),
running Android 10 / API 29 and Horizon `50.0.0.198.257`. Its store-installed
Bigscreen is `0.950.0.207286-beta-perma-rooms-beta` (version code 3814346),
different from the simulator APK. The user reports that Bigscreen online
support for this device was retired and confirms the same Oculus notice on it.
It is therefore not a working online baseline.

A live startup capture shows Platform SDK API `1.1.81.0` loading implementation
`1.1.82.0` from Horizon after the newer `com.oculus.platformsdkruntime` package
is absent. It also records managed `NameResolutionFailure` exceptions and
repeated `PointerInteraction.IsRequestingPrimaryInput` null references. Network
inspection separately finds a connected, validated network and successful DNS
resolution for `graph.oculus.com`. These observations do not establish the
precise server-side cause of the Quest's failure.

Read-only inspection of that headset's Horizon implementation establishes why
a generic ADB helper is not automatically a Bigscreen account provider:
`CallerUtils.getCallingPackage` derives package identity from Binder UID;
`ExternalPlatformLocal.getAppID` requires the installed library's app ID or
developer access to that app. Application authentication includes signer and
version and declares device-scoped authorization. No helper was installed,
no credentials were exported and no identity checks were changed. The local
system APK/code used for inspection stays under the ignored investigation
directory. Connecting this Quest does not give Klepton an authenticated
Bigscreen session.

There is a distinct UI routing problem worth testing independently. The APK's
`VRWrapper.IsOculusStore` returns true whenever `Platform.IsAndroid` is true,
before consulting any store metadata. Changing the reported installer would
not route it to email login. Its existing `TabletUI.GoToLogin` method does open
an account screen: a strict simulator experiment invoked that method once on
the active component through the IL2CPP embedding API. The method returned
without exception and the simulator visibly displayed **Create Account** with
email/username/password fields and a **LOG IN** button. The Oculus notice still
needs dismissing over it.

This experiment is enabled only by
`KL_PROBE_INVOKE=Bigscreen.UI.TabletUI::GoToLogin`, once at frame 1800. The new
generic probe accepts only an explicitly named zero-argument method and exactly
one active component; it does not modify the APK, provide an identity or alter
server authentication. Its disabled-state regression and both visionOS runtime
build/link gates pass. The normal installer path does not enable the probe.
Actual email sign-in and subsequent account/profile operations are still
unverified; reaching the account screen alone is not a login result.

New local evidence lives under `build/bigscreen-investigation/quest-reference/`:

- `reference-report.json`: version, network and platform findings without
  credential values.
- `bigscreen-relaunch-redacted.log`: app-only startup stream, sanitized before
  persistence; no device-wide log buffer was cleared.
- `store-selection-disassembly.txt`: read-only store-routing inspection.
- `simulator/manifest.json`, `simulator/rebuild.py`: dedicated simulator probe
  build, preserving the existing data container.
- `mprobe-gate-test.log`, `email-probe-runtime-build.log`: disabled probe gate
  and both platform runtime/link checks.

### Email login rejection investigation

The user reached the existing Login form but received “Couldn't log in”. The
running simulator logged `HTTP error: Input error. Call GetErrorList() to get
the full list.` twice per attempt, without the individual validation errors.
This establishes a reported HTTP input error, not successful login or a known
wrong password. The startup Meta-provider error remains a separate limitation.

A temporary, read-only debugger probe is armed on the existing simulator process
for the next attempt. It observes only whether login fields are empty, whether
the email contains an at sign or outer whitespace, the HTTP response status,
and allowlisted words/field names from CloudException validation details. It
does not log entered credentials, response tokens or arbitrary response text.
No APK or server response is altered. Login and playback remain unverified.

The installer diagnosis and visionOS warning now recognize this specific input
error ahead of the generic missing Meta-provider warning, with instructions to
check text entry and collect logs if the form is correct. The installer keeps
independent crash and unsupported-runtime errors at higher priority.

The next live attempt reached `Accounts.Login` with nonempty email and password
strings, but the email string contained no at sign. `HandleLoginResponse` received
HTTP 422 and `CloudException.GetErrorList` contained an email-not-valid validation
message. No credential values were retained. This confirms email validation
failed; it does not establish whether a username was entered, text entry dropped
a character, the password is correct, or later Meta-dependent login steps work.
The existing simulator was restarted after the debugger paused on Unity GC
signals; SIGUSR1 and SIGXCPU now pass through without debugger stops.

### Successful email login and the browser lifecycle crash

After replacing the accidentally entered username with the full account email,
the user logged in and returned to My Room. The read-only debugger recorded
HTTP 200 from `Accounts.HandleLoginResponse`. This verifies Bigscreen's own
email/password authentication for this APK in the simulator. The app still
reports missing Oculus identity during its profile/purchase follow-up; Meta
account integration, social features and movie playback are not verified.

The apparent freeze after login was a real strict-JNI abort on the inherited
instance method `com/vuplex/webview/WebView3D.pause()V`. Its original Java
implementation queues `android.webkit.WebView.onPause()`; `resume()` queues
`onResume()`. These are independent of the separate global pauseTimers API.
The WebKit adapter now implements both instance operations: it retains the page
and last texture, suspends media through WebKit's public playback-suspension
API, and stops snapshots until resume. An explicit setRenderingEnabled(false)
continues to apply after resume, and other browser views remain independent.

Validation: the strict JNI instance-call regression passes, including failed
initialization and calls after destroy. The real WebKit test verifies that one
view's texture stops advancing while another continues, and that resume does
not override a disabled renderer. Device and simulator runtime link gates and
Swift typechecking pass; the shared XCFramework retains both platforms.
The updated simulator app is installed with its data preserved. A diagnostic
AttemptLogin invocation could not locate BIG_APP_LOADER as a Unity component
and was skipped; no authentication result was invented. The ordinary account
screen is reopened for a fresh post-login transition test.

Evidence under `build/bigscreen-investigation/quest-reference/` includes
`post-login-crash-report.json`, `browser-pause-*-test.log`,
`browser-pause-runtime-build.log` and `simulator/browser-pause-rebuild.log`.

### YouTube setup, refresh and Oculus overlay transport

The user verified login and reached YouTube, where strict JNI aborted on
`WebView3D.setUserAgent(Z)V`. A direct diagnostic invocation of the existing
`HomePage.OnYoutubeSelected` callback reproduced the opening path in an offline
room and then found `setFocusedInputFieldEventsEnabled(Z)V`. Both are now backed
by WebKit: boolean UA selection chooses native mobile/desktop webpage preferences
and clears a custom UA; focus events classify TEXT/IFRAME/NONE without including
field contents. Global policy applies to current and future views; per-view
changes remain isolated. The original Java implementation confirms true selects
the default mobile UA and false selects a desktop UA. Klepton uses WebKit's
native content modes rather than advertising a Chromium engine.

Refreshing then exposed `BigWebView.ExitFullScreen(I)V`, confirmed by the user's
crash log and native SIGABRT report. It now closes WebKit media presentations;
IsFullScreen reads a cache maintained from WebKit's public fullscreenState KVO
notifications. A subsequent diagnostic invocation of the existing
`BigBrowser.OnReload` callback returned without exception or an unsupported JNI
call. This establishes that the refresh crash is addressed; movie playback is
still unverified.

The black browser panel also exposed an independent overlay transport issue.
OVROverlay requested swapchain images on Unity's main thread without a GL
context. Non-eye requests now queue real allocation at EndFrame4 on the render
thread and return NotInitialized until a retry can receive its actual handle.
The real ANGLE context is queried before any GL call; this also prevents a
debug-ANGLE null-context crash. The pending queue deduplicates requests and
ignores destroyed-layer requests. Render-thread allocation preserves the guest
texture binding and publishes a handle only after storage exists.

Legacy EnqueueSubmitLayer2 GLES submissions are now copied into Metal layer
storage at the render-thread frame boundary, before the frame fence. Vulkan's
existing path is preserved. Bigscreen also submits its browser panel with a
negative Y scale. That scale now becomes a positive physical height plus a
vertical texture reflection, so the compositor no longer rejects the panel as
invalid geometry. Reflected X scales are handled in the same way.

Validation includes the real WebKit page/UA/focus/lifecycle tests, strict JNI
regressions, a new ANGLE-to-Metal overlay regression with pixel readback, and
compositor math/Metal shader tests. Device and simulator runtime link gates and
Swift typechecking pass. The isolated simulator is rebuilt with both runtime
platforms preserved; UI testing and actual playback remain in progress. A
compressed ASTC source in the room's separate 1024x1024 overlay still fails a
framebuffer blit; this is distinct from the RGBA browser texture.

Evidence is under `build/bigscreen-investigation/quest-reference/`, including
`youtube-refresh-crash-signatures.json`, `youtube-refresh-report.json`,
`youtube-*-test.log`, `overlay-reflection-test.log`, and
`overlay-reflection-reproject-test.log`. `invoke_component.py` rearms the
explicit managed UI diagnostic on an attached simulator process; it never
provides an identity, credential or server response.

The updated simulator now visibly renders YouTube's homepage, including its
logo, search field and “Try searching to get started” text. The compositor logs
layer 6 as a real 1.139419x0.64312494 m quad at
(0, 1.5645729, -1.0796318), backed by the 1400x788 browser texture. The startup
Oculus notice was dismissed through its existing Close_BigscreenUI method and
the existing BigBrowser.Show method reopened the browser. This confirms browser
page rendering; actual video playback is awaiting user interaction.

A further source/test update propagates same-document history push/replace/hash
changes and dynamic page titles using Vuplex's URL-action schema, including
Title. This is needed for sites that switch videos without a full navigation.
Its real WebKit regression passes, and this update is installed in the current
simulator build.


### Browser taps and pointer occlusion (2026-10-29)

The original managed browser sends DEFAULT_BUTTON_ID=1 and
DEFAULT_BUTTON_STATE=1 to BigWebView.PointerDown/Up. The Android BigWebClient
routes these as touch DOWN/UP, but our adapter had interpreted button 1 using
Vuplex's explicit MouseButton enum, producing a right-click. The Bigscreen
adapter now translates these touch events to primary DOM taps. The separate
Vuplex mouse API retains right/middle button semantics. A new full JNI-to-real
WebKit regression (`tests/t_bigscreen_web_input.m`) failed on its DOM button
assertion before the fix and passes afterwards, including text focus and an
explicit Vuplex context-menu regression. It does not record entered text.

Bigscreen's browser layer 6 submits flags 0x44: OVRPlugin's onTop bit 0 is clear,
so the browser is an underlay. The compositor previously drew every quad after
the eye, covering the Unity pointer and popup. EnqueueSubmitLayer2 now decodes
composition order into an owned field, independently of other producers' flag
formats. The visionOS compositor draws those quads before the eye and uses the
guest's authored eye alpha with premultiplied blending only when a real underlay
was drawn. Ordinary eyes remain opaque. The underlay pass does not write depth
when an eye will follow, so it cannot discard the far-plane eye picture.

Metal pixel tests verify that an opaque laser pixel covers a browser, transparent
eye holes reveal it, premultiplied edges blend correctly, and the default eye
still ignores zero source alpha. The real ANGLE-to-Metal test also verifies
both onTop=1 and flags=0x44 composition metadata. The xros and xrsim runtime
link gates and full simulator app build pass. The current simulator process is
23047; YouTube renders, and the Oculus notice is visibly in front of its page.
The user confirmed that the laser appears over YouTube and the search box
responds to clicks. Actual moving video, audio and refresh after playback are
still awaiting validation. No Meta platform authentication is supplied by these
changes.

Evidence: `browser-input-before-fix.log`, `browser-input-after-fix.log`,
`browser-underlay-test.log`, `browser-underlay-image-test.log`,
`browser-input-layer-xros.log`, and `browser-pointer-rebuild.log` under the
ignored `build/bigscreen-investigation/quest-reference/` directory. The native
input trace records only integer routing, coordinates and buttons.


### Moving-video snapshot regression (2026-10-29)

`make build/t_webview_video && ./build/t_webview_video` generates a local H.264
clip with three alternating colors, plays it through the shipping WKWebView
adapter, and verifies that all three decoded colors reach BGRA texture snapshots
before and after reload. The test also checks ready state, paused state and media
error status. It runs on macOS, uses no network or account, and mutes its fixture;
it does not establish audio or YouTube playback in the visionOS simulator.

The first fixture assertion incorrectly required near-exact RGB endpoints after
lossy H.264/YUV conversion. Decoder samples were colored rather than black;
using a tolerance that distinguishes the three primaries makes the unmodified
adapter pass. A temporary canvas experiment was removed. No video capture
workaround or runtime change from this experiment is installed or retained.
Evidence: `webview-video-refresh-test.log` (successful final test) and
`webview-video-build.log` in the investigation directory. The simulator remains
on the build whose page input and pointer visibility the user confirmed.

The user supplied https://www.youtube.com/watch?v=K-Dt5ssd6F4 for the public
video test. A playback outcome was not included with the link, so moving video,
audio, and refresh for that URL remain unverified.


### Direct video-page navigation (2026-10-30)

At the user's request, the linked YouTube watch page was opened directly in the
running visionOS simulator, without another Bigscreen login. The page and its
video image are visibly unobstructed in the browser panel. Moving playback and
audio have not yet been confirmed.

Calling BigBrowser.Show alone leaves the home page layered above the browser.
The working navigation is TabletUI.GoToPage(BigUIState.BigBrowser, false, true,
false), where the APK metadata assigns BigBrowser=1. Invoke it through
il2cpp_runtime_invoke on the attached Klepton Guest thread at kl_mprobe_tick,
with exactly one active TabletUI instance, and check the managed exception.
Then kl_webview_load on the existing main browser's host opens the supplied
watch URL. For this process the main JNI browser is ID 4; resolve it afresh
before reuse. The debugger was detached and the simulator left running.

Resolve the il2cpp exports with kl_sym(kl_find_image("libil2cpp.so"), name).
The optimized static `il` aggregate is not reliably materialized by LLDB:
several fields read instruction bytes instead of function pointers. Explicit
export addresses worked. No account state or authentication response was changed.
HomePage.Minimize disables the whole tablet, including this browser; it is not
the correct way to remove the home page overlay. EnableTabletUI restores that
case before selecting the browser's page through GoToPage.


### Paused video and audio check (2026-10-30)

After the user reported no sound, read-only media diagnostics found the video
paused at 85.662 seconds, unmuted, with player volume 1 and no media error.
The WKWebView itself was rendering and not suspended. AVAudioSession was in
Playback category, routed to Speaker, with outputVolume approximately 0.6.
Calling the current video's play() through the existing WebKit adapter resumed
it. A subsequent check found paused=false, muted=false, volume=1, time=105.587,
one audio track, and no play() rejection or media error. Audible output remains
awaiting user confirmation. No runtime source change was needed for this step.

The native debugger was detached after each check. The diagnostic JSON records
only enumerated media state and counters, never document text, form fields,
credentials, account data or cookies. WebKit's optional decoded-byte/frame
counters returned zero and are not used to infer silence or decode failure.
For safe main-thread evaluation from LLDB, NSOperationQueue.mainQueue's
addOperationWithBlock worked; dispatch_get_main_queue's optimized inline helper
was unavailable in the debugger expression environment.


### Simulator audio routing resolved (2026-10-30)

The user confirmed that the entire simulator was silent, including room/menu
sounds. CoreAudio showed both KleptonBigscreenProbe and its WebKit GPU process
actively using the Mac's default Elgato Wave XLR output. That device was unmuted
at approximately 68% volume. Wave Link's selected monitor output was FIIO K19.

Device Hub > Device > Sound > Output > FIIO K19 changed only this simulator's
output. CoreSimulator's audio port then reported the explicit FIIO route, and
both game and WebKit process output devices changed from AudioDeviceID 123
(Wave XLR) to 119 (FIIO K19). The Mac default output remained 123. The user then
confirmed “I hear it now” and “Yes, sound works.” This resolves audible playback
for the current test environment. No audio-code change or app rebuild was needed.
Do not hard-code this user's output device into the app or installer.

For this environment, if audio is silent again after a route/device change,
check Device Hub's simulator output against the device being monitored, along
with the video's paused/muted state. Playback was initially paused and was
started directly before correcting the output route. The isolated simulator is
left running on the supplied public YouTube video with audible sound confirmed.


### Normal email-login fallback (2026-10-30)

The Quest build's Go Online path retries Meta authentication instead of exposing
its real email/password form. Earlier manual GoToLogin/GoToPage debugger calls
proved the UI existed, but did not fix this normal route. The runtime now has a
compatibility fallback for package com.bigscreenvr.bigscreen, version 4688857.
After the Oculus SDK consumes a failed logged-in-user message, a bounded request
is handled between Unity frames on its attached guest thread. The fallback uses
the IL2CPP embedding API and the app's own navigation: it reads Login and
PopupMessage from BigUIState metadata, ends the platform popup, and opens Login
through TabletUI.GoToPage. It resolves exactly one active tablet and retries
briefly while the UI initializes. It does not read form values or supply any
account, credential, token or successful server response.

An existing Bigscreen websocket connection prevents automatic navigation, so a
later failed Meta request cannot interrupt a connected session. Only failed
logged-in-user completions trigger the fallback; proof, token, organization,
purchase and other platform failures retain their normal handling. Other APK
packages/versions are untouched. KL_BIGSCREEN_EMAIL_LOGIN=0 disables the fallback
for diagnostics. The normal installer/runtime build includes it; no probe env
setting is needed.

The 11-case mock regression covers no request, wrong package/version, opt-out,
normal and duplicate failures, a connected session, delayed UI, managed failure,
missing UI metadata and bounded timeout. It is included in make check. Typed
platform-error and disabled managed-probe tests pass; device and simulator
runtime link gates and the full simulator app build pass. The freshly restarted
simulator (PID 98118) automatically displayed the actual Login form with Email,
Password, LOG IN, RESET and SKIP, with no Oculus notice covering it and no
KL_PROBE_INVOKE navigation. A fresh user sign-in is pending. The FIIO K19 simulator
output route was retained across the app restart.

Evidence: email-fallback-test.log, email-fallback-platform-test.log,
email-fallback-probe-gate-test.log, email-fallback-runtime-build.log and
email-fallback-simulator-build.log under the ignored investigation directory.


### Apparent freeze and simulator input recovery (2026-10-30)

The user could not interact with the new Login form and later clarified that the
entire simulator window could not be moved. The guest was still presenting near
60 Hz, the app was foreground-active, and no crash was recorded. A native
breakpoint verified that even the compositor's raw spatial-event callback was
not reached. Application/window metadata showed an active CPImmersiveScene with
its normal visible event window; no credential or text-field values were read.

The compositor now registers and clears onSpatialEvent on the main thread,
matching the public API's main-actor contract, and logs the first raw event per
renderer. Swift typechecking and the full simulator build passed. This alone
did not restore delivery. Reopening immersion and rebooting the isolated
simulator also did not restore delivery in automated tests. A temporary 2 m
reprojection-depth experiment made no difference and was not retained; the
reboot restored the normal environment/depth settings.

Restarting Device Hub with its “Quit and Keep Simulators Running” action and
reopening the compact simulator window restored raw and accepted spatial input.
The guest stayed alive through the interface restart (PID 4863), and a subsequent
test click produced the first callback marker and two accepted spatial events.
Device Hub changed from PID 2245 to 8074. The Login form remains visible and the
FIIO K19 simulator route is retained. The user subsequently confirmed that window
movement and login input respond, then reached an online room join. The guest was not
established to be deadlocked; do not label the initial symptom a login crash.

Evidence: login-input-before-simulator-reboot.log, login-input-window-state
metadata in the container, devicehub-input-sample.txt, login-input-swiftcheck.log,
login-input-main-thread-build.log and login-input-recovery-report.json in the
ignored investigation directory. These retain input/thread/window metadata,
not login contents.

### Online room join: real SCTP socket failure (2026-10-30)

The user's next room join crashed in `libBigSoup.so+0x22b0a0`,
`register_ulp_info`, with a null socket. `BigData::CreateConnection` passed the
unchecked result of `usrsctp_socket` to `usrsctp_set_ulpinfo`. In the APK's native
code, `usrsctp_socket` stores `socreate`'s result through `__errno`, then reads it
through a second `__errno` call. Klepton's previous accessor translated Darwin
errno on every access, discarding the guest's successful zero assignment and
replacing it with stale Linux EAGAIN (11). This falsely returned a null socket.

`runtime/libc/kl_errno_bridge.c` now supplies BigSoup with a thread-local Linux
errno slot. Guest writes are committed before each resolved native function
import. A signed ARM64 tail-call barrier preserves integer, vector, stack and
indirect-return arguments. Import resolution applies this only to BigSoup's
ELF, dylib and signed framework filenames; other libraries retain their existing
bindings. `KL_BIGSOUP_ERRNO=0` restores the previous behavior for diagnosis.
The APK and authentication responses are unchanged.

Validation uses the original APK's actual SCTP implementation in
`tests/t_bigscreen_sctp.c`: with the bridge disabled, an unrelated old EWOULDBLOCK
produces a null socket and guest errno 11. With the bridge enabled, AF_CONN socket
creation succeeds, the real ULP registration completes, and an invalid domain
still returns a genuine EINVAL (22). This local test does not establish a remote
association or join a room. `tests/t_errno_bridge.c` also checks repeated native
failures, Linux/Darwin error mapping, thread isolation, arguments and a large
return value. Device and simulator runtime link gates and the simulator app
build pass. A live debugger check confirmed the signed BigSoup errno import
points to the new accessor, then detached. The patched app is running; the next
human room join remains the end-to-end check.

Evidence in the ignored investigation directory: room-join-native-disassembly.txt,
room-join-sctp-creation.txt, room-sctp-before-fix.log, room-sctp-after-fix.log,
room-errno-test.log, room-errno-runtime-build.log, room-join-simulator-build.log
and room-join-crash-report.json. No account fields, tokens or room identifiers
are needed by these checks.

### Room join follow-up: incoming video surface (2026-10-30)

The next human attempt crashed at a different point. The boot log names
`android/graphics/SurfaceTexture.<init>(I)V` as the missing JNI implementation.
While reporting that failure on BigSoup's worker thread, the draw census issued
`glReadPixels`. SimMetalHost crashed in compressed texture readback and the app
aborted after losing its Metal-service connection. The JNI failure preceded the
Metal failure; this is not the previous null SCTP socket crash.

`runtime/jni/kl_jni_surface.c` now implements the measured SurfaceTexture
constructor, updateTexImage and its Surface producer. Construction does no GPU
work. ANativeWindow_fromSurface returns the consumer's actual decoder window,
and AMediaCodec releases real VideoToolbox frames into its queue. updateTexImage
acquires the latest frame and binds its IOSurface/Metal image to the guest's
external texture on a thread with a current GL context. Native windows and
configured codecs retain the queue across JNI surface release. The imported
AMediaFormat_setFloat also preserves floating-point values. This does not add
protected media support or synthesize an incoming stream.

The draw census now reports CPU metadata without any GL calls or pixel readback.
Explicit rendering probes remain separate from fatal reporting.

`tests/t_surface_decode.c`, included in make check, creates the JNI objects on a
context-free worker, configures a real native decoder surface, decodes two HEVC
frames from the repository fixture, latches the latest timestamp and reads
nonuniform, opaque 320x240 pixels from the actual ANGLE texture. It also checks
the floating-point format field and release ordering while the codec and native
window still hold the queue. This host test passes, along with browser texture,
errno, email fallback and platform-error regressions. Device/simulator runtime
link gates and the full simulator app build pass. The patched simulator is
running after recovery (PID 24550). The user then confirmed the online room join
works.
The first launch after installation exited before the guest booted because the
earlier SimMetalHost failure also broke the simulator's RealityKit device
initialization. Restarting this simulator with its data retained restored the
Metal service. The new process reaches the visible Login form and keyboard,
receives spatial input and has no current native fault or missing-JNI diagnostic.

Evidence: room-surface-test.log, room-surface-runtime-build.log,
room-surface-simulator-build.log, room-surface-*-regression.log and
room-join-video-surface-report.json in the ignored investigation directory.
The original platform API contracts are documented by Android for
[SurfaceTexture](https://developer.android.com/reference/android/graphics/SurfaceTexture)
and [Surface](https://developer.android.com/reference/android/view/Surface).

### Saved Bigscreen session after restart (2026-10-30)

The user confirmed the room join and requested persistent sign-in. Bigscreen
already writes its refresh token into settings.xml under persistentDataPath;
SharedPreferences and the simulator container survive application updates.
Startup was stopping at Quest account service failure before reaching the
existing saved-session check. No password store or credential-entry automation
is needed.

After that genuine Meta failure, the compatibility flow now asks
Accounts.GetRefreshToken whether a saved token exists, then starts the app's
CheckAccessTokenStatus task. That method performs the original status request
and RenewAccessToken flow, including server token rotation and persistence.
The host retains the task through an IL2CPP GC handle, polls only its completion
and boolean result on Unity's attached thread, and never copies token text.
Only a true result starts BIG_APP_LOADER.OnLogInSuccess, which fetches the
profile and connects normally. Missing tokens, rejection, cancellation and
faults return to the email form. A pending server task uses the existing client
timeout; it does not start a competing password request.

The first live restart verified server acceptance and a connected websocket,
but left the existing Login panel visible (state 25). The flow now waits for
the real websocket connection and calls TabletUI.GoHome if that panel remains
open. Other pages and already-connected sessions are preserved. The automatic
session attempt runs once per process, so it does not undo a later logout.
KL_BIGSCREEN_RESUME_SESSION=0 skips this behavior.

Twenty cases in tests/t_bigscreen_login.c cover the original gating and UI
behavior plus accepted/rejected/faulted/canceled tasks, GC handle lifetime,
delayed completion, empty saved tokens, opt-out, invocation failures and
navigation only after connection. The targeted tests and both platform link
gates pass. The final simulator restart (PID 37154) verified server acceptance,
an online websocket and automatic return to the visible My Room home panel,
without credential entry. A second process start reused the token persisted
through the earlier session renewal and app update. No native fault was recorded.

Evidence: session-resume-test.log, session-resume-runtime-build.log,
session-resume-simulator-build.log, session-state-debug.log and
session-resume-report.json under the ignored investigation directory.

### Social TV channel: native streaming player (2026-10-30)

The user's social TV channel attempt aborted on the missing JNI method
BigMediaPlayerAPI.StartStream(ILjava/lang/String;)V. This uses the APK's separate
ExoPlayer media path, rather than Vuplex/WebKit or BigSoup's peer decoder.
The boot log preserves the method signature; no source URL is needed to identify
the failure. The native fault file was absent because this was a deliberate
strict-JNI abort.

runtime/media/kl_stream.m now supplies an AVPlayer adapter for ordinary streams.
AVPlayerItemVideoOutput produces real BGRA IOSurface frames; a main-thread timer
records the latest frame, time, dimensions and state. Native playback supplies
audio. On Unity's guest thread, the JNI bridge delivers the APK's registered
Ready, Playing, Buffering, Ended and PlaybackError callbacks. Frames bind to the
guest's external texture only with a current ANGLE context, preserving the
previous texture binding. The frame loop handles upload even when the guest's
texture-id query comes from another thread. Pause, resume, seek, volume, stream
replacement and shutdown use real player state. Generation checks discard
outdated callbacks, including callbacks invalidated by a reentrant Stop.

Missing/unsupported addresses, unavailable sources and unsupported MPEG-DASH
return the app's source-error JSON callback. Protected-media entry points report
DRM absence through its existing callback. This adds no Widevine/FairPlay bridge
and does not claim protected playback. Error diagnostics use numeric native
codes and fixed categories; signed URLs and arbitrary NSError messages are not
logged.

tests/t_bigscreen_stream.m exercises the exact two-argument StartStream signature
against HTTP HLS on an ephemeral loopback server and also checks a local H.264
MP4. Both fixtures are generated locally with AVAssetWriter; no external account,
service or downloaded content is used. Both reach real Ready/Playing/Ended events
and red, blue and green GPU texture samples. The test also checks pause/resume,
floating-point seek and volume through JNI, preservation of texture bindings,
asynchronous open failure, unsupported-format/DRM callbacks and reentrant stop.
The stream test, existing JNI regression, both platform runtime link gates and
simulator app build pass. The installed app (PID 49193) restored its session and
reached My Room. A retry of the actual social TV channel is pending; the local
fixture result is not proof of that channel's format, authorization or playback.

Evidence: social-tv-boot.log, social-tv-stream-test.log,
social-tv-stream-test-build.log, social-tv-runtime-build.log,
social-tv-jni-regression.log, social-tv-simulator-build.log and
social-tv-crash-report.json in the ignored investigation directory.
The adapter uses Apple's public
[AVPlayerItemVideoOutput API](https://developer.apple.com/documentation/avfoundation/avplayeritemvideooutput).

The first real channel retry stayed alive and entered the room, but the
environment faded to black while the menu remained visible. Read-only live
metadata showed a ready, playing 1920x1080 stream at about 153 seconds, 2,866
produced/notified frames and zero uploaded frames. The Unity frame pump and
texture query had no current GL context, so their guarded upload correctly
declined the operation but never reached the actual render thread.

The bridge now registers a render service with kl_glfb. Presentation invokes it
on the current GL thread; if submission arrives without a context, the existing
deferred capture path invokes it before the next draw/flush on that thread.
No context is stolen or created on Unity's main thread. The local stream test
now exercises both direct presentation and a context-free submitter followed by
a render-thread flush, checking that video storage exists before the texture-id
query can perform an upload. H.264/HLS GPU colors, transport/error behavior and
GL binding preservation still pass. Both platform builds pass and the updated
simulator (PID 52752) restored its session. The user then confirmed that the
live channel video and room are visible. Channel audio was not separately
confirmed in this retry.

Additional evidence: social-tv-stream-state.log, social-tv-dark-room-boot.log,
social-tv-render-test.log, social-tv-render-runtime-build.log and
social-tv-render-simulator-build.log.

### Lobby AVPro player and avatar shader diagnosis (2026-10-30)

The next Lobby join aborted in strict JNI dispatch at AVProMobileVideo's
GetPluginVersion, before any video was opened. This is the APK's separate
AVPro 1.9.17 integration, not BigMediaPlayerAPI. The new kl_jni_avpro family
implements its manager/player interface using the same system media backend.
Construction loads the original libAVProLocal and runs its JNI_OnLoad and
nativeInit so its cached Java methods, native getters and rendering callbacks
remain usable. The reported 1.9.17 is the original Java wrapper's literal.

The bridge supplies actual dimensions, duration, playback/seek/buffering state,
audio/video metadata and texture frames. It supports pause, play, stop, seek,
loop, rate and volume. Decoded frames upload only on a current GL thread.
Render services now append with deduplication so this player and social TV
coexist. Closed players release rendering resources on that thread and recycle
their indices; native/source failures become AVPro's consumed error 100 rather
than terminating strict JNI dispatch. Unsupported byte offsets, HTTP headers,
extra audio-track selection and spatial audio options remain explicit limits.

The initial installed adapter entered the live Lobby, restored its saved session
and stayed connected (read-only websocket getter true, tablet state 108). The
user confirmed the Lobby looked good. Its two tutorial/poster videos then exposed
jar:file APK StreamingAssets URLs; those resolve to the existing staged assets
instead of being sent to AVPlayer as unsupported addresses. This does not
require extracting or re-downloading any user media.

tests/t_avpro.m loads the real original Android plugin, generates a local video,
opens it via the APK asset URL contract, and tests native JNI getters against
real decoding and three GPU colors. It also checks pause, seek, floating-point
volume, consumed asynchronous errors, index reuse and coexistence with another
render service. The existing H.264/HLS stream regression, JNI regression and
saved-session regression pass. Both visionOS runtime link gates pass.
The build including APK asset URL resolution is installed (PID 63242), has
restored its connected session and displays My Room. Live Lobby video panels
have not yet been checked in this final build.

The user also reported magenta avatar silhouettes. Captured failed fragment
shaders contain the actual _SkinTones, _CustomColors, _IrisOffsets,
_MicAmplitudes, _Gradients and _HiddenAvatarFlags buffers. ANGLE rejects their
std430 readonly shader-storage blocks under GLSL ES 3.00. A multiview variant
also fails on GL_OVR_multiview2. This is a remaining rendering compatibility
problem; the Lobby join/video adapter does not fix the avatar shaders.

Evidence in the ignored investigation directory: lobby-boot.log,
lobby-avpro-test.log, lobby-tv-regression.log, lobby-jni-regression.log,
lobby-login-regression.log, lobby-runtime-build.log, lobby-simulator-build.log,
lobby-assets-build.log, lobby-assets-simulator-build.log,
lobby-session-state.log and lobby-failed-shaders/*.glsl. Diagnostics omit
credentials, media source URLs and room identifiers.

### Packed avatar storage buffers (2026-10-30)

The avatar fragment shader's six read-only std430 blocks now translate to
std140 uniform blocks containing packed uvec4 words. Each scalar read selects
the original 32-bit word without changing the supplied float bit pattern or
introducing the padding of a std140 scalar array. The conversion accepts only
the measured scalar struct and read syntax of the named avatar buffers;
writable buffers and other layouts retain their normal unsupported errors.
Shader layout binding numbers are preserved before the general GLSL rewrite.

Program reflection presents those resources as shader-storage blocks and their
buffer variables, while excluding implementation blocks/members from Unity's
ordinary uniform enumeration. Normal uniform block and uniform indices map
back to the actual ANGLE resources. The host reserves the final eight uniform
buffer bindings and copies real guest data into 16 KiB GPU buffers. Generic
SSBO bindings remain independent of COPY_WRITE and UNIFORM_BUFFER state.
Buffer subdata, mapped writes, range offsets and deletion are handled; upload
generations avoid copying unchanged data and notice shared-context uploads.
This is read-only scalar storage compatibility, not general compute/SSBO-write
support. Buffer ranges above 16 KiB are diagnosed explicitly.

tests/t_avatar_buffers.c compiles and links the captured real avatar fragment
shader supplied from the ignored investigation directory. A locally authored
shader then reads all six buffers through real ANGLE/Metal rendering and checks
GPU colors, permuted layout binding numbers, reflection isolation, buffer
subdata/mapped/range updates and restoration of unrelated GL bindings. The
H.264/HLS streaming regression still passes, as do both visionOS runtime link
gates. The first updated simulator build (PID 71235) restored its session and
joined the live Lobby. Its real avatar shader compiled/linked with no failures
or oversized-range warnings, and read-only host metadata confirmed all six
original storage binding numbers (0 through 5). GPU tests also cover consumed
buffer-variable reflection properties and buffer deletion with GL state
preservation. The final simulator build (PID 72952) includes these cleanup and
reflection checks. Its first visual retry showed black avatars, leading to
the additional fixes below.

Evidence: avatar-test.log, avatar-runtime-build.log, avatar-tv-regression.log,
avatar-simulator-build.log, avatar-join-probe.log, avatar-live-state.log,
avatar-final-simulator-build.log and avatar-final-join-probe.log in quest-reference under
the ignored Bigscreen investigation directory.

The GLES 3.0 startup workaround also disabled Unity's storage-buffer binding
path. Current imports recommend GLES 3.1, with Unity's own
`-disable-gpu-skinning` argument delivered in the `unity` launch Bundle and
Intent extra (and newer initJni's command-line parameter). The argument is
scoped to the measured Bigscreen package/version; the original Unity binary
contains this switch, also documented in Unity's
[2020.2 release notes](https://unity.com/releases/editor/beta/2020.2.0b7).
Read-only fragment-buffer binding/range limits are supplied, with no compute
stage blocks advertised. Disabling GPU skinning avoids Internal-Skinning.main.

Unity 2020 uses both ES3 and ES3.1 reflection. The implementation now hides
compatibility uniform blocks/members consistently across both API families,
including glGetProgramiv, glGetActiveUniform*, glGetUniformIndices and the
program-interface queries. Exposing the internal blocks through ES3 caused a
native shader-reflection allocation crash at libunity.so+0x453b70; the expanded
reflection regression checks this view along with GPU colors and storage data.

Higher GLES descriptions exposed a separate mixed uniform-location contract:
Unity uses baked sampler locations as texture units but also queries uniform
locations by name. A 360-float array moved ANGLE sampler locations above 390,
causing invalid glUniform1i assignments and lost avatar/font texture inputs.
For this Bigscreen build, the bridge preserves original pins and allocates
unpinned uniforms/whole arrays after them. Queries, setters and getters now
share that namespace. Other profiles retain their existing location behavior.
tests/t_uniform_locations.c checks baked and named writes mixed on one program,
a 360-element array, sampler-unit assignment, getter round trips and real GPU
color. The installed build (PID 92524) restores readable text and has no invalid
sampler assignment errors. The user confirmed normal skin/clothing colors and
facial details on nearby live Lobby avatars. The draw trace also confirms all
six original source buffers and their GPU mirrors are bound (464, 1024, 320,
160, 1152 and 160 bytes respectively), with no shader compilation failures.

The installer offers GLES 3.1 and recommends it for the exact tested APK;
request validation and generated target code have corresponding coverage.
The installer Swift source typechecks, its app builds, the 41 installer tests
pass, and GLES version descriptions pass for defaults, 3.0/3.1, invalid input
and GLES2. An earlier cancellation test raced under concurrent build load;
the unchanged test passed on the full-suite retry.

Additional evidence: avatar-es3-runtime-build.log, avatar-es3-simulator-build.log,
avatar-uniform-test.log, avatar-gles-version-tests.log, avatar-installer-tests.log,
avatar-installer-swift.log, avatar-sampler-runtime-build.log and
avatar-sampler-simulator-build.log. The updated Mac installer is built at
build/installer-macos/Build/Products/Debug/KleptonInstaller.app. H.264/HLS media
regression, saved-session tests and uniform getter/property-query coherence
checks pass alongside the graphics and both-platform build gates. Verification
here is on the visionOS simulator; this avatar path has not been checked on a
physical Vision Pro.
