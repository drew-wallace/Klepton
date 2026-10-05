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
