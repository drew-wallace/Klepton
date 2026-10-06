# Steam Link VR capability restoration — 2026-10-05

The paid-program build enables Extended Virtual Addressing and Low-Latency
Streaming alongside Increased Memory Limit. All three Boolean entitlements
were verified in both the signed app and its embedded provisioning profile.
The profile uses team `DS9FZL757P`, permits the physical Vision Pro, and expires
on 2027-10-05. Deep signature verification passed.

Steam Link's authored entitlements already contain these keys. Generate the
project with `KLEPTON_TARGET=steamlink-vr KLEPTON_EXTENDED_VA=1
KLEPTON_LOW_LATENCY=1`; the resulting project uses `Klepton.entitlements`
instead of the prior filtered file. Automatic provisioning now succeeds with
the same team ID after its paid membership activation.

Both runtime platform slices compile, Swift typechecking and Steam Link
regressions pass, and the signed device build succeeds. The app was installed
on the physical headset with the matching 2.0.24 APK, assets, Qt plugins, and
metadata staged after installation. The build includes existing local settings
UI changes and retains the tracking code restored after the direct-query
experiment was reverted; that experiment's tracker-query path is absent from
the signed executable. Current-time Sense resampling, corrected velocity
coordinates, and newest decoded-frame output remain.

Artifact: `visionos/build/dd-device-steamlink-vr-paid-capabilities/Build/Products/Debug-xros/KleptonSteamLink.app`.

This verifies authorization to use the capabilities, not measured wireless
latency. [Apple states that low-latency wireless mode activates when system
resources allow](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.low-latency-streaming).
Physical streaming behavior, controller latency/overshoot, and sustained
stability still require a headset test against Carrara (`192.168.147.10`).
This does not add eye-tracking input or the separate Foveated Streaming
Provider entitlement.

## Audio and eye-focus follow-up

The first paid-capability headset session contains four Steam Link audio jitter
buffer underflows and three settings-window closures followed by native audio
restarts. These are separate sources of gaps: the native restart discards queued
PCM even if playback was healthy, while a Steam jitter-buffer underflow happens
before PCM reaches Klepton.

`kl_audio_resume` now preserves an existing output unit with a recent render
callback when neither Home nor an OS interruption held it. Actual hold recovery,
a missing unit, or a callback idle for over 500 ms still rebuilds output. The
independent watchdog repairs a callback that stops after the resume check. This
keeps window-close session activation without forcing a gap in healthy sound.
Native recovery tests cover preserved queued PCM, stale bookkeeping, dead or
missing output, Home, and interruptions. Swift session/mixing lifecycle tests
pass, and both visionOS platform builds succeed. Wireless audio improvement
still requires listening on the headset; this does not repair upstream packet
loss or change Steam Link's jitter-buffer algorithm.

Device artifact: `visionos/build/dd-device-steamlink-vr-audio-resume/Build/Products/Debug-xros/KleptonSteamLink.app`.
All three existing capabilities remain verified in its signature and profile.
It was installed with matching APK/assets staged afterward, and device launch
succeeded (PID 627). The dedicated simulator reached the SteamVR library and
published streamed video, with no Klepton fatal/fault marker during the short
check. Two upstream Steam audio underflows still occurred. The simulator was
terminated after the check to release Carrara for physical testing.

Dynamic eye-directed Steam streaming is still unimplemented. The APK requests
`XR_EXT_eye_gaze_interaction`, but ordinary app ARKit/Compositor Services APIs
do not expose the gaze input that would satisfy it. The visionOS 27 custom
streaming-provider API exposes an approximate focus region inside a provider
extension; [Apple requires separate provider approval](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.foveated-streaming-provider).
Approval status for team `DS9FZL757P` has been requested from the user and is not
yet confirmed. The [request form](https://developer.apple.com/contact/request/foveated-streaming-provider/)
is separate from joining the paid developer program. Adding the entitlement to
the current ordinary app would not itself supply gaze. A provider extension,
session/rendering integration, and a tested OpenXR gaze bridge are required;
the current build does not advertise eye-tracking support.
