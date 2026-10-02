# Walkabout standalone integration evidence

Status on September 30, 2026: **incomplete**. The local Steam backend and its
IPC path run in the visionOS simulator. The independent probe now demonstrates
approved Steam account login, base-game ownership and successful callbacks for
both ticket APIs, including a session request matching the game’s PlayFab
recipient and 1,024-byte buffer. The integrated simulator game now calls
`LoginWithSteam`, enters its actual `OnLoginSuccess` callback and reports its
PlayFab controller logged in. Its active shared settings match the packaged
Title ID `98AD5`. Private-room connection, physical execution, voice capture
and in-game paid DLC availability remain unverified.

## Experimental simulator game host

### Current startup evidence

The September 30 `game-host-metadata-directory-fixed-receipt-20260930.json`
run passes 16,384 frame-loop iterations with real Steam and both experimental
signal-mask workarounds off. Early read-only method breakpoints observe the
game's login, `PlayFabClientAPI.LoginWithSteam`, and the game's
`OnLoginSuccess`; no login-failure callback is observed. A later read-only
inspection finds `PlayFabController.IsLoggedIn=true` and its active shared
Title ID matching `98AD5`. The observer never reads ticket bytes, account
identifiers or response tokens. The redacted stage and scalar evidence is
`game-host-metadata-directory-auth-redacted.log` under ignored
`build/steam-runtime/`.

Two further startup-context gaps are corrected. Asset directory changes now
invalidate cached Android version/package/manifest metadata, so the game
reports version code `57013` instead of the probe's fallback `545`. OBB
directory enumeration now normalizes Unity's APK-prefixed `assets/` paths,
matching file reads while retaining the original path for loose-directory
merging. The expanded handoff fixture fails before each corresponding fix and
passes on macOS and the ARM64 visionOS simulator. All 15 C checks, 34 Steam
Python checks and 10 Walkabout Python checks pass via `make steamcheck`.

This integrated run discovers its UnitySubsystems directory and returns `0`
from the genuine Unity Display provider registration. The earlier Display
loading error is absent. A later read-only inspection finds a running OpenXR
session, 23,492 waited frames and 23,491 ended frames; the log records both
Vulkan eye swapchain slices exported to Metal. These observations establish
subsystem startup and frame submission, not a visible playable scene. This
launch used `KL_IMMERSIVE=0`; an immersive rendering run is being validated
separately. Physical Vision Pro acceptance remains outstanding.

### Earlier startup failures and verified repairs

The September 30 `game-host-jump-mask-fixed-receipt-20260930.json` run passes
16,384 frame-loop iterations, reaches the game's own
`SteamInternal_SteamAPI_Init` success result `0`, and requests a 426-byte
`GetAuthSessionTicket` through the guest API. Frame-loop iterations do not
establish visible gameplay, successful callback consumption or PlayFab login.
The same run reports an OpenXR Display registration error, zero loaded
PlayFabSharedSettings objects and a failed Addressables settings read.

The measured GC root cause is guest `longjmp` forwarding to Darwin `longjmp`.
Native tracing records 322 process-mask restores from a Steam worker. Darwin's
`sigprocmask(SIG_SETMASK)` applies its mask across process threads, blocking
Unity's signals 30/24. `runtime/kl_jump.c` and `kl_jump_entries.S` use the native
register-only jump pair and save/restore the calling thread's mask with
`pthread_sigmask`. The guest jump buffer retains its 256-byte bounds. The
concurrent GC regression fails on the old jump path and passes on macOS and
the simulator with the adapter. Separate tests cover all jump aliases,
mask-saving modes, zero/nonzero return values and buffer guards. Temporary
native setter interposition has been removed after preserving its evidence.

