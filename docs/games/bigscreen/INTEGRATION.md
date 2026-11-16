# Bigscreen Quest startup and WebKit integration

Current status (November 5, 2026): the user has confirmed email login, browser
interaction, audible playback, online rooms, social TV, and detailed Lobby
avatars in the simulator. On Vision Pro, login, room joining, TV playback and
voice transmission work. After the native-video layer and browser-cookie fixes,
some shared rooms show video, while others still show black. A theater loaded
its environment slowly on the first visit and both environment and video quickly
on rejoining. The remaining cloud-player capture investigation is recorded below.
Earlier transparency and seated-height reports remain part of the investigation.
The cloud-browser video capture fix works on Vision Pro. Its 10 FPS capture
limit is being replaced by adaptive video pacing (below). The user confirms
that the see-through playback menu still needs a fix.
Meta platform authentication remains unavailable.
Saved-session startup uses Bigscreen's own refresh-token flow. Earlier sections
retain the sequence of observed blockers.

### Physical headset login-screen crash (November 5, 2026)

The GLES 3.1 installer build reached the login screen on Vision Pro, then
faulted on a managed worker at `libunity.so+0x53c088`. Its IL2CPP stack runs
through `UnixNetworkInterfaceAPI.getifaddrs`, `LinuxNetworkInterfaceAPI`, and
`Bigscreen.Cloud.RemoteDesktop.SendRequestForDevices`. Unity was resolving
libc for the background network-interface scan. The synthetic libc handle
incorrectly searched every loaded guest image for missing symbols, exposing
another library's `UnityPluginLoad` callback. Unity's plugin initialization
then dereferenced a missing thread-local graphics device on the worker.

Synthetic core-library handles now resolve only the runtime shim's exports.
Explicit guest-image and process-wide symbol lookups retain their existing
behavior. This also prevents unrelated guest `JNI_OnLoad` exports from leaking
into libc. The fix applies to Unity guests using these handles, without
depending on Bigscreen's APK fingerprint or a simulator launch override.

`make loadercheck` exercises a small authored ELF exporting `UnityPluginLoad`.
The regression fails against the previous loader and passes with the fix:
libc still exposes `getifaddrs`/`freeifaddrs`, missing plugin callbacks report
an undefined symbol, and explicit-image/global lookup still finds the plugin.
Both visionOS platform compile/link gates and Swift typechecking pass. The
installer rebuilt, signed, and updated the existing physical installation as
job `2a90cecff45f`, including saved-data backup/restore and asset staging.

The user confirmed that the updated login popup stays open and responds on
Vision Pro, and that the monitored Quit actions were intentional. They then
joined a room and reported an immediate crash; room joining on the physical
headset requires a separate investigation. A diagnostic launch with
`KL_BOOT_WINDOW=0` had backgrounded and invalidated its immersive renderer;
that short run alone was not evidence of login stability.
Evidence is retained locally under
`build/bigscreen-investigation/headset-20261105/`, including the original
system crash report, regression results, build logs, and installation result.

### Physical room join: JavaScript result unboxing (November 5, 2026)

The next physical room join aborted in strict JNI dispatch. The system report
shows `klj_call_common` reached through Unity's AndroidJavaProxy unboxing while
`klj_vuplex_drain_events` delivered a JavaScript result. The matching retained
boot log names the missing operation:
`java/lang/String.toString()Ljava/lang/String;`. The user had restarted after
the crash, so that signature was in `klepton-boot.log.previous`, rather than
the current boot log; the original login fault retained in the previous crash
file was not the cause of this new SIGABRT.

The java.lang bridge now implements String.toString by returning the original
immutable string. This is general JNI behavior, without an APK-specific gate.
`make jnistringcheck` verifies identity and contents through the real JNI table,
including full-signature/name-only lookup, empty strings, and Unicode text.
The Bigscreen JNI regression also delivers a queued JavaScript result through
the ReflectionHelper proxy and unboxes its string on the guest thread, checking
that the request is consumed and its proxy reference released.

Both JNI regressions pass, along with the device/simulator compile/link gates
and Swift typechecking. Installer job `d23874d61e01` updated the same physical
app, preserving saved data and staging its assets. The room-join retest on
Vision Pro confirmed that another person's room opens and responds. Its video
screen was white while content played, and the menu's main content area was
blank; this is a separate rendering failure. The new physical launch resumed
its saved session,
completed the app's normal sign-in, and connected before a later intentional
Quit action; no missing JNI operation appeared in that run. The original
SIGABRT report and matching previous boot
log, test results, and installation evidence are retained under
`build/bigscreen-investigation/headset-20261105/` and diagnostics job
`803271b73971`.

### Additional room shaders lose texture mappings (November 5, 2026)

The physical room test above no longer aborts. Its retained log has thousands
of GL_INVALID_OPERATION messages; a bounded follow-up trace identifies repeated
unsupported glMemoryBarrier calls and an invalid framebuffer attachment. These
errors alone do not establish the cause of the white screen.

A separate GPU stress test reproduces a concrete mapping failure: after the
64-entry shader/program pin tables fill, the next program silently loses its
baked sampler location. The test expects location zero but receives 361, which
breaks Unity's mixed baked/named texture assignment contract. The pin registries
now grow as needed and collect names that GL has actually deleted. A shared
mutex protects growth, collection, lookup, and uniform remapping across contexts.
Objects marked for deletion but retained by an attachment/current binding remain
valid mappings, matching GL's deferred deletion behavior.

The expanded uniform regression keeps 96 programs live, verifies actual GPU
pixels for each, repeats room-style loading/deletion three times, and checks a
deleted-but-current program still draws correctly. It fails against the old
tables and passes with the fix. Avatar storage/reflection, local H.264, HTTP HLS,
both visionOS platform link gates, and Swift typechecking pass. Physical visual
verification is still required; the cache stress result does not by itself prove
that the reported room's video and menu render correctly.

Installer job `347200e955c3` rebuilt and updated the same headset app with the
registry fix, preserving its writable data and staging its assets. Evidence:
`white-uniform-before.log` (sampler 361 instead of zero),
`white-uniform-after.log`, `white-avatar-regression.log`,
`white-stream-regression.log`, `white-runtime-build.log`, and
`white-install.ndjson` under the local headset investigation directory.

The user subsequently confirmed that the video screen works on Vision Pro,
while the menu remains blank. Thus the registry repair is not sufficient to
resolve all of the room's rendering issues.

### Menu upload rejection and non-pausing diagnostics (November 5, 2026)

The browser BGRA upload helper always called glTexImage2D for its first frame,
even if Unity had allocated immutable texture storage. That produces
GL_INVALID_OPERATION and leaves the pixels unchanged. The helper nevertheless
returned success, advancing the browser's uploaded serial. A real ANGLE texture
test reproduces the unchanged pixels with immutable RGBA8 storage.

The helper now queries TEXTURE_IMMUTABLE_FORMAT and uploads into existing
storage with glTexSubImage2D. It checks that a host GL context is current,
reports bounded numeric upload failures, and returns failure so the browser can
retry its frame. An attempted resize of immutable storage fails explicitly and
preserves the previous pixels and the guest's bindings/unpack state. Mutable
first allocation, updates and resizing continue to work. The texture regression,
real browser input, local H.264 and HTTP HLS tests pass, as do both visionOS
platform compile/link gates and Swift typechecking.

The user also reported loud/static audio and near-zero frame rate requiring a
force quit. The last healthy compositor record is at 14:19:58; the live-debugger
attempt began at 14:19:57 and waited for attachment until 14:20:47. Before that
pause the log records steady rendering and guest PCM peaks below full scale.
The timing implicates the inspection as the stall trigger, but does not rule
out an independent audio issue. Live debugger attachment has been stopped.
The next physical test must run without a debugger before drawing conclusions
about audio stability.

`KL_WEBVIEW_TRACE=1` adds bounded CPU snapshot statistics, one DOM metadata
summary per browser and first-upload storage metadata. It records no page text,
URLs, credentials, screenshots or audio samples, and requires no GL readback or
thread suspension. This is intended to distinguish an already-blank browser
snapshot from a texture upload/draw failure. Ordinary builds leave it disabled.

Installer job `584f324b024f` updated the existing headset installation with this
upload fix and preserved saved data. Temporary `KL_WEBVIEW_TRACE=1` and
`KL_AUDIO_TRACE=1` settings were enabled for the next normal, debugger-free
test. The prior env-file state is retained locally in `menu-env-state.json` and
`menu-env-original.txt`; restore it when the investigation is complete.
The standalone WebKit and local-video tests also pass with tracing enabled,
including real CPU snapshot and DOM summaries. Evidence is in
`menu-upload-before.log`, `menu-upload-after.log`, `menu-webkit-test.log`,
`menu-webkit-video-test.log`, `menu-web-input-regression.log`,
`menu-stream-regression.log` and `menu-runtime-build.log` under the headset
investigation directory. Physical menu visibility and audio stability remain
pending user confirmation.

### Playback panels, stereo video and seated height (November 5, 2026)

The latest screenshots distinguish a rendering Twitch page with a black video
rectangle from overlapping native playback/menu panels. CPU snapshots show the
Twitch page has content and one video element; blank auxiliary views have empty
DOMs. The loaded browser textures are mutable, so immutable storage was not the
remaining cause in this run. Local `jar:file:...!/assets/web/youtube_busy.html`
navigation also failed with unsupported URL -1002. The URL adapter now resolves
both Android asset URL forms against staged assets, preserves query/fragment,
and rejects paths escaping the asset root.

Legacy layer destruction left its record in the active compositor list. It now
removes that record immediately while retaining backing storage for in-flight
frames. Deferred GLES publication rechecks liveness under the publication lock.
This fixes a concrete source of old panels remaining above current menus; it
does not establish that every reported transparency artifact has the same cause.

Disassembly of this APK's OVRPlugin 1.34 wrapper confirms five Android stack
arguments after the eight previously handled arguments: UV-rectangle override,
TextureRectMatrixf pointer, colour override, colour scale and colour bias. These
and the right-eye texture are now retained. Distinct GLES eye textures use
separate Metal mirror storage; both compositor eyes use the submitted crop and
colour/alpha settings. Tests cover real right-eye GPU pixels, per-eye stereo
crops, alpha scale, reflected axes, undefined upper padding in Android's boolean
stack slots, and destruction of active/pending layers.
Physical 3D content remains to be verified.

The physical log reports EyeLevel origin with a roughly 1.06 m head Y. After an
explicit EyeLevel request, the frontend now subtracts its first tracked eye
height from both head and controller positions. Subsequent head movement stays
relative to that height, and Recenter recalibrates it. FloorLevel, Stage and
guests that never explicitly select an OVR origin retain physical height. The
tracking regression covers seated movement, repeated origin requests, controller
offsets, recenter, invalid samples, FloorLevel and Stage.

WebKit's [snapshot video issue](https://bugs.webkit.org/show_bug.cgi?id=280824)
matches the black video rectangle, and its discussion reports canvas video as
a working snapshot path. `KL_WEBVIEW_VIDEO_CANVAS=1` enables an experimental
implementation for up to four visible, unprotected, inline main-frame videos,
capped at 1280×720 each. It paints only at the existing snapshot cadence, keeps
audio/playback in WebKit, uses pointer-transparent canvas siblings below player
controls, and removes unused canvases. Native-control video and cross-origin
frame video are not covered. The local H.264 test verifies moving RGB frames,
refresh, visible/clickable controls and canvas cleanup. This does not verify
live Twitch playback on Vision Pro. Safe tracing adds numeric playback readiness,
error, dimensions and canvas results without recording stream URLs or content.

