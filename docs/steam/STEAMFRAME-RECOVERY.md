# Steam Frame 0.3.0 recovery investigation

The image supplies genuine ARM64 Steam client components and a matching Linux
system library set. They have been extracted and inventoried. The recovery
Android client now starts its backend workers **inside the visionOS simulator
app**, creates a public game pipe, connects to its logged-out local user, and
returns `SteamUser023`. Account login, ownership, tickets, physical Vision Pro
execution, and PlayFab authentication remain unverified. Valve’s bundled HTTPS
authentication modules additionally returned a fresh anonymous QR challenge in
a simulator WebView. An interactive QR controller now renders challenges and
polls while the backend stays alive; approved token handoff is implemented but
has not been exercised with an account.
A subsequent bounded
frame pump delivered genuine callbacks and returned success from Valve's
shutdown API; dependency and worker assertions still occur.

## Source and integrity

Valve's [recovery directory](https://steamdeck-images.steamos.cloud/recovery/)
was checked on 2026-09-28. The selected normal recovery ZIP is
`steamframe-oobe-repair-20260922.5153644-0.3.0.img.zip`, 4,061,612,171 bytes.
Its observed SHA-256 is
`672df1b9c5c6b849df506c995a32fe136b53c4db688f924e6727639dd8b963a9`.
[The source lock](../../steam/tools/steamframe_recovery.lock.json) pins this archive and the
selected client artifact hashes. These are reproducibility pins observed after
HTTPS acquisition; no independent Valve image signature was verified.

The ZIP contains `lun0.img`, 7,516,192,768 bytes. Its SHA-256 is
`081a38c051e99c09db6ae91be994b67ef3347ea71f330d803ab861f0ba8cc654`.
The ZIP member CRC and GPT header/entry CRCs passed. `rootfs-A` is a 5 GiB Btrfs
partition at byte offset 335,561,728, with SHA-256
`668da2b1d32e9b779a1ee27ed570961ceaaf83a5dfeadd8129b62603223c319d`.
The extracted `/etc/os-release` reports SteamOS `0.3.0`, variant `vr`, OS build
`20260922.5152327`. The archive build and OS build are distinct identifiers.

The QDL ZIP was inspected through HTTP ranges. It contains a 32 GiB `lun0.img`,
two 256 MiB images, loaders, QDL executables, XML layouts, and flashing scripts.
Only XML layouts and script/license text were retrieved from that package.
The investigation did not flash a device, mount the image, or boot SteamOS.

## What was extracted

All images, binaries, UI assets, manifests, and logs are under ignored
`build/steam-runtime/steamframe-0.3.0/`. No Valve executable is tracked in Git.
The filesystem catalogue covers 227,778 entries. The extraction retains SteamOS
launch configuration, ARM64 Steam components, and Valve's browser UI scripts.

The root filesystem contains `/usr/lib/steam/steam.tar.zst`, 700,296,701 bytes,
SHA-256 `c81b576cf6fef747099363a68644fe048b044da465f4ded3ecabd913fc79f148`.
Its client build-date file reports **September 15, 2026, 19:45:26 UTC**. Its
beta channel marker is `linux_arm64_beta_861407927dc8efb407da7cf`. SteamOS 0.3.0
is not a Steam client version number; the numeric client version is unverified.

| Extracted path within `steam/` | Role and observed ABI |
| --- | --- |
| `androidarm64/libsteamclient.so` | 37,850,332-byte Android/bionic client; `SteamClient023`, `SteamUser023`, and engine factory marker |
| `androidarm64/steamservice.so`, tier/networking libraries | Android ARM64 companion components |
| `linuxarm64/steamclient.so`, `crashhandler.so` | Linux/glibc SDK client and crash handler |
| `steamrtarm64/steam`, `steamclient.so`, `steamui.so`, service/helpers | Full Linux/glibc ARM64 Steam client |
| `steamrtarm64/libSDL3.so.0`, image/font libraries | Linux/glibc SDL3; needs the glibc loader and EGL |
| `steamui/`, `clientui/` script assets | Native-client browser UI references for investigating login integration |

The Android client's SHA-256 is
`4b1318ea53168ecbf74318ef5b330eb0a41a3e2d335c14f3827504bebba4b40d`.
It differs from the previously pinned September client. The Linux client, SDL3,
and crash-handler binaries also differ. Individual sizes, hashes, imports,
interfaces, and ELF metadata are recorded in `steam-elf-inventory.json`.

The static dependency investigation retained **103 AArch64 Linux system
libraries** from the same root filesystem, including glibc, its loader, graphics,
networking, audio, and IPC support. The selected client/UI/helper/SDL roots have
no unresolved `DT_NEEDED` or interpreter edges in that inventory. This does not
cover runtime `dlopen`, plugins, process creation, kernel services, or correct
visionOS ABI behavior. Linux libraries must not be substituted as bionic libs.

No Lepton Android container image or Android SDL3 library was found in the
catalogued root filesystem or bundled Steam package. A Podman service override
references Lepton, but is not the container. Valve's [Lepton guidance](https://partner.steamgames.com/doc/steamhardware/steamframe/adb_lepton)
describes a container launched by Steam for Android applications. The recovery
image therefore supplies useful client/system artifacts while the Android
container/dependency set remains a separate acquisition question.

