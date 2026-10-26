#!/bin/bash
# Build the supplied Eleven APK with the genuine local Steam host.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ ! -f eleven.apk ]; then
    ln -s "$HOME/ovrport/eleven/eleven.apk" eleven.apk
fi
if [ ! -f eleven/AndroidManifest.xml ]; then
    apktool d -f -o eleven eleven.apk
fi
python3 steam/visionos/mksteam.py --target eleven --platform visionossim
export KLEPTON_TARGET=eleven KLEPTON_STEAM_LOCAL=1
export KL_STEAM_OFFLINE="${KL_STEAM_OFFLINE:-0}"
export KL_EXIT_ON_BACKGROUND="${KL_EXIT_ON_BACKGROUND:-0}"
export KL_FRAMES="${KL_FRAMES:-0}"
exec visionos/run.sh sim
