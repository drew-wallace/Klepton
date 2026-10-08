#!/usr/bin/env python3
"""Generate the visionOS host app's .xcodeproj with no Xcode GUI needed.

Grown from spikes/device-probe/gen_xcodeproj.py, but data-driven: the guest
libraries are a list, not seven hand-written object ids each. That list — and
the product name, bundle id and display name that go with it — now comes from
visionos/targets.py, keyed on KLEPTON_TARGET. There are two guests, and two apps
built from one tree must not collide: a bundle ID separates the INSTALLED apps
and nothing about their build outputs, so the target carries the product name
(hence the .xcodeproj, the .app and the derived-data directory) and the
Frameworks/ subdirectory its translations were staged into.

What the app links, and why it comes from three places:

  Klepton.xcframework   the C runtime, built by `make xros` (both slices).
                        Static, so it is inside the app binary and carries no
                        load-time cost of its own.
  <guest>.xcframework   the five klepton-ld translations, built by mkguest.sh.
                        Embedded and signed-on-copy, NOT linked — nothing
                        references their symbols; the runtime dlopens them by
                        path. Linking them would make dyld resolve an exports
                        trie that klepton-ld deliberately does not emit.
  ANGLE_*.xcframework   ANGLE retargeted to visionOS, built by mkangle.sh.
                        Embedded the same way and for the same reason: kl_glfb
                        dlopens libEGL/libGLESv2 by path, and linking a renderer
                        the app never names would only make dyld load it on
                        every run, including the ones that stay on the null
                        driver.
  Sources/              the Swift app layer and kl_app.c.
"""
import os, pathlib, subprocess, sys, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import targets as targets_mod

HERE = os.path.dirname(os.path.abspath(__file__))
KLT = targets_mod.resolve(os.environ.get("KLEPTON_TARGET") or targets_mod.DEFAULT)
# The PRODUCT name, which is also the .xcodeproj's, the .app's and the scheme's.
# The Swift sources are still Klepton*.swift whatever the product is called —
# they are the app layer, shared by every target, and renaming them per build
# would be five file copies to say one thing.
NAME = KLT["product"]
BUNDLE_ID = os.environ.get("KLEPTON_BUNDLE_ID", KLT["bundle"])
STEAM_LOCAL = os.environ.get("KLEPTON_STEAM_LOCAL", "0") == "1"
GAME_INFO_PLIST = "Info.plist"
if STEAM_LOCAL:
    if not KLT.get('steam_local', False):
        raise ValueError('KLEPTON_STEAM_LOCAL requires a target with an audited Steam profile')
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../steam/visionos"))
    import mksteam
    mksteam.verify_staged()
    import plistlib
    from game_version import installed_version
    # Freeze the APK's version with its translated executable. Documents data
    # can be replaced independently and must not determine the installed version.
    _info = plistlib.loads(pathlib.Path(HERE, "Info.plist").read_bytes())
    _root = pathlib.Path(HERE).parent
    _info.update(installed_version(_root / KLT['tree'],
                                  _root / KLT['apk'] if KLT.get('apk') else None))
    GAME_INFO_PLIST = f"build/GameInfo-{KLT['name']}.plist"
    _info_path = pathlib.Path(HERE, GAME_INFO_PLIST)
    _info_path.parent.mkdir(parents=True, exist_ok=True)
    _info_path.write_bytes(plistlib.dumps(_info))

# Keep Increased Memory Limit on: the current Personal Team can provision it.
# A 2026-09-28 device build confirmed that the same team refuses Extended
# Virtual Addressing and Low-Latency Streaming. Those remain in the authored
# plist, but are opt-in so rebuilding does not require hand-editing capabilities.
# KLEPTON_EXTENDED_VA=1 requires a team/profile supporting that capability.
# KLEPTON_LOW_LATENCY=1 additionally requires the streaming capability; it is
# for Foveated Streaming, not standalone guest rendering or audio.
ENTITLEMENTS = os.environ.get("KLEPTON_ENTITLEMENTS", "1") != "0"
OPTIONAL_ENTITLEMENTS = {
    "com.apple.developer.kernel.extended-virtual-addressing": "KLEPTON_EXTENDED_VA",
    "com.apple.developer.low-latency-streaming": "KLEPTON_LOW_LATENCY",
}
ENTITLEMENTS_FILE = "Klepton.entitlements"
_dropped = [k for k, env in OPTIONAL_ENTITLEMENTS.items()
            if os.environ.get(env, "0") != "1"]
