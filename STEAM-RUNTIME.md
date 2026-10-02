# Standalone Steam runtime feasibility gate

Status on 2026-09-30: **incomplete; the local backend is reachable in the
visionOS simulator; approved login, original SDK initialization, ownership and
genuine ticket delivery are demonstrated. The integrated game reaches its
actual PlayFab login-success callback in the simulator. Physical execution,
private rooms, voice and paid DLC gameplay remain unverified.**
An opt-in simulator variant now starts this backend in the Walkabout app and
hands off to Unity only after genuine startup ticket callbacks. Repeated runs
pass that handoff. A measured Darwin nonlocal-jump compatibility bug blocked
Unity's GC signals across process threads. The guest jump adapter now restores
only its caller's mask. A simulator run passes 16,384 frame-loop iterations,
initializes the game's own Steam API and requests a 426-byte session ticket.
Visible gameplay and game PlayFab login remain unverified; the run reports
Addressables/PlayFab resource and OpenXR Display failures. A second measured
handoff bug caches the probe's OBB directory after game configuration. Its fix
passes focused host and simulator tests; the integrated game now reads its
Addressables catalog and menu assets. A retired cached JNI ContentResolver in
PlayFab's device-identifier lookup caused the next abort. The pinned-singleton
fix passes focused host/simulator tests and the game now passes 8,192 frame-loop
iterations with the experimental mask workarounds off. A further cache reset
makes Android version/package/manifest metadata follow game configuration;
OBB directory enumeration now handles Unity's APK-prefixed asset paths. The
updated game reports version `57013`, discovers its subsystem manifest and
returns success from Display provider registration, without the earlier
Display loading error. It passes 16,384 frame-loop iterations and submits
stereo Vulkan frames. Early method observation captures the game's
`LoginWithSteam` request and its `OnLoginSuccess` callback; read-only state
inspection confirms its controller logged in and the active shared Title ID
matching packaged `98AD5`. Ticket bytes and response tokens are never read by
this observer. Visible gameplay and physical execution remain unverified.
See the [integrated game-host evidence](WALKABOUT-INTEGRATION.md#experimental-simulator-game-host)
for the build recipe, receipts and remaining work.
The [service investigation](STEAM-SERVICE.md) additionally demonstrates bounded
in-app worker startup and native server access for `androidarm64/steamservice.so`,
and reproduces the separate-client connection failure with Walkabout's original
SDK. Helper service commands and physical execution remain untested.
The Steam Frame recovery client starts its workers inside the app, returns
public pipe `2`, connects to logged-out user `1`, and supplies `SteamUser023`.
Physical execution, stable operation and complete resource reclamation remain
unverified. Approved simulator login is now demonstrated as described below. Valve's bundled HTTPS authentication modules also produced an
anonymous QR challenge inside a simulator WebView. The experimental interactive
controller now displays QR challenges, polls using Valve modules, keeps the
backend alive, and implements bounded token handoff and Keychain storage after
native logged-on confirmation. Anonymous polling and cancellation pass in the
simulator. An approved QR login now reaches native `BLoggedOn=true` and saves
the refresh credential in Keychain. Saved-session relaunch is now demonstrated in the simulator. The independent probe now packages Walkabout's genuine SDK and
C++ dependency; its logged-out initialization reaches the backend but returns
`3` with a `SteamUser023` creation failure. Debugger tracing identifies reversed
user/pipe arguments in that SDK bootstrap call. An explicit, hash-pinned
`--sdk-bootstrap-compat` experiment changes one instruction in an ignored input
copy, preserves the supplied original, and reaches the genuine user interface.
It then returns initialization result `1`: the genuine client reports App ID
`0`, so game-context setup remains unresolved. Moving App ID environment values
ahead of backend startup regressed user connection and was removed. A further
explicit `--separate-game-client` experiment uses distinct
signed client instances within one app. It receives the backend's global-user
IPC response, then native `SetAppIDForCurrentPipe(1408230,true)` returns `0`,
causing game SDK connection to fail. A September 30 debugger trace locates the
refusal in the backend's app-state check: `GetAppStateInfo` reports state `0`
and flags `0` for Walkabout, and the check rejects that record. The matching
Valve enums describe unknown application/license information; this logged-out
trace cannot distinguish which information is absent. Behavior after approved
account login initially failed at the same SDK connection seam. The authenticated
probe then exposed a callback batching bug: 256 valid callbacks caused the host
controller to stop. Bounded batches now yield to the next frame instead. A
60-second, five-second-interval retry window allows genuine SDK initialization
to wait for account metadata; interface/version failures are not retried.

Simulator run `0f1edfc4-1ae8-4684-ac0a-6bb8023136c7` reached approved native
login, original SDK initialization result `0`, genuine App ID `1408230` and
`BIsSubscribedApp(1408230)=true`. Its SDK results were `1, 1, 0`. Both ticket
requests returned zero handles and the session ticket length was zero; no
ticket callback or PlayFab request was delivered. The matching verified-launch
observation is saved under `build/steam-runtime/simulator-backend-login-interactive-walkabout-api-separate-client-steamframe/observations/894c9bca-4872-4449-9fec-38ad7e3f9d4f/`.
This establishes simulator login and base-game ownership, not ticket delivery,
server verification, DLC entitlements or physical execution.

Reopening cached client data also reached `posix_fadvise` and aborted at its
unresolved import. The shim now validates parameters/descriptors and returns
Linux `EOPNOTSUPP` without changing `errno` or claiming a cache operation. The
client tolerates this explicit unsupported result and starts. Focused regressions
cover callback batches larger than 256, borrowed-buffer release, delayed SDK
initialization and its retry bounds, and file-advice error handling. The table
generator cannot fully regenerate here because the configured JKXR guest tree
is absent; its hand-written symbol list and the existing header were updated
without dropping other guests’ forwards. These results do not satisfy the
standalone milestone.

Follow-up run `f766dfb4-dd33-4778-b872-c38ef4a8844b` reconnected the approved
Keychain login, initialized the original SDK and received a 426-byte session
ticket plus a separate `AzurePlayFab` Web API ticket. Matching callbacks `163`
and `168` each reported result `1`; their ticket buffers passed the existing
bounds/lifetime gates. The completed snapshot
`b097f8a2-70a7-4a09-a239-82c32048f79d` records ticket cancellation, SDK shutdown,
LogOff, backend shutdown and probe return after a request through the supported
cancellation mailbox. Server verification and game PlayFab login were not run.

Run `1e282cb7-c256-4dd7-ba41-2a0a1f8a39c0` additionally uses the game's observed
session arguments: a 1,024-byte buffer and a 136-byte networking identity set to
`PlayFab` by the original SDK's setter. Its 426-byte session ticket and separate
Web API ticket again received successful callbacks. The genuine DLC enumerator
returned 32 entries: all subscribed, all `BIsDlcInstalled=false` in this probe
without the game assets. Store-page availability is recorded independently
from subscription. Neither this bounded enumeration nor finding asset files
establishes purchased-course loading in the actual game. The current observation
is `4613a48b-66a2-4579-aa31-da5eecdaf9b5`. After cancellation through the
controller mailbox, final observation `1756cb6b-6d99-4be6-abb5-e7aff839e8c7`
records ticket cancellation, SDK shutdown, LogOff, backend shutdown and probe
return. The app process outlives that worker; complete resource reclamation
remains unverified.

The [Steam Frame 0.3.0 recovery investigation](STEAMFRAME-RECOVERY.md) records
the September 15 client build, matching Linux system libraries, measured
startup path, compatibility fixes and remaining assertions. The recovery
artifact is explicitly selected; earlier default-client results below remain
historical evidence for that different pinned build.

The [Walkabout integration investigation](WALKABOUT-INTEGRATION.md) statically
verifies packaged PlayFab Title ID `98AD5`, the game's session-ticket request
with recipient `PlayFab`, its hexadecimal encoding and `LoginWithSteam`
consumer. It also maps the private-room and Steam DLC methods and exposes
Walkabout's existing microphone opt-in. These findings and the simulator build
do not establish authenticated game login, room connection, voice or paid DLC.

## Acquired artifacts

Valve's public [Linux ARM64 client manifest](https://client-update.akamai.steamstatic.com/steam_client_linuxarm64)
provides Android/bionic and Linux/glibc client components. Version `1788652215`
is pinned in [the lock](tools/steam_runtime.lock.json), along with the original
manifest text. The downloader checks package sizes and SHA-256 values against
that snapshot and downloads over HTTPS. It does not independently validate
Valve's `kvsignatures`; hashes alone are not a separate signature verification.
The saved manifest allows reproduction after the live manifest changes.

| Package | Selected files | Observed ABI |
| --- | --- | --- |
| `bins_androidarm64_linuxarm64` | `libsteamclient.so`, `steamservice.so`, networking/tier libraries | AArch64, Android/bionic |
| `bins_sdk_linuxarm64_linuxarm64` | `steamclient.so`, launch wrapper, crash handler | AArch64, Linux/glibc |
| `bins_linuxarm64_linuxarm64` | `steamrtarm64/steam`, `steamclient.so`, `steamservice.so`, UI/helper libraries | AArch64, Linux/glibc |
| `steam_linuxarm64` | `ubuntu12_32/steam` bootstrapper | **i386**, despite the package name |
| `sdl3_linuxarm64`, `sdl3_linuxarm64_linuxarm64` | SDL/image/font components | i386, x86-64, and AArch64 **glibc**; no Android/bionic SDL3 found |

The Android client has `CreateInterface`, `SteamClient023`, `SteamUser023`, and
the private `CLIENTENGINE_INTERFACE_VERSION005` marker. Its direct dependencies
are `libandroid.so`, `liblog.so`, `libm.so`, `libdl.so`, and `libc.so`. This is
substantial client code, but the exports and strings do not prove an initialized
backend, a usable login flow, or in-process operation.

The Linux SDK client needs glibc and its dynamic loader. The full Linux client
also needs libraries including X11, OpenGL, PipeWire, PulseAudio, GLib, and udev.
Those are different requirements from Android/bionic compatibility. They are
not satisfied by renaming the Linux library to `libsteamclient.so`.

All downloaded binaries, extracted packages, translated images, inventory, and
probe logs live under ignored `build/steam-runtime/`. No client binary or account
data was added to tracked source. The inventory reads the verified archives,
records individual ELF hashes, imports, dependencies, entry points, TLS and
relocation metadata, and highlights imports relevant to processes, IPC, Java,
synchronization, and executable memory. It does not execute guest code.

## Reproduce

From the repository root:

```sh
python3 tools/steam_runtime.py fetch
python3 tools/steam_runtime.py inventory \
  --guest walkabout-57013/lib/arm64-v8a/libsteam_api.so
make steamcheck build/steam_probe
python3 tools/steam_runtime.py probe
```

The game library is optional for inventory; omit `--guest` on a checkout without
the supplied APK. The probe runs independently of Unity, graphics, and the game
data. It checks the hashes of its extracted inputs against the archive inventory.
Its exit status is deliberately nonzero: a successful load is not authenticated
client readiness. `gate.json` records process result codes, elapsed time, observed
constructor/interface/pipe/logged-on stages, and untested authentication stages.
Each run keeps immutable logs under `runs/<run_id>/`; top-level probe logs are
convenient latest copies.

An explicit constructor experiment runs in a disposable macOS process, with a
15-second timeout and offline/restart-bypass modes disabled:

```sh
python3 tools/steam_runtime.py probe --construct
```

`--construct` intentionally executes unproven code, including code paths with
unresolved imports; it is a failure-localization experiment, not a shipping
startup path. The Android constructors now return after the semaphore adapter
and TLS classification fix. The Linux candidate is inspected only. These probes
do not request or print tickets.

The next two experiments are explicit:

```sh
python3 tools/steam_runtime.py probe --interfaces
python3 tools/steam_runtime.py probe --backend
```

`--interfaces` uses the documented Steamworks 1.63 `SteamClient023` method
order and signatures. `--backend` also invokes the engine factory and the pinned
artifact's disassembled legacy pipe/global-user exports. It retains the backend
user while the public pipe is exercised. These private startup experiments have
no supported-platform reference and must not become a shipping login contract.
The basic modes call no private login method. The separate hash-pinned
`--login-probe` checks native state and a credential-free rejection; it cannot
authenticate an account. The public `SteamUser023` probe checks the
returned handle and logged-on state when startup survives.

## Observed blockers

1. **Backend worker instability.** `Steam_CreateGlobalUser` can return a real
   local user/pipe (`1`, `1`) and start client workers. One run then completed
   public `CreateSteamPipe` (`2`), `ConnectToGlobalUser` (`1`), and pipe release.
   Other runs fault in Valve's profiler at `libsteamclient.so+0x1c56b2c`, with
   an invalid current-node pointer. The failure is intermittent and remains
   after the Android pthread-key validity-bit correction. No reliable backend
   startup, authenticated identity, ownership, or ticket is established.
2. **Reached runtime dependencies.** Startup reports failed Valve shared-memory
   objects and missing `libSDL3.so` and `crashhandler.so`. The sampled client
   creates sockets and workers; this is more evidence than interface strings,
   but it is not a complete dependency closure or proof of in-process login.
   The SDL archives from the same pinned Valve manifest were acquired and
   inspected: their ARM64 SDL is glibc, not a bionic dependency substitute.
3. **Five unresolved Android imports:** `execvpe`, `execvp`, `posix_fadvise`,
   `mktemp`, and `sendfile64`. The current startup experiments did not establish
   that all five are required. No process execution was silently emulated.
4. **The Linux SDK candidate still has 89 unresolved imports and five refused
   x18 sites.** Glibc layouts require a separate compatibility/dependency audit.
5. **Physical execution and login remain absent.** A separate signed probe was
   built and installed on the connected physical Vision Pro. Launch did not
   produce an app log; the device reports a passcode requirement. Device
   execution, login/Steam Guard interaction, and signed ticket issuance are not
   verified. No Steam Frame is available for the supported-platform reference.
   The backend's credential/login contract is still being audited; no credential
method has been called.

AOT emission succeeded for macOS and visionOS. The ad-hoc-signed macOS
translation loads through dyld and reports the same five missing imports.
The separate app and embedded framework pass code-signature verification.
Installation does not establish execution or signed-framework loading on the
physical device, backend readiness, identity, ownership, callbacks, or tickets. No conclusion
that a standalone port is architecturally impossible follows from these failures.

## Compatibility changes and device probe

The new `kl_eventfd.c` implements the reached semaphore flag with a separate
64-bit counter, level readiness, overflow, guest read/write and FORTIFY hooks,
vector I/O, shared descriptor aliases, and close/dup/fcntl handling. Native
socket backing remains nonblocking; guest blocking flags are represented and
shared explicitly. Blocking operations handle interruptions and deferred thread
cancellation without leaving the counter mutex or references behind.
Focused tests cover empty/full readiness, semaphore draining, bounds, blocking
readers/writers, concurrent readers, cancellation, signal interruption, shared
status flags, fd reuse, and descriptor leaks. A full-runtime test resolves the
guest symbols and exercises the existing epoll adapter.

The TLS rewrite at `0x1dca7e8` was a real stack-protected function after an
OpenSSL attribution string. A narrowly guarded full-prologue exception is shared
by ELF and AOT rewriting; stale/truncated patterns and actual data still get
refused. The pinned Android client now has zero TLS and x18 refusals. Tests
check that the general data detector remains conservative.

Android pthread keys now carry bionic's bit-31 validity marker, as in the
[Android implementation](https://github.com/aosp-mirror/platform_bionic/blob/main/libc/bionic/pthread_key.cpp).
The guest APIs decode the key before accessing the existing per-thread slot
and generation table. Tests cover the 32-bit output bounds, invalid keys,
per-thread values, destructors, deletion, and stale data after slot reuse. This
fix alone did not eliminate the backend worker crash.

Set `KL_STEAM_DATA_ROOT` to redirect the observed `$HOME/Steam` file tree into
selected client data. The probe uses its scratch/app container, without changing
host `HOME`; path-boundary tests prevent mapping a sibling such as `SteamOther`.
The probe exports the same 157 baked CA roots already used by Klepton's Android
trust store to `cacert.pem`. OpenSSL parses all 157 certificates, and the client
no longer reports the initial missing-root bundle. Certificate verification is
not bypassed. No account authentication material has been provided.

Build the independent visionOS probe with an existing development team:

```sh
python3 tools/steam_probe_device.py --team "$KLEPTON_TEAM"
# Add --device <physical-UDID> to install and launch.
# Add --backend only for the unstable local-backend experiment.
```

The builder verifies artifact hashes, translates the pinned Android client into
an embedded framework, rejects translation refusals, signs via Xcode, and verifies
the app signature. It links the runtime archive by absolute path so a neighboring
`libklepton.dylib` cannot be selected in its place. Generated projects and logs
stay in ignored `build/steam-runtime/device/`. Receipts include artifact and app
hashes and explicitly untested authentication stages. The separate bundle uses
no Unity, game assets, or Walkabout data. Its log is in the app container at
`Library/Application Support/steam-probe.log`.

## Simulator validation and reproduction

The original default-client experiment used the booted ARM64 Vision Pro
simulator running visionOS 27.0 on 2026-09-28. The default probe loaded its translated client through dyld, bound
357 imports, and reported five missing imports and zero TLS/x18 refusals.
Constructors returned and the genuine `SteamClient023` factory succeeded.
`CreateSteamPipe` returned `0`; no client user or authentication was available.

The backend experiment created a logged-out local user/pipe (`1`, `1`), then
faulted in a guest worker at `libsteamclient.so+0x1c56b2c` with an unmapped `0x8`
address. The simulator crash report confirms `EXC_BAD_ACCESS` / `SIGSEGV`.
Reached diagnostics include failed Valve shared-memory mappings, absent
`libSDL3.so` and `crashhandler.so`, and refused raw syscall 131. This reproduces
the existing macOS failure on the simulator; it does not establish login.

On September 29, the recovery client advanced past those failures after
implementing resolved `dladdr` paths, the reached `O_TMPFILE` anonymous backing
path, and same-process signal-zero `tgkill` liveness checks. The genuine backend
then returned a public game pipe, user interface and logged-out state; the probe
released its public pipe and returned without a recorded fault. The next
probe added a five-second host frame pump and drained nine genuine callbacks
from the host/game pipes; Valve's IPC counter reported ten calls. After releasing
both pipes, `BShutdownIfAllPipesClosed` returned `true`. The earlier job-lifetime
assertion did not recur, but SDL3 and worker assertions remain. Complete resource
teardown, login and physical execution remain unverified. Reproduce with:

```sh
python3 tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --backend
```

The builder now collects the simulator log at
`build/steam-runtime/simulator-backend-steamframe/steam-probe.log`,
with a build/signing receipt and observed stages under
`build/steam-runtime/simulator-backend-steamframe/receipt.json`.
An observation timeout is not a runtime failure or permission to restart. New
receipts record the simulator launch PID, executable path and process start
time. Collect a later snapshot from that same launch with:

```sh
python3 tools/steam_probe_device.py --simulator booted \
  --observe build/steam-runtime/simulator-backend-login-interactive-walkabout-api-separate-client-steamframe/receipt.json \
  --observe-seconds 20
```

This read-only mode verifies process identity, installed app/framework hashes
and the matching run log. It creates an observation snapshot without changing
the build receipt, rebuilding, installing, terminating or launching an app.
A missing or reused PID, changed installed artifact or stale log fails clearly;
none triggers a restart. The app process and its backend worker have separate
lifetimes, so the receipt also records whether the probe worker returned.
`make steamcheck` now passes five C regression executables and 21 Python tests
on macOS. `make xros-device` compiles and links the device runtime; neither
result proves physical-device execution or authentication.

Four C regression executables were compiled against the ARM64 simulator SDK
and run with `simctl spawn`: `t_steam`, `t_eventfd`, `t_tls_classifier`, and
`t_steam_ipc`. All exited `0`. They verify forwarding using mocks, counter/IPC
semantics, TLS classification, and Android pthread-key behavior. Mock tickets
are test inputs and are not authentication evidence.

Logs and build receipts are under `build/steam-runtime/simulator/` and
`simulator-backend/`; the latter also contains the crash report. The stable
public-interface probe was installed after the earlier failing backend test.
The latest installed probe uses the recovery backend experiment.

The same independent probe can build and run on an already booted ARM64
visionOS simulator, without a development team:

```sh
python3 tools/steam_probe_device.py --simulator
# Or select a specific simulator: --simulator <UDID>
# Add --backend for the experimental local user creation path.
```

The builder uses the simulator SDK, runtime archive, and `visionossim` framework
translation, with an ARM64 app slice. Simulator and physical builds keep separate
outputs and receipts. Standard output and error share a descriptor so diagnostic
lines cannot overwrite one another in the app's log. Simulator execution is a
development result; physical loading and authentic login remain required.

## Changes made to Steam forwarding

`SteamAPI_ManualDispatch_RunFrame` and
`SteamAPI_ManualDispatch_FreeLastCallback` now forward their `HSteamPipe`
argument. The previous wrappers discarded it. Signatures and callback ownership
were checked against Valve's [Steamworks 1.63 header](https://github.com/ValveSoftware/Proton/blob/master/lsteamclient/steamworks_sdk_163/steam_api.h).

When no real initialization entry point exists, the internal/flat wrappers return
`k_ESteamAPIInitResult_NoSteamClient` and populate the SDK error buffer. When no
real ticket function exists, the ticket wrapper clears the output size and returns
an invalid handle. Real results and callback pointers are forwarded unchanged;
repeated resolution still cannot replace an original function with its wrapper.

`make steamcheck` runs offline regression tests for acquisition integrity, saved
manifest consistency, archive paths/symlinks, ELF parsing, missing artifacts,
probe failure reporting, wrapper recursion, pipe forwarding, callback ownership,
ticket buffer bounds, unavailable-runtime behavior, eventfd/epoll semantics, TLS
classification, Android thread-specific keys, thread liveness, anonymous file
backing and client path mapping. Mock ticket handles in
these tests are test data, not usable credentials. The changed Steam runtime
source also passes a visionOS SDK syntax check.

## Conditions for continuing beyond the gate

Resolve the remaining worker/shutdown assertions and dependency requirements.
Validate the SDL ABI and complete the
selected dependency closure. Establish a supported client startup and login
contract, then prove reliable local execution and authentic login on physical
Vision Pro. A locked-device install or logged-out handle does not clear this
gate. Retain failure reports and avoid guessing private login layouts.

Only then add the shell runtime controller, credential persistence, lifecycle
handling, and ticket consumer. Preserve Walkabout's actual session-ticket path;
probe the service-specific Web API path separately. PlayFab distinguishes those
ticket types via `TicketIsServiceSpecific`, and its Web API ticket identity is
`AzurePlayFab`. [PlayFab API](https://learn.microsoft.com/en-us/rest/api/playfab/client/authentication/login-with-steam?view=playfab-rest)

Final acceptance remains a genuine ticket accepted through Walkabout's actual
PlayFab login path for an owning account. Neither a passing unit test nor an
interface pointer is a substitute for that result.

## Current simulator authentication checks

`make steamcheck` passes ten C regression executables, 33 Python Steam tests
and ten Walkabout inspection tests after the callback, retry and file-advice
changes. The run log is
`build/steam-runtime/simulator-auth-regressions-20260930.log`. Simulator builds
and the updated physical-device probe also pass strict deep signature
verification. The current device build run ID is
`d0849988-6e60-4ba0-929b-74bec4ae61b8`; it has not been installed or launched.
These checks do not establish physical-device account login, actual game
PlayFab authentication, room joining, voice or purchased-course loading.

Reproduce saved-session simulator login with a previously approved Keychain
credential (the command contains no credential):

```sh
python3 tools/steam_probe_device.py --source steamframe-recovery \
  --auth-ui-login --use-saved-login --walkabout-api --separate-game-client \
  --simulator <UDID>
```

The saved-session option is simulator-only, selected explicitly and attempted
once. Without a stored credential the interactive QR controls remain
available. Real-mode ticket data reaches the diagnostic consumer only after
matching callback success; session and service-specific ticket types remain
separate. The current session request uses the game's observed PlayFab recipient
and 1,024-byte buffer; earlier NULL-recipient results remain historical. No
server or actual game's HTTP login was run by this probe.

## October 1 physical user test and recovery fixes

The user reports successful physical gameplay, private-room entry and incoming
voice. Genuine device startup ticket callbacks succeeded after an app restart;
first-login ticket recovery remains the reported issue. Outgoing voice produced
three matching JNI aborts, now traced to the missing Photon recorder contract.
See `WALKABOUT-INTEGRATION.md` for the crash classification and implementation.

Ticket cancellation was clearing the callback user ID without restoring it on
retry. Each attempt now restores the real SDK user; zero handles, partial
requests, transient callback results and timeouts retry within five minutes.
A user-requested retry is handed to the worker and reinitializes the genuine
startup SDK without restarting the local backend or app. Interface/ABI failures
remain explicit and ownership remains a genuine Steam query.

The sign-in controller now offers account/password login plus Steam Guard app
or email codes, using Valve's pinned encryption/authentication modules. No
password or code is retained. Reusable credentials keep the existing Keychain
policy. Fresh physical code approval and outgoing voice need retesting with
the updated build.
