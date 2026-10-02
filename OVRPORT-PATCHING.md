# ovrport patcher evaluation

Tested the official ovrport CLI 1.2.3 with stable runtime payload 3.3.0 on all seven supplied Quest APK variants. All seven patches completed and all output ZIP integrity checks passed. **The patches do not fix the current Klepton startup failures.**

Repository cloned to `/Users/drewwallace/ovrport-app`, revision `1c7cb335c81369fbfd2957ad0edcb238956a8e3c`. Official CLI archive SHA-256: `3239408ff1e97ad016916825c216dc3016c0ffa7db7c317221ed3f1244441594` (matched GitHub release metadata).

Selected patches: native library copying, OVRPlugin replacement when VrApi is present, removal of Java Frida load calls, Unreal device identity compatibility, and Unreal activity compatibility. Exact commands are recorded in [patch-results.json](../ovrport-app/build/klepton/patch-results.json). Originals were not modified. Walkabout was excluded because the supplied build is Steam/OpenXR.

## Results

| Target | Main native library | Klepton host startup | Patched APK |
|---|---|---|---|
| `4xvr-11026` | SHA-256 unchanged | `clock_nanosleep` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/4xvr-11026-ovrport.apk) |
| `4xvr-11026-vrp` | SHA-256 unchanged | `clock_nanosleep` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/4xvr-11026-vrp-ovrport.apk) |
| `roborecall-41778` | SHA-256 unchanged | `AConfiguration_getMcc` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/roborecall-41778-ovrport.apk) |
| `roborecall-41904` | SHA-256 unchanged | `AConfiguration_getMcc` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/roborecall-41904-ovrport.apk) |
| `roborecall-47091` | SHA-256 unchanged | `AConfiguration_getMcc` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/roborecall-47091-ovrport.apk) |
| `roborecall-47091-patched` | SHA-256 unchanged | `AConfiguration_getMcc` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/roborecall-47091-patched-ovrport.apk) |
| `roborecall-47091-v76` | SHA-256 unchanged | `AConfiguration_getMcc` | [APK](/Users/drewwallace/ovrport-app/build/klepton/patched/roborecall-47091-v76-ovrport.apk) |

## Why the patches do not resolve these failures

For 4XVR the patcher changes `libopenxr_loader.so` and `libovrplatformloader.so`. For Robo Recall it changes `libOVRPlugin.so` and `libovrplatformloader.so`. It also adds Android headset runtime libraries. Klepton intercepts these runtime library names with its own implementations (`runtime/kl_dl.c`); Android headset binaries do not supply the missing Klepton libc/NDK implementations. The Unreal activity patches modify Java bytecode, which Klepton does not execute. Neither `libUE4.so` nor `libvr4p-oculus.so` changed in any variant.

Each patched APK was extracted into an isolated test tree and run through the existing `build/m_boot` with its corresponding registered target, a 12-second guest alarm and three requested frames. All seven aborted on the unresolved imports above. This is a macOS host runtime probe, not a visionOS device test. The existing visionOS app builds still use the original APK inputs; there is no demonstrated benefit to rebuilding them from these patched copies.

Full commands, native-library comparisons, hashes and exit codes: [validation-results.json](/Users/drewwallace/ovrport-app/build/klepton/validation-results.json). Individual startup logs are in the same directory as `<target>-klepton-boot.log`.

Subsequent Klepton runtime work implemented the libc/Android NDK APIs identified
here. Current startup results are in [OVRPORT.md](OVRPORT.md); the table above
preserves the original patcher comparison and is not a current runtime status.