## Probe results and implementation

The original library-only probe reported 359 bound imports and seven unresolved
imports. The backend experiment on September 29 reached `__sched_cpualloc` and
trapped. Bionic CPU-mask allocation/free are now implemented with 64-bit word
rounding and allocation-overflow rejection. The rebuilt simulator app reports
361 bound imports and five unresolved imports: `execvpe`, `execvp`,
`posix_fadvise`, `mktemp`, and `sendfile64`. The focused IPC test now also covers
CPU-mask word boundaries and allocation overflow.

A TLS site at `0x1e007e8` was incorrectly classified as embedded data. Inspection
showed the same ten-instruction stack-protected function prologue seen in the
old client, with one relocated ADRP instruction changed. The shared ELF/AOT
classifier now accepts both exact measured patterns. Unknown patterns, damaged
prologues, and truncated buffers remain refused. Tests cover both real patterns
and those rejection cases.

The updated translation reports 11,820 TLS rewrites, 1,417 x18 veneers, and zero
refusals. The independent simulator app loads its translated framework through
dyld, finishes constructors, and obtains genuine `SteamClient023`. The factory
result `0` indicates success here: the interface pointer is non-null.

The later `CreateSteamPipe: 0` is a different result: zero is an invalid
`HSteamPipe`. Valve's SDK 1.63 `ISteamClient` header says this method creates a
communication pipe **to the Steam client**, documents `ConnectToGlobalUser` as
failing when there is no existing global user, and says the normal game path
usually gets set up by `SteamAPI_Init`. The raw probe calls `CreateInterface`
directly; it does not call `SteamAPI_Init`, start the Steam Frame host client,
or connect to an already running client service. In that library-only run, the backend had not been started. The SDK interface
version and vtable slot were correct; factory lookup alone did not establish
an operational client. The backend experiment below starts Valve's workers
inside the simulator app and now returns a valid public pipe. Thus the earlier
zero pipe did not establish an external-process requirement.

