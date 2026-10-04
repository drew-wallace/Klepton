# The visionOS host app

The Klepton app itself: Swift for the platform layer (P4).
`spikes/device-probe` stays what it is: a small, disposable thing
for answering device questions. This is the app those answers were for.

```bash
./run.sh              # the booted visionOS Simulator — rung 2, tests dyld
./run.sh device       # a physical Vision Pro         — rung 3, tests AMFI

KLEPTON_TARGET=superhot ./run.sh device      # ...for another guest
../build_run_vpro.sh superhot                # the same, wrapped: builds, runs,
                                             # and reads the log back
```

Both do the same five steps: build the runtime (`make xros`), translate the
guest libraries (`mkguest.sh`), regenerate the project, install, stage
assets, launch. Each builds only the slice it will install — `XROS_PLATFORMS`,
exported by `run.sh` and read by both — because a device build never loads the
simulator's runtime and each slice is a full compile of it. `make xros` on its
own still builds the pair, which is the gate. `KL_SKIP_STAGE=1` skips the upload when the assets are already
there, which on device is the difference between a 20-second loop and a
20-minute one.

## Which guest — one table, five consumers

`targets.py` is the authority: tree, APK, asset directory, entry library, boot
sequence, bundle id, display name and **product name**. `run.sh`, `mkguest.sh`,
`stage_assets.sh` and `gen_xcodeproj.py` all read it, and `make targets`
generates `runtime/kl_target_table.h` from it so the C runtime — the app's
`Sources/kl_app.c` and the host's `build/m_boot` — reads the same row.

That last one is not tidiness. Two drivers describing one guest differently is a
class of bug with no error surface at all: the build succeeds, the app installs,
and the guest is told the wrong thing about itself. The failure is silent in
both directions — one title's APK opened as another's zip, or a title writing
its saves into another's directory.

**A bundle id separates the installed apps and nothing else about a build**, so
the target also carries the PRODUCT name, and with it the `.xcodeproj`, the
`.app`, the derived-data directory and the `Frameworks/<target>/` the
translations are staged into. ANGLE is deliberately not per-target: it is the
same renderer for every guest and `mkangle.sh` writes it once, into
`Frameworks/` itself.

The id itself is **derived, not stored**: `$USER.dev.klepton.target.<target>`.
Two halves, for two different collisions. The `<target>` half separates the apps
on one device. **The `$USER` half separates DEVELOPERS**, and that one is not
cosmetic: an App ID can be registered to exactly one team, automatic signing
registers it on the first build, and so whoever builds this tree first silently
takes the id away from everyone else — the next person's build fails with
`Failed Registering Bundle Identifier: … cannot be registered to your
development team`, which reads like a broken project rather than a name already
spoken for, and it takes the two memory entitlements with it because those need
an explicit App ID.

The current Personal Team provisions Increased Memory Limit, but Xcode rejects
Extended Virtual Addressing and Low-Latency Streaming for that team. The project
generator therefore keeps the increased-memory entitlement and filters the two
optional keys by default. Use `KLEPTON_EXTENDED_VA=1` and/or
`KLEPTON_LOW_LATENCY=1` only with a supporting team/profile. The latter applies to
Foveated Streaming and is not needed for standalone games. The authored plist
retains all three keys; there is no need to delete capabilities by hand.

On a simulator whose camera starts at floor height, `KL_SIM_HEAD_HEIGHT=1.6`
lifts the sampled/composited head for testing floor-origin games. It has no
effect in a headset build. Boot and crash logs retain one prior run in
`klepton-boot.log.previous` and `klepton-crash.log.previous`.

`KLEPTON_BUNDLE_SCOPE` overrides the `$USER` part (an unset or empty one leaves
the id unscoped rather than emitting a leading dot); `KLEPTON_BUNDLE_ID`
overrides the whole id. **Changing the id orphans the container** — the assets,
the OBB and the guest's saves live in the old app's Documents, and the new id is
a new app with an empty one. That is not silent: the staging stamp is keyed on
the bundle id, so the first build after a change re-stages by itself. It is
still a 2.2 GB upload, and the old app is still installed until you delete it.

