# Discord audio during immersion

Investigation on October 8, 2026, from `steam-game-versions` (`a8c4e7f`). Changes
are on `immersive-audio-fix`. Assume the calling app is behaving correctly;
investigate Klepton and the OS resource/lifecycle boundary.

## Current conclusion and limits

The strongest existing evidence points at the transition into visionOS Full
Space, rather than missing mixing options. The base branch records a physical
reproduction on visionOS 27.0.1 with Discord 347.0 (112799) in a separate native
RealityKit app with no explicit audio code, in both mixed and full immersion.
Those are prior recorded observations, not fresh results from this investigation.
Ambient audio, earlier commits, spatial-audio changes, and Klepton's Home audio
handoff were previously tried without a reliable cure. Safari music continued
mixing. This makes Klepton's audio/rendering unnecessary to the recorded failure;
it does not identify the exact OS mechanism or establish a working workaround.

Apple documents that opening **any** ImmersiveSpace puts the app in Full Space
and backgrounds other apps. Mixed, progressive, and full describe replacement
of the real-world view; they do not preserve the Shared Space's multi-app
foreground execution. The actual comparison is a window/volume in Shared Space
versus an ImmersiveSpace in Full Space.
Sources: [SwiftUI spaces](https://developer.apple.com/videos/play/wwdc2023/10111/),
[Metal game spaces](https://developer.apple.com/videos/play/meet-with-apple/245/).

Background audio and VoIP are separate execution modes. Audio mixing permits
coexisting audio sessions; it does not grant another process background execution
or microphone access. Klepton cannot add capabilities, keepalive assertions, or
CallKit integration to another app. Adding `UIBackgroundModes=audio` or `voip`
to Klepton would apply only to Klepton and conflicts with the intended behavior
of stopping a backgrounded game's audio.
Source: [Background execution modes](https://developer.apple.com/documentation/xcode/configuring-background-execution-modes).

The physical headset was reached and confirmed as visionOS 27.0.1 (24M372).
Discord's main PID remained 390 across passive checks. No timed live call failure
was observed in this run, so that PID is not a before/after failure comparison.
Seven accessible jetsam reports, dated September 23 and September 28, were read:
none records Discord as a killed process. Two show it suspended; four show it
active/frontmost; one has no Discord entry. These old reports do not exclude a
new memory kill or prove why a current call stops. Device sysdiagnose collection
failed with CoreDeviceCLISupport.DiagnoseError and yielded an empty partial
archive. Unified log collection requires root; passwordless sudo was unavailable.
The Console UI fallback was also inaccessible because the Mac was locked.
No diagnostic data was sent to Apple. Raw jetsam reports and device diagnostic
bundles stay in `/tmp`, outside Git. Fresh, timestamped call observations and system audio/lifecycle logs remain
required before declaring the OS the exclusive cause.

### Recovered historical system trace

A separate October 3–4 reproduction bundle was recovered from
`/tmp/klepton-discord-immersion-repro.zip`. Its archived native probe source was
inspected: SwiftUI, RealityKit, and Foundation only, with a single unlit sphere;
no explicit audio session, output/capture unit, controller, guest, or ARKit
provider. Its event log confirms mixed/full open and close requests; the archived
tester report supplies the audibility results. That report also describes a
separate fully silent Klepton control and an empty Compositor Services control
that failed before guest/tracking startup. These are historical reported controls,
not new device measurements or results of the current `KL_AUDIO=0` implementation.

The bundle also contains five raw `audiomxd` events from an older Klepton entry,
now preserved in [the evidence file](evidence/discord-audio-system-events-2026-10-03.txt):

| October 3 local time (EDT) | Direct system evidence for the same Discord primary session |
| --- | --- |
| 21:50:41.278472 | AudioToolboxServerHandleInterruption: `Stop Now` |
| 21:50:41.278585 | Original status: `interrupted` |
| 21:50:41.282557 | Updated status: `non-resumable.PlayAndRecord_WithBluetooth` |
| 21:50:41.282636 | `PlayAndRecord_WithBluetooth/VideoChat`, **Mixable**, going inactive; NowPlayingApp: NO |
| 21:50:41.832676 | **BackgroundTaskSuspended**, **Background entitlement: YES** |

This directly establishes an audio-session stop/deactivation followed by task
suspension in that captured run. It supplies a stronger explanation of the cut
than attenuation alone or a guessed memory kill. The trace already labels the
caller's session mixable and entitled for background execution; missing mixing
or a missing background entitlement is not established by these events.
The archived report places Klepton's open at 21:50:40.327 and compositor audio
resume at 21:50:41.367, after the caller's deactivation. Those two boundary times
come from the report; their original full log was not recovered in this pass.

The raw excerpt omits the initiating reason and the OS policy decision. It does
not prove whether suspension caused the stop or the loss of the active audio
assertion allowed suspension, nor whether the rule is intentional or a defect.
It has no outgoing-microphone result. Apple exposes
[`sceneWasBackgrounded`](https://developer.apple.com/documentation/avfaudio/avaudiosession/interruptionreason)
as an interruption reason, which is consistent with this sequence; that exact
reason was not captured here. Do not attribute these system events to the native
probe's later run. Fresh controls are still needed to confirm a reliable host-side
workaround or show that all candidate workarounds fail.

## Avenues audited

| Avenue | Evidence and action | What remains to distinguish it |
| --- | --- | --- |
| Exclusive session category | All authored session configuration is centralized in KleptonAudio. Both playback and play-and-record use `mixWithOthers`, including recovery. No `soloAmbient`, `multiRoute`, `duckOthers`, or `interruptSpokenAudioAndMixWithOthers` path was found. | Compare the untouched probe and explicit mixable session variants on hardware. |
| Implicit media session activation | LoadingVideoPlayer is the one native AVPlayer. `isMuted` silences samples but does not disconnect AVPlayer from session activation/recovery. Now disconnected before playback on visionOS 27; excluded from `KL_AUDIO=0` on all supported OS versions. | Relevant only to guests presenting this loading video; cannot explain the prior empty native-app reproduction. |
| Spatial audio exclusivity | Normal game policy is fixed spatial audio; `KL_AUDIO_SPATIAL=1` leaves system defaults. SDK documentation describes bypassed audio as mixing directly into output, not claiming exclusive hardware. Probe compares default, fixed, and bypassed spatial behavior. | Physical comparison; spatialization can change apparent loudness/location without process suspension. |
| Now Playing replacement | Klepton already opts out of Now Playing candidacy. Candidacy is separate from category mixing. No MediaPlayer remote-command/Now Playing metadata path was found. | Check other audio hints and actual caller lifecycle, rather than assuming candidacy grants background rights. |
| System attention ducking | Apple's audio guidance says a non-Now-Playing app's audio can duck when attention moves to another app. This is separate from VPIO ducking and may sound like loss of audio. The recovered failing trace explicitly goes inactive and suspended, so attention attenuation alone cannot account for that run. | Record partial volume reduction separately from silence and verify session activation in the same timed test. |
| Route/sample-rate renegotiation | C measures device rate and resamples. Previously Swift requested 48 kHz, 10 ms, and stereo if no other audio was reported at that instant. A newly backgrounded or silent call can disappear from that hint. Playback-only now never requests those hardware preferences. Capture preserves them when other audio is reported. | Compare speakers and AirPods; inspect route changes, not only sample rate. |
| Microphone contention | Mic is opt-in and defaults off, but the toggle is persisted and `KL_MIC` can override it. Playback-only creates no capture unit. `defaultToSpeaker`/Bluetooth HFP options are used only when mic is on. Mixing does not guarantee concurrent capture. | Force `KL_MIC=0` for the baseline and record incoming and outgoing audio separately. |
| Voice-processing ducking | Native VoiceProcessingIO ducks other audio independently of the session's options, including use as a capture unit. It had default ducking. Now requests minimum ducking with advanced ducking disabled. This API offers no zero-ducking level. | `KL_MIC_RAW=1` chooses plain RemoteIO to isolate voice processing from capture/route contention. Raw capture sacrifices noise suppression and gain. |
| Reclaiming audio during interruption | Route/style recovery and the C watchdog already defer during interruptions. Ended notifications previously resumed unconditionally, and failed recovery cleared Swift's hold early. Now explicit absence of `shouldResume` retains the hold, and failed permitted recovery retains it. Malformed notifications are ignored. | A real foreground return still deliberately restores the game with mixing. Record actual OS interruption reason/options. |
| Failed startup activation | Startup previously announced an active/ready session even after activation threw, leaving the native watchdog free to attempt audio I/O. It now reports readiness only after successful setup and holds native recovery after failure. | Covered by a fresh-process failed-startup regression test; this is a separate recovery safeguard, not the cause established by the historical independent repro. |
| Background/recreated scene | App phase is observed at App scope; closing a window does not by itself background the immersive renderer. Home, renderer pause/invalidation, and Quit stop output/capture before deactivating. `notifyOthersOnDeactivation` applies only to releasing our session. | Compare closing settings, Digital Crown/Home, leaving immersion, and reopening. The known window-close recovery is retained. |
| Mixed/full/progressive | System guardian normally uses full; custom guardians and passthrough warnings use mixed. Both are Full Space. Progressive still backgrounds other apps and needs custom Metal portal/stencil rendering to work correctly in Klepton. | The RealityKit probe supports all three without altering guardian safety or the production renderer. |
| Shared Space and system Environments | A normal or volumetric window avoids app Full Space. Apple's system Environments are another useful control, distinct from opening an app ImmersiveSpace. | Compare a call while only a volume is open, and while turning up a system Environment. A shared-space stereo presentation would be a rendering feature, not a category flag. |
| Memory pressure/jetsam | Game eye textures, drawables, guest memory, and increased-memory-limit capability can increase overall pressure. Existing `[hb]` logs report our footprint/headroom and pressure notifications. The prior lightweight native repro weakens game allocation as a necessary cause; current historical jetsam reports do not identify Discord as a victim. | A new jetsam victim/reason at the failure timestamp is needed. Our headroom alone cannot measure another process's survival. |
| CPU/GPU/thermal starvation | Guest and feeder threads use high priorities; graphics can consume substantial GPU resources. No other-process priority or termination manipulation was found. Empty native immersion avoids this Klepton workload. | Test empty versus one sphere versus a game, and lower actual eye/render allocations if failure requires game load. Lowering running quality alone need not shrink allocated drawables. |
| Network/background scheduling | Steam Link adds network traffic; ordinary games need not. No shared VPN, proxy, route reconfiguration, or native guest audio-focus control was found. Android AudioManager setters/Bluetooth SCO shims are no-ops, not native system calls. | Remote participant connection state distinguishes lost packets/capture from local output mute; empty immersion removes game streaming bandwidth. |
| Screen recording, mirroring, system UI | These can introduce additional route, output, thermal, or interruption behavior. The failure needs a baseline with them absent. | Reintroduce one at a time, including Control Center/Home; keep route and interruption timestamps. |
| OS revision and install context | The headset's verified version matches the prior report. Simulator 27.0 validates the probe's scenes but cannot validate physical call coexistence. Developer launches may keep a process runnable that would otherwise suspend. | Compare a cold Home launch with debugger detached, the same headset/route on another available stable OS, and active-call background survival in another immersive app. |

Hardware preference changes can stop/restart I/O when applied to active sessions:
[Apple QA1631](https://developer.apple.com/library/archive/qa/qa1631/_index.html).
Voice-processing ducking is documented independently of session mixing:
[Voice processing](https://developer.apple.com/videos/play/wwdc2023/10235/).
The new AVPlayer disconnect API explicitly avoids session activation and route
reconfiguration: [AVPlayer system audio](https://developer.apple.com/documentation/avfoundation/avplayer/setdisconnectedfromsystemaudio(_:completionhandler:)).
Interruption policy: [Handling interruptions](https://developer.apple.com/documentation/avfaudio/handling-audio-interruptions).
Memory evidence: [Jetsam reports](https://developer.apple.com/documentation/xcode/identifying-high-memory-use-with-jetsam-event-reports).
Attention-dependent audio behavior:
[Playing audio](https://developer.apple.com/design/human-interface-guidelines/playing-audio).

## Remaining controlled device tests

The separate **Immersive Audio Probe** was built, signed, and installed on the
headset. Its remote launch could not be verified: the device connection was
invalidated/disconnected, the retry timed out, and its event file was not retrieved.
[Probe instructions](../../spikes/immersive-audio-probe/README.md)
describe its untouched-session, empty-space, volume, spatial/category, and
deactivation controls. No audio/microphone units run in the probe. Relaunch between
audio policies to avoid carrying category/spatial preferences forward.

Use one active call, with another person speaking continuously and checking the
outgoing microphone. Note the exact time, route, style, and whether the call stays
connected. Avoid recording/mirroring for the first pass.

1. Untouched session, empty space: Shared Space volume, mixed, progressive, full.
   If the call stops only in ImmersiveSpace, the OS scene/background transition is
   isolated without Klepton audio, capture, tracking, guest, or compositor.
2. Repeat with one sphere. A difference indicates scene/content workload rather
   than the mere open request; it does not yet prove jetsam.
3. Relaunch for ambient/mixing, playback/default spatial, playback/fixed spatial,
   playback/bypassed spatial, and release-after-entry controls. Observe recovery
   after release, leaving the space, and Home. A successful policy is a candidate
   workaround that must then be repeated in Klepton.
4. Cold-launch the updated game with `KL_AUDIO=0 KL_MIC=0
   KL_AUDIO_DIAGNOSTICS=1`. This now disables Swift session activation/permission,
   C output/capture, and loading video. Compare `KL_IMMERSIVE=0` only for guests
   that can actually render in a window; disabling immersion is not a windowed
   VR implementation.
5. Use normal game audio with `KL_MIC=0`; then optional mic capture, then
   `KL_MIC_RAW=1` capture. Isolate voice-processing attenuation from losing a
   shared input route. Repeat relevant cases on speakers and AirPods.
6. Capture Discord's PID before/after each transition, the probe events, system
   audio logs, RunningBoard lifecycle/assertion changes, and any fresh jetsam
   report. An unchanged PID is consistent with suspension; a killed PID requires
   a termination reason. Incoming audio only failing suggests routing/mixing;
   outgoing only failing suggests input ownership/background capture; both
   stopping with lost call connectivity suggests scheduling/network suspension.

Collect system logs in Console with the connected device selected, or use an
authorized root shell for `log collect --device-udid "$PROBE_DEVICE" --last 10m
--output /tmp/immersive-audio.logarchive`. Do not filter the initial archive so
system process naming cannot hide the relevant audio service; inspect copies
for Discord, the probe/game bundle, interruption/background events, and resource
termination. A sysdiagnose at a timed reproduction is the appropriate next OS
artifact if unified logs do not identify the mechanism.

No workaround is called confirmed until the call is heard in both directions
through repeated entry/exit and Home/Resume on the physical headset.

## Validation performed

- `python3 tests/test_visionos_audio.py`: real C watchdog/interruption and device
  gates, Swift policy recovery/failure, explicit denied resumption, malformed
  notifications, silent-session variants, route-preference preservation, and
  failed-startup holds without falsely announcing native session readiness.
- `python3 tests/test_visionos_session.py`: frame-clock suspend/resume behavior.
- `make swiftcheck`: all production Swift against the visionOS SDK/C bridge.
- Native C audio syntax check against the visionOS SDK, deployment target 26.
- Probe builds for simulator and signed device; simulator mixed, progressive,
  and full each log `open result=opened` with an untouched session and empty scene.
- Verified hardware install, passive process checks, and historical jetsam
  inspection. Remote launch/log retrieval failed as the connection disconnected.
  No fresh physical call outcome has yet been supplied.
