"""Exercise the app's real frame clock without a headset or guest assets."""
import pathlib
import subprocess
import tempfile


def main():
    repo = pathlib.Path(__file__).resolve().parents[1]
    includes = [
        "runtime", "runtime/jni", "runtime/libc", "runtime/gfx", "runtime/xr",
        "runtime/media", "runtime/guest", "runtime/diag", "steam/runtime",
        "steam/steamlink/runtime/guest", "steam/steamlink/runtime/libc",
    ]
    with tempfile.TemporaryDirectory(prefix="klepton-session-") as temp:
        binary = pathlib.Path(temp) / "session-test"
        subprocess.run([
            "xcrun", "clang", "-O1", "-Wl,-dead_strip",
            '-DKL_TARGET_DEFAULT="beatsaber"',
            *[f"-I{repo / directory}" for directory in includes],
            str(repo / "tests/test_visionos_session.c"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
