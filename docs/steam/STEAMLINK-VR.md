# Steam Link VR on visionOS

Steam Link 2.0.23 uses the Qt configuration shell to discover and pair with a
PC, then hands the authorized session to its OpenXR streaming activity.
The shell opens in a window; the immersive space opens when pairing hands off.
The pairing/report window closes after that handoff; `KL_BOOT_WINDOW=1` keeps
it open for diagnostics. Reopening the app's window provides its settings.

Supply the APK locally (the APK and extracted files are ignored by Git):

```sh
ln -s "$HOME/ovrport/Steam Link VR/steamlinkvr-2.0.23.apk" steamlink-vr.apk
apktool d -f -o steamlink-vr steamlink-vr.apk
```

Build and run on a booted visionOS Simulator:

```sh
KLEPTON_SIMULATOR=<simulator-udid> \
  ./steam/steamlink/build_run_slink_vpro.sh --sim --host <pc-ip>
```

`KLEPTON_SIMULATOR` selects the same device for installation, staging, and
launching. Unset, the scripts use the first booted visionOS simulator. The
wrapper stages assets on first use; `--stage` forces another upload. Set
`KL_AUDIO=0` for UI diagnostics if the simulator has no working audio device.
`KL_VIEW_POKE='0.5,0.58@12'` drives the same guest mouse path as a real tap;
coordinates are fractions of the shell surface and times are seconds after
the window surface appears. These are diagnostics, not normal launch defaults.
The audio fill target is 60 ms; `KL_AUDIO_LATENCY_MS=<milliseconds>` overrides
it for measured network jitter. This is a buffer target, not an end-to-end
latency measurement.

Steam and SteamVR must run on the PC. Select the discovered PC and start
pairing, then enter the displayed PIN in Steam on the PC. Test discovery,
pairing, and the handoff in the simulator before moving to a headset.

## Signing for wireless streaming

The Steam Link target keeps the three authored capabilities: Increased Memory
Limit, Extended Virtual Addressing, and Low-Latency Streaming. Other targets
retain the guardian branch's Increased Memory Limit-only default.

Apple describes [Low-Latency Streaming](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.low-latency-streaming)
as permission to use low-latency wireless networking for streamed game content
on visionOS. Activation depends on available system resources. This is distinct
from the Foveated Streaming Session entitlement and does not change Steam's
protocol or the app's VideoToolbox/ANGLE decoder path. Extended Virtual
Addressing increases address-space capacity; it is not a network latency control.

Device signing needs a team and provisioning profile supporting these keys.
The local Personal Team previously refused both optional capabilities. Adding
keys to a plist cannot grant them to that team. Use `KLEPTON_TEAM=<team-id>`
with a supporting team, or explicitly disable them for a diagnostic build:

```sh
KLEPTON_EXTENDED_VA=0 KLEPTON_LOW_LATENCY=0 \
  ./steam/steamlink/build_run_slink_vpro.sh --host <pc-ip>
```

That diagnostic build cannot demonstrate Apple's low-latency wireless mode.
The simulator runs without provisioning and cannot measure headset wireless
latency, real tracking/controller input, or device hardware video decoding.

## Eye-tracked streaming

**Vision Pro eye input is not currently passed to this APK.** The APK's
`assets/config/hmd_config.json` declares `XR_EXT_eye_gaze_interaction` and binds
`/user/eyes_ext/input/gaze_ext/pose`. The actual simulator session reports
`[XRQ] Eye gaze interaction supported: No` and
`[XRQ] FB Eye tracking Social supported: No`. Klepton implements neither eye
tracking extension. Its head/controller poses do not supply eye gaze.

Compositor Services' device foveation and the sampled rasterization-rate curve
in `captureDisplayFoveation` optimize local rendering. They do not send a
moving eye focus to the Steam host. Receiving and compositing the client's
multiple resolution layers also does not prove eye-tracked streaming.