if ENTITLEMENTS and _dropped:
    import plistlib
    src = pathlib.Path(__file__).with_name("Klepton.entitlements")
    d = plistlib.loads(src.read_bytes())
    for k in _dropped:
        d.pop(k, None)
    out = pathlib.Path(__file__).with_name("build") / "Klepton-filtered.entitlements"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(plistlib.dumps(d))
    ENTITLEMENTS_FILE = "build/Klepton-filtered.entitlements"

ENTITLEMENTS_SETTING = (f"\t\t\t\tCODE_SIGN_ENTITLEMENTS = {ENTITLEMENTS_FILE};\n"
                        if ENTITLEMENTS else "")
GUEST = KLT["libs"].split()
if STEAM_LOCAL:
    GUEST = [g for g in GUEST if g not in ('libsteam_api', 'libc++_shared')]
ANGLE = ["ANGLE_libEGL", "ANGLE_libGLESv2"]
# ...and the OTHER renderer, for a guest whose graphics API is Vulkan (BONELAB).
# Same treatment as ANGLE for the same reasons: one driver for every guest that
# asks, dlopened by path at runtime (runtime/gfx/kl_vulkan.c), and 4.6 MB a slice —
# so making it per-target would cost a table row to save less than the row.
#
# OPTIONAL, unlike ANGLE, and that is the only difference: MoltenVK is a
# vendored prebuilt (`make mvk`) that a bare checkout does not have, and three of
# the four targets never ask for Vulkan. Absent, kl_vulkan.c's __has_include stub
# refuses BY NAME, which is the right answer for those three; making it mandatory
# would stop everyone's build for a dependency most targets do not use.
MVK = ["MoltenVK"] if os.path.isdir(
    os.path.join(HERE, "Frameworks", "MoltenVK.xcframework")) else []
# Where each set is staged on the SOURCE side. The guest's is per-target so the
# two apps do not overwrite each other's translations in place; ANGLE's is not,
# because it is the same renderer for every guest. Inside the built .app both
# land flat in Frameworks/, which is why kl_app.c needs to know nothing about it.
GUEST_DIR = f"Frameworks/{KLT['name']}"
ANGLE_DIR = "Frameworks"
# Both sets are embedded-not-linked, so the pbxproj treatment is identical and
# the generator stays data-driven — the two lists differ only in what a missing
# one means, which is why main() reports them separately.
STEAM_FRAMEWORKS = ['libsteamclient_backend', 'libsteamclient', 'libsteam_api', 'libc++_shared'] if STEAM_LOCAL else []
EMBED = GUEST + ANGLE + MVK + STEAM_FRAMEWORKS


def detect_team():
    if os.environ.get("KLEPTON_TEAM"):
        return os.environ["KLEPTON_TEAM"]
    try:
        pem = subprocess.run(["security", "find-certificate", "-c", "Apple Development",
                              "-p"], capture_output=True, text=True).stdout
        subj = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                              input=pem, capture_output=True, text=True).stdout
        m = re.search(r"OU\s*=\s*([A-Z0-9]{10})", subj)
        if m:
            return m.group(1)
    except Exception:
        pass
    return ""


TEAM = detect_team()

_n = [0]
def oid(tag):
    _n[0] += 1
    return ("KLEPT0N" + f"{_n[0]:04d}" + tag).upper().ljust(24, "0")[:24]


