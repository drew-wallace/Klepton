# Eleven Table Tennis integration

Status on October 5, 2026: offline and authenticated simulator startup and visible immersive rendering verified. The original Steam SDK confirms app ID 488310, ownership, and successful diagnostic ticket callbacks after the user approved Valve sign-in. The user verified good physical gameplay but reported popping/clicking audio. The user confirmed joining an online match and hearing the other player clearly. FMOD game audio still produces popping/clicking despite the output-producer registration fix.

## Inputs and Steam context

The supplied APK is `~/ovrport/eleven/eleven.apk`, package `quest.eleven.forfunlabs`, version `0.436.35_2026.9.4.1.25.53.652` (210648353). The companion Steam app ID is 488310. The APK contains Unity OpenXR, native Eleven physics, FMOD, Opus, and its original Steamworks SDK.

`games/eleven/steam/steam_api.lock.json` pins the original SDK and its SteamApps009 interface. The existing local Steam host uses that SDK with the pinned SteamFrame client. It checks the returned app context and real ownership. The Steam host's independent ticket probe does not prove that Eleven's game server accepts authentication.

## Simulator

```sh
games/eleven/run_simulator.sh
```

This prepares the extracted APK when needed, stages the genuine Steam host, and builds/installs the Eleven target in the booted visionOS simulator. Sign in to Steam through the simulator's login panel. No APK or Steam binaries belong in Git.

For startup diagnostics without a real Steam login:

```sh
KL_STEAM_OFFLINE=1 KL_TRACE_FRAME_PROGRESS=1 KL_TRACE_FMOD=1 games/eleven/run_simulator.sh
```

Offline mode uses the existing diagnostic Steam implementation. It cannot establish online readiness. The user authorized physical testing after simulator mouse interaction remained ineffective. Simulator-only controller rays, tracking areas, OpenXR image capture, and Eleven base-vertex emulation additions have been removed. Keep immersive mode enabled for visual checks: the headless frame-pump mode has no tracked head pose and places the camera at the origin.

## Compatibility changes and checks

- A 4,096-frame simulator run completes without the earlier JNI/import aborts. An immersive run supplies a 1.60 m head pose and displays the table, room, and readable language-selection onboarding. Diagnostic captures verified both Vulkan eye textures. Nonzero guest PCM reaches CoreAudio. The ignored screenshot is `build/eleven-visible-onboarding.png`.
- Newer FMOD Java AudioDevice output feeds signed 16-bit PCM to the existing native sink. Android StreamingAssets URLs resolve to the extracted APK asset tree. All 13 banks and the ElevenMaster buses loaded successfully in simulator diagnostics.
- System-service caches retain their Java objects across local-frame retirement; Eleven’s periodic memory query previously reused a retired ActivityManager. The shared JNI cache fixture reproduces this lifetime pattern.
- The avatar SDK uses real File access, construction, existence, and directory creation, plus the native libc `ldiv` ABI.
- MovieCapture context and cryptographic helpers use the existing Activity and CommonCrypto implementations. Android recording encoders are unavailable.
- Android network-interface queries and system-property access use native host information. Screen wide-color support remains disabled consistently with the existing SDR surface.
- Opus's two `mov sp, x18` sites now use the virtual x18 value without touching Apple's reserved register. The executable veneer fixture and original Opus instruction classifier pass.

The authenticated simulator launch loads the original Eleven SDK and reports successful initialization, app ID 488310, and `subscribed=1`. Independent session and Web API ticket probes receive successful result-1 callbacks with validated layouts. The session probe returned 234 bytes. These probes establish genuine Steam ownership and ticket delivery, and do not establish Eleven’s own server authentication. The same launch completed 4,096 game frames without a crash. A continuous run reuses the user-approved saved Steam session. A read-only method observer recorded Eleven’s own `SteamPlatform.Initialize` and `GetUser` calls, and did not observe server authentication callbacks. A stopped scalar inspection counted 6,420 ended OpenXR frames and zero live input hands before the simulator input change. Simulator mouse-trigger delivery was observed, but clicks did not advance the language screen. The user chose to continue setup testing in the headset. Simulator input and capture experiments were removed before the physical build.


The runtime also compiles against the hardware visionOS SDK. The cleaned physical build was signed, verified, and installed on the paired Vision Pro. The APK and extracted game assets were staged successfully. CoreDevice confirmed a physical launch; the app then received a Quit Klepton button action and exited cleanly before guest boot. The user subsequently verified physical gameplay and reported popping/clicking audio. A regression fixture reproduced Unity/OpenSL and FMOD Java streams sharing a cursor and inserting silent blocks. The output mixer registers the default producer separately so concurrent streams mix on one timeline; both registration orders and ring wraps pass. That change did not resolve the audible issue. The user subsequently joined an online match and heard clear voice audio. A physical inspection confirmed FMOD writes 256 signed samples (128 stereo frames) per block. A concurrent-writer fixture reproduced another failure: Unity holds the shared writer lock while waiting for playback, blocking FMOD even when FMOD has room on its own timeline. Each writer now retains its converted PCM and releases the lock while waiting. The fixture covers existing and newly registered concurrent producers, output samples, retained waiting buffers, and ring wraps. The update was installed and launched on the headset. Startup logs show both 256-frame Unity and 128-frame FMOD producers feeding concurrently through more than 6,400 writes with zero reported underruns. Physical listening confirmation is still required. Static inspection confirms that the pinned backend contains Eleven’s User023, Apps009, Utils010, Friends018, Input006, and Timeline004 interface strings; actual SDK creation for User023, Apps009, and Utils010 is verified after sign-in. The remaining interface strings alone do not prove those interfaces have been created.

Run `make elevencheck` for focused PCM, Steam app context, stack veneer, and profile separation checks, and `make steamcheck` for the existing shared Steam/Walkabout regressions. `make assetcheck` covers StreamingAssets path mapping and the existing packaged asset handoff.

## Physical build and testing

The user authorized switching to the paired Vision Pro after simulator clicks did not advance onboarding. The device build uses the existing physical controller input and genuine Steam host; simulator input, tracking-area, capture, and Eleven base-vertex additions have been removed.

```sh
python3 steam/visionos/mksteam.py --target eleven --platform visionos
KLEPTON_TARGET=eleven KLEPTON_STEAM_LOCAL=1 \
KL_STEAM_OFFLINE=0 KL_FRAMES=0 KL_IMMERSIVE=1 KL_EXIT_ON_BACKGROUND=0 \
visionos/run.sh device
```

This installs **Eleven Table Tennis** (`drewwallace.dev.klepton.target.eleven`), stages the APK and extracted game assets, and launches continuous immersive play. Sign in to Steam if prompted; the physical device has a separate session store from the simulator. Complete onboarding, check paddle input and audio, then try Eleven's online menu and a match with another player.

## Acceptance still required

1. Verified offline: sustained simulator frames and a visible Eleven onboarding scene. Setup interaction and gameplay controls remain unverified.
2. Verified: real Steam login, app ID 488310, ownership, and independent diagnostic ticket callbacks with the original SDK.
3. Verified by the user: joined an online match and heard the other player clearly.
4. Verified by the user: physical gameplay and online voice work. Game audio still pops/clicks after the producer-registration fix; investigate the FMOD source PCM separately from Unity voice output.
