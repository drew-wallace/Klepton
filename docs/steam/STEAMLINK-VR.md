# Steam Link VR on visionOS

Steam Link 2.0.24 uses the Qt configuration shell to discover and pair with a
PC, then hands the authorized session to its OpenXR streaming activity.
The shell opens in a window; the immersive space opens when pairing hands off.
The pairing/report window closes after that handoff; `KL_BOOT_WINDOW=1` keeps
it open for diagnostics. Reopening the app's window provides its settings.

Supply the APK locally (the APK and extracted files are ignored by Git):

```sh
ln -sfn "$HOME/ovrport/Steam Link VR/android-steamlinkvr-release.apk" steamlink-vr.apk
apktool d -f -o steamlink-vr steamlink-vr.apk
```

The current local APK reports version 2.0.24, version code 5002389. When changing
APKs, replace both the symlink and the extracted tree, rebuild guest frameworks,
and force staging with `--stage` so the app receives the matching APK and assets.

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
The private controller power layout also matches the supplied 2.0.23 and 2.0.24 APKs.
Battery reporting needs no additional entitlement. The simulator has no real
headset/controller battery readings, so its fallback behavior and streaming
integration can be tested there; measured percentages require physical devices.

## Controller prediction

Steam Link defaults to `KL_SENSE_PREDICT_MS=0`: ARKit updates each controller
sample to the current time, without predicting ahead of now. The host still
receives controller velocity for its own prediction. Other guests retain their
50 ms cap. `KL_SENSE_RAW_ANCHOR=1` explicitly returns the last sensor sample for
comparison; this was the behavior of the earlier zero-cap diagnostic.

On the headset, the original default predicted about 26.5 ms ahead. A 10 ms
cap improved accuracy, but 5 ms still overshot during rapid motion. Raw samples
removed overshoot but visibly trailed. Network lag also increased trailing in
the 5 ms test. Updating to the current time addresses sample age separately
from future prediction; the user reported a clear improvement on the physical
headset. Simulator testing cannot validate this behavior. This change does not
eliminate network delay.

## Decoded-frame backlog

Steam Link defaults to `KL_VTDEC_LATEST=1`: when multiple decoded frames are
waiting, the decoder hands out the newest completed frame and discards older
completed output. All compressed frames are still decoded, preserving reference
state, and the returned image keeps its original presentation timestamp. Other
guests retain FIFO. `KL_VTDEC_LATEST=0` restores FIFO for comparison.

The decoder reports queue high-water and stale-output drops. This policy targets
catch-up delay after a decoder/consumer stall; it does not reduce network transit
time. The HEVC regression models an eight-frame backlog, checks recovery to the
newest timestamp, and verifies subsequent dependent frames still decode.
The 2.0.24 simulator stream renders SteamVR's library at 1152x4608 with this
policy, and its log confirms a stale decoded frame was discarded. Native
video-texture checks pass. The effect on physical headset latency is unmeasured.

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

Steam Link VR 2.0.24 (5002389) builds and starts in a dedicated visionOS 27
simulator, reuses the existing pairing with the LAN PC, enters the OpenXR
activity, receives HEVC at 1152x4608, and renders SteamVR's library. The
32-slot eye cache holds the client's separate-eye swapchains without repeated
evictions; the previous 16-slot cache evicted streaming images every frame.
The 2.0.23 simulator audio stream was exercised at a 60 ms target with zero
reported underruns. The JNI/libc/entitlement checks, video decoder/texture
checks, discovery and XR input gates pass. The 2.0.24 simulator and signed
device app builds succeed; the signed device app has Increased Memory Limit
enabled and both optional streaming capabilities disabled.

The 2.0.23 build was signed with a Personal Team and installed on a physical
Vision Pro with Extended Virtual Addressing and Low-Latency Streaming disabled.
It entered immersive streaming, decoded video, and reported real headset and
Sense controller battery levels. Rapid controller motion was reported rough;
prediction timing and dropped stream frames need further investigation. Eye
input, microphone behavior, measured wireless latency, and provisioning for the
two disabled capabilities remain unverified.