PROJ    = oid("PROJ");  TARGET = oid("TGT");   PRODUCT = oid("PROD")
G_ROOT  = oid("GROOT"); G_SRC  = oid("GSRC");  G_FW    = oid("GFW");  G_PROD = oid("GPRODS")
BP_SRC  = oid("BPSRC"); BP_FRM = oid("BPFRM"); BP_EMB  = oid("BPEMB")
CL_PROJ = oid("CLPRJ"); CL_TGT = oid("CLTGT")
C_PRJ_D = oid("CPRJD"); C_PRJ_R = oid("CPRJR")
C_TGT_D = oid("CTGTD"); C_TGT_R = oid("CTGTR")
F_C = oid("FC"); F_H = oid("FH"); F_BRIDGE = oid("FBRDG")
B_C = oid("BC")
# The app icon: an asset catalog holding one solid-image-stack per target that
# has one (AppIcon-<name>.solidimagestack). Targets without one build exactly
# as before - the catalog compiles empty for them and no APPICON setting is
# emitted, so the icon is strictly additive.
F_ASSETS = oid("FASSET"); B_ASSETS = oid("BASSET"); BP_RES = oid("BPRES")
import os as _os
# Compile-time launcher opt-in (also drives LAUNCHER_COND + the display name below).
_launcher_on = _os.environ.get("KL_CUSTOM_LAUNCHER", "").strip().lower() in ("1", "true", "yes", "on")
# NB: the bundle id is deliberately NOT switched by the launcher flag. It is a SIGNING
# identity, and forcing an unregistered id (e.g. com.noosphere.hl1vr) makes every local
# device build fail code-signing and silently relaunch the old app. For the neutral
# store id, register the App ID first, then archive with an explicit
# KLEPTON_BUNDLE_ID=com.noosphere.hl1vr. Local launcher builds keep the dev id and sign.
_assets_dir = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "Assets.xcassets")
def _stack_exists(n): return _os.path.isdir(_os.path.join(_assets_dir, n + ".solidimagestack"))
# With the launcher on, a trademark-free monogram icon (AppIcon-<t>-launcher) replaces
# the target's normal icon when one is present; otherwise the normal icon stands. Both
# are gitignored, so this is strictly local/opt-in.
_icon_base = f"AppIcon-{KLT['name']}"
_icon_name = (f"{_icon_base}-launcher"
              if _launcher_on and _stack_exists(f"{_icon_base}-launcher")
              else _icon_base)
HAS_ICON = _stack_exists(_icon_name)
ASSETCATALOG_SETTING = ((f"\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = \"{_icon_name}\";\n"
                         if HAS_ICON else "")
                        # Info.plist's CFBundleIconName is $(KL_APPICON_NAME) —
                        # always defined so expansion never leaves a literal
                        # "$(...)" in the bundle; the placeholder names no stack
                        # and behaves exactly like the old no-icon build.
                        + f"\t\t\t\tKL_APPICON_NAME = \"{_icon_name if HAS_ICON else 'AppIcon'}\";\n")

# Trim the asset catalog to THIS target. The shared Assets.xcassets holds one icon
# stack per target (~26), and actool compiles ALL of them into the app's Assets.car
# (~75 MB of icons the app never uses). So build a per-target catalog under build/
# (gitignored) carrying only this target's normal + launcher stacks, and reference
# THAT below instead of the full catalog. Iconless targets get an empty catalog,
# which builds exactly as the shared one did for them.
import shutil as _shutil
_appicon_catalog = f"build/appicon/{KLT['name']}.xcassets"
_cat_abs = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _appicon_catalog)
_shutil.rmtree(_cat_abs, ignore_errors=True)
_os.makedirs(_cat_abs, exist_ok=True)
_src_contents = _os.path.join(_assets_dir, "Contents.json")
if _os.path.exists(_src_contents):
    _shutil.copy(_src_contents, _os.path.join(_cat_abs, "Contents.json"))