Evidence is retained under the local `headset-20261105` investigation directory:
`enqueue-layer-disassembly.txt`, `panels-reproject-test2.log`,
`panels-touch-test3.log`, `panels-final-layer-test.log`,
`panels-final-canvas-test.log`, `panels-web-test2.log`, and
`panels-abi-test.log`. Tests pass and both visionOS platform gates and
Swift typechecking pass (`panels-abi-runtime-build.log`). The final headset update
completed as installer job `020733946c3e`, preserving saved data and staging assets,
and is recorded in `panels-final-install.ndjson`;
the temporary env file additionally enables the canvas workaround and must be
restored from `menu-env-original.txt` when the investigation finishes. No live
debugger is used for this retest.

### Black stereo video and missing microphone control (November 5, 2026)

The next headset test still showed black 3D video and no working microphone.
Diagnostics job `65977f3d4fa8` records OpenSL recorder creation refused twice
because the microphone toggle is off, followed by Unity's recording-device
initialization error. The native control window restricted its microphone
section to a fixed list of built-in targets, excluding imported Bigscreen. The
section now appears for imported APKs as well. Capture remains off until the
user enables it and grants recording permission. The panel explains that a game
whose capture already failed may need reopening to retry. The saved setting's
publish path is present in the log; no preference-restoration failure was proven.

The same run contains video conversion shader failures. Both multipass and
multiview variants unconditionally require `GL_OVR_multiview2`. This rejects
even the multipass shader whose multiview body is inactive. The GLSL adapter
now makes that directive optional only in Unity's multipass variant, identified
by its stereo conditional with no multiview keyword definition. Genuine
multiview variants retain their requirement and view IDs. An authored shader
regression and the captured vertex/fragment pair both compile, link and render
distinct left/right pixels from a side-by-side texture. Guest shader source is
retained only in the ignored investigation directory, not redistributed in tests.
This is a candidate fix for black video, pending the user's physical retest.

Separately, backgrounding the app at 15:32:41 invalidated its running H.264
decoder: callback OSStatus -12911 was followed by repeated submit error -12903,
including after the immersive space reopened. This timing does not establish
the cause of black 3D video before backgrounding. The decoder now defers fatal
session recovery to its submitting thread, drains callbacks, and recreates the
session using cached parameter sets at the next random-access frame. Dependent
frames are skipped while waiting; ordinary bad-packet errors keep the session.
Repeated failures are rate limited in logs. `make decoderrecoverycheck`
invalidates actual H.264 and HEVC sessions, tests the asynchronous malfunction
path and verifies nonblank recovered pixels with matching presentation times.

Evidence under `headset-20261105`: `stereo-mic-diagnose.ndjson`,
`stereo-video-shader-test2.log`, `stereo-captured-shader-test.log`,
`stereo-decoder-recovery-test2.log`, `stereo-mic-runtime-build.log`, and
`stereo-mic-install.ndjson`. Both platform build gates and Swift typechecking pass.
Installer job `8973db1b70b3` completed the physical update, preserving saved data
and staging assets. The HEVC decode and uniform-mapping GPU regressions pass.
Microphone capture and the room's 3D picture still require physical confirmation.

### Recorder state mismatch and continuing room rendering failures (November 5, 2026)

