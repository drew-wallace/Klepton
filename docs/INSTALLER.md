# Klepton APK Installer

A native Mac frontend for a **prepared Klepton checkout**. It converts one
standalone ARM64 APK, signs its native libraries and visionOS app, installs it
on a paired Vision Pro, and stages its APK/assets/game data. It does not launch
the installed app. Conversion and installation do not establish playability.

## Open the utility

```sh
make installer-open
```

`make installer` builds without opening it. The result is
`build/installer-macos/Build/Products/Debug/KleptonInstaller.app`.
The Mac utility is signed locally and runs without App Sandbox so it can invoke
the checkout's Xcode/Python/build tools. It is not a standalone distribution.
It locates its original checkout automatically; **Checkout…** selects another
prepared checkout. The app itself supports macOS 14 or later on Apple Silicon;
the conversion pipeline still requires the Xcode/SDK versions used by Klepton.

1. Use **Steam Library** to sign in, select a game and release branch, and
   click **Download**, **Update**, or **Use Downloaded**. Alternatively, select
   **Local APK** and drop an APK or use **Choose APK…** (`⌘O`). The importer
   snapshots and decodes the APK into ignored storage without replacing existing game trees.
2. Review the compatibility profile and Steam mode. Exact APK fingerprints
   distinguish known variants; package/version/entry fingerprints supply a
   fallback. Ambiguous and unrecognized packages stay experimental. Every
   existing Klepton profile is also available for manual selection.
3. Add matching OBBs or a complete external data folder when required. Nearby
   candidates are suggestions only. For loose data, select the directory whose
   **contents** belong at the displayed `android-files/…` destination.
4. Select a paired Vision Pro and signing team. Keep the headset unlocked and
   close the existing game before updating it. **Build & Install** stops after
   installation and staging; open the game on the headset yourself.

**Advanced** exposes the supported boot sequence, an entry library actually in
this APK, a stable bundle identifier, the relative loose-data destination, and
verbose tool output. Original CLI defaults remain unchanged. For JKXR, select
the retail `base` directory containing the `assets*.pk3` files. For Walkabout,
select the entire OBB directory, including its loose bundles.

The utility checks prerequisites; it does not install Homebrew packages,
download renderers, change Xcode accounts, or toggle Developer Mode. Steam
client/runtime payloads are downloaded only by the explicit channel **Update** buttons.
Copy the setup commands shown for missing dependencies. A signing
certificate in the keychain is not sufficient by itself: Xcode also needs a
signed-in Apple account or a valid provisioning profile for the chosen team,
bundle identifier, capabilities and device.
Signing teams are read from each usable certificate's organizational-unit
field, matched to the private-key identity by fingerprint. The identifier in a
certificate's display name is not used as a team identifier.
MoltenVK is required for known Vulkan profiles and APKs that declare a required
Vulkan feature; GLES-only profiles can use an ANGLE-only prepared checkout.

## Download games from your Steam library

Steam Library requires Node.js 22 or later and the pinned helper dependencies:

```sh
brew install node
make installer-steam-deps
```

Local APK import works without these dependencies. The Mac build remembers
the Node executable used to build it; the backend also checks Homebrew and
local nvm installations. `KLEPTON_NODE` can select a different executable.

Sign in with the Steam mobile app's QR scanner or your account name and
password. Steam Guard supports mobile approval and email/authenticator codes.
Passwords and codes are never saved. Remembered refresh tokens are stored in
macOS Keychain, passed to helpers through pipes, and excluded from logs,
command arguments, request files, and receipts. **Sign Out** removes the
remembered session and clears the displayed account library. Sign-in here is
separate from the Steam runtime's sign-in on Vision Pro.

The searchable library starts with **VR titles** and **Android bundle
available** enabled. These filters use Steam metadata, licensed depots and
Android APK launch options; Klepton's audited profile list does not limit
discovery. The library includes games directly licensed to the signed-in
account; shared family licenses are excluded. Downloads use English content.
On load, update checks focus on cached games matching both default filters.
Newly owned titles are also inspected, so new VR Android releases appear.
The first sign-in discovers the full library to establish that metadata.
Changing either checkbox fetches the full library and enables full refreshes
for the rest of the session; typing in search filters the loaded titles.
Release branches default to `public`. Other accessible branches are selectable;
password-protected branches are labeled and disabled. If the publisher exposes
multiple Android APK launch options, select one before downloading.

