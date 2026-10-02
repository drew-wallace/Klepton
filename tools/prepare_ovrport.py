#!/usr/bin/env python3
"""Unpack the supplied ovrport variants without modifying the source collection.

Run from any directory: python3 tools/prepare_ovrport.py [../ovrport]
Requires apktool. Translation: KLEPTON_TARGET=<target> bash visionos/mkguest.sh
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "visionos"))
from targets import TARGETS

SOURCES = {
    "4xvr-11026": "4xvr/4xvr.apk",
    "4xvr-11026-vrp": "4xvr/4XVR Video Player (Pro + Trial Bypass) v11026+1.10.26 -VRP/cn.vr4p.oculus4xvrplayerov.apk",
    "roborecall-41778": "Robo Recall - Unplugged/41778/RoboRecall-Android-shipping-arm64-es2.apk",
    "roborecall-41904": "Robo Recall - Unplugged/41904/RoboRecall-Android-shipping-arm64-es2.apk",
    "roborecall-47091": "Robo Recall - Unplugged/47091/RoboRecall-Android-shipping-arm64-es2.apk",
    "roborecall-47091-patched": "Robo Recall - Unplugged/Robo Recall [1.0] patch+savefix+90Hz/com.YourCompany.RoboRecall.apk",
    "roborecall-47091-v76": "Robo Recall - Unplugged/Robo Recall- Unplugged v47091+1.0 -VRP v76/Robo Recall (1.0) patch+savefix+90Hz.apk",
    "walkabout-57013": "Walkabout Mini Golf - Steam/WalkaboutMiniGolf.apk",
}


def link(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(destination):
        if destination.is_symlink() and destination.resolve() == source.resolve():
            return
        raise RuntimeError(f"Refusing to replace existing file: {destination}")
    destination.symlink_to(source.resolve())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=ROOT.parent / "ovrport")
    args = parser.parse_args()
    source = args.source.resolve()
    apktool = shutil.which("apktool")
    if not apktool:
        parser.error("apktool is required (brew install apktool)")
    missing = [str(source / p) for p in SOURCES.values() if not (source / p).is_file()]
    if missing:
        parser.error("Missing APKs:\n" + "\n".join(missing))
    report = ROOT / "build/ovrport"
    report.mkdir(parents=True, exist_ok=True)
    inventory = []
    for name, relative in SOURCES.items():
        apk = source / relative
        target = TARGETS[name]
        tree = ROOT / target["tree"]
        link(apk, ROOT / target["apk"])
        if not tree.exists():
            with (report / f"{name}-decode.log").open("w") as log:
                subprocess.run([apktool, "d", "-s", "-o", str(tree), str(apk)],
                               stdout=log, stderr=subprocess.STDOUT, check=True)
        if not (tree / "AndroidManifest.xml").is_file() or not (tree / "apktool.yml").is_file():
            raise RuntimeError(f"Incomplete decode: {tree}; inspect the decode log")
        userdata = Path.home() / "Library/Application Support/Klepton/userdata" / name
        if name.startswith("roborecall-"):
            data = sorted(apk.parent.rglob("*.obb"))
        elif name.startswith("walkabout-"):
            data = sorted((apk.parent / "obb").iterdir())
            # Device staging resolves the directory once, then copies it whole.
            # Link the directory itself so its loose bundles are real files there.
            link(apk.parent / "obb", userdata / target["obb"])
        else:
            data = []
        for file in data:
            if not name.startswith("walkabout-"):
                link(file, userdata / target["obb"] / file.name)
        inventory.append({"target": name, "source": str(apk), "data_files": len(data)})
        print(f"{name}: ready ({len(data)} external data files)", flush=True)
    (report / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")


if __name__ == "__main__":
    main()