else:
    open(_os.path.join(_cat_abs, "Contents.json"), "w").write(
        '{\n  "info" : { "author" : "xcode", "version" : 1 }\n}\n')
for _stack in (_icon_base, f"{_icon_base}-launcher"):
    _s = _os.path.join(_assets_dir, _stack + ".solidimagestack")
    if _os.path.isdir(_s):
        _shutil.copytree(_s, _os.path.join(_cat_abs, _stack + ".solidimagestack"))

# The Swift half of the language split: the App/UI, and the Compositor Services
# renderer adds. A list rather than an id pair each, for the same reason
# the guest libraries are one.
SWIFT = ["KleptonApp.swift", "KleptonCompositor.swift", "KleptonControllers.swift",
         "KleptonAudio.swift", "KleptonShell.swift", "KleptonTuning.swift",
         "KleptonChroma.swift", "KleptonMic.swift",
         "KleptonGuardian.swift", "KleptonGuardianGeometry.swift", "KleptonGuardianRuntime.swift",
         "KleptonGuardianMapping.swift", "KleptonGuardianRenderer.swift",
         "KleptonGuardianPersistence.swift", "KleptonGuardianCoordinates.swift",
         "KleptonLauncher.swift", "KleptonHL1.swift"]
if STEAM_LOCAL:
    SWIFT += ['../../steam/visionos/Sources/kl_steam_host.c', '../build/SteamLocal/SteamHostLogin.swift']
swift = [{"name": s, "ref": oid(f"FS{i}"), "bld": oid(f"BS{i}")} for i, s in enumerate(SWIFT)]

swift_buildfiles = "\n".join(
    f'\t\t{s["bld"]} /* {s["name"]} in Sources */ = {{isa = PBXBuildFile; fileRef = {s["ref"]}; }};'
    for s in swift)
swift_filerefs = "\n".join(
    f'\t\t{s["ref"]} = {{isa = PBXFileReference; lastKnownFileType = {"sourcecode.c.c" if s["name"].endswith(".c") else "sourcecode.swift"}; '
    f'path = "{s["name"]}"; sourceTree = "<group>"; }};' for s in swift)
swift_children = "\n".join(f'\t\t\t\t{s["ref"]},' for s in swift)
swift_sources  = "\n".join(f'\t\t\t\t{s["bld"]},' for s in swift)
F_RT    = oid("FRT");   B_RT_LNK = oid("BRTLK")

# One file ref + one embed build-file per embedded framework.
guest = [{"name": g, "ref": oid(f"FG{i}"), "emb": oid(f"BG{i}"),
          "dir": 'Frameworks/SteamLocal' if g in STEAM_FRAMEWORKS else GUEST_DIR if g in GUEST else ANGLE_DIR}
         for i, g in enumerate(EMBED)]

buildfiles = "\n".join(
    f'\t\t{g["emb"]} /* {g["name"]} in Embed */ = {{isa = PBXBuildFile; fileRef = {g["ref"]}; '
    f'settings = {{ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }}; }};'
    for g in guest)

# Quoted, and it is not decoration: a pbxproj value may go unquoted only if it
# is alphanumerics, `_`, `.`, `/` and `-`. `libc++_shared` has a `+` in it, and
# the failure is not a bad reference — the whole project becomes unreadable,
# `xcodebuild` says "damaged ... due to a parse error" and names no file.
filerefs = "\n".join(
    f'\t\t{g["ref"]} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; '
    f'name = "{g["name"]}.xcframework"; path = "{g["dir"]}/{g["name"]}.xcframework"; sourceTree = "<group>"; }};'
    for g in guest)

embeds = "\n".join(f'\t\t\t\t{g["emb"]},' for g in guest)
fwchildren = "\n".join(f'\t\t\t\t{g["ref"]},' for g in guest)

