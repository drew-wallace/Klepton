# apk conversions

These targets convert the eight supplied APK variants in `../apk`. Native
library translation succeeds for all eight. **They are experimental ports, not
verified playable apps.** Runtime compatibility and device signing remain
separate requirements.

The translator emits x18/TLS warnings for some libraries; a produced framework
does not establish that every instruction or runtime path is supported.

| Target | Source variant | Translated libraries |
|---|---|---:|
| `4xvr-11026` | `4xvr/4xvr.apk`, version 1.10.26 | 21 |
| `4xvr-11026-vrp` | 4XVR version 1.10.26, VRP folder | 21 |
| `roborecall-41778` | Robo Recall 41778 | 4 |
| `roborecall-41904` | Robo Recall 41904 | 4 |
| `roborecall-47091` | Robo Recall 47091 | 4 |
| `roborecall-47091-patched` | Robo Recall patch+savefix+90Hz folder | 4 |
| `roborecall-47091-v76` | Robo Recall VRP v76 folder | 4 |
| `walkabout-57013` | Walkabout Mini Golf Steam, version 6.7 | 15 |

Each target has its own unpacked tree, APK link, product, framework directory,
bundle identifier and userdata directory. Source APKs and external data are
linked, not modified. Keep `../apk` available. Robo Recall's matching OBB is
linked under `Android/obb/com.YourCompany.RoboRecall`; Walkabout's entire `obb`
directory is linked, including its 124 loose bundles and main OBB.

The existing Makefile excludes the libraries replaced by Klepton and the
`libfrda`/`libscript` injected libraries. Non-ELF `.so` configuration files are
also excluded. The original APKs remain intact.

## Reproduce

```sh
python3 tools/prepare_apk.py ../apk
make targets
make build/klepton-ld xros
make angle-all mvk
bash visionos/mkangle.sh
bash visionos/mkmvk.sh
KLEPTON_TARGET=roborecall-41778 bash visionos/mkguest.sh
KLEPTON_TARGET=roborecall-41778 python3 visionos/gen_xcodeproj.py
```

Replace the target with any name in the table. Guest XCFrameworks contain both
device and simulator slices under `visionos/Frameworks/<target>/`. Generated
products and proprietary app data are ignored by Git.

To build all eight unsigned device apps (including dependencies):

```sh
python3 tools/build_apk.py
```

Use `--skip-dependencies` after dependencies have been built, `--target <name>`
to select one variant, or `--simulator` for simulator apps. Successful app paths
are recorded in `build/apk/apps-device.json` or `apps-simulator.json`.
The app bundle alone does not include the APK, assets or OBB data: after
installation, stage those with the matching `KLEPTON_TARGET` and
`visionos/stage_assets.sh`, or use the existing `build_run_vpro.sh <target>`
workflow once signing and compatibility are ready.

Xcode 27.0 and the visionOS 27 SDK build the runtime with a visionOS 26 deployment
target. Xcode's Metal Toolchain component is also required for ANGLE:

```sh
xcodebuild -downloadComponent MetalToolchain
```

On a fresh ANGLE checkout, if `gn` reports a missing `python3_bin_reldir.txt`,
run `vendor/depot_tools/ensure_bootstrap` using its **absolute path** and retry.
The Makefile uses Apple's linker because the pinned LLVM linker cannot parse
Xcode 27's SDK stubs. `ANGLE_GN_ARGS` can override this selection.

## Runtime findings

- **4XVR:** the host probe now loads the entry library with zero unresolved
  imports and reaches its XR session and render path. It stops when the app
  requests `ovr_IAP_GetViewerPurchasesDurableCache`: Klepton has no legitimate
  Quest purchase/entitlement service to answer that request. A real service
  integration and headset validation are still required.
