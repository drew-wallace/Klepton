#!/usr/bin/env python3
"""Build unsigned visionOS apps for the prepared apk variants.

Run prepare_apk.py first. Assets remain in the per-target userdata directories
and must be staged separately by visionos/stage_assets.sh when installing.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

from prepare_apk import ROOT, SOURCES, TARGETS


def run(command, log, env=None):
    result = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}; see {log.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=list(SOURCES), action="append")
    parser.add_argument("--simulator", action="store_true")
    parser.add_argument("--skip-dependencies", action="store_true",
                        help="Use already-built runtime, ANGLE and MoltenVK")
    args = parser.parse_args()
    report = ROOT / "build/apk"
    report.mkdir(parents=True, exist_ok=True)
    if not args.skip_dependencies:
        with (report / "dependencies.log").open("w") as log:
            run(["make", "build/klepton-ld", "xros", "angle-all", "mvk"], log)
    with (report / "renderer-frameworks.log").open("w") as log:
        run(["bash", "visionos/mkangle.sh"], log)
        run(["bash", "visionos/mkmvk.sh"], log)
    platform = "visionOS Simulator" if args.simulator else "visionOS"
    sdk = "xrsimulator" if args.simulator else "xros"
    mode = "simulator" if args.simulator else "device"
    results = []
    for name in args.target or SOURCES:
        env = dict(os.environ, KLEPTON_TARGET=name)
        product = TARGETS[name]["product"]
        output = report / name
        output.mkdir(parents=True, exist_ok=True)
        derived = ROOT / "visionos/build" / f"apk-{mode}-{name}"
        print(f"Building {name} ({mode})", flush=True)
        with (output / f"app-{mode}.log").open("w") as log:
            run(["bash", "visionos/mkguest.sh"], log, env)
            run(["python3", "visionos/gen_xcodeproj.py"], log, env)
            # The simulator's dyld enforces signatures on nested translated
            # frameworks. Ad-hoc signing is enough for xrsimulator and needs
            # neither a development team nor a provisioning profile.
            signing = (["CODE_SIGNING_ALLOWED=YES", "CODE_SIGN_IDENTITY=-"]
                        if args.simulator else ["CODE_SIGNING_ALLOWED=NO"])
            run(["xcodebuild", "-project", f"visionos/{product}.xcodeproj",
                 "-scheme", product, "-configuration", "Debug",
                 "-destination", f"generic/platform={platform}",
                 "-derivedDataPath", str(derived), *signing,
                 *(["ARCHS=arm64"] if args.simulator else []),
                 "build"], log, env)
        app = derived / "Build/Products" / f"Debug-{sdk}" / f"{product}.app"
        if not (app / "Info.plist").is_file():
            raise RuntimeError(f"Build succeeded but app is missing: {app}")
        # xrsimulator accepts an ad-hoc identity, while a device build is left
        # unsigned for the caller's development team/provisioning step.
        results.append({"target": name, "app": str(app), "signed": bool(args.simulator)})
        (report / f"apps-{mode}.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"Built {app}", flush=True)


if __name__ == "__main__":
    main()