`KLEPTON_TARGET` selects it at BUILD time; the chosen name is compiled into the
app as `KL_TARGET_DEFAULT`, because an app launched by hand from the Home View
has no environment at all. `KL_TARGET` overrides that at run time — but the two
apps embed *different* guest frameworks, so pointing one at the other's target
stops at `kl_app_configure` with "missing guest libraries" rather than
misbehaving.

## What lives where, and why

|  | where | why |
|---|---|---|
| C runtime | `build/Klepton.xcframework`, linked **statically** | `make xros` builds both slices; `run.sh` passes `XROS_PLATFORMS=xros` or `=xrsim` so a run builds only the one it will install, which halves it. Static, so it is inside the app binary — no load-time cost, and nothing for dyld to resolve. |
| guest libraries | `Frameworks/<name>.xcframework`, **embedded, not linked** | klepton-ld output. Embedded so Xcode code-signs them; a loose Mach-O elsewhere in the bundle is only *sealed* by the outer signature, which is not the same thing and is not what AMFI accepted in P3. Not *linked*, because nothing references their symbols — the runtime `dlopen`s them by path, and linking would make dyld want an exports trie klepton-ld deliberately does not emit. |
| APK assets (2.2 GB) | the app's **Documents container**, staged once | They change only when the APK does; the code changes every few minutes. Bundling them would put a multi-minute upload in front of every build. |

The asymmetry is deliberate: **code in the bundle, data in the container.** P3
established that AMFI accepts a `klepton-ld` dylib inside a bundle we signed,
and established nothing at all about one pushed into Documents afterwards.
Assets carry no code, so they have no such constraint.

## Returning from Home

Walkabout builds generated with `KLEPTON_STEAM_LOCAL=0` include no Steam backend
and default to offline diagnostics on every launch, including Home launches
after Quit. The window labels this mode. `KLEPTON_STEAM_LOCAL=1` keeps real Steam
authentication as its default. An explicit `KL_STEAM_OFFLINE` launch override
still takes precedence. Simulator camera height defaults to a standing 1.6 m;
`KL_SIM_HEAD_HEIGHT` overrides it without changing physical-headset tracking.

Home retains the current guest session by default. The immersive display can
be destroyed by visionOS, so Klepton parks guest frame production and attaches
a fresh display when the app is reopened. It keeps the guest's textures and
GPU fence, restores tracking and audio, and does not run boot again. If the
system cancels opening the immersive space, use **Resume session** in the app
window. `KL_EXIT_ON_BACKGROUND=1` restores cold launches for diagnostics.
The Klepton window stays open on cold boot and after re-entry so settings and
Quit remain accessible beside the game. `KL_BOOT_WINDOW=0` explicitly closes
it after immersion opens for diagnostics; the default is `1`.

The **Guardian** panel offers three modes. **Default visionOS area** uses full
immersion and the system's standard movement boundary (approximately 1.5 m from
the initial head position). **Draw on the ground** uses mixed immersion: allow
World Sensing, look at the detected floor, pinch and trace the perimeter, then
choose **Finish boundary**. Multiple strokes connect in order; Finish closes
the outline. Crossed outlines and areas smaller than 1 m² are rejected. **Mapped
environment** outlines connected detected floor, excludes reconstructed walls
and furniture with a 5 cm margin, and requires **Use mapped area** after review.
Floor coverage combines the current room envelope, detected floor-plane polygons,
and classified floor reconstruction. Mapping uses the measured physical floor
height; when no floor plane is available, it weights floor height by triangle
area rather than triangle count so a densely scanned local patch cannot dominate.
Classified floor faces never enter the obstacle pass. Plane polygons are used
instead of their rectangular extents. Local `[guardian] auto map` diagnostics
report source face counts and covered, clear, and connected area every five seconds.
Surface distances are sampled at 5 cm spacing, interpolated into closed contours,
and simplified within 7.5 mm. Detected wall planes supply straight wall faces;
the raw mesh supplies furniture contours and obstacle holes. The drawn outline
also drives proximity checks. There is no additional floor-edge erosion.
Unmapped floor remains outside the area. Mapping is an estimate and can miss
objects, so check the outline and leave space for arms and controllers.