- **Robo Recall:** the host probe now passes Android configuration, OBB access,
  the older UE4 JNI signatures and the x18 `adr` translation path. Version
  41778 completes NativeActivity startup and submits thousands of host frames;
  the other four variants also submit frames in short watchdog probes. One
  platform import (`ovrKeyValuePair_makeString`) remains unresolved but was not
  called in those runs. Visible rendering, gameplay and input remain unverified
  on a headset. In the visionOS 27 simulator, 41778 loads its translated
  libraries, completes `JNI_OnLoad`, opens the immersive space and sees its
  main OBB. The UE4 descriptor path is mapped into staged `UE4Game` data, which
  removes the initial `Failed to open descriptor file` message. It then calls
  `AndroidThunkJava_ForceQuit` in one run and faulted in guest worker threads
  before presenting a picture. Investigation found an x18 `ADR` veneer that
  baked in an unslid address; fixing it exposed an ANGLE staging copy that read
  pixel rows from ASTC block data, then ESSL 3.00 varying-location/link errors.
  With those fixed, the simulator mounts the pak, loads textures, submits XR
  frames, and visibly renders the Robo Recall splash image in the immersive
  space. After the splash, a solid magenta field exposed a DoubleWide eye bug:
  both eyes use one GL texture name, but binding it separately to Metal slices
  0 and 1 made the second bind replace the storage the guest renders into. The
  second compositor eye now aliases slice 0 without rebinding the GL name. A
  fresh simulator run removes the magenta and shows the game's light-gray
  post-splash field, with frames and eye draws continuing. The next issue is
  why the game stays on that field instead of showing its menu or world; full
  gameplay rendering and input remain unverified.
  The source folder supplies one matching main OBB containing the game pak and
  intro video; no supplied Robo Recall variant has a patch OBB. The generic
  missing-patch warning therefore does not establish missing content here.
  On September 28, the device recovery build fixed OpenSL streaming-position
  reporting, counted in-flight buffers in queue state, cancelled stale
  completions after Clear, and copied guest PCM before releasing the queue
  lock so scene teardown cannot free bytes the sink is still reading. The
  OpenSL regression test and OpenXR input test pass. Unbounded splash-layer
  descriptor logging is capped, and the host retains one previous boot/crash
  log on relaunch. The signed build was installed on the physical headset with
  saves preserved/backed up. Restoring Extended Virtual Addressing and
  Low-Latency Streaming was attempted: Xcode explicitly rejects both for the
  selected Personal Team. They are now opt-in; Increased Memory Limit remains
  signed and provisioned. Simulator probes with a raised camera and direct hub/
  arena startup continue submitting frames but show a gray field, without
  reproducing the reported level-transition crash. The repeated announcer,
  residual magenta and actual save-to-level transition remain unverified; the
  available device fault is an older, already-fixed missing accessory-tracking
  usage-description failure. A fresh headset reproduction is still needed.