A read-only debugger inspection also finds Unity's cached OBB directory at
`Library/Application Support/SteamRuntime/obb` and zero indexed OBB entries,
despite its configured files directory pointing to `Documents/android-files`.
Both the settings JSON and serialized PlayFab settings are present in the
staged main OBB. `kl_jni_set_files_dir` and `kl_jni_set_obb_rel` now invalidate
the derived path. `tests/t_asset_handoff.c` reproduces the probe-to-game
handoff and reads stored and deflated OBB resources. It fails before the fix
and passes on macOS and the ARM64 simulator afterward. The integrated
`game-host-asset-handoff-fixed-receipt-20260930.json` run reads settings JSON
and menu/course-image bundles; its prior PlayFabSharedSettings warning,
Addressables read failure and Photon static-initialization exception are
absent. It reaches the game's ticket request, then aborts in
`klj_GetObjectClass`, called by `PlayFabSettings.get_DeviceUniqueIdentifier`.
The cached JNI ContentResolver had been retired at a local-frame boundary.
Its factory now uses the pinned singleton helper. A focused JNI regression
fails on the old factory and passes on macOS and the ARM64 simulator across
repeated typed/erased method calls and frame retirement. The integrated
`game-host-jni-refcache-fixed-receipt-20260930.json` run passes 8,192 frame-loop
iterations and requests the game's 426-byte ticket without the earlier resource
warnings or invalid-object abort. Both experimental mask workarounds are off,
and temporary native setter interposition is absent. A native sample includes
running Unity/Steam frames and no identified GC acknowledgment wait. Late
debugger login-method observation records no hits and cannot establish whether
a request or response occurred before attachment. Game PlayFab success remains
unverified; the Display registration error persists. All 15 C checks, 34 Steam
Python checks and 10 Walkabout Python checks pass via `make steamcheck`.
A first rebuild accidentally linked the older XCFramework; its
receipt is labeled `asset-handoff-stale-runtime` and is excluded from fix
validation. Rebuild `make xros XROS_PLATFORMS=xrsim` before linking the app;
`make xros-sim` alone does not refresh the XCFramework consumed by Xcode.

The following run history records the failures that led to these fixes.

### Earlier startup investigation

The game app now has an opt-in local Steam startup gate. `steam/visionos/mksteam.py`
attests the recovery client and original game SDK/C++ inputs, translates separate
backend and game-client instances, stages Valve's login modules, and records
output hashes. `KLEPTON_STEAM_LOCAL=1` selects those frameworks and the shared
login controller when generating the Walkabout project. The variant now supports
ARM64 simulator and physical test builds. Packaging enables device testing;
it does not establish successful physical login or game acceptance.

On September 30, the integrated app demonstrated saved Keychain login, original
SDK initialization, base-game ownership and successful callback results for
both ticket APIs. The startup probe cancels its own tickets and shuts down its
SDK connection before publishing stage `9`. Unity starts only after that stage;
the backend worker stays alive and the startup probe stops dispatching the SDK's
callbacks. The successful handoff is recorded in
`build/steam-runtime/game-host-second-receipt-20260930.json` and repeated in the
third/fourth receipts. The first integrated run timed out waiting for ticket
callbacks and correctly kept Unity stopped. A later run enumerated 38 DLC,
32 subscribed and six unsubscribed; none reported installed. Enumeration changes
as Steam metadata arrives, so it is not a fixed entitlement override.

The game's own authentication remains unverified. Unity stalls in its first
`nativeRender` call, at the GC acknowledgment wait
`libil2cpp.so+0x2eb32c0` (relative to the embedded ELF base, rather than the
Mach-O load address). Read-only debugger observations show a target of six
acknowledgments and a semaphore count of zero. `pthread_kill(...,30)` reports
success, and the installed GC handlers use signals 30/24. Unblocking those
signals on the host boot thread before loading Unity did not resolve the wait;
that experimental change was removed. `KL_TRACE_SIGMASK=1` adds native worker
mask/handler diagnostics without changing signal behavior. Initial Unity
workers inherit a blocked mask; later workers inherit an unblocked one.
The additional opt-in `KL_UNITY_GC_THREAD_SIGNALS=1` experiment unblocks the
two measured signals only at translated Unity/IL2CPP worker entry. In the sixth
receipt it allows the first render call to return and the frame loop to start,
but the seventh/eighth runs still stall at GC. The eighth has no early debugger
attachment and its remaining wait is for the registered finalizer thread, with
one expected acknowledgment and zero observed. The worker change is therefore
experimental, disabled by default, and not a stable GC fix.