The **Passthrough starts** slider sets the warning distance from 15–80 cm;
the new default is 30 cm. Custom modes smoothly reveal passthrough starting at
that distance and show the room fully by 5 cm; outside, during setup, or on tracking loss, the game is
transparent. Guest controller input is suppressed during setup. The final Metal
pass applies to projection layers, skyboxes, and guest overlays alike. The
nearest physical headset, controller body/aim point, or tracked hand decides the
fade; controller/hand checks include a 5 cm margin. Guardian coordinates stay in
the physical ARKit world even if the game is recentered. Missing positional
tracking for a connected controller keeps the room visible.

The default mode retains visionOS's headset boundary. Controller proximity is
checked against a 1.5 m radius around the initial physical head position. Its
warning temporarily switches to mixed immersion so alpha can reveal passthrough,
then returns to full once the headset and controllers are clear, with a 5 cm
hysteresis band. This controller warning uses the distance slider; Apple's own
headset warning is controlled by visionOS. The supplemental area keeps its entry
position across style changes.

Accepted drawings and automatic outlines are saved locally relative to persistent ARKit `WorldAnchor`s,
with a separate record for each boundary. Points are encoded against the actual
tracked transform from `anchorUpdates`, rather than assuming the requested transform was
accepted unchanged. Applying a drawing retains its detected floor height; a
restored boundary is aligned to the current detected floor. Returning from Home
retains the approved boundary's anchor ID and restores the outline into the new
session's world frame before resuming. Old world coordinates are hidden while
the anchor and floor are being recognised. Both saved drawn and automatic boundaries
also apply without confirmation after relaunching the app: their tracked room
anchor must relocalize around the wearer, and fresh head and floor tracking must
be available. Automatic boundaries additionally wait for fresh room mapping.
An unrecognised
or untracked anchor never enables the saved guardian; redraw or choose the default
area instead. Redrawing replaces the recognised boundary while retaining other
rooms. **Forget saved boundaries** removes the saved outlines and their anchors.
Automatic outlines retain interior obstacle holes across anchor rotation. Live map
updates can shrink an accepted area but never silently expand it, and those
reductions update the saved outline. **Remap area** starts a fresh review.
**Remove unwanted spots** opens passthrough for editing enclosed obstacle outlines
in the automatic map. Look inside a false obstacle's outline on the floor and
pinch once to remove it. **Undo removal** restores the last deletion; **Cancel**
restores the outline from before editing; **Done** keeps the changes. A preview
still needs **Use mapped area**, while an already active boundary saves the edit
immediately. The outside perimeter cannot be deleted by this tool. Removed spots
are stored as polygons in the same room-anchor frame as the boundary and remain
ignored by live mapping after Home/relaunch. Remapping clears those overrides.
Mapping and saved-anchor updates cannot overwrite a pending edit; tracking loss
keeps passthrough visible, and anchor loss cancels pending edits.
On visionOS 27, `WorldTrackingProvider.allAnchors` can report poses in a different
coordinate frame from `anchorUpdates`. Guardian poses use the update stream only,
and a new world provider on immersive re-entry supplies fresh poses in that
space's frame. Records made with the previous mixed-source implementation are
retained for deletion but must be redrawn/remapped once before reuse.
Room/floor reconstruction
and persisted anchor relocalization require physical-device verification.

**Quit Klepton** at the bottom of the window closes the immersive display,
stops the guest frame thread, and terminates Klepton along with all remaining
game threads. It is available before boot and after returning to the settings
window. Reopening after Quit starts a fresh game; going Home still retains the
current session. An unresponsive guest cannot indefinitely block Quit.

This restores an in-memory session; force-quitting or visionOS reclaiming the
process still requires a fresh launch. Validate on a headset by going Home
from a running game, reopening, and repeating several times. The log should
show one guest startup and multiple compositor layers, with no second boot.

The frame-clock regression test runs without game assets:
`python3 tests/test_visionos_session.py`.

With an updated Walkabout simulator build installed, the UI regression test
presses Home and reactivates the app three times, checking that the settings
window stays available and the game resumes automatically. It also checks Quit
before boot, Quit with a running game, and a fresh launch after Quit:

```bash
xcodebuild -project tests/visionos/SessionTests.xcodeproj -scheme SessionTests \
  -destination 'platform=visionOS Simulator,id=<booted-simulator-udid>' \
  -derivedDataPath visionos/build/session-ui-tests/dd CODE_SIGNING_ALLOWED=NO test
```