The retest enabled capture and granted microphone permission, but other users
still could not hear speech. Diagnostics job `a9f0f4ce009d` contains two runs
with a working built-in input route and OpenSL recorder construction. The final
capture reports have nonzero peaks (22555 and 14578), while the guest read zero
samples. Its requested record state was logged as STOPPED. The bridge had
incorrectly numbered recording states 0/1/2; the public
[Android OpenSL header](https://android.googlesource.com/platform/system/media/+/07c08a29e5c0a8250a5225f57d610eb1250ac225/opensles/include/SLES/OpenSLES.h)
defines STOPPED/PAUSED/RECORDING as 1/2/3. Thus Unity's request to record never
ran the feeder. Those constants are corrected, invalid states are rejected, and
GetPosition now reports delivered recording frames in milliseconds instead of
always returning zero. State and position getters share the recorder lock.

`tests/t_opensl_record.c` exercises public vtables against synthetic PCM only:
the opt-in gate, literal standard state numbers, real feeder callbacks, partial
reads, 48-to-16 kHz resampling, pause/resume and recording position. It does not
open a microphone or record user audio. The test is included in `openslcheck`.
Playback, overlay GPU and WebKit regressions pass, along with both visionOS
platform gates and Swift typechecking. Physical voice transmission remains
unconfirmed until the next run.

The same retest still had a black theater with visible avatars, a visible room
without its movie, and later a white movie screen with transparent media menus.
There are no shader compile failures in these runs. Decoder recovery succeeded
once after an invalid session, so the prior fixes did not resolve all playback
paths. The user could not identify the source or room name. No further visual
fix is claimed from this evidence. Browser diagnostics now restart their
sampling window on navigation and include later frames, capped at eight
navigations per view. `KL_OVRP_OVERLAY_TRACE` adds bounded per-layer crop,
colour/alpha and underlay metadata, without GPU readback or debugger attachment.
These are enabled for a room-transition reproduction in the next build.

Evidence under `headset-20261105`: `room-mic-retest-diagnose.ndjson`,
`room-mic-final-recorder-test.log`, `room-mic-playback-regression.log`,
`room-mic-overlay-regression.log`, `room-mic-final-webview-test.log`,
`room-mic-final-runtime-build.log`, and `room-mic-install.ndjson`.
The full `openslcheck` gate passes. Installer job `76f40f0ff8e9` completed the
physical update, preserving saved data and staging assets. The temporary env
file now also enables `KL_OVRP_OVERLAY_TRACE`; restore the original env file
when the investigation is complete.

### Shared-video copy and room cookie crash (November 5, 2026)

The user confirms voice transmission now works. In diagnostics job
`9e60b1dddd2c`, the corrected OpenSL recorder delivered 47306 buffers and the
guest read 504.60 seconds of the 504.61 seconds captured, without ring overruns.
Shared-room movies remained black, while local and social TV playback worked.
Four native H.264 streams decoded, and their submitted overlay handles exactly
match their SurfaceTexture handles. Those handles contain native YCbCr images;
the TV adapter uses BGRA images.

The existing texture regression proved native YCbCr can be sampled. Extending it
to the actual composition-layer mirror reproduces the missing link: attaching
the decoded YCbCr texture to a read framebuffer produces status 0x8cd6 and blit
error 0x506. A process-wide one-message warning had hidden these failures after
the startup splash's unrelated incomplete framebuffer. The mirror now samples
externally backed video into an ordinary RGBA layer with an internal shader;
both native and BGRA sources follow the same sampling semantics. Other layer
textures keep their existing framebuffer-blit path. Copy errors are bounded per
layer instead of suppressed process-wide.

`build/t_video_layer` exercises real decoded 8-bit and Main10 frames, compares
the complete compositor-layer pixels with ordinary texture sampling, and checks
program, framebuffer, VAO, viewport, texture units, sampler, write mask and render
enable restoration. It also verifies that copying defers while a delete-pending
program is current, because switching programs would destroy the guest's object.
The native test fails before this fix and passes afterward; mean mirror error is
at most 0.04/255 in the tested frames. Test readback is confined to authored media
and uncompressed host RGBA textures; no headset GPU readback is added.

A temporary `KL_VTDEC_BGRA=1` comparison made one previously black shared room
visible on Vision Pro. Joining a different room then aborted at the missing
static JNI call `WebView3D.setCookie(Ljava/lang/String;)V` (diagnostics job
`358a9ad1f109`). This was a Klepton app crash, not a system crash. The original
DEX implementation accepts Vuplex Cookie JSON. The adapter now stores that
cookie through the shared WebKit store and queues subsequent explicit loads
until pending cookie writes finish. Domain, path, Secure, HttpOnly, expiry,
replacement and expired-cookie deletion have real WebKit/network tests. Cookie
data is never logged. The JNI regression resolves the inherited static call.

Both platform gates and Swift typechecking pass, alongside the video-layer,
cookie, JNI, browser and ordinary-overlay regressions. Evidence under
`headset-20261105`: `video-layer-before.log`, `video-copy-final-layer-test.log`,
`video-cookie-test2.log`, `video-cookie-jni-test.log`,
`video-copy-overlay-regression.log`, `video-copy-final-web-test.log`,
`video-copy-final-runtime-build.log` and `video-copy-cookie-install.ndjson`.
The next physical test showed content in several rooms; some cloud-player rooms remained black (see below).
Installer job `61ac9a4aec41` completed the update, preserving saved data and
staging assets. The temporary decoder override is now `KL_VTDEC_BGRA=0` to test
the corrected native-copy path instead of relying on the converted-output
comparison. Other bounded diagnostic flags remain enabled for this retest.

### Cloud-browser shadow video (November 5, 2026)

The next user test reports several working rooms, a black Star Wars lobby
screen, and one theater that recovered after rejoining. Diagnostics job
`3de5595c5bc0` contains native H.264 decoder sessions and cloud-browser pages.
The native overlay errors occur before decoded images arrive; no subsequent
video-copy errors are recorded. This does not establish that every native
stream succeeds, but separates those paths from the remaining browser cases.

The public cloud-browser page embeds its player in an open shadow root. The
existing snapshot workaround only searched the light DOM, so it could not
paint that player's video. Video discovery now traverses open roots with
bounded node/depth limits. Pointer hit testing, keyboard focus and scrolling
also descend through open roots. Capture retains its four-visible-video and
1280×720 limits. Closed roots, cross-origin frames and protected video remain
outside this workaround.

`t_webview_video` can place authored H.264 inside two nested open roots via
`KL_TEST_SHADOW_VIDEO=1`. Before the fix, its canvas/click assertion fails;
afterward, changing video pixels, refresh, overlay-button clicks, keyboard
input, scrolling and canvas cleanup pass. The light-DOM video, browser input
and cookie tests also pass, along with both visionOS build gates. Browser diagnostics now count
deep videos and report HTTP status, script/rejection counts and numeric error
categories. Navigation logging records origins without query strings, paths,
credentials or raw error text. The user confirmed that formerly black cloud video now displays;
this change does not establish a cause for delayed environment loading.
Installer job `7f3b46abb890` completed installation and asset staging, preserving
saved data. The existing canvas and bounded diagnostic flags remain enabled.

### Browser video cadence and remaining menu transparency (November 6, 2026)

The user confirms cloud video now works, but reports a low frame rate and the
same see-through playback menu. Diagnostics job `0d2e15eae6c3` shows the cloud
player running at readyState 4, 1280×720, unpaused, with one canvas drawn and no
reported media, script or page error. Every browser snapshot was still limited
to 10 FPS. That local ceiling can explain choppiness without network lag.

Playing canvas video now targets 30 FPS; static pages and paused video stay at
10 FPS. `KL_WEBVIEW_VIDEO_FPS` overrides the video target within 10–60 FPS. A
one-shot timer schedules from the preceding capture's start, with one capture
in flight. Slow captures cannot queue or incur an extra full interval after
completion. Bounded cadence logs measure completed FPS and mean capture time;
this is a target, not a promise of 30 FPS on the headset.

The authored nested-shadow fixture reproduces the 10.5 FPS baseline. At the
cloud player's 1280×720 source / 1400×788 viewport size, the revised path measures
25.9 and 27.6 FPS, 9.4 FPS paused, and 26.6 FPS after resuming. Video colors,
controls, refresh, keyboard, scrolling and cleanup pass. Both visionOS build
gates and Swift typechecking pass. Physical cadence verification is pending.
The see-through menu remains unresolved; layer diagnostics now include source
presence, submission index and pose to investigate its composition separately.

### Cloud Browser panel order and low-memory callback (November 6, 2026)

The user identified the see-through panel as Cloud Browser. After quitting,
log collection succeeded (`bbf6e36a8764`). The source browser snapshot has no
near-zero-alpha sample points, and the player remains ready/playing with successful
canvas draws. Multiple theater and small-panel underlays share the source.
`EnqueueSetupLayer2` previously discarded compositionDepth. The bridge now
retains it and stably orders explicitly ranked legacy layers back to front,
with smaller depths in front, as documented by
[Meta](https://developers.meta.com/vr/documentation/unity/unity-ovroverlay/). Equal depths and lists
from other APIs retain their ordering. The real ANGLE-to-Metal regression
submits the front layer before the back one and verifies compositor order,
alongside existing image, stereo, crop, colour and destruction checks. This
fixes a supported API gap; whether it resolves the user's panel remains to be
verified on Vision Pro.

A separately collected previous-run crash (`00186940414e`) entered managed
code from `kl_mem_pressure_fire` on a dispatch memory-pressure worker and
faulted at `libil2cpp+0xa570e8`, address 0x10. OS events now atomically coalesce
and reach nativeLowMemory at the next guest frame poll, on the frame thread
that already runs Unity. OS warnings still deliver when footprint polling is
disabled. `t_lowmem_thread` registers a real synthetic JNI callback and checks
worker deferral, coalescing, thread identity and warning delivery. It passes,
as do both platform build gates and Swift typechecking. No debugger attachment
or headset GPU readback is involved.
Installer job `c6ce6fbb73ba` completed the frame-rate, layer-order and deferred
low-memory update, preserving saved data and staging assets. Physical playback
rate and Cloud Browser panel verification remain pending.

### Immersive renderer remains paused at startup (November 6, 2026)

The next physical test did not get past the Klepton window. Diagnostics job
`99b2d3961042` reaches boot completion, successful openImmersiveSpace and spatial
input registration, but no ANGLE initialization or guest frames. Startup
milestones were added before/after renderer activation, graphics device lookup,
EGL creation and pipeline setup. Diagnostic build `1f31e8a71424` and logs
`e9ee545b66ad` / `a76b404c894b` show waitUntilRunning blocked in state paused for
at least 51 seconds, returning invalidated only after Quit dismisses the space.
Browser capture and OVR layer ordering have not run at that stage.

Reopen VR now remains available
in the control window even when openImmersiveSpace returned opened. After
eight seconds the window explains the wait and directs the user to any headset
prompt. The diagnostic `KL_RENDERER_STARTUP_RETRY=1` allows one foreground
reopen, but defaults off: Apple documents that paused can indicate a pending
system immersion confirmation, which should not be dismissed automatically.
The policy retains its retry budget across replacement renderers and ignores callbacks
from obsolete renderers. An invalidated renderer that never started Unity also
skips nativePause. The standalone Swift policy regression covers timeout,
foreground/quit guards, retry limits, stale callbacks and healthy activation;
Swift typechecking passes. Build `310cc3b0e3c6` exercised the one-time retry: the
old renderer invalidated cleanly but the replacement returned userCancelled
(logs `bb58a0a70f66`). Manual activation and any system prompt are being checked.
See [Apple’s renderer lifecycle documentation](https://developer.apple.com/documentation/CompositorServices/drawing-fully-immersive-content-using-metal).
Installer job `cd488508369d` completed the manual-recovery build with automatic
retry off, preserved saved data and staged assets. Launching from Home and
manual activation are awaiting user confirmation; guest rendering has not yet
been verified in this build.
The next user attempt still appeared inert. Logs `55bb64fc5964` confirm that
manual controls do invalidate/create fresh renderer instances, but each remains
paused before graphics-device lookup. Immediate reopen sometimes returns
userCancelled. A headset restart and comparison with another immersive app
have been requested to distinguish system state from app-specific activation.

### PHASE capture initialization exception after reboot (November 6, 2026)

Restarting Vision Pro resolved the renderer activation stall. The next two
launches reached Bigscreen's title and aborted with an uncaught AVFAudio
exception, -10868. Diagnostic job `14ec23076cf9` and the matching system IPS
report (`KleptonImportcd488508369d-2026-11-06-015257.ips`) locate the exception
inside PHASE StreamRenderer construction / AVAudioEngine.connect. At that
moment the guest thread is inside cap_build -> AudioUnitInitialize ->
AUPhaseIO::Initialize, creating Unity's OpenSL recorder. This is microphone
initialization, before browser playback, rather than the earlier paused view.

The duplex unit requested signed-int16 capture PCM. Device-facing duplex input
and output now use canonical Float32 noninterleaved planes. Capture converts
into the existing interleaved int16 ring with saturation and nonfinite values
silenced; Android readers keep the same rate, channels and sample type.
Speaker output deinterleaves the actual mix into per-channel buffers, preserving
the echo-cancellation reference. Callbacks remain allocation/lock free. The
macOS HAL capture format remains int16.

VoiceIOTests now rejects integer capture at format negotiation and reproduces
the old failed microphone-open assertion. Afterward it validates canonical
input/output formats, distinct stereo planes, float conversion boundaries,
nonfinite samples, underrun silence, shared-device lifecycle, route recovery
and fallback. The capture filter, PCM mix and OpenSL/AAudio clock/alias tests
also pass, along with both visionOS build gates. This is a supported-format
correction; physical initialization is pending retest.
`PhaseAudioFormatTests.m` additionally feeds the actual runtime ASBDs into
Apple’s offline AVAudioEngine graph and verifies distinct stereo samples.
It passes with synthetic PCM and no physical capture/playback devices.
Installer job `a2f2810fe23e` completed the update, preserving saved data and
staging assets. The first launch reached a running renderer but was closed
with Quit before capture initialization. The next manual run stayed open for
three minutes; diagnostics `55fb977151d6` show successful Float32 VPIO capture
initialization at 48 kHz, including a later rebuild. The user confirms that
Bigscreen gets past the title screen and stays open.

### Cloud video after closing the control window (November 6, 2026)

The same run exposed another capture failure: the cloud-browser document
reported hidden=1 while its video remained readyState=4, playing, 1280×720,
with a successfully drawn canvas and no media error. Every sampled native
snapshot pixel had zero alpha. The user confirmed closing the separate
Klepton control window. One snapshot waited 74 seconds for a screen update.
The cloud video source was healthy, but its hidden native host could no
longer supply usable page snapshots.

Native snapshots now use afterScreenUpdates=NO. With the canvas workaround
enabled, an opaque, full-viewport, playing MediaStream can also supply its
origin-clean canvas directly while the native host is hidden or detached.
The exported JPEG is bounded to 1280×720 and 4 MiB, decoded through ImageIO
after checking its dimensions, then published through the existing BGRA
texture path. No frame data is logged or saved. Protected video, tainted
canvases and ordinary HTML player pages do not use this export path. Native
page capture resumes when the host returns, preserving ordinary page controls.

`tests/t_webview_hidden_video.m` plays a synthetic MediaStream inside a shadow
root, hides its NSWindow, reopens it, detaches the browser view, then reattaches
it. Changing RGB frames, corner orientation and opaque alpha survive both
hidden and detached phases; direct capture occurs only in those phases.
Normal page input/navigation and nested HD H.264 playback,
refresh, pause/resume pass as well, with playing captures around 26–28 FPS.
Both visionOS runtime build gates pass. Physical closed-window playback remains
to be checked. Installer job `e3810c9bf38c` completed the headset update,
restored saved data and staged assets; diagnostic settings were byte-verified
after installation, then the app was launched successfully.

The physical test still failed after closing the control window. Diagnostics
`a7e90587b582` show the compositor and guest continued running, but the direct
capture reported ineligible (category 0) and native snapshots again had all
zero alpha. Source video remained playing with readyState=4. The previous
guard rejected every CSS transform and required the video element itself to
cover 90% of the viewport, which excludes centered or letterboxed cloud players
inside full-page shadow hosts. That layout reproduces the rejection locally;
the existing log cannot distinguish which eligibility condition the real
player failed.

Capture now accepts positive axis-aligned scaling and translation, using
the video's transformed bounds, and allows a letterboxed video inside a
full-page shadow host. Rotation, reflection, protected media and native
controls remain excluded. An entirely transparent hidden-host snapshot while
video is playing no longer replaces a usable texture, preventing empty frames
from feeding the room lighting. Rejection diagnostics now report a specific
numeric eligibility category, video/host dimensions and native media playback
state, without URLs or page text.

The centered/letterboxed fixture fails on the previous guard and passes after
the correction through visible, hidden, reopened, detached and reattached
phases. It also verifies that an injected transparent frame does not advance
the published texture serial. Browser input/navigation, nested HD H.264 video,
refresh, pause/resume and both visionOS build gates pass. Physical video and
audio after closing the window remain to be retested.

Tested October 28, 2026 with the local standalone Quest APK:

- Package: `com.bigscreenvr.bigscreen`
- Version: `0.950.2.a276a2-beta-perma-rooms-beta`, code `4688857`
- APK SHA-256: `6cae96955397be3522c238196635a061c5f9da465f032cbe34b817dddbb4a18a`
- Unity `2020.3.33f1`, ARM64 IL2CPP, Oculus XR Plugin

The first physical Vision Pro boot reached Unity's native startup and aborted
with `fatal: called unresolved import 'putenv'`. This is a missing Klepton libc
binding, rather than evidence that the APK needs Android headset patching.
`runtime/libc/kl_shim.c` now serves `putenv`, preserving caller-owned string
storage and the bare-name removal behavior. `tests/t_putenv.c` checks lookup,
subsequent mutation of the supplied storage, and removal through the guest shim.

After that fix, the Mac lifecycle probe reached a graphics initialization wait.
This APK's `OculusUnity.initOculus()` obtains the Unity view's Surface and calls
the native `initComplete(Surface)`. Klepton only called the newer plugin's
`surfaceCreated(Surface)`. Both native signatures are `(JNIEnv*, jobject,
Surface)`; Klepton now resolves the newer export first, then the older export.
`tests/t_oculus_init.c` exercises both generations with real JNI Surface objects
and checks callback caching. No proprietary APK is needed for these tests.

The unmodified APK then completes 3,000 Mac frame-pump iterations with the null
graphics driver. With actual ANGLE/Metal enabled it submits Oculus eye layers
and reaches a later, explicit runtime stop:

```
[jni] no host implementation for com/vuplex/webview/WebView3D.setAlternativePointerInputSystemEnabled(Z)V
```

That was the next observed blocker in the first investigation. The simulator
work below implements those configuration calls and reaches the unavailable
browser itself. Browser functionality was still unimplemented at that stage. Do not label
this build compatible or interpret the earlier null-driver frame count as a
rendered, playable application.

## Why ovrport is not a required preprocessing step

The local `~/ovrport-app` checkout was checked against upstream and was already
at `1c7cb335c81369fbfd2957ad0edcb238956a8e3c`. Its library-copy and Unity/Java
patches target other Android headset runtimes. Klepton serves Oculus/VrApi
runtime library names itself. They do not implement the missing Darwin libc
binding or execute Bigscreen's Java WebView code on visionOS. No ovrport output
replaced the original APK or the imported library tree.

[AVP Play](https://github.com/myMartek/avp-play) provides useful store-ownership,
per-game recipe and error-reporting patterns. Its native installer/toolchain
workflow also translates Quest APKs with Klepton. Its reviewed installer and
toolchain code do not establish that this Bigscreen APK works or that ovrport
would supply these missing runtime methods.

## Evidence and reproduction

Local evidence is retained under `build/bigscreen-investigation/` (ignored):

- `klepton-boot.log`: original physical-device failure.
- `original-host-after.log`: libc fix alone; later startup wait.
- `original-host-metal.log`: initialization wait before callback fix.
- `original-host-oculus.log`: 60 frame-pump iterations after callback fix.
- `original-host-long.log`: 3,000 null-driver frame-pump iterations.
- `original-host-angle.log`: actual ANGLE/Metal and the Vuplex JNI stop.
- `installer-check.log`, `oculus-test-build.log`: validation evidence.

The `original` and `original.apk` entries in that evidence directory point at
the installer's immutable import. Reproduce the rendered Mac probe there with:

```sh
KL_ANGLE_DIR=/absolute/path/to/Klepton/vendor/out/Debug \
  KL_GLFB=1 KL_GLFB_MTL=1 KL_LIFECYCLE=1 KL_FRAMES=3000 KL_ALARM=90 \
  ../../build/m_boot original/lib/arm64-v8a
```

`KL_ANGLE_DIR` must be absolute because this probe runs outside the checkout
root. Both fixes build for the physical visionOS runtime and the signed
Bigscreen app. For new headset failures, use **Collect Headset Logs** in the
Mac installer; its report distinguishes missing runtime methods from missing
game content. Select the Bigscreen install job if another app was installed
more recently. Keep backups private; share diagnostic logs only after review.

The signed app with both fixes was installed on the paired physical Vision Pro
as job `bigscreen-fix-8bd562e8`. All 35 inventoried writable files were backed
up and restored, the APK/assets were staged in the new container, and the
transfer receipt was updated. There is no pending recovery. The original
backup remains in that job. Subsequent remote launch attempts timed out at
20 and 15 seconds without a new boot log, so the updated build's physical
startup was unverified at that stage. The simulator follow-up below removes
that device dependency; the initial Vuplex stop above was from the Mac with
actual ANGLE rendering.

Validation: `make installer-check` passes (51 installer Python tests, 14 Steam
update tests, 44 Node tests and Swift model checks). The model checks also
cover restoring the latest completed diagnostics job and ignoring unfinished
installs. Both new C runtime tests pass; the signed device app and Mac
installer build successfully. These checks do not establish full playability.


## visionOS simulator follow-up

The isolated **Klepton Bigscreen** visionOS 27.0 simulator
(`B7CF191F-8115-40ED-96DF-42D3155434F0`) reproduces the startup failures without
requiring the physical headset. It uses bundle `dev.klepton.bigscreen.probe`,
the original APK, a separate container and a build manifest under
`build/bigscreen-investigation/simulator/`.

The added JNI families implement the Java behavior needed for source-less
startup: Vuplex input flags, user agent, autoplay and DRM-request settings;
Bigscreen event callback registration; retained media/browser contexts;
independent idle player and web-client IDs; and external GLES texture creation.
Before a media source is opened, the APK's transport calls have no underlying
player to act on, no available frame and zero duration/position. These handlers
model that initial state. They do not open a movie, report playback success,
provision Widevine, produce browser pixels or emit invented completion events.
Actual source-opening/browser-construction operations remain unsupported.

The default GLES 3.2 description also led to an `Internal-Skinning.main` shader
failure followed by a native crash at `libunity.so+0x565930`. A diagnostic
injected setter first confirmed that a GLES 3.0 description passed that point.
The supported `KL_GLES_VERSION=3.0` switch now makes that choice without injection.
Version strings, GLSL strings and numeric version queries agree, while unset
and invalid values preserve existing target defaults. The Mac installer offers
**Advanced → GLES compatibility → GLES 3.0** for Unity imports, automatically
suggested only for this exact tested APK fingerprint. The selected setting is
validated and baked into the generated app; the final simulator run uses that
manifest setting rather than a launch override.

With strict JNI handling and GLES 3.0, the simulator renders Bigscreen's
apartment environment and remains alive. The retained initial final report
records 5,507 compositor iterations, 5,402 with a picture, no unresolved calls
and no native faults. This establishes rendered startup, not a working login,
network session or movie player. The visible browser-backed panel is unavailable:

```
[jni] Vuplex browser unavailable: Klepton needs a WebView browser/texture/JavaScript bridge; ovrport preprocessing cannot provide it
E/Unity: WebViewUnavailableException: The Android System WebView package is currently unavailable.
```

`isWebViewAvailable()` now returns false deliberately because no browser bridge
exists. Bigscreen takes its own unavailable-WebView error path. A separate Mac
probe also encountered a later null IL2CPP thread-local-state read at
`libil2cpp.so+0xa570e8`; that fault was not observed in the final simulator run
and has not been fixed. One earlier isolated Mac permissive diagnostic was used
to enumerate the remaining Java calls (`surface-diagnostic.log`); its results
were not used as compatibility evidence. Final simulator tests use strict JNI.

The visionOS status window now continues reading a bounded log tail after
native initialization. The browser failure changes the green initialization
badge to **Runtime support needed**, with a visible explanation and log-export
instructions. The warning remains visible while the room renders. Export log
shares the complete retained log file even though the live display uses a bounded
tail. The Mac
inspector warns about this tested APK's browser dependency before building,
and the log collector recognizes the unavailable-browser exception.

Reproduction helpers and evidence:

- `simulator/manifest.json`, `simulator/rebuild.py`: isolated simulator build,
  install, staging and launch (no physical-device mutation).
- `simulator/final-simulator-boot.log`, `simulator/final-report.json`: strict
  simulator startup evidence with the selected GLES setting.
- `simulator/host-availability.log`: default GLES shader failure.
- `simulator/host-gles30.log`: original GLES 3.0 diagnostic.
- `simulator/host-idle-players.log`: separate Mac IL2CPP TLS failure.
- `simulator/jni-tests.log`, `simulator/gles-version-tests.log`: new runtime
  regressions; `simulator/final-installer-check.log`: installer validation.

The physical headset retains the earlier two-fix build; this follow-up uses the
simulator. Rebuild through the updated installer to include these later changes.
The WebKit follow-up below supersedes the unavailable-browser result. Login and
actual movie playback remain development work.


## System WebKit follow-up

`runtime/media/kl_webview.m` embeds Apple's `WKWebView` on macOS and visionOS.
The C API in `kl_webview.h` queues browser commands onto Apple's main thread.
A view stays attached outside the host window's visible area: the game consumes
snapshots, without an extra browser panel covering its native status window.
Each browser owns its navigation delegate, script-message handler, snapshot
buffer and timer. Closing invalidates the timer, removes the handler and view,
and rejects pending events. Both ARC and the runtime's non-ARC builds are used.

`runtime/jni/kl_jni_vuplex.c` supplies the APK's WebView3D constructors, HTML and
URL loading, JavaScript execution/results, page messages, resize, basic DOM
pointer/scroll/key input, user agent and navigation operations. Asset URLs under
`file:///android_asset/` resolve into the imported APK's real asset directory,
including relative stylesheets. The page message adapter is an independent small
implementation of the public `window.vuplex` API and `_vuplexBridge.postMessage`.
TLS validation and WebKit's normal browser restrictions remain in force.

The browser never calls guest code from Apple's main thread. It queues bounded
events, then `kl_driver.c` drains them between Unity frames on the attached guest
thread. `UnityPlayer.nativeUnitySendMessage` receives real UTF-8 byte arrays;
JavaScript result consumers use Unity's proxy callback. Both observed
`ReflectionHelper.newProxyInstance` signatures are supported. Destroyed views
lose their queued destinations and pending consumer references.

Snapshots run at up to 10 Hz, with one request in flight per view, and produce
BGRA pixels at the requested game viewport size. Vuplex's real render call runs
on Unity's graphics thread. `kl_glfb_upload_webview_bgra` allocates storage for
the external texture name on its first frame or resize, then updates it while
restoring the previous texture/PBO/unpack state. The existing decoded-video
upload continues to use its original preallocated-storage behavior.

Bigscreen's `SetupClient` retains that same native view and its pointer/scroll
entry points route input to it. Android custom full-screen video views, Widevine
and Bigscreen's source-opening media player are not implemented by this browser
adapter. DOM-generated input also does not create trusted hardware gestures.
Do not interpret an initialized browser or its initial about:blank texture as a
working Bigscreen page, authenticated session or movie player.

The strict original-APK simulator run records three `WKWebView` initializations
and first texture uploads (1400×788, 1400×788 and 1152×788), without unresolved
JNI calls or the previous stream of native GL errors. The apartment renders.
The application remains on its initial data-loading panel and logs
`An unknown error occurred. Request failed.` No application page load is observed
in this run. A separate `KL_TRACE_NET=1` run reaches
`main-shark-cloud-api.bigscreencloud.com` and
`main-shark-api.bigscreencloud.com`; those connections alone do not establish
which request failed or its cause. No response, entitlement or login was forged.

Validation:

- `make build/t_webview && build/t_webview`: real WebKit page and pixels, DOM
  click, JavaScript result, messages in both directions, independent views,
  resize, APK asset URL/relative CSS, navigation failure and shutdown.
- `make build/t_bigscreen_jni && build/t_bigscreen_jni`: deferred cross-thread
  events into a registered Unity callback, UTF-8 payloads, invalid initialization,
  destroyed-view event rejection, both proxy generations and the Java
  UnitySendMessage wrapper, in addition to the earlier startup contracts.
- `make build/t_webview_texture` then
  `KL_GLFB=1 KL_ANGLE_DIR="$PWD/vendor/out/Debug" build/t_webview_texture`:
  actual ANGLE framebuffer readback checks BGRA conversion, first allocation,
  updates, resize, texture/PBO/unpack state restoration and the existing video
  upload path. Requires the prepared ANGLE checkout.
- `make installer-check`: Python, Node and Swift installer regressions, including
  actionable WebKit load/render diagnostics.
- Device/simulator runtime compilation and full-archive link gates, followed by
  the isolated simulator app build/install/launch.

The installer now describes this as an experimental WebKit integration, while
retaining the exact-APK GLES 3.0 suggestion. Browser navigation/content-process
errors point to connectivity and collected logs; persistent texture failures
point to a runtime development report. Early transient snapshots during WebKit
startup do not trigger that warning. Rebuilding through the installer includes
the bridge; the physical headset has not been updated by this simulator work.

Evidence is retained locally under `build/bigscreen-investigation/` in
`webview-test.log`, `webview-jni-test.log`, `webview-installer-check.log`,
`webview-texture-test.log`, `webview-final-build.log` and the isolated simulator
`webview-final-simulator-boot.log` / `webview-final-report.json`. These logs may contain
local paths or application details; review before sharing.

## Initial loading, graphics and account-service follow-up

The initial `An unknown error occurred. Request failed.` was traced to the
Oculus SDK callback chain, not to a failed Bigscreen HTTP response. Returning
request ID zero from `ovr_User_GetLoggedInUser` left this APK's Unity request
awaiter registered indefinitely. A nonzero request with a correctly typed
error completion lets Bigscreen take its own offline path and open the menu.
It then displays `Couldn't reach Oculus. You won't be able to use Bigscreen's
online features.` This is an accurate missing-service result, not a successful
Meta login. No HTTP request to Meta was made by that local handler.

Opening the menu exposed a second crash: Unity's clipboard native code called
`strlen(NULL)` at `libunity.so+0x692338`. The original APK's Java implementation
returns a non-null empty string when no clipboard text exists. The JNI adapter
now does the same; the soft-input regression covers both empty and populated
clipboards.

The unreadable rectangular glyphs were R8 font atlas alpha swizzles ignored by
ANGLE's Metal simulator backend. Native texture swizzling is now enabled on
Apple Silicon simulators. The bundled simulator renderer also predated the
tracked ASTC mip-copy fixes. Rebuilding it from the reconciled patch restored
the room's wood floor and other sampled textures. The existing GPU copy
self-test now checks four R8 alpha samples in addition to 23 ASTC samples and
five invalid-copy cases. Both pass in the simulator without a feature override.
Both device and simulator ANGLE frameworks have been rebuilt; physical-headset
rendering has not been verified in this follow-up.

### What automatic Oculus login needs

Read-only inspection of this exact APK's metadata and native method tables
establishes this sequence:

| Stage | Guest method / native offset | Result needed |
| --- | --- | --- |
| Startup identity | `BIG_APP_LOADER.GetOculusUser`, `0xb89150` | `Users.GetLoggedInUser` |
| Offline retry | `SettingsScreen_Account.ReturnToLogin_Offline`, `0xdce0e8` | Restarts `StartConnectionFlow` |
| Automatic login | `Accounts.OculusLogin` continuation, `0x18c5e70` | `Utils.GetOculusUserData` |
| Account data | User-data callback, `0x1ead79c` | `Users.GetOrgScopedID` after the real user |
| Account proof | Organization callback, `0x1eae1fc` | `Utils.GetOculusUserProof` |
| Proof request | Proof continuation, `0x22f4510` | `Users.GetUserProof` |

`Bigscreen.Cloud.OculusUserData` contains the Oculus app ID, user ID, alias,
image URLs, organization-scoped ID and nonce. The startup log supplies app ID
`2497738113633933`. A synthetic local profile cannot provide that authenticated
identity. Meta documents a proof nonce as single-use and verified by the app's
backend: [User proof API](https://developers.meta.com/vr/reference/platform/v207/function_o_v_r_requests_user_8h_ovr_user_get_user_proof/).
Meta's standalone development initialization requires an app ID and suitable
credentials; the documented SSO linking flow requires the app owner's
registered redirect and server credentials. Neither is supplied by adding a
WebView. See [native setup](https://developers.meta.com/vr/documentation/native/ps-setup/)
and [account linking](https://developers.meta.com/vr/documentation/unity/ps-account-linking/).

The user confirmed that Settings → Account & Prefs → Go Online returns to Home
and shows the same Oculus notice. That matches the retry method above.
`Accounts.Login` (`0xe09a54`, continuation `0x18c51c4`) contains a separate
email/password HTTP route, but reaching that form and signing in through the
unmodified UI remain unverified. Its existence does not establish an available
fallback from this APK's offline state.

The platform adapter now also completes missing user-proof and organization-ID
requests with typed errors (`0x22810483`, `0x18f0b01b`) instead of request zero.
Online access-token requests fail explicitly; the existing synthetic token is
confined to explicit `KL_NET_OFFLINE=1` diagnostics. The installer and visionOS
status window distinguish missing Meta account support from connectivity,
browser or password errors. These changes improve failure/recovery behavior;
they do not implement a Meta account provider or establish online playability.

[AVP Play's Meta login](https://github.com/myMartek/avp-play/blob/main/core/Sources/AVPPlayCore/MetaLogin.swift)
uses Meta's platform utility to obtain a token for store/library operations.
That source contains no running-game user-proof bridge; its token must not be
treated as Bigscreen's fresh SDK proof nonce.

Validation and investigation evidence under `build/bigscreen-investigation/`:

- `auth-types.json`, `managed-methods.json`, `auth-disassembly.txt`,
  `login-continuations.txt`, `offline-ui-disassembly.txt`: local read-only
  inspection of the user's APK (not committed or redistributed).
- `auth-errors-test.log`: real request queue, unique request IDs, typed account
  errors and no fabricated online access token.
- `auth-installer-check.log`: installer diagnostics/model regressions.
- `simulator/fresh-angle-regressions.log`: R8 and ASTC GPU readback checks.
- `touch-pose-test.log`: exact Touch pose through render/physics node reads,
  both hands, rotated rays and physical-frame correction on/off.

Simulator input uses the system selection ray when no tracked hands exist.
It applies the same standing-height lift as the simulated head and inverts the
physical controller grip correction before publishing the aimed Touch pose.
The prior implementation applied that correction twice, pitching selections
below the simulator pointer. Physical controller publishing is unchanged.
The input regression checks the pose seam; visual alignment still needs
confirmation in Bigscreen after rebuilding the simulator app.

The remaining online blocker is a genuine Meta account provider. A signed-in
Quest is the next reference environment to compare the real platform flow and
investigate whether an authorized bridge is practical. Merely connecting it
does not automatically provide the simulator with a valid session. Login and
playback have not yet been demonstrated.

## Quest reference and direct account-screen experiment

The user's USB-connected reference headset is an original Quest (`monterey`),
running Android 10 / API 29 and Horizon `50.0.0.198.257`. Its store-installed
Bigscreen is `0.950.0.207286-beta-perma-rooms-beta` (version code 3814346),
different from the simulator APK. The user reports that Bigscreen online
support for this device was retired and confirms the same Oculus notice on it.
It is therefore not a working online baseline.

A live startup capture shows Platform SDK API `1.1.81.0` loading implementation
`1.1.82.0` from Horizon after the newer `com.oculus.platformsdkruntime` package
is absent. It also records managed `NameResolutionFailure` exceptions and
repeated `PointerInteraction.IsRequestingPrimaryInput` null references. Network
inspection separately finds a connected, validated network and successful DNS
resolution for `graph.oculus.com`. These observations do not establish the
precise server-side cause of the Quest's failure.

Read-only inspection of that headset's Horizon implementation establishes why
a generic ADB helper is not automatically a Bigscreen account provider:
`CallerUtils.getCallingPackage` derives package identity from Binder UID;
`ExternalPlatformLocal.getAppID` requires the installed library's app ID or
developer access to that app. Application authentication includes signer and
version and declares device-scoped authorization. No helper was installed,
no credentials were exported and no identity checks were changed. The local
system APK/code used for inspection stays under the ignored investigation
directory. Connecting this Quest does not give Klepton an authenticated
Bigscreen session.

There is a distinct UI routing problem worth testing independently. The APK's
`VRWrapper.IsOculusStore` returns true whenever `Platform.IsAndroid` is true,
before consulting any store metadata. Changing the reported installer would
not route it to email login. Its existing `TabletUI.GoToLogin` method does open
an account screen: a strict simulator experiment invoked that method once on
the active component through the IL2CPP embedding API. The method returned
without exception and the simulator visibly displayed **Create Account** with
email/username/password fields and a **LOG IN** button. The Oculus notice still
needs dismissing over it.

This experiment is enabled only by
`KL_PROBE_INVOKE=Bigscreen.UI.TabletUI::GoToLogin`, once at frame 1800. The new
generic probe accepts only an explicitly named zero-argument method and exactly
one active component; it does not modify the APK, provide an identity or alter
server authentication. Its disabled-state regression and both visionOS runtime
build/link gates pass. The normal installer path does not enable the probe.
Actual email sign-in and subsequent account/profile operations are still
unverified; reaching the account screen alone is not a login result.

New local evidence lives under `build/bigscreen-investigation/quest-reference/`:

- `reference-report.json`: version, network and platform findings without
  credential values.
- `bigscreen-relaunch-redacted.log`: app-only startup stream, sanitized before
  persistence; no device-wide log buffer was cleared.
- `store-selection-disassembly.txt`: read-only store-routing inspection.
- `simulator/manifest.json`, `simulator/rebuild.py`: dedicated simulator probe
  build, preserving the existing data container.
- `mprobe-gate-test.log`, `email-probe-runtime-build.log`: disabled probe gate
  and both platform runtime/link checks.

### Email login rejection investigation

The user reached the existing Login form but received “Couldn't log in”. The
running simulator logged `HTTP error: Input error. Call GetErrorList() to get
the full list.` twice per attempt, without the individual validation errors.
This establishes a reported HTTP input error, not successful login or a known
wrong password. The startup Meta-provider error remains a separate limitation.

A temporary, read-only debugger probe is armed on the existing simulator process
for the next attempt. It observes only whether login fields are empty, whether
the email contains an at sign or outer whitespace, the HTTP response status,
and allowlisted words/field names from CloudException validation details. It
does not log entered credentials, response tokens or arbitrary response text.
No APK or server response is altered. Login and playback remain unverified.

The installer diagnosis and visionOS warning now recognize this specific input
error ahead of the generic missing Meta-provider warning, with instructions to
check text entry and collect logs if the form is correct. The installer keeps
independent crash and unsupported-runtime errors at higher priority.

The next live attempt reached `Accounts.Login` with nonempty email and password
strings, but the email string contained no at sign. `HandleLoginResponse` received
HTTP 422 and `CloudException.GetErrorList` contained an email-not-valid validation
message. No credential values were retained. This confirms email validation
failed; it does not establish whether a username was entered, text entry dropped
a character, the password is correct, or later Meta-dependent login steps work.
The existing simulator was restarted after the debugger paused on Unity GC
signals; SIGUSR1 and SIGXCPU now pass through without debugger stops.

### Successful email login and the browser lifecycle crash

After replacing the accidentally entered username with the full account email,
the user logged in and returned to My Room. The read-only debugger recorded
HTTP 200 from `Accounts.HandleLoginResponse`. This verifies Bigscreen's own
email/password authentication for this APK in the simulator. The app still
reports missing Oculus identity during its profile/purchase follow-up; Meta
account integration, social features and movie playback are not verified.

The apparent freeze after login was a real strict-JNI abort on the inherited
instance method `com/vuplex/webview/WebView3D.pause()V`. Its original Java
implementation queues `android.webkit.WebView.onPause()`; `resume()` queues
`onResume()`. These are independent of the separate global pauseTimers API.
The WebKit adapter now implements both instance operations: it retains the page
and last texture, suspends media through WebKit's public playback-suspension
API, and stops snapshots until resume. An explicit setRenderingEnabled(false)
continues to apply after resume, and other browser views remain independent.

Validation: the strict JNI instance-call regression passes, including failed
initialization and calls after destroy. The real WebKit test verifies that one
view's texture stops advancing while another continues, and that resume does
not override a disabled renderer. Device and simulator runtime link gates and
Swift typechecking pass; the shared XCFramework retains both platforms.
The updated simulator app is installed with its data preserved. A diagnostic
AttemptLogin invocation could not locate BIG_APP_LOADER as a Unity component
and was skipped; no authentication result was invented. The ordinary account
screen is reopened for a fresh post-login transition test.

Evidence under `build/bigscreen-investigation/quest-reference/` includes
`post-login-crash-report.json`, `browser-pause-*-test.log`,
`browser-pause-runtime-build.log` and `simulator/browser-pause-rebuild.log`.

### YouTube setup, refresh and Oculus overlay transport

The user verified login and reached YouTube, where strict JNI aborted on
`WebView3D.setUserAgent(Z)V`. A direct diagnostic invocation of the existing
`HomePage.OnYoutubeSelected` callback reproduced the opening path in an offline
room and then found `setFocusedInputFieldEventsEnabled(Z)V`. Both are now backed
by WebKit: boolean UA selection chooses native mobile/desktop webpage preferences
and clears a custom UA; focus events classify TEXT/IFRAME/NONE without including
field contents. Global policy applies to current and future views; per-view
changes remain isolated. The original Java implementation confirms true selects
the default mobile UA and false selects a desktop UA. Klepton uses WebKit's
native content modes rather than advertising a Chromium engine.

Refreshing then exposed `BigWebView.ExitFullScreen(I)V`, confirmed by the user's
crash log and native SIGABRT report. It now closes WebKit media presentations;
IsFullScreen reads a cache maintained from WebKit's public fullscreenState KVO
notifications. A subsequent diagnostic invocation of the existing
`BigBrowser.OnReload` callback returned without exception or an unsupported JNI
call. This establishes that the refresh crash is addressed; movie playback is
still unverified.

The black browser panel also exposed an independent overlay transport issue.
OVROverlay requested swapchain images on Unity's main thread without a GL
context. Non-eye requests now queue real allocation at EndFrame4 on the render
thread and return NotInitialized until a retry can receive its actual handle.
The real ANGLE context is queried before any GL call; this also prevents a
debug-ANGLE null-context crash. The pending queue deduplicates requests and
ignores destroyed-layer requests. Render-thread allocation preserves the guest
texture binding and publishes a handle only after storage exists.

Legacy EnqueueSubmitLayer2 GLES submissions are now copied into Metal layer
storage at the render-thread frame boundary, before the frame fence. Vulkan's
existing path is preserved. Bigscreen also submits its browser panel with a
negative Y scale. That scale now becomes a positive physical height plus a
vertical texture reflection, so the compositor no longer rejects the panel as
invalid geometry. Reflected X scales are handled in the same way.

Validation includes the real WebKit page/UA/focus/lifecycle tests, strict JNI
regressions, a new ANGLE-to-Metal overlay regression with pixel readback, and
compositor math/Metal shader tests. Device and simulator runtime link gates and
Swift typechecking pass. The isolated simulator is rebuilt with both runtime
platforms preserved; UI testing and actual playback remain in progress. A
compressed ASTC source in the room's separate 1024x1024 overlay still fails a
framebuffer blit; this is distinct from the RGBA browser texture.

Evidence is under `build/bigscreen-investigation/quest-reference/`, including
`youtube-refresh-crash-signatures.json`, `youtube-refresh-report.json`,
`youtube-*-test.log`, `overlay-reflection-test.log`, and
`overlay-reflection-reproject-test.log`. `invoke_component.py` rearms the
explicit managed UI diagnostic on an attached simulator process; it never
provides an identity, credential or server response.

The updated simulator now visibly renders YouTube's homepage, including its
logo, search field and “Try searching to get started” text. The compositor logs
layer 6 as a real 1.139419x0.64312494 m quad at
(0, 1.5645729, -1.0796318), backed by the 1400x788 browser texture. The startup
Oculus notice was dismissed through its existing Close_BigscreenUI method and
the existing BigBrowser.Show method reopened the browser. This confirms browser
page rendering; actual video playback is awaiting user interaction.

A further source/test update propagates same-document history push/replace/hash
changes and dynamic page titles using Vuplex's URL-action schema, including
Title. This is needed for sites that switch videos without a full navigation.
Its real WebKit regression passes, and this update is installed in the current
simulator build.


### Browser taps and pointer occlusion (2026-10-29)

The original managed browser sends DEFAULT_BUTTON_ID=1 and
DEFAULT_BUTTON_STATE=1 to BigWebView.PointerDown/Up. The Android BigWebClient
routes these as touch DOWN/UP, but our adapter had interpreted button 1 using
Vuplex's explicit MouseButton enum, producing a right-click. The Bigscreen
adapter now translates these touch events to primary DOM taps. The separate
Vuplex mouse API retains right/middle button semantics. A new full JNI-to-real
WebKit regression (`tests/t_bigscreen_web_input.m`) failed on its DOM button
assertion before the fix and passes afterwards, including text focus and an
explicit Vuplex context-menu regression. It does not record entered text.

Bigscreen's browser layer 6 submits flags 0x44: OVRPlugin's onTop bit 0 is clear,
so the browser is an underlay. The compositor previously drew every quad after
the eye, covering the Unity pointer and popup. EnqueueSubmitLayer2 now decodes
composition order into an owned field, independently of other producers' flag
formats. The visionOS compositor draws those quads before the eye and uses the
guest's authored eye alpha with premultiplied blending only when a real underlay
was drawn. Ordinary eyes remain opaque. The underlay pass does not write depth
when an eye will follow, so it cannot discard the far-plane eye picture.

Metal pixel tests verify that an opaque laser pixel covers a browser, transparent
eye holes reveal it, premultiplied edges blend correctly, and the default eye
still ignores zero source alpha. The real ANGLE-to-Metal test also verifies
both onTop=1 and flags=0x44 composition metadata. The xros and xrsim runtime
link gates and full simulator app build pass. The current simulator process is
23047; YouTube renders, and the Oculus notice is visibly in front of its page.
The user confirmed that the laser appears over YouTube and the search box
responds to clicks. Actual moving video, audio and refresh after playback are
still awaiting validation. No Meta platform authentication is supplied by these
changes.

Evidence: `browser-input-before-fix.log`, `browser-input-after-fix.log`,
`browser-underlay-test.log`, `browser-underlay-image-test.log`,
`browser-input-layer-xros.log`, and `browser-pointer-rebuild.log` under the
ignored `build/bigscreen-investigation/quest-reference/` directory. The native
input trace records only integer routing, coordinates and buttons.


### Moving-video snapshot regression (2026-10-29)

`make build/t_webview_video && ./build/t_webview_video` generates a local H.264
clip with three alternating colors, plays it through the shipping WKWebView
adapter, and verifies that all three decoded colors reach BGRA texture snapshots
before and after reload. The test also checks ready state, paused state and media
error status. It runs on macOS, uses no network or account, and mutes its fixture;
it does not establish audio or YouTube playback in the visionOS simulator.

The first fixture assertion incorrectly required near-exact RGB endpoints after
lossy H.264/YUV conversion. Decoder samples were colored rather than black;
using a tolerance that distinguishes the three primaries makes the unmodified
adapter pass. A temporary canvas experiment was removed. No video capture
workaround or runtime change from this experiment is installed or retained.
Evidence: `webview-video-refresh-test.log` (successful final test) and
`webview-video-build.log` in the investigation directory. The simulator remains
on the build whose page input and pointer visibility the user confirmed.

The user supplied https://www.youtube.com/watch?v=K-Dt5ssd6F4 for the public
video test. A playback outcome was not included with the link, so moving video,
audio, and refresh for that URL remain unverified.


### Direct video-page navigation (2026-10-30)

At the user's request, the linked YouTube watch page was opened directly in the
running visionOS simulator, without another Bigscreen login. The page and its
video image are visibly unobstructed in the browser panel. Moving playback and
audio have not yet been confirmed.

Calling BigBrowser.Show alone leaves the home page layered above the browser.
The working navigation is TabletUI.GoToPage(BigUIState.BigBrowser, false, true,
false), where the APK metadata assigns BigBrowser=1. Invoke it through
il2cpp_runtime_invoke on the attached Klepton Guest thread at kl_mprobe_tick,
with exactly one active TabletUI instance, and check the managed exception.
Then kl_webview_load on the existing main browser's host opens the supplied
watch URL. For this process the main JNI browser is ID 4; resolve it afresh
before reuse. The debugger was detached and the simulator left running.

Resolve the il2cpp exports with kl_sym(kl_find_image("libil2cpp.so"), name).
The optimized static `il` aggregate is not reliably materialized by LLDB:
several fields read instruction bytes instead of function pointers. Explicit
export addresses worked. No account state or authentication response was changed.
HomePage.Minimize disables the whole tablet, including this browser; it is not
the correct way to remove the home page overlay. EnableTabletUI restores that
case before selecting the browser's page through GoToPage.


### Paused video and audio check (2026-10-30)

After the user reported no sound, read-only media diagnostics found the video
paused at 85.662 seconds, unmuted, with player volume 1 and no media error.
The WKWebView itself was rendering and not suspended. AVAudioSession was in
Playback category, routed to Speaker, with outputVolume approximately 0.6.
Calling the current video's play() through the existing WebKit adapter resumed
it. A subsequent check found paused=false, muted=false, volume=1, time=105.587,
one audio track, and no play() rejection or media error. Audible output remains
awaiting user confirmation. No runtime source change was needed for this step.

The native debugger was detached after each check. The diagnostic JSON records
only enumerated media state and counters, never document text, form fields,
credentials, account data or cookies. WebKit's optional decoded-byte/frame
counters returned zero and are not used to infer silence or decode failure.
For safe main-thread evaluation from LLDB, NSOperationQueue.mainQueue's
addOperationWithBlock worked; dispatch_get_main_queue's optimized inline helper
was unavailable in the debugger expression environment.


### Simulator audio routing resolved (2026-10-30)

The user confirmed that the entire simulator was silent, including room/menu
sounds. CoreAudio showed both KleptonBigscreenProbe and its WebKit GPU process
actively using the Mac's default Elgato Wave XLR output. That device was unmuted
at approximately 68% volume. Wave Link's selected monitor output was FIIO K19.

Device Hub > Device > Sound > Output > FIIO K19 changed only this simulator's
output. CoreSimulator's audio port then reported the explicit FIIO route, and
both game and WebKit process output devices changed from AudioDeviceID 123
(Wave XLR) to 119 (FIIO K19). The Mac default output remained 123. The user then
confirmed “I hear it now” and “Yes, sound works.” This resolves audible playback
for the current test environment. No audio-code change or app rebuild was needed.
Do not hard-code this user's output device into the app or installer.

For this environment, if audio is silent again after a route/device change,
check Device Hub's simulator output against the device being monitored, along
with the video's paused/muted state. Playback was initially paused and was
started directly before correcting the output route. The isolated simulator is
left running on the supplied public YouTube video with audible sound confirmed.


### Normal email-login fallback (2026-10-30)

The Quest build's Go Online path retries Meta authentication instead of exposing
its real email/password form. Earlier manual GoToLogin/GoToPage debugger calls
proved the UI existed, but did not fix this normal route. The runtime now has a
compatibility fallback for package com.bigscreenvr.bigscreen, version 4688857.
After the Oculus SDK consumes a failed logged-in-user message, a bounded request
is handled between Unity frames on its attached guest thread. The fallback uses
the IL2CPP embedding API and the app's own navigation: it reads Login and
PopupMessage from BigUIState metadata, ends the platform popup, and opens Login
through TabletUI.GoToPage. It resolves exactly one active tablet and retries
briefly while the UI initializes. It does not read form values or supply any
account, credential, token or successful server response.

An existing Bigscreen websocket connection prevents automatic navigation, so a
later failed Meta request cannot interrupt a connected session. Only failed
logged-in-user completions trigger the fallback; proof, token, organization,
purchase and other platform failures retain their normal handling. Other APK
packages/versions are untouched. KL_BIGSCREEN_EMAIL_LOGIN=0 disables the fallback
for diagnostics. The normal installer/runtime build includes it; no probe env
setting is needed.

The 11-case mock regression covers no request, wrong package/version, opt-out,
normal and duplicate failures, a connected session, delayed UI, managed failure,
missing UI metadata and bounded timeout. It is included in make check. Typed
platform-error and disabled managed-probe tests pass; device and simulator
runtime link gates and the full simulator app build pass. The freshly restarted
simulator (PID 98118) automatically displayed the actual Login form with Email,
Password, LOG IN, RESET and SKIP, with no Oculus notice covering it and no
KL_PROBE_INVOKE navigation. A fresh user sign-in is pending. The FIIO K19 simulator
output route was retained across the app restart.

Evidence: email-fallback-test.log, email-fallback-platform-test.log,
email-fallback-probe-gate-test.log, email-fallback-runtime-build.log and
email-fallback-simulator-build.log under the ignored investigation directory.


### Apparent freeze and simulator input recovery (2026-10-30)

The user could not interact with the new Login form and later clarified that the
entire simulator window could not be moved. The guest was still presenting near
60 Hz, the app was foreground-active, and no crash was recorded. A native
breakpoint verified that even the compositor's raw spatial-event callback was
not reached. Application/window metadata showed an active CPImmersiveScene with
its normal visible event window; no credential or text-field values were read.

The compositor now registers and clears onSpatialEvent on the main thread,
matching the public API's main-actor contract, and logs the first raw event per
renderer. Swift typechecking and the full simulator build passed. This alone
did not restore delivery. Reopening immersion and rebooting the isolated
simulator also did not restore delivery in automated tests. A temporary 2 m
reprojection-depth experiment made no difference and was not retained; the
reboot restored the normal environment/depth settings.

Restarting Device Hub with its “Quit and Keep Simulators Running” action and
reopening the compact simulator window restored raw and accepted spatial input.
The guest stayed alive through the interface restart (PID 4863), and a subsequent
test click produced the first callback marker and two accepted spatial events.
Device Hub changed from PID 2245 to 8074. The Login form remains visible and the
FIIO K19 simulator route is retained. The user subsequently confirmed that window
movement and login input respond, then reached an online room join. The guest was not
established to be deadlocked; do not label the initial symptom a login crash.

Evidence: login-input-before-simulator-reboot.log, login-input-window-state
metadata in the container, devicehub-input-sample.txt, login-input-swiftcheck.log,
login-input-main-thread-build.log and login-input-recovery-report.json in the
ignored investigation directory. These retain input/thread/window metadata,
not login contents.

### Online room join: real SCTP socket failure (2026-10-30)

The user's next room join crashed in `libBigSoup.so+0x22b0a0`,
`register_ulp_info`, with a null socket. `BigData::CreateConnection` passed the
unchecked result of `usrsctp_socket` to `usrsctp_set_ulpinfo`. In the APK's native
code, `usrsctp_socket` stores `socreate`'s result through `__errno`, then reads it
through a second `__errno` call. Klepton's previous accessor translated Darwin
errno on every access, discarding the guest's successful zero assignment and
replacing it with stale Linux EAGAIN (11). This falsely returned a null socket.

`runtime/libc/kl_errno_bridge.c` now supplies BigSoup with a thread-local Linux
errno slot. Guest writes are committed before each resolved native function
import. A signed ARM64 tail-call barrier preserves integer, vector, stack and
indirect-return arguments. Import resolution applies this only to BigSoup's
ELF, dylib and signed framework filenames; other libraries retain their existing
bindings. `KL_BIGSOUP_ERRNO=0` restores the previous behavior for diagnosis.
The APK and authentication responses are unchanged.

Validation uses the original APK's actual SCTP implementation in
`tests/t_bigscreen_sctp.c`: with the bridge disabled, an unrelated old EWOULDBLOCK
produces a null socket and guest errno 11. With the bridge enabled, AF_CONN socket
creation succeeds, the real ULP registration completes, and an invalid domain
still returns a genuine EINVAL (22). This local test does not establish a remote
association or join a room. `tests/t_errno_bridge.c` also checks repeated native
failures, Linux/Darwin error mapping, thread isolation, arguments and a large
return value. Device and simulator runtime link gates and the simulator app
build pass. A live debugger check confirmed the signed BigSoup errno import
points to the new accessor, then detached. The patched app is running; the next
human room join remains the end-to-end check.

Evidence in the ignored investigation directory: room-join-native-disassembly.txt,
room-join-sctp-creation.txt, room-sctp-before-fix.log, room-sctp-after-fix.log,
room-errno-test.log, room-errno-runtime-build.log, room-join-simulator-build.log
and room-join-crash-report.json. No account fields, tokens or room identifiers
are needed by these checks.

### Room join follow-up: incoming video surface (2026-10-30)

The next human attempt crashed at a different point. The boot log names
`android/graphics/SurfaceTexture.<init>(I)V` as the missing JNI implementation.
While reporting that failure on BigSoup's worker thread, the draw census issued
`glReadPixels`. SimMetalHost crashed in compressed texture readback and the app
aborted after losing its Metal-service connection. The JNI failure preceded the
Metal failure; this is not the previous null SCTP socket crash.

`runtime/jni/kl_jni_surface.c` now implements the measured SurfaceTexture
constructor, updateTexImage and its Surface producer. Construction does no GPU
work. ANativeWindow_fromSurface returns the consumer's actual decoder window,
and AMediaCodec releases real VideoToolbox frames into its queue. updateTexImage
acquires the latest frame and binds its IOSurface/Metal image to the guest's
external texture on a thread with a current GL context. Native windows and
configured codecs retain the queue across JNI surface release. The imported
AMediaFormat_setFloat also preserves floating-point values. This does not add
protected media support or synthesize an incoming stream.

The draw census now reports CPU metadata without any GL calls or pixel readback.
Explicit rendering probes remain separate from fatal reporting.

`tests/t_surface_decode.c`, included in make check, creates the JNI objects on a
context-free worker, configures a real native decoder surface, decodes two HEVC
frames from the repository fixture, latches the latest timestamp and reads
nonuniform, opaque 320x240 pixels from the actual ANGLE texture. It also checks
the floating-point format field and release ordering while the codec and native
window still hold the queue. This host test passes, along with browser texture,
errno, email fallback and platform-error regressions. Device/simulator runtime
link gates and the full simulator app build pass. The patched simulator is
running after recovery (PID 24550). The user then confirmed the online room join
works.
The first launch after installation exited before the guest booted because the
earlier SimMetalHost failure also broke the simulator's RealityKit device
initialization. Restarting this simulator with its data retained restored the
Metal service. The new process reaches the visible Login form and keyboard,
receives spatial input and has no current native fault or missing-JNI diagnostic.

Evidence: room-surface-test.log, room-surface-runtime-build.log,
room-surface-simulator-build.log, room-surface-*-regression.log and
room-join-video-surface-report.json in the ignored investigation directory.
The original platform API contracts are documented by Android for
[SurfaceTexture](https://developer.android.com/reference/android/graphics/SurfaceTexture)
and [Surface](https://developer.android.com/reference/android/view/Surface).

### Saved Bigscreen session after restart (2026-10-30)

The user confirmed the room join and requested persistent sign-in. Bigscreen
already writes its refresh token into settings.xml under persistentDataPath;
SharedPreferences and the simulator container survive application updates.
Startup was stopping at Quest account service failure before reaching the
existing saved-session check. No password store or credential-entry automation
is needed.

After that genuine Meta failure, the compatibility flow now asks
Accounts.GetRefreshToken whether a saved token exists, then starts the app's
CheckAccessTokenStatus task. That method performs the original status request
and RenewAccessToken flow, including server token rotation and persistence.
The host retains the task through an IL2CPP GC handle, polls only its completion
and boolean result on Unity's attached thread, and never copies token text.
Only a true result starts BIG_APP_LOADER.OnLogInSuccess, which fetches the
profile and connects normally. Missing tokens, rejection, cancellation and
faults return to the email form. A pending server task uses the existing client
timeout; it does not start a competing password request.

The first live restart verified server acceptance and a connected websocket,
but left the existing Login panel visible (state 25). The flow now waits for
the real websocket connection and calls TabletUI.GoHome if that panel remains
open. Other pages and already-connected sessions are preserved. The automatic
session attempt runs once per process, so it does not undo a later logout.
KL_BIGSCREEN_RESUME_SESSION=0 skips this behavior.

Twenty cases in tests/t_bigscreen_login.c cover the original gating and UI
behavior plus accepted/rejected/faulted/canceled tasks, GC handle lifetime,
delayed completion, empty saved tokens, opt-out, invocation failures and
navigation only after connection. The targeted tests and both platform link
gates pass. The final simulator restart (PID 37154) verified server acceptance,
an online websocket and automatic return to the visible My Room home panel,
without credential entry. A second process start reused the token persisted
through the earlier session renewal and app update. No native fault was recorded.

Evidence: session-resume-test.log, session-resume-runtime-build.log,
session-resume-simulator-build.log, session-state-debug.log and
session-resume-report.json under the ignored investigation directory.

### Social TV channel: native streaming player (2026-10-30)

The user's social TV channel attempt aborted on the missing JNI method
BigMediaPlayerAPI.StartStream(ILjava/lang/String;)V. This uses the APK's separate
ExoPlayer media path, rather than Vuplex/WebKit or BigSoup's peer decoder.
The boot log preserves the method signature; no source URL is needed to identify
the failure. The native fault file was absent because this was a deliberate
strict-JNI abort.

runtime/media/kl_stream.m now supplies an AVPlayer adapter for ordinary streams.
AVPlayerItemVideoOutput produces real BGRA IOSurface frames; a main-thread timer
records the latest frame, time, dimensions and state. Native playback supplies
audio. On Unity's guest thread, the JNI bridge delivers the APK's registered
Ready, Playing, Buffering, Ended and PlaybackError callbacks. Frames bind to the
guest's external texture only with a current ANGLE context, preserving the
previous texture binding. The frame loop handles upload even when the guest's
texture-id query comes from another thread. Pause, resume, seek, volume, stream
replacement and shutdown use real player state. Generation checks discard
outdated callbacks, including callbacks invalidated by a reentrant Stop.

Missing/unsupported addresses, unavailable sources and unsupported MPEG-DASH
return the app's source-error JSON callback. Protected-media entry points report
DRM absence through its existing callback. This adds no Widevine/FairPlay bridge
and does not claim protected playback. Error diagnostics use numeric native
codes and fixed categories; signed URLs and arbitrary NSError messages are not
logged.

tests/t_bigscreen_stream.m exercises the exact two-argument StartStream signature
against HTTP HLS on an ephemeral loopback server and also checks a local H.264
MP4. Both fixtures are generated locally with AVAssetWriter; no external account,
service or downloaded content is used. Both reach real Ready/Playing/Ended events
and red, blue and green GPU texture samples. The test also checks pause/resume,
floating-point seek and volume through JNI, preservation of texture bindings,
asynchronous open failure, unsupported-format/DRM callbacks and reentrant stop.
The stream test, existing JNI regression, both platform runtime link gates and
simulator app build pass. The installed app (PID 49193) restored its session and
reached My Room. A retry of the actual social TV channel is pending; the local
fixture result is not proof of that channel's format, authorization or playback.

Evidence: social-tv-boot.log, social-tv-stream-test.log,
social-tv-stream-test-build.log, social-tv-runtime-build.log,
social-tv-jni-regression.log, social-tv-simulator-build.log and
social-tv-crash-report.json in the ignored investigation directory.
The adapter uses Apple's public
[AVPlayerItemVideoOutput API](https://developer.apple.com/documentation/avfoundation/avplayeritemvideooutput).

The first real channel retry stayed alive and entered the room, but the
environment faded to black while the menu remained visible. Read-only live
metadata showed a ready, playing 1920x1080 stream at about 153 seconds, 2,866
produced/notified frames and zero uploaded frames. The Unity frame pump and
texture query had no current GL context, so their guarded upload correctly
declined the operation but never reached the actual render thread.

The bridge now registers a render service with kl_glfb. Presentation invokes it
on the current GL thread; if submission arrives without a context, the existing
deferred capture path invokes it before the next draw/flush on that thread.
No context is stolen or created on Unity's main thread. The local stream test
now exercises both direct presentation and a context-free submitter followed by
a render-thread flush, checking that video storage exists before the texture-id
query can perform an upload. H.264/HLS GPU colors, transport/error behavior and
GL binding preservation still pass. Both platform builds pass and the updated
simulator (PID 52752) restored its session. The user then confirmed that the
live channel video and room are visible. Channel audio was not separately
confirmed in this retry.

Additional evidence: social-tv-stream-state.log, social-tv-dark-room-boot.log,
social-tv-render-test.log, social-tv-render-runtime-build.log and
social-tv-render-simulator-build.log.

### Lobby AVPro player and avatar shader diagnosis (2026-10-30)

The next Lobby join aborted in strict JNI dispatch at AVProMobileVideo's
GetPluginVersion, before any video was opened. This is the APK's separate
AVPro 1.9.17 integration, not BigMediaPlayerAPI. The new kl_jni_avpro family
implements its manager/player interface using the same system media backend.
Construction loads the original libAVProLocal and runs its JNI_OnLoad and
nativeInit so its cached Java methods, native getters and rendering callbacks
remain usable. The reported 1.9.17 is the original Java wrapper's literal.

The bridge supplies actual dimensions, duration, playback/seek/buffering state,
audio/video metadata and texture frames. It supports pause, play, stop, seek,
loop, rate and volume. Decoded frames upload only on a current GL thread.
Render services now append with deduplication so this player and social TV
coexist. Closed players release rendering resources on that thread and recycle
their indices; native/source failures become AVPro's consumed error 100 rather
than terminating strict JNI dispatch. Unsupported byte offsets, HTTP headers,
extra audio-track selection and spatial audio options remain explicit limits.

The initial installed adapter entered the live Lobby, restored its saved session
and stayed connected (read-only websocket getter true, tablet state 108). The
user confirmed the Lobby looked good. Its two tutorial/poster videos then exposed
jar:file APK StreamingAssets URLs; those resolve to the existing staged assets
instead of being sent to AVPlayer as unsupported addresses. This does not
require extracting or re-downloading any user media.

tests/t_avpro.m loads the real original Android plugin, generates a local video,
opens it via the APK asset URL contract, and tests native JNI getters against
real decoding and three GPU colors. It also checks pause, seek, floating-point
volume, consumed asynchronous errors, index reuse and coexistence with another
render service. The existing H.264/HLS stream regression, JNI regression and
saved-session regression pass. Both visionOS runtime link gates pass.
The build including APK asset URL resolution is installed (PID 63242), has
restored its connected session and displays My Room. Live Lobby video panels
have not yet been checked in this final build.

The user also reported magenta avatar silhouettes. Captured failed fragment
shaders contain the actual _SkinTones, _CustomColors, _IrisOffsets,
_MicAmplitudes, _Gradients and _HiddenAvatarFlags buffers. ANGLE rejects their
std430 readonly shader-storage blocks under GLSL ES 3.00. A multiview variant
also fails on GL_OVR_multiview2. This is a remaining rendering compatibility
problem; the Lobby join/video adapter does not fix the avatar shaders.

Evidence in the ignored investigation directory: lobby-boot.log,
lobby-avpro-test.log, lobby-tv-regression.log, lobby-jni-regression.log,
lobby-login-regression.log, lobby-runtime-build.log, lobby-simulator-build.log,
lobby-assets-build.log, lobby-assets-simulator-build.log,
lobby-session-state.log and lobby-failed-shaders/*.glsl. Diagnostics omit
credentials, media source URLs and room identifiers.

### Packed avatar storage buffers (2026-10-30)

The avatar fragment shader's six read-only std430 blocks now translate to
std140 uniform blocks containing packed uvec4 words. Each scalar read selects
the original 32-bit word without changing the supplied float bit pattern or
introducing the padding of a std140 scalar array. The conversion accepts only
the measured scalar struct and read syntax of the named avatar buffers;
writable buffers and other layouts retain their normal unsupported errors.
Shader layout binding numbers are preserved before the general GLSL rewrite.

Program reflection presents those resources as shader-storage blocks and their
buffer variables, while excluding implementation blocks/members from Unity's
ordinary uniform enumeration. Normal uniform block and uniform indices map
back to the actual ANGLE resources. The host reserves the final eight uniform
buffer bindings and copies real guest data into 16 KiB GPU buffers. Generic
SSBO bindings remain independent of COPY_WRITE and UNIFORM_BUFFER state.
Buffer subdata, mapped writes, range offsets and deletion are handled; upload
generations avoid copying unchanged data and notice shared-context uploads.
This is read-only scalar storage compatibility, not general compute/SSBO-write
support. Buffer ranges above 16 KiB are diagnosed explicitly.

tests/t_avatar_buffers.c compiles and links the captured real avatar fragment
shader supplied from the ignored investigation directory. A locally authored
shader then reads all six buffers through real ANGLE/Metal rendering and checks
GPU colors, permuted layout binding numbers, reflection isolation, buffer
subdata/mapped/range updates and restoration of unrelated GL bindings. The
H.264/HLS streaming regression still passes, as do both visionOS runtime link
gates. The first updated simulator build (PID 71235) restored its session and
joined the live Lobby. Its real avatar shader compiled/linked with no failures
or oversized-range warnings, and read-only host metadata confirmed all six
original storage binding numbers (0 through 5). GPU tests also cover consumed
buffer-variable reflection properties and buffer deletion with GL state
preservation. The final simulator build (PID 72952) includes these cleanup and
reflection checks. Its first visual retry showed black avatars, leading to
the additional fixes below.

Evidence: avatar-test.log, avatar-runtime-build.log, avatar-tv-regression.log,
avatar-simulator-build.log, avatar-join-probe.log, avatar-live-state.log,
avatar-final-simulator-build.log and avatar-final-join-probe.log in quest-reference under
the ignored Bigscreen investigation directory.

The GLES 3.0 startup workaround also disabled Unity's storage-buffer binding
path. Current imports recommend GLES 3.1, with Unity's own
`-disable-gpu-skinning` argument delivered in the `unity` launch Bundle and
Intent extra (and newer initJni's command-line parameter). The argument is
scoped to the measured Bigscreen package/version; the original Unity binary
contains this switch, also documented in Unity's
[2020.2 release notes](https://unity.com/releases/editor/beta/2020.2.0b7).
Read-only fragment-buffer binding/range limits are supplied, with no compute
stage blocks advertised. Disabling GPU skinning avoids Internal-Skinning.main.

Unity 2020 uses both ES3 and ES3.1 reflection. The implementation now hides
compatibility uniform blocks/members consistently across both API families,
including glGetProgramiv, glGetActiveUniform*, glGetUniformIndices and the
program-interface queries. Exposing the internal blocks through ES3 caused a
native shader-reflection allocation crash at libunity.so+0x453b70; the expanded
reflection regression checks this view along with GPU colors and storage data.

Higher GLES descriptions exposed a separate mixed uniform-location contract:
Unity uses baked sampler locations as texture units but also queries uniform
locations by name. A 360-float array moved ANGLE sampler locations above 390,
causing invalid glUniform1i assignments and lost avatar/font texture inputs.
For this Bigscreen build, the bridge preserves original pins and allocates
unpinned uniforms/whole arrays after them. Queries, setters and getters now
share that namespace. Other profiles retain their existing location behavior.
tests/t_uniform_locations.c checks baked and named writes mixed on one program,
a 360-element array, sampler-unit assignment, getter round trips and real GPU
color. The installed build (PID 92524) restores readable text and has no invalid
sampler assignment errors. The user confirmed normal skin/clothing colors and
facial details on nearby live Lobby avatars. The draw trace also confirms all
six original source buffers and their GPU mirrors are bound (464, 1024, 320,
160, 1152 and 160 bytes respectively), with no shader compilation failures.

The installer offers GLES 3.1 and recommends it for the exact tested APK;
request validation and generated target code have corresponding coverage.
The installer Swift source typechecks, its app builds, the 41 installer tests
pass, and GLES version descriptions pass for defaults, 3.0/3.1, invalid input
and GLES2. An earlier cancellation test raced under concurrent build load;
the unchanged test passed on the full-suite retry.

Additional evidence: avatar-es3-runtime-build.log, avatar-es3-simulator-build.log,
avatar-uniform-test.log, avatar-gles-version-tests.log, avatar-installer-tests.log,
avatar-installer-swift.log, avatar-sampler-runtime-build.log and
avatar-sampler-simulator-build.log. The updated Mac installer is built at
build/installer-macos/Build/Products/Debug/KleptonInstaller.app. H.264/HLS media
regression, saved-session tests and uniform getter/property-query coherence
checks pass alongside the graphics and both-platform build gates. Verification
here is on the visionOS simulator; this avatar path has not been checked on a
physical Vision Pro.

### Intermittent outgoing echo (November 6, 2026)

The user clarifies that room participants previously reported echo in their
outgoing voice, but had no complaints in the latest session. Successful voice
transmission is preserved as an established result; the intermittent echo is
not treated as a permanently bad microphone or proven local playback reverb.

The old native capture unit disabled its output bus and played the game through
a separate RemoteIO. Native voice processing therefore lacked the actual game
speaker mix on its reference bus. Opted-in capture now uses one duplex VPIO unit
for both directions, retaining noise suppression/AGC and supplying the real
speaker samples to echo cancellation. The existing default session mode and
minimum other-audio ducking policy remain. Capture downsampling also gains an
anti-alias FIR filter, with native-rate samples left unchanged.

`make voicecheck` exercises the speaker-reference connection and recovery with
fake AudioUnits plus synthetic capture and mixing checks. This does not measure
acoustic echo attenuation on the Vision Pro. Room participants still need to
confirm voice clarity and the absence of echo during speech over game/movie
playback, including after Home/Resume. No physical listening result is claimed
for this change.

### Hollow/reverberant voice after native capture starts (November 6, 2026)

The user reports that additional room participants describe their outgoing
voice as echoing in a bathroom. They could not distinguish a delayed duplicate
from hollow reverberation. This is an audible-quality failure despite successful
voice transmission; the earlier no-complaints session does not resolve it.

A newer physical install already contained duplex capture and the PHASE float
format correction. Diagnostics job `2429e006c0a2`, collected from installation
`a2f2810fe23e`, shows VoiceProcessingIO at 48 kHz mono, a shared speaker reference,
147.39 seconds captured / 147.38 read, no dropped frames, and last render error 0.
No raw fallback or rejected voice-property request is logged. These observations
establish working capture, not acoustic echo attenuation or speech clarity.

Session restoration still enforced `.default` even when native AUVoiceIO had
selected `.voiceChat`. Apple's [chat-mode documentation](https://developer.apple.com/documentation/avfaudio/avaudiosession/mode-swift.struct/videochat)
states that instantiating voice I/O implicitly selects voiceChat. The host now
uses a lock-free native hint to retain chat mode while a voice unit is selected,
including initialization. Playback-only and raw capture remain default mode;
the fallback hint clears on initialization failure and teardown. Regression tests
cover implicit chat-mode preservation without redundant category changes and
returning to default for raw fallback. Native initialization also reads back and
logs the effective bypass/AGC properties instead of inferring their state from
successful pre-initialization requests.

`python3 tests/test_visionos_audio.py`, `make voicecheck`, the Apple offline audio
graph fixture, `make swiftcheck`, and both visionOS runtime platform gates pass.
The mode conflict is a concrete policy issue; its contribution to the reported
sound is an inference. A physical listening comparison is still required, and
no microphone samples were recorded for these diagnostics.

Installer job `102cb6d3b3f5` completed the physical update, verified and restored
saved writable data, and finished APK/asset transfer. The app was stopped at
the install check and was not launched by this workflow. The next manual room
session should compare voice quality with the same participant and include a
brief playback-quiet interval to distinguish room coloration from speaker
feedback. Effective bypass/AGC and session-mode logs are available on that run;
no acoustic improvement is claimed until the user reports it.
