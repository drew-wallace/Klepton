# Immersive audio isolation probe

This is a standalone native visionOS app. It links no Klepton runtime, guest,
Compositor Services renderer, controller tracking, or microphone capture. The
default **Untouched audio session** policy never obtains `AVAudioSession`'s
shared instance or activates/configures it. Merely observing notifications does
not activate it. Its empty RealityView isolates the transition into Full Space;
an optional sphere checks whether rendering any content changes the result.

## Build and run

Open `ImmersiveAudioProbe.xcodeproj` in Xcode, select your development team, and
run on Vision Pro. No special entitlements or background modes are required.
The bundle ID is `dev.klepton.immersive-audio-probe`, so this installs beside the
games. CLI builds:

```sh
xcodebuild -project spikes/immersive-audio-probe/ImmersiveAudioProbe.xcodeproj \
  -scheme ImmersiveAudioProbe -destination 'generic/platform=visionOS Simulator' \
  -derivedDataPath /tmp/klepton-immersive-audio-probe CODE_SIGNING_ALLOWED=NO build

xcodebuild -project spikes/immersive-audio-probe/ImmersiveAudioProbe.xcodeproj \
  -scheme ImmersiveAudioProbe -destination 'generic/platform=visionOS' \
  -derivedDataPath /tmp/klepton-immersive-audio-probe-device \
  -allowProvisioningUpdates DEVELOPMENT_TEAM="$KLEPTON_TEAM" build

xcrun devicectl device install app --device "$PROBE_DEVICE" \
  /tmp/klepton-immersive-audio-probe-device/Build/Products/Debug-xros/ImmersiveAudioProbe.app
xcrun devicectl device process launch --device "$PROBE_DEVICE" \
  dev.klepton.immersive-audio-probe
```

Start a Discord call before opening the probe. First keep the audio policy
untouched, rendering off, and test mixed, progressive, and full immersion. Leave
the space between selections. Test the shared-space volume while immersion is
closed. Record incoming voice, outgoing voice, whether the remote participant
still sees a connected call, and recovery after leaving the space or opening
Home. Mark observations with the buttons, then Share log. Logs also persist in
`Documents/probe-events.log` and reset on a fresh process launch:

```sh
xcrun devicectl device copy from --device "$PROBE_DEVICE" \
  --domain-type appDataContainer --domain-identifier dev.klepton.immersive-audio-probe \
  --source Documents/probe-events.log --destination /tmp/immersive-audio-probe-events.log
xcrun devicectl device info processes --device "$PROBE_DEVICE" --search Discord
```

Repeat with one sphere. Then compare ambient, playback with default spatial
behavior, fixed spatial audio, bypassed spatial audio, and releasing the active
session immediately after entry. **Quit and relaunch between audio policies**:
deactivation does not restore the original category/spatial settings. There are
no audio units or players in any policy; explicit session activation isolates
policy side effects without playback or capture. Choosing untouched after a
configured policy is rejected until relaunch. Automatic simulator smoke tests
can pass `SIMCTL_CHILD_KL_PROBE_AUTO_IMMERSION=mixed`, `progressive`, or `full`
to `simctl launch`; these always use an untouched session and empty space.

All immersion styles enter Full Space. The shared-space volume control is the
one that avoids that transition. This probe uses RealityKit, so progressive
immersion requires no custom Metal portal/stencil support. Adding progressive
to Klepton's compositor itself would require that implementation.

A retained Discord PID does not establish that its process is unsuspended,
capture is running, packets are arriving, or audio is routed. Use process/scene
and system audio logs alongside observations. A failure in this untouched empty
probe removes Klepton's own audio/rendering/resource load as a necessary cause;
it still requires system diagnostics to distinguish OS background audio policy,
capture restrictions, interruption, and route behavior.