The ninth run completes 6,498 frames before a later GC wait: 18 expected
acknowledgments, zero observed, stop count 32, and 104 entries each into the
suspend/resume handlers across earlier cycles. This rules out total failure to
enter the signal trampoline. The tenth run uses bounded in-process
`KL_TRACE_FRAME_PROGRESS=1` logging and no LLDB attachment. It completes at least
4,096 frames, then native sampling again finds the GC wait. Bounded per-thread
futex diagnostics show some Unity/IL2CPP workers subsequently waiting with
mask `0xfbfee027`, which blocks both measured GC signals, despite entry
unblocking to `0xdb7ee027`. The source of that mask change remains unresolved.
Masks obtained by debugger function evaluation are excluded: the debugger can
change masks while evaluating the query.

`tests/t_signal_wait.c` now exercises 32 genuine suspend/resume cycles through
the guest ABI wrappers with both one worker and 24 concurrent workers (800
suspend/resume pairs). Workers park on native condition variables and Klepton's
translated futex; acknowledgments alternate between timed semaphore waits and
polled counts. A separate native worker repeatedly changes its own mask while
GC delivery runs. It passes on both macOS and the ARM64 visionOS simulator
using the shipping simulator archive, including the x18 context-repair branch.
The test does not execute Unity's translated handler, so it does not establish
that the game's GC barrier is fixed. Receipts and redacted logs for the ninth
and tenth runs are in `build/steam-runtime/`.

### Controlled shim comparison and signal-delivery fixes

The paired September 30 runs use the same installed signed app, framework
hashes and game-data container. Explicit offline diagnostics
(`KL_STEAM_OFFLINE=1`) bypass the backend and login UI, pass the 16,384-frame
milestone and reach the game's own Steam initialization. This records startup
progress, not verified visible gameplay. Real mode completes saved login and
both startup ticket callbacks, reaches the 4,096-frame milestone, then stalls
in GC. The real run also reports the Display registration error; the offline
run does not. The receipts are `game-host-paired-offline-receipt-20260930.json`
and `game-host-paired-real-receipt-20260930.json` under `build/steam-runtime/`.
Real mode never selects the shim as a fallback.

Additional launches use the same installed app to isolate the process working
directory and the native bootstrap thread's mask. Restoring only the directory
stalls before the first frame. Clearing only the bootstrap mask, and restoring
both settings, each reach the 4,096-frame milestone before the GC wait and
Display error. Scalar diagnostics confirm that each selected change happened;
neither is a sufficient fix. Workers can subsequently wait with `0xfbfee027`
despite a recorded GC handler source/return mask of zero. These controls remain
historical simulator experiments. Working-directory restoration now defaults
on to preserve Unity resource lookup after backend startup
(`KL_STEAM_HOST_RESTORE_CWD`); signal-mask clearing remains off
(`KL_STEAM_HOST_ZERO_GAME_MASK`).

The concurrent regression exposed two concrete lock violations in the shim.
The valid `sem_post` path called `getenv` through a trace check from inside the
GC signal handler, even with tracing disabled. A native crash report shows
recursive acquisition of the environment lock held by the interrupted worker.
After that lookup was removed, native sampling exposed the collector blocked
in the `pthread_kill` trace check on an environment lock held by an already
suspended worker. Both valid paths now avoid environment lookups and stdio.
The expanded regression passes with those changes, including concurrent mask
changes by an independent worker.

The rebuilt signed game host still completes genuine startup ticket callbacks
but stalls before the first frame in its latest launch. Native sampling again
finds the GC acknowledgment wait. Subsequent debugger reads of metadata only
find stop count 10, a registered finalizer at stop count 9, and one prior entry
each into signals 30/24. No debugger was attached before the stall observation.
This fixes demonstrated shim defects; it does not prove the integration
regression resolved. Evidence is saved as
`game-host-signal-delivery-fixed-receipt-20260930.json`, its redacted log, native
sample and GC metadata report. The Steam regression suite passes (12 C checks,
33 Steam Python checks and 10 Walkabout Python checks).