The exact Valve header is saved at
`build/steam-runtime/steamframe-0.3.0/sdk-1.63/isteamclient.h` (SHA-256
`cf409526affe5956b561b40b71feb8600acae5971de91115b42a79974ab08387`), from
[Valve's Steamworks SDK 1.63 header](https://github.com/ValveSoftware/Proton/blob/proton_11.0/lsteamclient/steamworks_sdk_163/isteamclient.h).
Valve's [Lepton documentation](https://partner.steamgames.com/doc/steamhardware/steamframe/adb_lepton)
describes Lepton as a container that Steam launches for Android apps. The
recovery image provides its host Steam client and Linux library closure, but the
probe does not start that host service.
The existing client-source default is preserved; the recovery build is explicit.
The recovery build's four legacy startup/release exports were independently
disassembled on September 29. Their ARM64 forwarding signatures match the
older pinned client: pipe creation takes no arguments; pipe release takes a
pipe; global-user creation takes a pipe output pointer; user release takes pipe
and user. The builder now permits the disposable `--backend` experiment for
this hash-verified artifact. Private login methods are audited for this exact client hash. The separate
`--login-probe` mode tests login-state queries and a credential-free rejection.
No password or refresh-token setter has been called.

## Can Klepton start and reach the Steam backend?

**The backend can start and be reached through public Steamworks interfaces
inside the simulator app.** The September 29 run used the hash-pinned recovery
client translated into a signed Mach-O framework, without starting the SteamOS
`steam` executable, desktop Steam, or an offline diagnostic implementation.
The measured sequence was:

| Operation | Observed result |
| --- | --- |
| Constructors and `CLIENTENGINE_INTERFACE_VERSION005` factory | Returned; engine present, factory status `0` |
| Verified `Steam_CreateGlobalUser` startup export | Local user `1`, backend pipe `1` |
| `SteamClient023::CreateSteamPipe` | Public pipe `2` |
| `ConnectToGlobalUser(2)` | User `1` |
| `GetISteamUser(1, 2, "SteamUser023")` | Non-null genuine interface |
| `GetHSteamUser()` | Matches user `1` |
| `BLoggedOn()` | `false`, consistent with Valve's logged-out state |
| Public pipe release | `true` |
| Five-second host frame/callback pump | Completed; nine callbacks on host/game pipes; IPC counter `10` in the first run |
| Backend user/pipe release | Called; probe returned without a recorded fault |
| `BShutdownIfAllPipesClosed()` | `true` after both pipes were released |

This demonstrates local backend startup and a usable public interface path,
not account authentication. No ownership, ticket, PlayFab session, DLC
entitlement, room connection, or voice was established. The callback IDs were
internal client notifications, not successful ticket callbacks. Callback
payloads were neither printed nor retained. The probe copies the SDK 1.63
`CallbackMsg_t` field layout (POSIX packing of four bytes; ARM64 size 20), checks
metadata, and calls `Steam_FreeLastCallback` before advancing the queue.

The earlier zero public pipe was observed before `Steam_CreateGlobalUser`
started the local backend. The same public interface now creates pipe `2` and
connects to the backend. This distinguishes a missing startup step from an
intrinsic requirement for an external Steam process. The separately extracted
`steamservice.so` has not been started or established as a ticket prerequisite;
this evidence concerns the client backend in `libsteamclient.so`.

Three reached compatibility defects were fixed rather than bypassing Valve's
implementation:

- `dladdr` returned a bare SONAME instead of the loaded image's pathname.
  Valve strips the filename to locate its installation directory; the empty
  directory led to `strlen(NULL)` in shader-cache setup. Both ELF and translated
  framework paths now report the resolved backing filename. `dl_iterate_phdr`
  reports the same owned pathname.
- Valve's bionic shared-memory helper calls `open(".", 0x404002, 0777)`:
  `O_TMPFILE | O_RDWR`. The flag translation previously attempted to write-open
  the directory. The adapter now creates an exclusive random file in the chosen
  directory and unlinks it before returning the descriptor. Tests cover umask,
  link count, descriptor aliases, shared mappings, directory selection and
  access rejection. Linux `linkat(AT_EMPTY_PATH)` publication and the tmpfile
  status flag are not implemented; this supports the measured anonymous
  backing path, not the complete Linux tmpfile API. The previous
  `ValveIPCSharedObj-Steam` creation failures disappeared in the simulator.
- The profiler calls AArch64 `tgkill(getpid(), tid, 0)` to test worker liveness.
  Returning `ENOSYS` caused Valve to destroy profiles still owned by live
  workers. A temporary allocation/TSD trace captured another worker deleting
  a previously initialized profile before the owning worker crashed.
  Same-process signal-zero checks now enumerate native thread IDs and validate
  their pthread handles. Thread ports and enumeration storage are released;
  vanished ports are distinguished from lookup errors. Signal delivery and
  other processes remain unsupported. Regression tests check live workers,
  exited workers, invalid IDs and unsupported delivery. The profiler faults
  disappeared and the public pipe call returned `2`.

Remaining diagnostics include absent `crashhandler.so` and `libSDL3.so` and a
`ThreadProcess called` assertion during connection. A work-item/job-lifetime
assertion was seen during an earlier release without a frame pump; it did not
recur in the later bounded pump/shutdown runs. Valve's shutdown API returned
`true`, which is narrower evidence than proving every worker and OS resource
was reclaimed. Prolonged operation, complete teardown and login remain separate
requirements.
Five unresolved imports remain, none reached in this run. Missing SDL3 must not
be considered resolved by using the Linux/glibc library as a bionic substitute.

The latest probe log is
`build/steam-runtime/simulator-backend-steamframe/steam-probe.log`.
The first callback run is saved at
`build/steam-runtime/steamframe-0.3.0/backend-callback-pump.log`; earlier startup
evidence is in `backend-tgkill-fixed.log`.
Earlier failure logs and temporary diagnostic probe source are retained under
ignored `build/steam-runtime/`. The final build/signing receipt is in
`build/steam-runtime/simulator-backend-steamframe/receipt.json`; the startup
wrapper disassembly is in `steamframe-0.3.0/backend-startup-disassembly.txt`.
No temporary profiler or origin-comparison hooks remain in the probe. The
experimental private login calls retain the guarded offsets documented below.
This is simulator evidence only. The visionOS device runtime compile/link check
is useful development evidence but cannot establish physical signed loading.
A September 29 `devicectl` check found the paired Vision Pro locked and could
not obtain a live connection; device execution remains unverified. The latest
backend/callback probe also builds as a signed physical-device app and passes
`codesign --verify --deep --strict`; its receipt is
`build/steam-runtime/device-steamframe/receipt.json`.

The [September 30 helper experiment](STEAM-SERVICE.md) now demonstrates its
constructors, one worker creation, matching native IPC-server getter, stop and
shutdown inside the simulator app. Service message handling and a ticket role
remain unproven; the helper is separate from the account backend.

The earlier report's statement that `androidarm64/steamservice.so` had no
exports was an inventory-reading error: the inventory field is `entry_points`,
not `exports`. Direct ELF symbol inspection confirms `CreateInterface` and
other exports. It is a bionic shared library, with no ELF process interpreter;
its service factory and authenticated role have not been established. Its
presence alone does not demonstrate an authenticated client backend.

The image also provides the full SteamOS startup: its user-systemd
`steam.service` is tied to `graphical-session.target`, launches `RUNSTEAM.sh`,
and executes the ARM64 Linux/glibc `steamrtarm64/steam` PIE executable with
Steam Frame UI flags. The host/client/UI import Linux process and IPC calls,
X11/GL, PipeWire/PulseAudio, GLib, udev and NetworkManager. Those imports do not
prove that every facility is required by the in-process ticket path. Valve's
[Lepton guide](https://partner.steamgames.com/doc/steamhardware/steamframe/adb_lepton)
describes the normal SteamOS arrangement, where Steam launches the Android
container for games; it does not rule out the measured private backend path.

The local XROS 27 SDK exposes `posix_spawn` and `AppExtensionProcess`. Apple's
[extension-process API](https://developer.apple.com/documentation/extensionfoundation/appextensionprocess)
can start Apple-platform extensions and establish XPC. This does not itself
execute Valve's Linux ELF or supply Linux services. Klepton does not currently
implement an independent Linux process runtime. However, the full SteamOS
launcher incompatibility is **not evidence that the client backend cannot run
in-process**. The reached
client-path, anonymous-file and worker-liveness contracts are now implemented.
Next is the genuine login integration, remaining dependency/worker/shutdown
failures, and physical-device verification.

Valve's extracted UI scripts reference native browser bindings including
`SteamClient.User.StartLogin`, `SetLoginCredentials`, and
`RegisterForLoginStateChange`. Locations are saved in
`login-bridge-references.json`. These are research leads for the genuine runtime
integration. The bundled login UI passes a refresh token and account name to
`SteamClient.Auth.SetLoginToken`. Independent disassembly found the genuine
`IClientUser` proxy's `SetLoginToken` at slot 56 accepting two C strings,
`SetLoginInformation` at slot 54 accepting two strings and a boolean, and
`LogOn` at slot 1 forwarding a 64-bit value. A secondary-vtable relocation and backend-body audit now establish that
`SetLoginInformation` takes password first, account name second, and a remember
flag. `SetLoginToken` takes refresh token first and account name second. `LogOn`
forwards a `CSteamID` value into the CM identity setter; passing a password
pointer would be an ABI error. These setters have not been called.

The credential-free probe uses the genuine public interface's current SteamID,
queries native state (`0`, logged out), and calls the verified `Steam_LogOn`
export. It returned `5` (`k_EResultInvalidPassword`) without credentials, and
`Steam_LogOff` returned. This exercises the login path's local rejection; it
does not authenticate an account.

`GetWebUITransportInfo` takes a generated protobuf object. Its embedded schema
establishes field 1 as `uint32 port`, field 2 as `string auth_key`; the audited
generated object is 40 bytes, with the port at offset 32 and a tagged
`ArenaStringPtr` at offset 24. The probe uses Valve's constructor/destructor
and masks the string pointer tag. It reports key presence only. On September
30, the method returned true with a nonzero port and key, and a bounded native
TCP connection to `127.0.0.1` at that port succeeded inside the simulator app.
Valve's bundled JS module `45121` uses `/transportsocket/` and protobuf-based
local WebSocket authentication. TCP reachability does not establish that
WebSocket authentication. A separate SteamUI process/endpoint has not been
started.

A bounded `URLSessionWebSocketTask` to `ws://localhost:<port>/transportsocket/`
returned HTTP `403`, error `-1011`, without timing out. A minimal RFC 6455 HTTP
upgrade over an in-app IPv4 socket also returned `403`. Both sent Valve's
embedded UI origin, `https://steamloopback.host`. A temporary comparison trace
confirmed Valve's origin comparison matched; it forwarded the original result
and was removed from the final probe. Five hundred host frames before login
and fifty more after lazy listener creation did not change either rejection.
Thus this is a reachable listener with an unresolved upgrade rejection, not a
WebSocket authentication or account-login success. No local transport auth key
or Steam authentication message was sent. The rejected requests contain only
standard HTTP upgrade headers; this probe does not reimplement Steam's protocol.

A September 30 debugger run established the actual rejection sequence for
both clients: origin validation returns `1` at `0x1e420c8`; the server calls
`0xd2c284` through its registered callback at `0x1e42108`; that callback returns
`0`, and execution enters the `403` branch at `0x1e42214`. Disassembly shows
this Android callback always returns false. Its inputs and the registered
Webhelper PID cannot change that return. Extra startup operations or a libc
shim therefore cannot make this callback accept the connection. The final
probe retains no debugger or comparison interposition hooks and does not
bypass the rejection.

The recovery's full Linux client has a different implementation of the same
class's connection callback. At `0x56c128` it checks the connection and obtains
and verifies the peer process before returning acceptance. That Linux file's
SHA-256 is `3b044464e6cf912c92de65a12520da020a156141ecb45235a5de682cb7ab08a0`.
It was inspected, not executed in visionOS. This comparison does not establish
portability of the Linux dependency closure or account authentication.
The dynamic gate trace is saved in ignored `websocket-gate-debugger-2.log`;
the earlier origin-matched trace is in
`steamframe-0.3.0/websocket-origin-matched-403.log`.
The pinned disassembly ranges and audit are saved in
`callback-login-abi-disassembly.txt` and `callback-login-abi-audit.json` under the
ignored extraction directory.
The experimental private calls are enabled only by the recovery builder
after verifying the exact client SHA-256; each measured engine/proxy/export
address is checked before invocation. The guard rejects other artifacts and
ABI layouts. [The ABI record](../../steam/tools/steamframe_login_abi.json) records signatures
and scope. No credentials or tickets were supplied. These findings guide
integration of Valve's supported login path;
they do not establish authentication or justify a replacement Steam protocol.

## Valve HTTPS authentication UI probe

The WebSocket rejection is not required for the separately tested HTTPS UI
path. Hash-pinned Valve modules now load in a nonpersistent `WKWebView` inside
the signed simulator app. The native wrapper suppresses the desktop shell
entry point, exposes Valve's existing Webpack loader, and preserves all module
bodies. The wrapper uses authentication module `13459` and HTTPS transport
module `79853`; Valve performs protobuf encoding, request submission and reply
decoding. No Steam transport or authentication protocol is reimplemented.

A file-origin WebView loaded the modules but returned a failed transport
result. Loading the signed local scripts with Valve's UI base origin,
`https://steamloopback.host`, produced `BeginAuthSessionViaQR` result `1` with
nonempty challenge URL, client ID and request ID. A clean rebuilt run reproduced
this result without debugger hooks or script-loading errors. The request uses
Valve's SteamClient platform enum (`1`), a descriptive device name and no
account credentials; its optional OS field is unset because no visionOS Steam
OS enum mapping is verified.

This is **an anonymous QR challenge only**. The probe neither displays the
challenge for account approval nor polls for a refresh token. It does not hand
credentials to the native backend, authenticate an account, obtain tickets or
prove game ownership. Challenge URLs, request IDs, response bodies and tokens
are never forwarded to logs or the report. WebView state is nonpersistent;
there is no password input or credential storage in this anonymous experiment.
The separate interactive controller below implements QR approval polling and
token handoff; account login still needs authenticated execution evidence.

[The UI source lock](../../steam/tools/steam_auth_ui.lock.json) pins the four recovered
script artifacts. [The packager](../../steam/tools/steam_auth_ui.py) rejects changed files
and unknown bootstrap layouts before packaging. Generated Valve resources
remain under ignored `build/`. The `--auth-ui-probe` option is explicit and
requires the recovery source; ordinary backend probes make no QR API request.

```sh
python3 steam/tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --login-probe --auth-ui-probe
```

The clean log, input pins, signing results and receipt are in
`build/steam-runtime/simulator-backend-login-auth-ui-steamframe/`. A successfully
built physical-device app remains indirect evidence until the unlocked headset
runs it; no physical authentication acceptance is claimed. The updated probe
also builds for `xros` and passes `codesign --verify --deep --strict`; its
receipt is `build/steam-runtime/device-login-auth-ui-steamframe/receipt.json`.

`make steamcheck` passes seven C regression executables and 26 Python tests,
including the offline JavaScript login adapter lifecycle test. Recovery parser tests reject corrupted GPT checksums, truncated
entry tables, partitions beyond the disk image, escaped extraction destinations,
and oversized/truncated member data; they preserve literal POSIX backslashes.
Simulator results and signed app/framework hashes are under
`build/steam-runtime/simulator-steamframe/`.

## Interactive local login controller

The separate `--auth-ui-login` mode uses the same attested Valve modules. Its
explicit sign-in button begins a QR session; Core Image renders the challenge
locally. Valve's `PollAuthSessionStatus` encodes and decodes every poll. Requests
respect the returned interval, bound individual waits, handle rotated client IDs
and challenges, and stop after five minutes. Cancellation prevents late replies
from transferring credentials. An agreement response ends the experiment; it
is never silently accepted. The nonpersistent WebView rejects subsequent page
navigation and checks main-frame origin before accepting bridge messages.

The probe worker retains the genuine engine/user/pipe while the UI waits and
pumps frames and callbacks away from rendering. A mutex-protected mailbox
accepts one bounded refresh-token/account-name command. Only that worker calls
the independently audited `SetLoginToken(token, accountName)` and `Steam_LogOn`;
input hash and function addresses remain guarded. It queries the backend's own
identity after the setter and reports native `BLoggedOn`, without fabricating
identity or success. Connecting waits are bounded at two minutes and the whole
probe session at ten minutes. The controller is experimental and has not yet
been integrated into Walkabout's shell.

The native model stores refresh token/account name in Keychain **only after
native logged-on confirmation**, using this-device-only, unlocked accessibility.
It does not store passwords, Guard codes, access tokens or QR challenges. The
UI offers saved-login reuse and logout/forget. Tokens and account names never
enter diagnostic messages; report messages contain whitelisted stages and
integer results. Borrowed callback buffers are freed before the next callback.
Stop cancels UI polling, clears the mailbox, logs off, releases user/pipe handles
and requests backend shutdown. A true shutdown result alone does not prove all
worker resources have been reclaimed.

```sh
# Bounded simulator test: create a QR challenge, poll without account approval,
# then cancel and shut down. This cannot satisfy authentication acceptance.
python3 steam/tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --auth-ui-smoke
# Interactive simulator session; sign-in starts only from the visible button.
python3 steam/tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --auth-ui-login
# Build a signed device app; add --device <UDID> to install and launch.
python3 steam/tools/steam_probe_device.py --team <TEAM> --source steamframe-recovery --auth-ui-login
```

The September 30 simulator smoke run established: native backend ready, Valve
UI modules ready, QR rendered, three successful pending-approval polls, explicit
cancellation, native LogOff return, and `BShutdownIfAllPipesClosed=true`.
`interactive_native_logged_on=false`; no account approved the challenge and no
token was delivered. Evidence is in
`build/steam-runtime/simulator-backend-login-interactive-steamframe/receipt.json`
and `steam-probe.log`. The signed device app in
`build/steam-runtime/device-login-interactive-steamframe/` passes strict codesign
verification; physical execution remains unverified. Offline tests cover mailbox
bounds, concurrent submission, clearing on cancellation, single-flight QR start,
approved-response handoff, late-response cancellation and server rejection.
Offline fixtures never enter Valve's native backend or the network.

Walkabout's [official Steam store page](https://store.steampowered.com/app/1408230/Walkabout_Mini_Golf_VR/)
verifies App ID `1408230`. The SDK probe below now attempts that game context, but initialization has
not yet succeeded, ownership and tickets remain unverified, and the game's
packaged PlayFab Title ID is now statically verified as `98AD5`; its actual runtime
request remains unobserved. The [game integration investigation](../games/walkabout/INTEGRATION.md)
maps its session-ticket recipient, PlayFab consumer, private-room call and DLC check.
Next is an approved native session, then those
checks and Walkabout's actual PlayFab login. Private rooms, paid DLC and voice
still require their own end-to-end validation on physical Vision Pro.

## Walkabout SDK and ticket probe

`--walkabout-api` packages the game's **original** `libsteam_api.so` plus its
`libc++_shared.so` dependency as signed translated frameworks. The
[game SDK lock](../../games/walkabout/steam/walkabout_steam_api.lock.json) pins both input sizes and
SHA-256 values, the actual App ID and exported interface versions. Source files
remain outside tracked source; translated files and generated projects remain
in ignored `build/`. The builder rejects changed inputs and refused translation
sites. Receipts include input, translated and embedded signed hashes. Framework
names use the existing loader's `+` to `x` mapping for the C++ dependency.

The probe uses the genuine flat exports to call `SteamAPI_InitFlat`, acquire
`SteamUser023`, `SteamApps008`, `SteamUtils010` and the SDK's own user/pipe, then
check `GetAppID()==1408230`. Standard `SteamAppId`/`SteamGameId` environment values
supply that app context; they do not grant ownership. The SDK is initialized
first while logged out, and retried once after native logged-on confirmation.
Initialization errors remain actual SDK results. No generic offline interface
or fabricated success can satisfy this mode.

On September 30 the simulator loaded both frameworks and completed their
constructors without a recorded fault. The SDK reached the local
`libsteamclient.so`, then returned **`SteamAPI_InitFlat=3`**, reporting
`CreateFn failed for SteamUser023 User`. Redacted diagnostics classify the error
as naming `SteamUser023`. The public probe interface had been available on its
own pipe; the SDK's initialization path has not yet succeeded. This logged-out
run does not establish whether the interface becomes available after approved
login or whether an additional compatibility problem remains. It issued **no
tickets**, made **no ownership claim**, and did not reach `GetAppID`. Valve QR
polling continued with the backend alive, cancellation returned from native
LogOff, and backend shutdown returned true. Evidence is in
`build/steam-runtime/simulator-backend-login-interactive-walkabout-api-steamframe/receipt.json`
and `steam-probe.log`; immutable snapshots are under `walkabout-api-smoke-runs/`.

### SDK bootstrap trace and explicit compatibility experiment

An anonymous simulator debugger trace separated the interface failure from
backend reachability. The original SDK created pipe `3`, connected user `1`,
and obtained genuine `SteamUtils010`, then called `GetISteamUser` with user `3`
and pipe `1`. The client returned NULL. Valve's
[public signature](https://partner.steamgames.com/doc/api/ISteamClient#GetISteamUser)
takes user first, pipe second. The SDK stores pipe then user at this bootstrap
site; its `ldp w1,w2,[x21]` at ELF VA `0x24624` supplies the opposite order.
Other public getters must retain their normal argument order.

`--sdk-bootstrap-compat` is an explicit experiment requiring `--walkabout-api`.
[The correction tool](../../steam/tools/steam_sdk_compat.py) requires the original whole-file
SHA-256, ARM64 ELF header, one executable segment mapping and the exact measured
instruction. It creates an ignored translation-input copy with only that
instruction changed to `ldp w2,w1,[x21]`. The supplied original SDK remains
unchanged; this variant executes a derived SDK input. It does not change client
factory results, authentication, ownership or ticket generation. The receipt
records both original and derived hashes, address, file offset and instructions.
Offline tests check rejection of unknown inputs and instruction mismatches,
virtual-to-file address mapping and preservation of all other bytes.

The September 30 retest (`8ea6c6bf-c07a-4639-b487-9408fe69c3a4`) reached the
genuine user interface with user `1`, pipe `3`. `SteamAPI_InitFlat` advanced from
result `3` to result `1`. A second debugger trace confirmed the next failing
branch: genuine `SteamUtils010::GetAppID()` returned **`0`** at SDK return VA
`0x24654`. It did not report Walkabout's App ID `1408230`. This is an unresolved
game-context setup failure; no App ID return value is overridden. Static
inspection finds a client wrapper reading `SteamAppId` during its construction,
before the probe's later SDK setup. Moving the environment values ahead of
backend startup was tested and regressed `ConnectToGlobalUser` to zero, so that
change was removed. The required client game-context registration/lifecycle is
still unproven, including its behavior after approved login.

The corrected variant still completed the native frame pump (nine callbacks,
IPC counter `10`), served its loopback endpoint (WebSocket HTTP `403`), generated
and polled a Valve HTTPS QR challenge, then cancelled, returned from LogOff and
reported backend shutdown true. It remained logged out and issued no tickets.
Immutable receipt, redacted log and debugger metadata are under
`build/steam-runtime/sdk-compat-smoke-runs/8ea6c6bf-c07a-4639-b487-9408fe69c3a4/`.

To reproduce the bounded anonymous simulator experiment:

```sh
python3 steam/tools/steam_probe_device.py --source steamframe-recovery \
  --auth-ui-smoke --walkabout-api --sdk-bootstrap-compat --simulator booted
```

These results establish in-app backend startup and reachability in the
simulator. They do not establish physical Vision Pro execution, an approved
Steam session, successful game SDK initialization or the standalone milestone.
The same experimental variant builds for physical visionOS and passes strict
deep codesign verification; its receipt is
`build/steam-runtime/device-login-interactive-walkabout-api-sdk-compat-steamframe/receipt.json`.
That receipt contains no physical execution observation. Validation also passed
the seven C regressions and 28 Python tests in `make steamcheck`.

### Separate in-app backend and game-client instances

`--separate-game-client` is a second explicit `--walkabout-api` experiment. The
builder translates the same pinned Valve client into two signed frameworks:
`libsteamclient_backend.framework` for backend startup/login and
`libsteamclient.framework` for the game's SDK. The backend uses a distinct
Mach-O install name (`--install-name` in the existing translator). The probe
checks the actual loaded ELF bases and logs whether they are distinct; naming
two files alone is insufficient. Both instances remain inside one app process.
The builder records the second client's input, translated and signed hashes.
The original Valve binary and supplied game library are retained; the separate
SDK operand-order experiment still requires its own explicit flag.

The final anonymous simulator run
`cfa1db6b-ed85-4665-a9c5-c445d71d3ce4` confirmed `distinct=1`, loaded both
frameworks and completed QR polling, cancellation and backend shutdown. Its
SDK initialization returned `1`, classified as `ConnectToGlobalUser failed`.
An earlier debugger run with the same client/SDK inputs traced the native
connection path, without changing registers or return values:

| Native operation | Observed result |
| --- | --- |
| SDK creates its own game-client pipe | `1` |
| Global-user IPC response parsed at client VA `0x1284db4` | User `1` |
| `IClientUtils` proxy slot 18, `SetAppIDForCurrentPipe`, VA `0x1118608` | Called with App ID `1408230` and boolean `true` |
| Native registration return at VA `0x1284ef0` | App ID `0` |
| Subsequent SDK `ConnectToGlobalUser` result | `0` |

The proxy's embedded method name and serialization were audited against the
pinned ELF. It serializes a `uint32` App ID and one-byte boolean and decodes a
`uint32` result. The client connection function branches to failure when that
result is zero and the requested App ID was nonzero. This demonstrates that
the backend responds across the distinct instance boundary; the failure is in
native game-context registration rather than absence of the global-user IPC
response. No return value, ownership result or authentication state is forced.

Two further September 30 debugger relaunches of the final installed probe
(processes `30984` and `32235`) traced the refusal inside the backend. The
installed probe debug dylib, both signed client frameworks and SDK hashes
match the final build receipt. The native dispatcher calls the backend method
at VA `0x129a274`, which adjusts `this` and enters `0x1299f60`. The pipe record
exists, the requested App ID remains `1408230`, and the backend finds its one
native user object. Its app check at `0x12fe58c` then calls client-apps slot 71,
VA `0xd51f60`; the implementation's profiling string names it `GetAppStateInfo`.
The output's first two words are `0` and `0`. The executed branch at
`0x12fe6ac` requires the first word to equal `4`, so the check returns false,
registration returns App ID `0`, and SDK connection returns user `0`.

The field interpretation is inferred from the implementation and matching
[Valve enum definitions](https://partner.steamgames.com/doc/api/steam_api):
`EAppReleaseState` value `0` means unknown application information or missing
license information, and ownership flags `0` mean unknown ownership. For the
observed branch, state `4` (released), ownership bit `0` set and restriction bit
`2` clear would be required. The logged-out trace does **not** establish whether
application metadata, account licenses or both are absent, or whether approved
login will populate them correctly. The client's real checks remain intact.

This establishes startup and cross-instance backend communication **inside the
simulator app process**. It does not require the full SteamOS `steam.service`
launcher in the measured path, and it does not establish a role for the separate
`steamservice.so` helper. The native WebUI loopback port is separately reachable,
but its WebSocket upgrade still returns `403`; the bundled HTTPS login modules
use a separate transport. These are distinct observations.

Redacted evidence, installed artifact hashes, the debugger script and the narrow
native disassembly are saved under
`build/steam-runtime/app-registration-trace-20260930/`. Explicit cancellation,
native LogOff and `BShutdownIfAllPipesClosed=true` were observed after the trace;
complete worker/resource reclamation remains unproven. No account was approved,
and no ticket was requested. The next discriminating test is native account
login followed by genuine metadata/license loading and SDK initialization.

Snapshots and redacted debugger metadata are under
`build/steam-runtime/separate-client-smoke-runs/`. The corresponding physical
visionOS app builds and passes strict deep codesign verification under
`build/steam-runtime/device-login-interactive-walkabout-api-sdk-compat-separate-client-steamframe/`.
A fresh September 30 device check still reported the paired Vision Pro locked
and disconnected, so no physical execution observation was collected. Seven C
regressions, 28 Python tests and the standalone host probe build passed.

```sh
python3 steam/tools/steam_probe_device.py --source steamframe-recovery \
  --auth-ui-smoke --walkabout-api --sdk-bootstrap-compat \
  --separate-game-client --simulator booted
```

After successful native and SDK login, the implemented diagnostic checks
`BIsSubscribedApp(1408230)` before requesting tickets. It independently invokes
[the session and Web API ticket methods](https://partner.steamgames.com/doc/api/ISteamUser)
through Walkabout's SDK. The session probe uses an explicit NULL recipient; it
does not change the game's observed recipient identity or ticket choice. The
Web API probe requests `AzurePlayFab`. It treats session and service-specific
tickets separately; neither is substituted for the other. A failed/empty
session request does not suppress the independent Web API request.

The [ticket delivery gate](../../steam/tools/steam_probe_ticket_gate.h) checks callback IDs,
SDK user, requested handle, exact POSIX callback sizes and bounded ticket lengths.
It requires successful matching callbacks plus nonempty bytes for both delivery
results. Web API bytes are copied before the borrowed callback is released;
session bytes remain owned from the original bounded request. The callback is
freed before fetching another. Request buffer guards detect out-of-bounds
writes. Rejection, connection loss, timeout and shutdown cancel outstanding
handles and clear owned ticket buffers. Tickets never enter UI messages or logs.
These branches are implemented and covered by offline callback/buffer tests,
**but have not executed against an approved Steam account**. Delivery alone
would still require ticket-type-appropriate server verification and the game's
actual PlayFab request/response before authentication acceptance.

The device variant builds and passes `codesign --verify --deep --strict` with
all three frameworks embedded. Its receipt is under
`build/steam-runtime/device-login-interactive-walkabout-api-steamframe/`; no
physical execution, ticket delivery or paid DLC check is claimed. Reproduce:

```sh
python3 steam/tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --auth-ui-smoke --walkabout-api
python3 steam/tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --auth-ui-login --walkabout-api
python3 steam/tools/steam_probe_device.py --team <TEAM> --source steamframe-recovery --auth-ui-login --walkabout-api
```

The next authenticated test must establish native `BLoggedOn`, repeat the SDK
initialization, and inspect actual ownership/ticket callback results. Paid DLC
entitlement/content mapping, PlayFab Title ID and actual game login, private
rooms, and voice still remain necessary for the full standalone objective.
The [static game inventory](../games/walkabout/INTEGRATION.md) now identifies the packaged
PlayFab default and the game's actual ticket/room/DLC methods; it does not
establish their authenticated runtime behavior.

## Reproduce

```sh
mkdir -p build/steam-runtime/steamframe-0.3.0
curl --fail --location --retry 3 \
  --output build/steam-runtime/steamframe-0.3.0/steamframe-oobe-repair-20260922.5153644-0.3.0.img.zip \
  https://steamdeck-images.steamos.cloud/recovery/steamframe-oobe-repair-20260922.5153644-0.3.0.img.zip
python3 -m venv build/steam-runtime/venv
build/steam-runtime/venv/bin/python -m pip install -r steam/tools/steamframe_recovery.requirements.txt
build/steam-runtime/venv/bin/python steam/tools/steamframe_recovery.py
make steamcheck
python3 steam/tools/steam_probe_device.py --simulator --source steamframe-recovery --backend
```

The extractor streams the ZIP, verifies its hash and CRC, checks GPT CRCs and
bounds, and retains only the root filesystem. It reads Btrfs without mounting.
Symlinks from the package remain manifest entries rather than host symlinks.
`--stage prepare`, `catalog`, `extract`, or `dependencies` reruns individual
stages. Writable probe data uses its scratch/app container. Logs contain no
login secrets, ticket bytes, or authenticated-session tokens.

The next feasibility work is to integrate genuine login, resolve the remaining
reached dependencies and worker assertions, and establish physical execution.
The local backend startup/public-interface/callback path is already demonstrated
in the simulator. The builder collects its bounded result into the receipt
without marking the authentication gate ready. Each build embeds a unique run
ID, and collection rejects a completed log from any earlier launch; regression
tests cover stale-log rejection and waiting for a matching launch. The
credential-free login/transport log and receipt are under
`build/steam-runtime/simulator-backend-login-steamframe/`. Reproduce with:

```sh
python3 steam/tools/steam_probe_device.py --simulator <UDID> --source steamframe-recovery --login-probe
``` A successful local Steam session and ticket accepted through
Walkabout's actual PlayFab path remain the milestone acceptance checks.