# Compile-time opt-in for the custom "configure, then Start" launcher (hl1/hl2/
# portal). Only when this project is generated with KL_CUSTOM_LAUNCHER set does the
# build carry the KL_CUSTOM_LAUNCHER Swift active-compilation condition that
# klLauncherTitle() (Sources/KleptonLauncher.swift) gates on — so a shipping build
# bakes the launcher in with no runtime env. Default builds (and visionos/run.sh)
# omit it and keep the original autoboot / "Boot"-button shape. Applies to both
# Debug and Release (this is the target-level COMMON block); $(inherited) preserves
# the project-level DEBUG condition.
# With the launcher on, the game builds present a compact trademark-free name; every
# other build (and every other target) keeps its normal display name from targets.py.
_launcher_display = {"hl1": "HL1VR", "hl2": "HL2VR", "portal": "P1VR"}
_display = (_launcher_display.get(KLT['name'], KLT['display']) if _launcher_on
            else KLT['display'])
# Trailing newline, glued at line-start to the next setting — the same shape as
# ENTITLEMENTS_SETTING / ASSETCATALOG_SETTING above, so it lands on its own line in
# the output and expands to nothing (no blank line) when the flag is off.
_conditions = (['KL_CUSTOM_LAUNCHER'] if _launcher_on else [])
if STEAM_LOCAL:
    _conditions += ['KL_STEAM_GAME_HOST']
elif KLT.get('steam_local', False):
    # A game with no bundled backend is a diagnostic build. Make its offline
    # bootstrap survive Home launches, which do not inherit run.sh/test env.
    _conditions += ['KL_STEAM_DIAGNOSTICS']
LAUNCHER_COND = ('\t\t\t\tSWIFT_ACTIVE_COMPILATION_CONDITIONS = "'+ ' '.join(_conditions)+' $(inherited)";\n'
                 if _conditions else "")
STEAM_C_COND = ('\t\t\t\tARCHS = arm64;\n' if STEAM_LOCAL else '')
STEAM_C_DEFINE = ('\t\t\t\t\t"KL_STEAM_GAME_HOST=1",\n' if STEAM_LOCAL else '')
F_STEAM_ASSETS, B_STEAM_ASSETS = oid('FSTEAMASSETS'), oid('BSTEAMASSETS')
STEAM_ASSETS_REF = (f'\t\t{F_STEAM_ASSETS} = {{isa = PBXFileReference; lastKnownFileType = folder; path = build/SteamLocal/SteamAuthAssets; sourceTree = SOURCE_ROOT; }};\n' if STEAM_LOCAL else '')
STEAM_ASSETS_BUILD = (f'\t\t{B_STEAM_ASSETS} = {{isa = PBXBuildFile; fileRef = {F_STEAM_ASSETS}; }};\n' if STEAM_LOCAL else '')
STEAM_RES = f'\t\t\t\t{B_STEAM_ASSETS},\n' if STEAM_LOCAL else ''