The sixth run also exposed the managed input diagnostic entering IL2CPP after
1,200 frames despite `KL_PROBE_INPUT` being unset. Its missing off guard is now
restored. A focused regression checks that unset/explicitly-off diagnostics make
no guest image lookup while explicitly enabled input and independent XR probes
still attempt initialization. There are no observed game Steamworks calls or
PlayFab login results in these runs. The XR provider reports a Display subsystem
header mismatch and `xrPollEvent: XR_ERROR_FUNCTION_UNSUPPORTED` in the one run
that gets past the first GC barrier; that provider path also needs investigation.
The ninth run's native dispatch table records zero calls to Klepton's actual
`xrPollEvent` implementation. Static inspection locates the error in
`libUnityOpenXR.so`'s `UnityPluginLoad`, following Display lifecycle registration
for `OpenXR XR Plugin` / `OpenXR Display`. The packaged OBB manifest declares
those same names and plugin version `1.16.1`. The registration failure is still
unresolved; the later reported poll error is not evidence that Klepton's
`xrPollEvent` returned that result.

With the ordinary Walkabout frameworks and game data already staged, build this
experiment from the repository root:

```sh
python3 steam/visionos/mksteam.py
KLEPTON_TARGET=walkabout-57013 KLEPTON_STEAM_LOCAL=1 python3 visionos/gen_xcodeproj.py
make xros XROS_PLATFORMS=xrsim
xcodebuild -project visionos/KleptonWalkabout57013.xcodeproj \
  -scheme KleptonWalkabout57013 -sdk xrsimulator \
  -destination 'generic/platform=visionOS Simulator' \
  -derivedDataPath build/steam-runtime/game-host-simulator-dd build
```

Launch in real mode (`KL_STEAM_OFFLINE=0`). The game app uses its own Keychain
entry; approve its Steam Guard QR when no saved credential is available.
Do not run this build through a launcher that implicitly selects offline mode.
Startup diagnostics live in `Library/Application Support/SteamRuntime/`; Unity
redirects subsequent output to `Documents/klepton-boot.log`. Reinstallation
rotates the container path, so capture its current path and process identity
before reading evidence. Simulator success does not satisfy the physical or
Walkabout PlayFab acceptance gates.

This investigation reads the supplied original `walkabout-57013` game binary,
IL2CPP metadata, and main OBB without executing or changing them. It narrows the
integration work beyond the independent Steam runtime probe. Authentication
results remain separate from static findings.

## Game authentication path