The installed visionOS 27 SDK provides a supported source of approximate eye
input for custom streaming protocols:
[`FoveatedStreamingProviderContext.latestFocusRegion`](https://developer.apple.com/documentation/foveatedstreaming/foveatedstreamingprovidercontext/latestfocusregion).
The region contains a direction relative to the device pose, distance, and
timestamp. It is available inside a
[`FoveatedStreamingExtension`](https://developer.apple.com/documentation/foveatedstreaming/foveatedstreamingextension),
not on the app's ordinary ARKit/Compositor Services session. This requires
[`com.apple.developer.foveated-streaming-provider`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.foveated-streaming-provider),
which Apple explicitly requires developers to request approval for. A paid
team or the two restored capabilities alone cannot grant that approval.

The integration therefore needs a visionOS 27 streaming-provider extension
running the Steam client/runtime, a host app initiating a Foveated Streaming
session with that provider, and a bridge from genuine focus-region samples to
the APK's OpenXR gaze action. The bridge must convert device-relative direction
to the requested OpenXR space, preserve sample timing, and return inactive /
invalid tracking when a sample is absent or stale. It must not substitute head
forward, controller aim, or simulated gaze and report it as eye tracking.
Apple's built-in CloudXR streaming provider cannot be substituted for Steam's
protocol by adding an entitlement.

Before claiming this works, test on a provisioned physical headset: keep the
head still and move the eyes, verify the APK's gaze action changes and its
streaming focus follows, then verify invalid/stale input stops being reported
as tracked. Do not log eye coordinates in normal diagnostics. Simulator
streaming success cannot establish any of these conditions. Provider approval,
the provider/client migration, and that physical acceptance test are pending;
the eye-tracking extensions remain unadvertised.

## Battery telemetry

This branch includes upstream
[PR #5](https://github.com/shinyquagsire23/Klepton/pull/5): headset battery level
and charging state from `UIDevice`, plus independent PSVR2 Sense readings from
`GCController`. The runtime shares those readings with Android BatteryManager,
OVRPlugin, OpenXR battery queries, and Steam Link's controller power properties.
The private controller power layout also matches the supplied 2.0.23 APK.
Battery reporting needs no additional entitlement. The simulator has no real
headset/controller battery readings, so its fallback behavior and streaming
integration can be tested there; measured percentages require physical devices.

## Regression checks

```sh
make slinkcheck                        # JNI startup and text input; signing policy
make hevc vidtex xrinput bcast         # video decode/textures, XR input, discovery
make xros                             # compile/link both visionOS platforms
```

`vidtex` needs the vendored patched ANGLE build. The JNI regression exercises
the newer SDL back-button method, USB tethering request, both keyboard method
signatures, UTF-8 text delivery, and backspace without an APK or Steam host.
The network regression checks Linux IPv6 address parsing and formatting,
including curl's DNS-name probe that controls TLS Server Name Indication.
It also verifies worker-thread name queries and recoverable Linux realtime
scheduling requests. Realtime receive threads get best-effort Darwin QoS;
Linux FIFO/RR scheduling remains unavailable.

## Validation status (2026-10-04)

Steam Link VR 2.0.23.2363 discovers and pairs with a LAN PC in a dedicated
visionOS 27 simulator. The authorized session enters the OpenXR activity,
receives HEVC at 1152x4608, and renders SteamVR's library with audio. The
32-slot eye cache holds the client's separate-eye swapchains without repeated
evictions; the previous 16-slot cache evicted streaming images every frame.
The simulator audio stream has been exercised at a 60 ms target with zero
reported underruns. The JNI/libc/entitlement checks, video decoder/texture
checks, discovery and XR input gates pass, and both visionOS runtime slices
and the unsigned full device app build.

Physical headset tracking, controllers, microphone, native video texture
interop, wireless latency, and low-latency entitlement provisioning remain
unverified. Xcode currently reports no signed-in account and no provisioning
profile for this app. Configure a supporting team before device installation.