COMMON = f"""
{STEAM_C_COND}
				CLANG_ENABLE_MODULES = YES;
{ENTITLEMENTS_SETTING}				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = "{TEAM}";
				ENABLE_PREVIEWS = NO;
				GENERATE_INFOPLIST_FILE = YES;
{ASSETCATALOG_SETTING}				INFOPLIST_FILE = "{GAME_INFO_PLIST}";
				// One entry per runtime source directory: a runtime header is
				// included by BARE NAME everywhere, including from
				// Klepton-Bridging-Header.h, so each directory holding one has
				// to be on the search path. Same list as the Makefile's
				// RUNTIME_INC; a new runtime/<area>/ needs a line in both.
				//
				// **The source tree comes BEFORE $(inherited)**, which carries
				// $(BUILT_PRODUCTS_DIR)/include — a directory Xcode copies
				// headers into and never removes them from. A header that MOVED
				// between runtime directories therefore leaves a copy at its old
				// bare path there for the life of the derived data, and with
				// $(inherited) first that stale copy shadows the real one: the
				// build fails on a symbol the header plainly declares, naming a
				// Swift line and not a header. Measured after runtime/<area>/
				// existed, on the per-target derived data that predated it.
				HEADER_SEARCH_PATHS = (
					"$(SRCROOT)/../runtime",
					"$(SRCROOT)/../runtime/jni",
					"$(SRCROOT)/../runtime/libc",
					"$(SRCROOT)/../runtime/gfx",
					"$(SRCROOT)/../runtime/xr",
					"$(SRCROOT)/../runtime/media",
					"$(SRCROOT)/../runtime/guest",
					"$(SRCROOT)/../runtime/diag",
                    "$(SRCROOT)/../steam/runtime",
                    "$(SRCROOT)/../steam/steamlink/runtime/guest",
                    "$(SRCROOT)/../steam/steamlink/runtime/libc",
					"$(inherited)",
				);
				// Two levels of escaping, and the first attempt had one. The
				// pbxproj layer eats a backslash, and the build system then
				// unquotes the setting VALUE the way a shell would — so a
				// single \" here reaches clang as -DKL_TARGET_DEFAULT=beatsaber
				// and fails with "use of undeclared identifier 'beatsaber'",
				// which names the target and says nothing about quoting.
				GCC_PREPROCESSOR_DEFINITIONS = (
					"$(inherited)",
{STEAM_C_DEFINE}\
					"KL_TARGET_DEFAULT=\\\\\\"{KLT['name']}\\\\\\"",
				);
				INFOPLIST_KEY_CFBundleDisplayName = "{_display}";
				INFOPLIST_KEY_GCSupportsControllerUserInteraction = YES;
				// NOT UIApplicationSceneManifest_Generation. Setting it makes Xcode
				// GENERATE the scene manifest and overwrite the one in Info.plist —
				// with UISceneConfigurations as an EMPTY dict, i.e. the window role
				// and no immersive-space role at all. The app then has no immersive
				// scene session, and the failure is invisible from inside: the space
				// "opens", a LayerRenderer runs, drawables arrive fully formed and
				// every frame presents, into nothing. Apple's own template does not
				// set this and declares the manifest by hand; so do we now
				// (visionos/Info.plist). This was the black immersive space.
				LD_RUNPATH_SEARCH_PATHS = (
					"$(inherited)",
					"@executable_path/Frameworks",
				);
				MARKETING_VERSION = 1.0;
				// VideoToolbox/CoreMedia/CoreVideo are kl_vtdec's, and
				// they were missing here for the whole of the Steam Link arc:
				// kl_vtdec joined RUNTIME_SHIP, `make xros` kept passing because
				// its own link line has them, and nothing built the APP until a
				// second target needed one. The failure is a link error naming
				// VTDecompressionSession*, which reads as a missing SDK rather
				// than as a build setting that was never updated.
				OTHER_LDFLAGS = "-lz -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface";
				PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID};
				PRODUCT_NAME = "$(TARGET_NAME)";
				SDKROOT = xros;
				SUPPORTED_PLATFORMS = "xros xrsimulator";
				SWIFT_OBJC_BRIDGING_HEADER = "Sources/Klepton-Bridging-Header.h";
{LAUNCHER_COND}				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 7;
				// visionOS 26, not 2.0. The device runs 27 and the SDK is 26, and an
				// app declaring a 2.0 minimum is asking the system for five-major-
				// versions-ago behaviour — which for Compositor Services is not a
				// detail: 26 reworked the drawable model (queryDrawables,
				// Drawable.Target, CompositorContent, Metal 4 residency), and the
				// pre-26 path is what a 2.0 app gets. It also makes those APIs
				// unavailable at compile time, which is how it was noticed at all.
				XROS_DEPLOYMENT_TARGET = 26.0;
"""