The game's metadata is version `39`, with variable-width indices and section
headers containing offset, size and count. Klepton's existing runtime method-name
resolver handles version `24`; this investigation adds a separate read-only
inspector rather than applying that layout to this game. The v39 layout was
checked against [Cpp2IL's pinned source](https://github.com/SamboyCoding/Cpp2IL/tree/b5ad444b82267cb1e4b88b8b373c008105bdea52/LibCpp2IL/Metadata).
The inspector validates image/type/method ownership, table bounds, tokens,
codegen module counts, ARM64 relative relocations and executable method targets.
It resolves all 145 modules and 152,950 methods in the supplied build.

| Finding | Original-game evidence |
| --- | --- |
| Ticket method | `FarBridge.Worlds.SteamworksInterface.GetAuthTicket`, ELF VA `0x302528c` |
| Ticket API | Calls `Steamworks.SteamUser.GetAuthSessionTicket` at `0x3025490`; no Web API ticket call in this method |
| Buffer | Allocates a 1,024-byte managed byte array and passes its length |
| Recipient identity | Creates a 136-byte `SteamNetworkingIdentity` and calls `SetGenericString("PlayFab")` at `0x3025464` |
| Ticket length and handle | Reads the API's output length and saves the returned handle on the platform interface |
| Encoding | Formats each ticket byte with the literal `{0:x2}`, producing lowercase hexadecimal |
| Consumer | `FarBridge.Weywot.PlayFabController.Login` creates `LoginWithSteamRequest` and calls `PlayFabClientAPI.LoginWithSteam` at `0x302dd58` |
| Account creation | Constructs nullable `CreateAccount=true`, then stores it into the request |
| Ticket selection field | The packaged request model has seven fields and **no** `TicketIsServiceSpecific` field |

The configured **packaged default** PlayFab Title ID is `98AD5`. Its
`PlayFabSharedSettings` object is in main-OBB member
`assets/bin/Data/698b2db098268c640929a7b8090a31eb`. The inspector verifies the
member's size, CRC and SHA-256, the serialized object name and MonoScript
reference, and the concatenated script-asset and referenced MonoScript hashes.
It reads only the first declared settings field, `TitleId`. The game SDK fills
an unset request Title ID from its settings. Runtime overrides, the resolved HTTP
endpoint and the actual request have not been observed, so this default is not
reported as a verified runtime Title ID.

Preserve this session-ticket path and its recipient identity. The independent
probe's first successful session request used a NULL recipient. Its subsequent
request matches the game's `PlayFab` recipient and buffer size; the separate
`GetAuthTicketForWebApi("AzurePlayFab")` request remains a service-specific
infrastructure diagnostic;
neither proves that the game's own login succeeds. PlayFab documents hexadecimal
bytes and distinguishes session tickets (`TicketIsServiceSpecific=false`) from
Web API tickets (`true`). The absent field in this packaged SDK is an observed
model fact, not a claim about a successful server response.
[PlayFab API](https://learn.microsoft.com/en-us/rest/api/playfab/client/authentication/login-with-steam?view=playfab-rest)

The local game ticket method formats the returned bytes immediately. Callback
completion, request timing, cancellation and the actual PlayFab response still
need live observation. Keep ticket bytes, PlayFab session/entity tokens, Steam
login credentials and room passwords out of diagnostics. Allowed observations
are stages, bounded lengths, ticket-type/recipient classifications, Title ID,
HTTP/result codes and callback completion.

## Private rooms, voice and paid DLC

`Mighty.Multiplayer.Launcher.MultiplayerLauncher.JoinPrivateRoom`, VA
`0x32fe9b0`, calls Photon PUN's `JoinOrCreateRoom` at `0x32febc4`. This is the
private-room path to exercise once real authentication and the game are ready.
No Photon connection, authentication mapping, room creation or room join has
been demonstrated by the static map.

`FarBridge.Worlds.SteamworksInterface` implements `IPlatformDLC.DoesUserOwnProduct`.
Its method at `0x3026180` checks the product reference and reads its numeric
content ID, then calls `Steamworks.SteamApps.BIsDlcInstalled`. The runtime must
supply genuine Steam app/DLC information and correctly map the corresponding
supplied course bundles. Finding those bundles on disk does not establish
ownership or availability in the game. Purchased-course acceptance requires
checking the owning account's actual results and loading each owned course.
The independent simulator probe now enumerates 32 DLC entries and queries
`BIsSubscribedApp` and `BIsDlcInstalled` separately. All 32 report subscribed,
while none reports installed in the probe without game content. These are
genuine client results, not evidence that the game can load those courses.
The snapshot is recorded in the [runtime investigation](../../steam/STEAM-RUNTIME.md).

`MultiplayerVoice.UpdateMicrophone`, VA `0x34b5470`, queries and requests
`UnityEngine.Android.Permission` before changing microphone state. The
private-room launcher also has `JoinRoomPromptForMicrophone` and permission
callbacks. Klepton already has opt-in native capture and OpenSL/AAudio adapters,
but the Walkabout shell hid its microphone panel. That panel is now exposed for
`walkabout-57013`, with microphone use still off by default and enabled through
the user's toggle. The shared panel and usage-description text describe voice
chat without restricting it to Steam Link.

The Unity permission shim currently reports the manifest permission state;
its callback request path does not invoke Unity's request callback. That is a
remaining compatibility audit, not a verified permission/capture contract.
Physical microphone permission, recorder startup, nonempty audio, Photon voice
transmission and remote audibility all remain required. A successful simulator
compile does not establish microphone capture or voice chat.

## Reproduce and validate

The tracked [artifact lock](../../../games/walkabout/tools/walkabout_login.lock.json) records original
metadata/binary hashes, selected managed-method tokens and ELF addresses, the two
relevant string literals, request-field names, audited callsites and the supplying
settings object's provenance. No game binary, ticket or account data is tracked.

```sh
python3 games/walkabout/tools/walkabout_inspect.py
python3 games/walkabout/tools/walkabout_inspect.py --obb /path/to/main.57013.com.MightyCoconut.WalkaboutMiniGolf.obb
make steamcheck
```

`--obb` additionally verifies the settings and its script provenance. The Data
directory is inferred from the metadata path; `--assets` can specify it explicitly.
The report defaults to ignored `build/walkabout-auth-analysis/inspection.json`.
Unknown versions, different supplied artifacts, invalid table bounds,
non-executable method targets and ambiguous module mappings fail explicitly.
The tool never starts the game, sends an HTTP request or changes entitlement or
authentication behavior.

Ten focused offline tests cover version/stride/bounds/token rejection,
method/type ownership, virtual-to-file mapping, ARM64 relocations, executable
method targets, artifact hashes, settings extraction and script-split provenance.
These run with twelve C and 33 Python Steam regressions in
`make steamcheck`. All passed on September 30. The updated Walkabout visionOS
simulator app also builds successfully. Its first build attempt exposed a local
XCFramework containing only the device slice; rebuilding the standard generated
XCFramework with `XROS_PLATFORMS='xros xrsim'` restored both slices and the build
passed. The original validation did not launch the game; the subsequent
experimental integrated simulator runs are recorded above.

Evidence and narrow disassembly remain under ignored
`build/walkabout-auth-analysis/`. The original binary and metadata hashes remain
unchanged. See [the runtime investigation](../../steam/STEAMFRAME-RECOVERY.md) for backend
execution evidence and the logged-out app-registration refusal.

### Physical installation on October 1

The real-Steam game host was built for physical visionOS and installed on the
connected Vision Pro. The runtime, translated game libraries and pinned Steam
backend/client libraries were rebuilt for the device; renderer frameworks
already contain device slices. Code signatures and physical-platform load
commands passed verification for all 23 executable binaries. Existing game
data survived installation: the APK, 900,002,283-byte main OBB and 124 loose
bundles remain present. The Android manifest and version metadata were refreshed
without re-uploading the full game data.

The handoff now also clears the cached declared-permission list when the guest
assets change. A focused fixture reproduces the old stale manifest permissions
and passes after the reset. Physical microphone authorization and voice capture
remain separate acceptance checks.

Build and installation evidence is recorded in
`build/steam-runtime/hardware-build-receipt-20261001.json` and
`hardware-install-20261001.json`. Installation does not establish physical
Steam login, game PlayFab authentication, multiplayer, voice or DLC gameplay.

### Physical playtest and follow-up fixes on October 1

The user reports that the physical build plays well, joins a private room and
receives other players' voices. First-login startup initially failed to deliver
tickets, then succeeded after an app restart. Enabling outgoing voice aborts
both before and after granting native microphone permission. This is physical
user-test evidence, not automated verification of every DLC course or voice
transmission.

Three device crash reports show the same JNI abort at Photon's
`AudioInAEC.GetMinBufferSize(II)I`. The game log names that missing implementation.
The supplied DEX defines the recorder's full public surface and its retained
short-array contract: fill the PCM16 array, then invoke `DataCallback.OnData()`;
invoke `OnStop()` when the recorder exits. The new Photon JNI family implements
that surface with opted-in native capture, format conversion, retained JNI
references and a worker separate from CoreAudio's render callback. Unsupported
Android effect objects report unavailable; host audio processing is independent.
Capture-ring reads now serialize with close, preventing the microphone toggle
from freeing storage while the recorder is reading it.

The ticket controller had a concrete retry bug: cancellation cleared the gate's
user ID, and subsequent requests did not restore it. Each request now restores
the genuine SDK user. A five-minute recovery window retries transient results,
zero and partial requests, ownership metadata and callback timeouts; cancelled
attempts release their handles and reject late callbacks by handle. A native
retry mailbox restarts startup SDK initialization without restarting the app.
It never changes ownership or replaces genuine credentials.

The Steam login screen adds account/password login followed by an offered Steam
Guard app or email code. Valve's pinned RSA and authentication modules perform
the requests. Passwords and Guard codes are ephemeral; only an approved refresh
credential is saved in Keychain. The QR presentation is larger with a white
quiet zone, and the user can change login methods while waiting.

Focused fixtures cover code rejection and cancellation, retry callback matching,
partial ticket recovery, retained PCM arrays, resampling, stop and relaunch.
Real code-login approval and outgoing physical voice still require a fresh test.

The updated device build passed signing/platform verification and was installed
successfully. Existing APK/OBB data and all 124 loose bundles were checked after
installation. The headset was locked, so this update was not automatically
launched. Evidence is in `build/steam-runtime/user-fixes-validation-receipt.json`
and `user-fixes-hardware-install.json`; code login and outgoing voice remain
pending physical retest.

### DLC ownership correction on October 1

The user reports that purchased courses appear unowned on the physical build,
despite saved unlocks from those courses. The earlier genuine device SDK probe
enumerated 38 DLC entries: 32 subscribed, six unsubscribed, and all reported
uninstalled. Saved unlocks are separate from current Steam licenses.

The original `IPlatformDLC.DoesUserOwnProduct` method reads the product App ID
and tail-calls `SteamApps.BIsDlcInstalled` at `0x30261ec`. Valve documents that
query as requiring both ownership and installation. Klepton supplies the
Android OBB/course bundles outside Steam depot storage, so that installation
condition incorrectly rejects genuinely owned courses in this packaging.
See [Valve's API contract](https://partner.steamgames.com/doc/api/ISteamApps#BIsDlcInstalled).

`games/walkabout/tools/walkabout_dlc_compat.py` prepares a derived AOT input whose single tail
call instead targets the game's existing `SteamApps.BIsSubscribedApp` wrapper.
The same product ID and null MethodInfo are passed; both wrappers have the same
static AppId-to-bool signature. The resulting answer comes from the genuine
Steam account subscription query, including false for unowned products. The
Steam installation API remains unchanged. Unity still loads the supplied
course assets through its normal content path; this adapter does not establish
that every owned course's files are present or loadable.

The original ELF and metadata remain unchanged. Preparation requires the exact
original artifact hash, executable method regions, caller/wrapper hashes and
original branch target. Unknown or already modified inputs fail before replacing
an output. `visionos/mkguest.sh` prepares the adapter only for Walkabout 57013,
before framework signing, for both device and simulator. Disable it for an A/B
build with `KL_GUEST_PATCH_OFF=walkabout-dlc-ownership` (or `KL_GUEST_PATCH=0`).
Generated inputs and receipts stay in ignored `build/walkabout-dlc-compat/`.

All 16 Walkabout inspection/adapter fixtures pass, including original-input
preservation, exact call scope, unknown-build refusal, wrapper guards and
repeatable preparation. Actual owned-course loading and the unowned-course
display still require a physical retest.

The updated real-Steam physical app built successfully, passed signature and
physical-platform checks for all 23 binaries, and was installed on the connected
Vision Pro. The signed IL2CPP framework contains the audited redirected branch.
All 124 course bundles, the main OBB and APK survived installation with matching
names/sizes. The headset is locked, so no automatic launch was attempted.
Evidence is in `build/steam-runtime/dlc-compat-validation-receipt.json` and
`dlc-compat-hardware-install.json`. The earlier login/ticket/Photon fixes are
included; their outstanding physical retests remain outstanding.

### Second unmute crash and distorted decorations on October 1

The user confirms that DLC access now works. Unmuting still aborts, and reports
stretched, moving geometry above Mount Olympus and a cone/pyramid at Forgotten
Fairyland's entrance. The new device crash at 15:11:56 is a strict JNI abort in
`CallBooleanMethodA`. Its guest caller maps to the `Start` invocation in
`Photon.Voice.Unity.AndroidAudioInAEC`'s constructor (`0x56b1ab0`). Subsequent
relaunches replaced both the current and previous boot logs, so the exact
requested signature was not retained in that crash's log.

The reflection shim previously returned the caller's requested signature rather
than the declared Java signature. The supplied ReflectionHelper scores reference
assignability, so Unity's concrete Activity/generated proxy can resolve to the
declared `Start(Activity, DataCallback, int, int, int, bool, bool, bool)` method.
The shim now canonicalizes those supported argument forms for this method only;
primitive counts/return type/staticness remain strict. This fixes the demonstrated
constructor call path without enabling permissive JNI fallback.

There was a second callback defect: ReflectionHelper proxies carry a Unity GC
handle and use `nativeProxyInvoke(long, String, Object[])`, while bitter JNIBridge
proxies use `invoke(long, Class, Method, Object[])`. Both natives are registered
by the supplied game. Proxies now retain their origin and dispatch through the
corresponding native ABI. The fixture tests full reflection-to-JNI dispatch,
both native ABIs, disabled proxies, and modern callbacks from the capture worker
using synthetic PCM. Physical microphone transmission still needs retesting.

Rendering uses Vulkan/MoltenVK on the physical headset. A simulator workaround
was incorrectly defaulted on for Walkabout on every platform: it rebases vertex
attribute bindings and submits zero vertexOffset. This changes shader vertex
index semantics and depends on cached pipeline strides, even when Metal supports
the original indexed draw. The runtime now queries MoltenVK's documented
append-only Metal feature prefix and defaults to native draws when base-vertex
support is present. Only unsupported Walkabout devices use the existing rebase;
an unknown capability uses the simulator fallback on simulator, native forwarding
elsewhere. `KL_VK_EMULATE_BASE_VERTEX` remains an explicit A/B override.
This corrects a concrete draw-contract defect; it has not yet been proven to be
the cause of the two reported decorations. Course-asset inspection identified
the Olympus and amusement-park bundles and their dynamic/wind/cloud/flame
materials, but does not identify the failing draw. No asset or shader was
modified, and no geometry was hidden to mask the symptom.

`make vkvertexcheck` passes actual draw-wrapper fixtures for supported,
unsupported and unknown devices, checking original draw arguments, sparse
bindings and restoration. The expanded Photon recorder fixture passes. The
updated physical app builds, all 23 binaries pass platform/signature checks,
and it was installed successfully with all 124 course bundles/main OBB preserved.
The signed DLC adapter remains present. Build/install receipts are under
`build/steam-runtime/mic-graphics-followup-*`; physical unmute and both course
decorations remain pending retest.

### Exact reflection signature from the third unmute crash

The user confirms that the visual glitches are gone on the physical headset.
Unmuting still aborts. This time the last-run log was preserved, and the
16:13:21 device report agrees with its explicit missing JNI binding:

```
AudioInAEC.Start(Lcom.unity3d.player.UnityPlayerActivity;Lcom.exitgames.photon.audioinaec.AudioInAEC$DataCallback;IIIZZZ)Z
```

Unity's reflection request uses dotted reference names. The previous fixture
covered concrete Activity/proxy types in slash-separated JNI notation and missed
that spelling. ReflectionHelper now normalizes a copied reference descriptor
before resolving the declared method and converting it back to JNI. The same
boundary normalizes constructor and field descriptors, including reference
arrays and nested classes. Primitive types, staticness and caller strings remain
unchanged. Strict JNI dispatch remains enabled.

Adding the exact device signature to `t_photon_audio` reproduces the failure
before this fix and passes afterward. The fixture also checks array/return/field/
constructor normalization, source-string preservation, both proxy ABIs and a
capture worker using synthetic PCM. The JNI reference-cache regression passes.
The failure occurred before recorder startup, so the user test does not yet
establish native capture or outgoing voice. Evidence is in ignored
`build/steam-runtime/mic-third-*`; no account, ticket or voice samples are
included in this report.

A follow-through audit found a second defect before deploying this update:
`CallBooleanMethodA` decoded `SetBuffer([S)Z` as a short scalar, truncating
the retained PCM array pointer. A public JNI SetBuffer fixture reproduces a
segmentation fault before the fix. Both A and guest AAPCS64 V decoders now
consume array descriptors as one reference, including primitive and nested
arrays; float/double arrays use GP slots, while following float/double scalars
retain FP slots. The focused argument fixture reproduces the width defect
before the fix and checks GP/FP register selection, stack spills and malformed
array descriptors afterward. All three fixtures (`t_photon_audio`,
`t_jni_array_args`, `t_jni_refcache`) pass. This second defect was found in
code/fixtures, not claimed as a second observed headset crash.

The final physical Vision Pro build succeeded and was installed successfully.
All 23 executable binaries are visionOS device builds and pass deep signature
verification. The previously verified DLC adapter is still present, and the
before/after inventory matches the APK, main OBB and all 124 course bundles.
Receipts: `mic-third-final-hardware-build.log`,
`mic-third-final-artifact-verification.json`, `mic-third-install.json` and
`mic-third-data-{before,after}.json` under ignored `build/steam-runtime/`.
Physical unmute and outgoing voice remain pending user retest.