When running `testHomeAndReopen` alone, check `Documents/klepton-boot.log`: it should contain one guest
frame-thread startup, four compositor layers, and presented frames after each
reopen. Run these UI tests with the diagnostic simulator build installed. They
use the app's normal launch defaults, without Steam-mode or camera overrides.
Window/presentation checks do not establish that the game's menu loaded; also
inspect the visible game after the splash and check the log for startup errors.

## Audio mixing and recovery

With the game microphone off, Klepton uses `playback` with `mixWithOthers`.
Enabling the guest microphone uses `playAndRecord` with mixing; leave it off
when another app handles the call microphone. Simultaneous microphone capture
is not guaranteed. Klepton opts out of Now Playing eligibility and uses a small,
fixed spatial experience so game audio remains independent of its settings
window. When other audio is already playing and the guest microphone is off,
it preserves the route's sample-rate, buffer, and channel preferences. The C
output measures the hardware rate and resamples the guest audio.

Startup, immersive entry, foreground returns, guardian-style changes, route
changes, and media-services resets restore mixing policy before activation.
Category changes are skipped when the policy already matches, avoiding route
notification loops. An OS interruption blocks route/style reactivation and the
C watchdog's timeout override until an ended notification or successful explicit
foreground recovery. The watchdog still repairs stopped render callbacks when
there is no active interruption.

Closing the settings window while immersion remains active restores game audio
after the scene transition. The user verified this fix on the headset. Home,
Quit, route disconnection, and microphone-mute interruptions do not use that
window-close recovery. Run `python3 tests/test_visionos_audio.py` to exercise
policy restoration, failed activations, interruption handling, and window-close
recovery with a simulated session and the real C watchdog.

Mixing requests cannot guarantee another app's background behavior. Discord
347.0 (112799) on visionOS 27.0.1 still stopped in both mixed and full native
RealityKit immersion in a separate app with no explicit audio code. This
reproduced without Klepton's guardian, game, or compositor. No Discord workaround
was confirmed; Home/Resume recovery was intermittent. Safari music continued
mixing in the tester's comparisons.

## Language boundary

Swift owns the App, and will own the ImmersiveSpace, Compositor Services,
Metal and ARKit. C owns the guest. The seam is `Sources/kl_app.h` — four
functions — imported through `Sources/Klepton-Bridging-Header.h`. There is no
Objective-C anywhere and none is needed; see below for why the
boundary lands there.

`Sources/kl_app.c` is `t_boot`'s sequence with the harness removed: no forked
DRM-guard self-test, no re-exec'd recon child (an app bundle never forks, and
the Metal-refuses-forked-children reason for it is moot once it does not), no
argv, no SDL viewer. What is left is load libmain → `JNI_OnLoad` →
`NativeLoader.load` → `UnityPlayer.initJni`.

## Reading a run

Everything the guest prints goes to `Documents/klepton-boot.log`,
**line-buffered** — an unimplemented JNI slot aborts the process by design, and
a fully-buffered stream loses the whole report when the process dies on a
signal. The UI polls the file while the run is going and offers it via
ShareLink afterwards; on the simulator it is easier to read straight off disk:

```bash
UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
BUNDLE=$(python3 targets.py beatsaber bundle)     # $USER.dev.klepton.target.<target>
LC_ALL=C less "$(xcrun simctl get_app_container $UDID $BUNDLE data)/Documents/klepton-boot.log"
```

`LC_ALL=C` because the log contains guest bytes — the same trap as on the host.

## Gotchas that cost time here

- **Reinstalling rotates the data container.** Assets staged before an install
  end up in an orphaned container and the app reports them missing, which reads
  like a staging bug rather than an ordering one. `run.sh` stages *after*
  installing, always.
- **`$VAR…` is not `$VAR` followed by an ellipsis.** bash folds the multi-byte
  character into the variable name and dies with `unbound variable` on a name
  you never wrote. Brace it.
- **The archives need real Make prerequisites.** With only a `.PHONY` rule
  producing `build/xros/libklepton.a`, an edited runtime source left a stale
  archive, the xcframework was built from it, and the app silently ran the
  *previous* loader — which presents as a bug in the app. Fixed in the Makefile.