Selecting a game only displays its details. **Remote** shows the selected
branch's Steam build ID and branch update date. **Downloaded** shows the build
actually acquired and the UTC-recorded completion date, displayed in local
time. Missing dates are **Unavailable**. Steam build IDs satisfy the version
display; Android version names remain visible after APK inspection.

**Download** acquires a new copy; **Update** appears when its Android depot
manifest IDs differ from the current branch. Desktop-only build changes do
not trigger Android updates. **Use Downloaded** verifies and reuses an existing
copy without contacting Steam or changing its acquisition date. **Repair**
appears for damaged cached content. An update also leaves a **Use Downloaded**
option for the previous good copy. Library metadata refreshes after sign-in,
when reopening with a remembered session, and through **Refresh**. A failed
refresh marks remote versions unknown and retains local downloads.

Downloads retain the Android release's applicable licensed depots and shared
data. Transfers prefer HTTPS-capable content servers and measure each server's
network throughput separately from unpacking time. Up to six chunks download
concurrently, with at most two connections per host. The scheduler favors
faster servers, periodically samples alternatives, and temporarily avoids
failed or substantially slower servers. Successfully verified chunks are
retained when a connection is replaced within the current operation.

A chunk request is aborted after eight seconds without data, after twenty
seconds total, or when it remains below 32 KiB/s after a five-second grace
period. Requests retry on other servers with bounded backoff. These deadlines
close the underlying connection, including servers that trickle bytes forever.
Two background workers unpack downloaded chunks so decompression cannot block
network timers. A separate ninety-second watchdog stops a transfer that makes
no verified progress, even if status heartbeats are still arriving.

Progress updates every second with verified percentage, recent network speed,
estimated remaining time, active CDN count, server switches, and time since
the last verified progress. **Unpacking** distinguishes CPU work from receiving
data. ETA is based on recent verified throughput and can vary while changing
servers. Network speed counts compressed bytes received, including retries;
the main progress counter only advances for verified files/chunks.

The helper verifies chunk and whole-file Steam checksums; the backend
independently hashes every file before publishing an atomic receipt. Completed payloads
and receipts are isolated by account, app, branch, APK selection, and content
revision under `build/apk-installer/steam-library/`. Cancelled or failed
downloads leave previous completed copies intact. The cache is not garbage
collected automatically, so multiple revisions can consume disk space.

Successful acquisition runs the existing APK inspection and prepares Step 2.
Adjacent `obb/` data is selected for Step 3 automatically. The adjacent folder
also supplies its relative destination automatically, even
when an updated APK does not match a pinned compatibility profile. For Unity
games such as Walkabout this is `obb`; both OBB files and loose course bundles
go into `android-files/obb`. This packaging default does not enable old binary
adapters or certify compatibility with a new game version.

Other loose files are listed for manual mapping using the existing data controls. Downloading
does not build or install an app, and Android availability does not establish
Klepton compatibility. Split APK restrictions and runtime audit checks apply
equally to Steam downloads.

## Steam runtime and experimental APKs

Under **Steam mode**, **Steam channel** selects **Stable** or **Beta** for the
next Build & Install. The choice is remembered on this Mac. The four version
rows show the current local stable/beta versions and the latest remote
stable/beta versions from Valve's live Steam Frame ARM64 `vr` update feeds.
The versions are SteamOS payload versions, rather than Steam client's numeric
build IDs. **Refresh versions** rechecks the feeds; a failed check leaves local
versions visible and reports the connection error.