- **Walkabout:** Unity's `JNI_OnLoad`, NativeLoader and `initJni` complete.
  The translated macOS build completes 5,000 headless frame iterations through
  Vulkan/OpenXR and exports non-black eye textures to Metal. A simulator run
  reaches the Walkabout main-menu initialization path and continues presenting
  frames; zero `eglSwapBuffers` calls are expected on this Vulkan path.
  `libngfx.so` now loads in the host probe using per-thread ELF TLS descriptor
  resolution. Unity's prefixed `loadLibrary("libngfx")` spelling is also
  normalized to the packaged `libngfx.so` name. The unsupported OpenXR
  extension advertisement has been removed.
  The Unity OpenXR subsystem manifest is stored in Walkabout's OBB rather than
  its APK. The Unity OBB index now serves deployment-relative `assets/...`
  entries, so the simulator loads `UnityOpenXR` without the former display
  subsystem/header-mismatch error. A MoltenVK compatibility fallback also
  converts a rejected two-layer attachment view into a valid layer-0 2D view;
  the simulator then reaches `XR_SESSION_STATE_FOCUSED` and the compositor
  records eye pictures without the previous render-worker fault. This validates
  startup and the display path, not full stereo fidelity or gameplay.
  Walkabout's Steam bootstrap is now traced in the simulator. The offline shim
  supplies `SteamClient023`, `ISteamUser`, `ISteamFriends`, and the generic
  interface dispatch that Steamworks.NET requests. The observed calls include
  `SteamAPI_ISteamUser_GetSteamID`,
  `SteamAPI_ISteamFriends_GetPersonaName`, and
  `SteamAPI_ISteamUserStats_RequestUserStats`; they now return a deterministic
  diagnostic identity (`Klepton Offline`) and an invalid stats call handle, so
  there is no Steam initialization failure or forced quit. This remains an
  offline compatibility layer: it does not provide Steam IPC, entitlements,
  cloud saves, callbacks, or multiplayer state.
  The first real networking surface is now identified: Walkabout asks for
  `SteamNetworkingUtils004`, `SteamNetworkingSockets012`,
  `SteamNetworkingMessages002`, and `STEAMTIMELINE_INTERFACE_V004`, then asks
  for an auth session ticket. With `KL_TRACE_STEAM=1`, the offline interface
  proxy also logs the C++ vtable slots used after the flat API lookup. Those
  calls are the boundary a host Steam bridge must implement; returning zeroes
  is enough for boot, but cannot create a lobby or carry game packets.
  The package metadata includes Steamworks.NET, PlayFab, Photon PUN, and Photon
  Voice assemblies, and contains API names such as `LoginWithSteam` and
  `GetPhotonAuthenticationToken`. This proves those SDK APIs are packaged, not
  that Walkabout invokes them in this build or uses Photon for its current
  room transport. The simulator log reports `[SavableManager] Not connected to
  PlayFab. Skipping cloud sync.` and later shows Steam identity calls followed
  by `GetAuthSessionTicket -> 0` from the offline shim. It does not show a
  successful PlayFab login, Photon authentication, room join, or voice session.
  The exact Steam -> PlayFab -> Photon path therefore remains unconfirmed.
  The game also enumerates Steam networking interfaces, but the offline shim
  returns the same generic fake object for every interface, so its vtable-slot
  trace cannot establish whether any Steam networking methods are actually
  used.
  The simulator trace found and fixed a separate transport issue: Linux
  `SOCK_NONBLOCK`/`SOCK_CLOEXEC` bits were being passed directly to Darwin,
  causing HTTPS sockets to fail with `Protocol wrong type`. The fixed sockets
  connect and exchange TLS data in a network trace; this does not establish
  successful PlayFab authentication.
  The supplied `libsteam_api.so` is an Android ARM64 Steamworks client library.
  Its ELF dependencies are `libandroid.so`, `liblog.so`, `libdl.so`,
  `libc++_shared.so`, and `libc.so`. Its strings also reference
  `libsteamclient.so` and the Steam client IPC path/error. Neither
  `libsteamclient.so` nor a Steam client process/IPC service is present in the
  app package or this visionOS runtime. The generated `libsteam_api` framework
  is a translated copy of that guest library; it does not add the missing Steam
  client service. Valve documents Android ARM64 Steamworks libraries for
  [Steam Frame](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom?language=english),
  but that support does not by itself provide a visionOS Steam runtime.
  A separate [standalone Steam feasibility gate](steam/STEAM-RUNTIME.md) now acquires
  pinned Valve ARM64 client artifacts and inventories their dependencies. The
  Android client maps and now finishes its constructors after counting `eventfd`
  and TLS rewriting fixes. An experimental host backend can create a logged-out
  local user and pipe but has intermittent worker crashes and missing runtime
  dependencies. A separate signed probe is installed on Vision Pro; execution is
  awaiting an unlocked device. No genuine ticket or PlayFab login has been
  demonstrated, and the runtime gate remains uncleared.
  The physical-device prerequisites are now cleared: this Mac has a valid Apple
  Development identity, Developer Mode is enabled on the connected Vision Pro,
  and the signed Walkabout app plus its 14 GB data set installed successfully.
  With `KL_STEAM_OFFLINE=0`, the physical trace loads the Android ARM64
  `libsteam_api.so` and reaches `SteamAPI_RestartAppIfNecessary`. A diagnostic
  `KL_STEAM_SKIP_RESTART_CHECK=1` bypasses only that relaunch check; the next
  lookup is `SteamInternal_SteamAPI_Init`, where the app terminates with signal
  4 before the function returns. No `SteamAPI_Init` success, auth ticket,
  PlayFab login, Photon login/room join, or voice handshake follows. This is
  consistent with the missing Steam client service, although the trace does not
  expose a more specific Steam error. The added restart-check switch does not
  fake an identity or ticket.
  The simulator repro now pinpoints the failure without relying on the physical
  crash report. With the shim disabled, `SteamInternal_SteamAPI_Init` asks to
  load `libsteamclient.so`; that file is not in the app, and Steam logs
  `Could not determine Steam client install directory`. Steamworks.NET then
  reports `SteamAPI_Init() failed`, and Walkabout quits. This confirms the
  missing Steam client runtime/IPC dependency is the immediate blocker, not an
  unsupported instruction in Klepton's translator.
  The simulator's offline control gets past that bootstrap and confirms Walkabout
  queries `SteamClient` interfaces, including
  `SteamNetworkingUtils004`, `SteamNetworkingSockets012`,
  `SteamNetworkingMessages002`, and `STEAMTIMELINE_INTERFACE_V004`. It calls
  `ISteamUser::GetAuthSessionTicket`; the diagnostic shim returns zero and logs
  that no signed ticket was produced. The game labels the fake identity
  `Klepton Offline`; PlayFab reports it is not connected. This confirms the
  Steam bootstrap and ticket request, but not a complete Steam -> PlayFab ->
  Photon sequence. No real identity, profile, room, voice, or reconnect has been
  demonstrated. Valve describes Steam session tickets as signed credentials and
  ticket verification as a secure-server operation in its
  [authentication guide](https://partner.steamgames.com/doc/features/auth).
  The old simulator binary also exposed an interposition bug: resolving the same
  Steam symbol a second time could replace its saved original with Klepton's
  wrapper, recursively overflowing the stack. `steam/runtime/kl_steam.c` now preserves
  the original entry point across duplicate resolution. The simulator build now
  ad-hoc signs its generated nested frameworks before install; otherwise dyld
  refused to load the translated guest frameworks as unsigned.
  Addressables' `jar:file:` requests are now served from the OBB index: the
  virtual directory probe succeeds and both `assets/aa/settings.json` and
  `assets/aa/catalog.bin` open from the archive. Unity split-resource probes
  (`.resG`, `.resS`, and `.res`) are mapped to the bare hash names used by the
  Android asset layout, and ZIP-deflated OBB entries are inflated on demand;
  the previous `Resource image couldn't be loaded completely` failures no
  longer appear in the simulator log.
  Walkabout now emulates MoltenVK's unsupported non-zero-base-vertex indexed
  draws by rebasing the bound vertex-buffer offsets; the simulator log shows
  those draws completing without the former `vkCmdDrawIndexed` feature error.
  Small sampled R8 views are also promoted to one-layer 2D-array views
  for Unity's shadow-mask path. Unity still emits three `unity_ShadowMasks`
  2D-to-2DArray assignment warnings during scene setup, but the app continues
  presenting frames (the remaining message is a managed Unity texture-dimension
  mismatch rather than a Vulkan submission failure).
  The supplied build uses UnityPlayerGameActivity and Steam/OpenXR, which need
  further runtime validation.

For the simulator probe, launch Walkabout with both variables forwarded into the
app:

```sh
KL_STEAM_OFFLINE=1 KL_TRACE_STEAM=1 KL_EXIT_ON_BACKGROUND=0 \
  KLEPTON_TARGET=walkabout-57013 ./visionos/run.sh
```

The resulting `Documents/klepton-boot.log` records the Steam interface and
method calls. `KL_STEAM_OFFLINE=1` is the simulator-safe mode; a real host
bridge would begin where the shim currently supplies `SteamClient023`.

The real-Steam simulator probe can be repeated without the headset:

```sh
KL_STEAM_OFFLINE=0 KL_TRACE_STEAM=1 KL_STEAM_SKIP_RESTART_CHECK=1 \
  KL_EXIT_ON_BACKGROUND=0 KLEPTON_TARGET=walkabout-57013 ./visionos/run.sh sim
```

Use the simulator-only diagnostic identity as a control (it is not authenticated):

```sh
KL_STEAM_OFFLINE=1 KL_TRACE_STEAM=1 KL_EXIT_ON_BACKGROUND=0 \
  KLEPTON_TARGET=walkabout-57013 ./visionos/run.sh sim
```

Walkabout's captured eye images are in
`build/apk/walkabout-57013/captures-long/`; its extended run is recorded in
`build/apk/walkabout-57013/render-long.log`.

Per-target translation, framework, import and startup logs are in
`build/apk/<target>/`. The `.dylib` files at that level target visionOS;
`macos/` contains separately translated, ad-hoc-signed host libraries.

The macOS ABI, haptics, HEVC, OpenXR space/input, soft input, controller,
broadcast, fault, executed x18 `adr` and TLSDESC register preservation tests
pass. The runtime's
device/simulator build gates and Swift typecheck pass. `make check` cannot
complete because its il2cpp fixture is absent at
`beatsaber/lib/arm64-v8a/libil2cpp.so`. These checks do not prove guest gameplay.

The Apple Development identity and connected physical Vision Pro are now
verified. A signed Walkabout device build installed and ran far enough to load
the translated ARM64 Steam library; Steam initialization itself still traps
before returning an authenticated session.

## apk app patcher evaluation

The official apk app was cloned and tested on separate copies of all seven
Quest APK variants. Patching succeeded, but Klepton startup still stops at the
same missing imports. See [docs/APK-PATCHING.md](APK-PATCHING.md) for the
comparison, output APKs and test evidence. The conversion inputs remain the
original APKs.