PBX = f"""// !$*UTF8*$!
{{
	archiveVersion = 1;
	classes = {{}};
	objectVersion = 60;
	objects = {{

/* Begin PBXBuildFile section */
{STEAM_ASSETS_BUILD}
{swift_buildfiles}
		{B_C} /* kl_app.c in Sources */ = {{isa = PBXBuildFile; fileRef = {F_C}; }};
		{B_ASSETS} /* Assets.xcassets in Resources */ = {{isa = PBXBuildFile; fileRef = {F_ASSETS}; }};
		{B_RT_LNK} /* Klepton.xcframework in Frameworks */ = {{isa = PBXBuildFile; fileRef = {F_RT}; }};
{buildfiles}
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
{STEAM_ASSETS_REF}
		{PRODUCT} /* {NAME}.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; sourceTree = BUILT_PRODUCTS_DIR; }};
{swift_filerefs}
		{F_C} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = kl_app.c; sourceTree = "<group>"; }};
		{F_H} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = kl_app.h; sourceTree = "<group>"; }};
		{F_BRIDGE} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "Klepton-Bridging-Header.h"; sourceTree = "<group>"; }};
		{F_ASSETS} = {{isa = PBXFileReference; lastKnownFileType = folder.assetcatalog; path = {_appicon_catalog}; sourceTree = "<group>"; }};
		{F_RT} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = Klepton.xcframework; path = ../build/Klepton.xcframework; sourceTree = "<group>"; }};
{filerefs}
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		{BP_FRM} = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{B_RT_LNK},
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXCopyFilesBuildPhase section */
		{BP_EMB} /* Embed Frameworks */ = {{
			isa = PBXCopyFilesBuildPhase;
			buildActionMask = 2147483647;
			dstPath = "";
			dstSubfolderSpec = 10;
			files = (
{embeds}
			);
			name = "Embed Frameworks";
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXCopyFilesBuildPhase section */

/* Begin PBXGroup section */
		{G_ROOT} = {{
			isa = PBXGroup;
			children = (
				{G_SRC},
				{G_FW},
				{G_PROD},
				{F_ASSETS},
			);
			sourceTree = "<group>";
		}};
		{G_SRC} /* Sources */ = {{
			isa = PBXGroup;
			children = (
{swift_children}
				{F_C},
				{F_H},
				{F_BRIDGE},
			);
			path = Sources;
			sourceTree = "<group>";
		}};
		{G_FW} /* Frameworks */ = {{
			isa = PBXGroup;
			children = (
				{F_RT},
{fwchildren}
			);
			name = Frameworks;
			sourceTree = "<group>";
		}};
		{G_PROD} /* Products */ = {{
			isa = PBXGroup;
			children = (
				{PRODUCT},
			);
			name = Products;
			sourceTree = "<group>";
		}};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		{TARGET} /* {NAME} */ = {{
			isa = PBXNativeTarget;
			buildConfigurationList = {CL_TGT};
			buildPhases = (
				{BP_SRC},
				{BP_FRM},
				{BP_RES},
				{BP_EMB},
			);
			buildRules = ();
			dependencies = ();
			name = {NAME};
			productName = {NAME};
			productReference = {PRODUCT};
			productType = "com.apple.product-type.application";
		}};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		{PROJ} = {{
			isa = PBXProject;
			attributes = {{
				BuildIndependentTargetsInParallel = 1;
				LastSwiftUpdateCheck = 1600;
				LastUpgradeCheck = 1600;
				TargetAttributes = {{
					{TARGET} = {{ CreatedOnToolsVersion = 16.0; }};
				}};
			}};
			buildConfigurationList = {CL_PROJ};
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (en, Base, );
			mainGroup = {G_ROOT};
			productRefGroup = {G_PROD};
			projectDirPath = "";
			projectRoot = "";
			targets = (
				{TARGET},
			);
		}};
/* End PBXProject section */

/* Begin PBXResourcesBuildPhase section */
		{BP_RES} = {{
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{B_ASSETS},
{STEAM_RES}
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXResourcesBuildPhase section */

/* Begin PBXSourcesBuildPhase section */
		{BP_SRC} = {{
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{swift_sources}
				{B_C},
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		{C_PRJ_D} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ALWAYS_SEARCH_USER_PATHS = NO;
				CLANG_ENABLE_OBJC_ARC = YES;
				COPY_PHASE_STRIP = NO;
				DEBUG_INFORMATION_FORMAT = dwarf;
				ENABLE_STRICT_OBJC_MSGSEND = YES;
				GCC_OPTIMIZATION_LEVEL = 0;
				GCC_PREPROCESSOR_DEFINITIONS = ( "DEBUG=1", "$(inherited)", );
				ONLY_ACTIVE_ARCH = YES;
				SWIFT_ACTIVE_COMPILATION_CONDITIONS = "DEBUG $(inherited)";
				SWIFT_OPTIMIZATION_LEVEL = "-Onone";
			}};
			name = Debug;
		}};
		{C_PRJ_R} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ALWAYS_SEARCH_USER_PATHS = NO;
				CLANG_ENABLE_OBJC_ARC = YES;
				COPY_PHASE_STRIP = NO;
				DEBUG_INFORMATION_FORMAT = "dwarf-with-dsym";
				ENABLE_STRICT_OBJC_MSGSEND = YES;
				SWIFT_COMPILATION_MODE = wholemodule;
			}};
			name = Release;
		}};
		{C_TGT_D} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{{COMMON}			}};
			name = Debug;
		}};
		{C_TGT_R} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{{COMMON}			}};
			name = Release;
		}};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		{CL_PROJ} = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{C_PRJ_D},
				{C_PRJ_R},
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
		{CL_TGT} = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{C_TGT_D},
				{C_TGT_R},
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
/* End XCConfigurationList section */
	}};
	rootObject = {PROJ};
}}
"""