Each remote row has its own **Update** button, disabled when the local version
and build match the remote release and the local files pass validation. Damaged
local files can still be repaired. Updating downloads that channel's signed
RAUC bundle, verifies it against the pinned Valve production key (or
Valve's production/development keys for beta), reconstructs and SHA-256 checks
the signed root filesystem, and retains its Android ARM64 Steam libraries and
login UI modules. The recovery directory is only a bootstrap source; it is not
used to discover new releases. This follows the acquisition method documented
by [Steamac](https://github.com/fxgl/steamac/blob/main/scripts/steps/10-rootfs.sh).
The production CA is the same key shipped by SteamOS; the development key was
extracted from `/etc/rauc/development_keys/2549da1b.0` in the production-signed
stable 0.4.5 root filesystem.

Acquisition uses an isolated Python environment with pinned dependencies and
builds `desync` v1.1.4 into `build/steam-updates/tools/` using Go. If Go is missing,
install it with `brew install go` and retry. Allow about 13 GiB of temporary
space and several minutes per channel. The expanded OS image is discarded after
a successful update. Cancelled downloads can resume; a failed or cancelled
update leaves the previous channel pointer and its files intact. Updating one
channel never selects it or replaces the other channel. Downloading does not
modify apps already installed on the headset; Build & Install applies the
selected channel.

The initial stable fallback is the locally extracted 0.3.0 recovery runtime.
Beta must be downloaded explicitly and never falls back to stable. Each build
records its selected channel, OS version/build, source fingerprint, runtime
input hashes, login ABI header, and UI module hashes. Switching channels
invalidates previously staged Steam artifacts. The new 0.4.5 and 0.5.6 OS
payloads currently contain the same Steam client bytes. Their hash-specific
interactive session profile rebases the existing private ABI checks, preserving
live method-address verification. The credential-free WebUI transport diagnostic
remains limited to the recovery client. Unknown future client hashes are
downloaded and clearly marked as needing a Klepton compatibility update; they
cannot silently use recovery offsets. Acquisition and compilation do not verify
headset authentication or game server acceptance.

**Auto** chooses the genuine local Steam host only when the selected profile
has an audited integration and the imported SDK binaries pass its existing
size/hash checks. Steam sign-in remains in the headset app. An arbitrary SDK
match alone never identifies a game or proves server compatibility.

Other Steam APKs use **Offline diagnostics**, visibly labeled on both the Mac
and headset. This supplies no authenticated Steam services. Selecting a game
profile cannot override the pinned checks. The existing Walkabout instruction
adapter is separately enabled only for its audited original binary.

Generic imports support inferred Unity/IL2CPP, UE4, and NativeActivity startup.
If the inference is ambiguous, explicitly select the boot sequence and entry
library. There is no ART/JVM, x86 or ARM32 emulation. Split APKs, AAB, APKM, and
XAPK collections are rejected. Existing runtime exclusions still apply to
replaced/injected libraries and non-ELF `.so` files. Graphics remain under the
existing Klepton runtime. **Advanced → GLES compatibility → GLES 3.0** limits
the advertised GLES description for experimental apps whose shaders fail on
the default path; it does not replace ANGLE or MoltenVK. The tested Bigscreen
APK suggests this measured workaround automatically. Other APKs keep their
target default. The choice is validated, recorded in the build manifest and
baked into the generated app, so it applies to launches from Home View and
requires rebuilding. An invalid value stops the build. The diagnostic
`KL_GLES_VERSION=3.0` environment override makes the same choice for host or
simulator probes. A lower GLES description does not implement missing browser,
media or online services.

## Progress, backups, and recovery

One installer job may use a checkout at a time. Progress is stage-based:
inspection, validation, local dependency build, translation, optional Steam
staging, project generation, app build/signing, backup, install, restore, and
asset transfer. Cancel terminates the subprocess group, retains evidence, and
never labels a partial job complete. Complete logs are written even when
verbose streaming is off; **Show Logs** displays recent progress and **Reveal
Logs** opens the retained file.

Jobs, imports, receipts and backups live under `build/apk-installer/`. This
folder is restricted to the local user because saved game data can include
session information. Do not share backups as bug-report attachments.

After installation, saved-data restoration, and asset transfer all succeed,
the installer removes that job's build cache, staged payload, restored save
backup, and decoded APK tree. Original APKs and Steam downloads (including
course/OBB data) remain cached for updates. Decoded files are recreated locally
on the next build. Small manifests, transfer receipts, and logs remain available
for **Collect Headset Logs**. Failed or pending recovery jobs are not pruned.

Before updating an existing bundle the backend checks that it is stopped,
inventories its container, and copies writable Documents and Library data
(except Library caches). Known immutable APKs, extracted asset trees, and OBB
payloads are excluded. Unknown files are backed up. Missing/unknown inventory
fields, insufficient disk space, a failed copy, or a running app stop the
update before installation. Backups include file hashes and are checked before
restoring into the post-install container. User-supplied seed files cannot
replace backed-up saves.

An interrupted update with a backup leaves a pending recovery record. Another
install for that device/bundle is blocked. **Restore Saved Data** first backs up
the current writable state separately, then restores the original update's
backup. It requires the existing app to be installed and stopped. Keep both
jobs until recovery is confirmed. The equivalent command is:

```sh
python3 tools/utility.py recover --job /absolute/path/to/build/apk-installer/jobs/JOB
```

If the app was removed, reinstall the signed `.app` retained in that original
job with Xcode before running recovery. Recovery does not launch the game.

Successful transfer receipts identify the device, bundle, actual container,
APK fingerprint and supplied-data hashes. Unchanged assets may be reused only
when those match and the receipt in the current device container agrees.
Metadata is refreshed even then. Missing receipts, changed containers, or
interrupted transfers cause restaging; a local stamp alone is insufficient.

## Apps that close or fail to start

**Collect Headset Logs** retrieves the selected install's current and previous
`klepton-boot.log` and `klepton-crash.log` from Vision Pro. The app defaults to
the most recent completed install, including jobs made before this feature was
added. **Choose Install…** selects a different job under
`build/apk-installer/jobs/`; its manifest identifies the exact device and bundle.
Reproduce the failure first and keep that headset unlocked and connected.
Collection does not launch the app or change its data. **Reveal Headset Logs**
opens the retained folder containing the logs and `report.json`.

The result distinguishes missing runtime functions/JNI methods, missing content,
and runtime faults. Missing OBBs or unreadable bundles point to the data controls
and **Advanced → Data folder destination**. Unsupported functions or methods
need a Klepton development fix; changing a profile does not implement them.
Unknown results do not establish that the app works. If the app exits before
writing any logs, retrieve its system crash report in **Xcode → Window → Devices
and Simulators → View Device Logs**. Missing individual logs are recorded in the
report; one failed copy does not discard the other logs. Current and previous
runs are kept separate so an old failure cannot be diagnosed as the current one.

Only the four diagnostic filenames are copied, plus a report with APK
fingerprint and boot settings. Backups, credentials and saved-game trees are
excluded. Review the logs before sharing them; app output can contain personal
information. Build logs remain separate from headset logs.

The visionOS status window continues checking for late runtime errors after
native initialization. An unavailable Vuplex browser replaces the green
initialization badge with **Runtime support needed** and an explanation with
log-export instructions. This can occur while the guest continues rendering a
room; a rendered environment alone does not establish a working login or player.
WebKit page-load failures suggest checking connectivity and collecting logs;
persistent browser texture failures request a runtime development report.
The tested Bigscreen fingerprint describes its experimental WebKit integration
during APK inspection. Browser initialization and texture upload work in the
simulator; login, online services and movie playback remain unverified. See
[Bigscreen integration](games/bigscreen/INTEGRATION.md) for the current evidence.

```sh
python3 tools/utility.py diagnose --job build/apk-installer/jobs/INSTALL_JOB
```

Quest APKs already use Klepton's Oculus/VrApi implementations. They do not
inherently need ovrport preprocessing, which patches Android headset runtimes
and Java code rather than adding missing visionOS runtime functionality. See
[the Bigscreen investigation](games/bigscreen/INTEGRATION.md) for an example.

## Backend interface

All stdout is newline-delimited JSON with `schema_version: 1`, `stage`,
`status`, and `message`. Status is `running`, `log`, `result`, `error`, or
`cancelled`. Result/error events include relevant `log_path`, `job_path`,
`app_path`, and `recovery_job` fields. Raw command output goes to the job log.
A cancellation exits 130; a failure exits 1. APKs and game data are never
interpolated into shell command strings.

```sh
python3 tools/utility.py doctor
python3 tools/utility.py steam-status
python3 tools/utility.py steam-update --channel stable
python3 tools/utility.py steam-update --channel beta
python3 tools/utility.py steam-library --action refresh
python3 tools/utility.py inspect --apk '/path/to/Game.apk'
python3 tools/utility.py run --request /path/to/request.json
```

`doctor` returns prerequisite checks, paired physical Vision Pro devices,
keychain development teams, and the existing profiles. `inspect` returns
metadata, the content-addressed `import_id`, boot inference, matching profiles,
audited Steam profiles, data requirements, suggestions, and estimated payload
size.

`steam-library` accepts `login`, `refresh`, `cached`, `download`, and `use`.
Its request is one JSON line on stdin; authentication secrets must never be
placed in command arguments or a saved request file. Additional stdin lines
can supply `guard_code`. The native UI handles `steam-auth` events before
generic logging: `auth` carries QR/Guard prompts and `credential` carries a
Keychain-only refreshed session. `steam-library` result events carry game,
branch, launch and local receipt summaries. `steam-game` result events carry
the existing inspection fields plus `downloaded_data` and `unmapped_data`.
The native app manages this private interface; a Steam Web API key and a
running Steam desktop client are not required.

Submit the APK inspection's import ID in a build/install request:

```json
{
  "schema_version": 1,
  "import_id": "64-character SHA-256 returned by inspect",
  "profile": "eleven",
  "steam_mode": "auto",
  "steam_channel": "stable",
  "kind": "unity",
  "entry": "libmain",
  "bundle": "",
  "data_path": "",
  "team": "YOURTEAMID",
  "device": "identifier returned by doctor",
  "data": [],
  "verbose": false
}
```

Use an empty profile for experimental inference. Empty bundle/data destination
values use defaults. `steam_mode` is `auto`, `none`, `runtime`, or `offline`.
`steam_channel` is `stable` (the default) or `beta` and is forwarded to every
Steam staging and verification stage as `KLEPTON_STEAM_CHANNEL`. Direct CLI
builds without that environment variable retain their original recovery pin.
`data` contains absolute OBB/folder paths. Destination paths are relative to
Android storage; absolute paths and traversal are rejected.

`KLEPTON_BUILD_MANIFEST` is an optional internal interface shared by target
resolution, translation, Steam staging, project generation, and asset staging.
It separates absolute host source paths from relative container paths and
names isolated build products. Project generation compiles a static target
record into the app and registers it before runtime lookup. Known targets
retain their identity; generic imports get a stable package-derived identity.
The committed runtime target table is not regenerated for imports.

## Verification

```sh
make installer-check
make installer
python3 -m unittest discover -s steam/tests -p test_steam_updates.py
```

The fixture suite exercises archive rejection, inference/ambiguity, known and
generic manifests, native C target registration, Steam audit checks, required
data, save collisions, backup grouping, subprocess cancellation, failures
before install, interrupted transfers/restores, retry, and container receipts.
It does not require proprietary APK fixtures or a connected headset.

Steam fixtures also cover library/depot filtering, branches and multiple APKs,
download/update/repair states, full-width manifest IDs, large library responses,
interactive Guard input, cancellation and network failures, receipt persistence,
account isolation, and secret-free logs. Native model checks validate the
Python JSON contract and the displayed action transitions. Steam CDN calls are
mocked in tests; a live owned-game download requires Steam sign-in.

The installer currently targets physical Vision Pro deployment. Simulator
build/install is available through Klepton's existing tools and the same
optional manifest, and is useful for validating imported target startup.
There is no automatic game launch, batch queue, dependency installer, or
arbitrary environment-variable editor in this UI.
