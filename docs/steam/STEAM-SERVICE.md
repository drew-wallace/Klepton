# Steam service execution inside visionOS

Investigation on September 30, 2026: **the account backend can start and answer
client IPC inside the visionOS simulator app. The separate Android service
helper can also start a worker inside that app.** Physical execution and account
authentication remain unverified.

There are three different components in the recovery image:

| Component | Measured result | Limits |
| --- | --- | --- |
| Android `libsteamclient.so` account backend | Native global user and pipe creation, `SteamUser023`, callbacks and client IPC work inside one simulator app process | Logged out; no ownership or ticket established |
| Android `steamservice.so` helper | Constructors, one successful worker creation, native server getter, stop and shutdown return | No service command or helper IPC message tested; returning from shutdown does not prove complete resource reclamation |
| SteamOS `steam.service` | Static unit inspection: starts `RUNSTEAM.sh`, which executes the Linux/glibc ARM64 Steam program | Its systemd, shell and Linux process environment has not been ported or run inside the visionOS app |

The first component supplies the measured account/Steamworks backend. The helper
is a separate dependency; its startup does not establish that it issues tickets
or that it is needed for the measured account path. The Linux launcher is not
required by the backend startup path already exercised in the simulator.

## Helper experiment

The helper is extracted from the hash-pinned Valve recovery ZIP, not the mobile
app or SDK. Its pin is in [the recovery lock](../../steam/tools/steamframe_recovery.lock.json):
`androidarm64/steamservice.so`, 7,610,992 bytes, SHA-256
`5625b2a0fe98b4cfb4af52266550e74d25d7e283b0503c319bdadfc51070b1b6`.
It is Android/bionic AArch64 with dependencies on `libandroid.so`, `liblog.so`,
`libm.so`, `libdl.so` and `libc.so`.

The isolated [helper probe](../../steam/tools/steam_service_probe.h) verifies the loaded
image owner and exact export offsets before invoking the private functions:

| Export | ELF virtual address | Audited call |
| --- | --- | --- |
| `SteamService_StartThread` | `0x24c7c8` | One C string naming the IPC instance; returns a server pointer |
| `SteamService_GetIPCServer` | `0x24c7b8` | No arguments; returns the native singleton |
| `SteamService_Stop` | `0x24c770` | No arguments; requests stop |
| `SteamService_Shutdown` | `0x24c788` | No arguments; invokes native teardown |

Disassembly shows `StartThread` forwarding its name to
`InitIPC(name,false,false,true)`. The call is specific to this audited binary,
not a published Steamworks ABI. A forwarding observer counts successful native
thread creations whose entry point belongs to the helper; it does not change
the entry, thread arguments or result. No service message format is recreated.

The translated framework has zero TLS/x18 refusals. The simulator app and its
embedded framework pass strict signature verification. The observed sequence is
constructors returned, non-null server, matching getter, one worker created,
stop returned and shutdown returned. Two unresolved imports, `posix_fadvise`
and `mktemp`, and the attempted load of an absent `crashhandler.so` remain
visible. No guest assertion or fault
was observed during this bounded run. This is not a complete dependency or
long-duration stability result.

Reproduce independently of Unity and all game data:

```sh
python3 steam/tools/steam_probe_device.py --source steamframe-recovery \
  --service-probe --simulator booted
# Build and sign for the physical device without attempting launch:
python3 steam/tools/steam_probe_device.py --source steamframe-recovery \
  --service-probe --team "$KLEPTON_TEAM"
make steamcheck
```

Receipts and redacted logs are in ignored
`build/steam-runtime/simulator-service-steamframe/` and
`build/steam-runtime/device-service-steamframe/`. The helper experiment is
isolated from backend/login/game experiments. It never calls credentials,
ticket or ownership methods, and receipts never treat a server pointer as an
authenticated account or successful IPC message.

## Why Walkabout still receives zero

A fresh separate-client simulator run used **Walkabout's unmodified SDK**:
`--auth-ui-smoke --walkabout-api --separate-game-client`, without
`--sdk-bootstrap-compat`. Run `d9c519fe-a80f-464d-8a28-33735037873b` loaded
distinct backend and game-client instances, created backend user/pipe `1`,
public pipe `2`, delivered nine callbacks and counted ten IPC calls. The genuine
SDK returned initialization result `1`, classified as `ConnectToGlobalUser`
failure. Native login remained false. The observation deadline expired before
this run demonstrated cancellation or shutdown; those fields remain false.

A subsequent original-SDK smoke run, `9aefe919-0862-48b9-8eff-9f7089f20c88`,
was re-observed from its still-live original launch PID `48758` after the initial
deadline. Its log confirms three successful anonymous polls, cancellation,
native LogOff and `BShutdownIfAllPipesClosed=true`. No restart was used to obtain
that evidence. Its initial timeout was therefore an observation limit, not
proof of stalled polling or teardown. The completed snapshot is preserved in
`build/steam-runtime/original-sdk-smoke-runs/9aefe919-0862-48b9-8eff-9f7089f20c88/`.
No account was approved, and the genuine SDK result remained `1` while logged
out. An interactive physical-device probe using this unmodified SDK and two
client instances also builds and passes strict deep signature verification.

The earlier [native registration trace](STEAMFRAME-RECOVERY.md) observed the
game-client receiving global user `1` over IPC, then the backend rejecting
`SetAppIDForCurrentPipe(1408230,true)` with App ID `0`. The backend's app record
had state `0` and ownership flags `0`. That trace used the separately documented
SDK operand experiment; it is not a new trace of the unmodified SDK run.
The fresh failure is consistent with that earlier diagnosis, but does not prove
the same internal branch was executed.

Starting a service therefore does not resolve the remaining authenticated app
context. The next discriminating test is approved native Steam login for an
owning account, followed by real metadata/license loading, original SDK
initialization and ticket callbacks. No metadata, ownership, interface result
or return value is forced. The paired Vision Pro still reported locked and
disconnected during the fresh device check, so only simulator execution and
device build/signing are established.

## Authenticated simulator follow-up

Run `0f1edfc4-1ae8-4684-ac0a-6bb8023136c7` now demonstrates approved QR
authentication, native logged-on state and a successful retry through the
unmodified Walkabout SDK: initialization `0`, App ID `1408230`, genuine
base-game subscription `true`. The host no longer treats a full batch of valid
callbacks as a fatal error. The verified final observation
`662f067b-736e-457e-8f12-4b3886966b2e` records SDK shutdown, LogOff,
`BShutdownIfAllPipesClosed=true` and probe return after its ten-minute lifecycle.
That return does not establish complete worker/resource reclamation.

Both ticket APIs initially returned zero handles, so genuine tickets and
PlayFab remain unproven. The next probe reconnects the approved Keychain login
and retries zero-handle requests for a bounded period. It preserves both API
types and the original SDK; ownership and ticket results are never overridden.

The follow-up saved-session run `f766dfb4-dd33-4778-b872-c38ef4a8844b` delivered
both genuine ticket types through matching successful callbacks. Its next run
`1e282cb7-c256-4dd7-ba41-2a0a1f8a39c0` repeated delivery using the game's
`PlayFab` recipient and 1,024-byte session buffer, and observed 32 subscribed DLC
entries with none installed in the independent probe. These results supersede
the earlier zero-handle observation; they do not establish a PlayFab server
response or purchased-course availability in the game. The updated physical
probe compiles and passes strict deep signature verification, while the latest
device listing still reports the paired headset disconnected.