def main():
    for g, d, how in [(g, GUEST_DIR, "visionos/mkguest.sh") for g in GUEST] + \
                     [(a, ANGLE_DIR, "visionos/mkangle.sh") for a in ANGLE]:
        p = os.path.join(HERE, d, f"{g}.xcframework")
        if not os.path.isdir(p):
            print(f"!! missing {p} — run {how} first", file=sys.stderr)
            return 1
    if not os.path.isdir(os.path.join(HERE, "..", "build", "Klepton.xcframework")):
        print("!! missing build/Klepton.xcframework — run `make xros` first", file=sys.stderr)
        return 1

    proj = os.path.join(HERE, f"{NAME}.xcodeproj")
    os.makedirs(proj, exist_ok=True)
    with open(os.path.join(proj, "project.pbxproj"), "w") as f:
        f.write(PBX)
    print(f"wrote {proj}")
    print(f"  KLEPTON_TARGET            = {KLT['name']}")
    print(f"  DEVELOPMENT_TEAM = {TEAM or '(NOT DETECTED - set KLEPTON_TEAM)'}")
    # ENTITLEMENTS_FILE, not the literal name: this used to print
    # "Klepton.entitlements" unconditionally, so a build that was actually
    # signing against a FILTERED copy said it was signing against the authored
    # one. A summary that contradicts what went into the pbxproj is worse than
    # no summary — it is the line you would check to rule the filter out.
    print(f"  CODE_SIGN_ENTITLEMENTS = "
          f"{ENTITLEMENTS_FILE if ENTITLEMENTS else '(none - KLEPTON_ENTITLEMENTS=0)'}"
          + (f"  (dropped: {', '.join(_dropped)})" if ENTITLEMENTS and _dropped else ""))
    print(f"  PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID}")
    print(f"  embedded guest frameworks: {', '.join(GUEST)}")
    print(f"  embedded ANGLE:            {', '.join(ANGLE)}")
    # Named either way. An absent MoltenVK is legitimate for three of the four
    # targets and fatal for the fourth, and the difference is invisible until a
    # Vulkan guest is on a headset — so the build says which one it made.
    print(f"  embedded MoltenVK:         "
          + (', '.join(MVK) if MVK else "(none - run 'make mvk' for a Vulkan guest)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
