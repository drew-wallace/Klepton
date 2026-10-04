"""Exercise the real Swift audio lifecycle with a host-only AVFAudio fake."""
import os
import pathlib
import subprocess
import tempfile


def main():
    repo = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="klepton-audio-") as temp:
        interruption_test = pathlib.Path(temp) / "interruption-test"
        subprocess.run([
            "xcrun", "clang", "-O1", "-Wl,-dead_strip", "-I", str(repo / "runtime"),
            "-framework", "AudioToolbox", "-framework", "CoreAudio",
            str(repo / "tests/visionos/AudioInterruptionTests.c"),
            str(repo / "runtime/kl_env.c"), "-o", str(interruption_test),
        ], check=True)
        subprocess.run([str(interruption_test)], check=True, timeout=10)
        library = pathlib.Path(temp) / "libAVFAudio.dylib"
        subprocess.run([
            "xcrun", "swiftc", "-emit-library", "-emit-module", "-module-name", "AVFAudio",
            str(repo / "tests/visionos/AudioSessionFake.swift"), "-o", str(library),
            "-emit-module-path", str(pathlib.Path(temp) / "AVFAudio.swiftmodule"),
        ], check=True)
        binary = pathlib.Path(temp) / "audio-test"
        subprocess.run([
            "xcrun", "swiftc", "-I", temp, "-L", temp, "-lAVFAudio",
            str(repo / "visionos/Sources/KleptonAudio.swift"),
            str(repo / "tests/visionos/AudioSessionTests.swift"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=10,
                       env={**os.environ, "DYLD_LIBRARY_PATH": temp})
        subprocess.run([str(binary), "--suspended-boot"], check=True, timeout=10,
                       env={**os.environ, "DYLD_LIBRARY_PATH": temp})


if __name__ == "__main__":
    main()
