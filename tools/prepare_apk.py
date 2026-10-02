#!/usr/bin/env python3
"""Unpack the supplied apk variants without modifying the source collection.

Run from any directory: python3 tools/prepare_apk.py [../apk]
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

import runpy
SOURCES = {}
for game in ('4xvr', 'roborecall', 'walkabout'):
    SOURCES.update(runpy.run_path(str(ROOT / 'games' / game / 'targets.py'))['SOURCES'])


def link(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(destination):
        if destination.is_symlink() and destination.resolve() == source.resolve():
            return
        raise RuntimeError(f"Refusing to replace existing file: {destination}")
    destination.symlink_to(source.resolve())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=ROOT.parent / "apk")
    args = parser.parse_args()
    source = args.source.resolve()
    apktool = shutil.which("apktool")
    if not apktool:
        parser.error("apktool is required (brew install apktool)")
    missing = [str(source / p) for p in SOURCES.values() if not (source / p).is_file()]
    if missing:
        parser.error("Missing APKs:\n" + "\n".join(missing))
    report = ROOT / "build/apk"
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
