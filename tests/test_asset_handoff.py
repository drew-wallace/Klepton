"""Exercise the shared JNI path handoff without account or game data."""
import pathlib
import subprocess
import sys
import tempfile
import zipfile

def main():
    with tempfile.TemporaryDirectory(prefix="klepton-asset-handoff-") as temp:
        root = pathlib.Path(temp)
        probe, game = root / "SteamRuntime", root / "android-files"
        probe.mkdir()
        (game / "obb").mkdir(parents=True)
        for directory, code, name in [(probe, 111, "probe"), (game, 57013, "game")]:
            (directory / "assets").mkdir()
            (directory / "apktool.yml").write_text(f"versionCode: '{code}'\nversionName: {name}\n")
            (directory / "AndroidManifest.xml").write_text(
                f'<manifest package="test.{name}">'
                f'<uses-permission android:name="android.permission.{"INTERNET" if name == "probe" else "RECORD_AUDIO"}"/>'
                '<application>'
                f'<meta-data android:name="fixture.context" android:value="{name}"/>'
                '</application></manifest>')
        with zipfile.ZipFile(game / "obb" / "main.fixture.obb", "w") as archive:
            archive.writestr("assets/aa/settings.json", '{"fixture":true}',
                             compress_type=zipfile.ZIP_STORED)
            archive.writestr("assets/bin/Data/resource", "packaged settings fixture",
                             compress_type=zipfile.ZIP_DEFLATED)
            archive.writestr("assets/bin/Data/UnitySubsystems/UnityOpenXR/UnitySubsystemsManifest.json", "{}")
        subprocess.run([*sys.argv[1:], str(probe), str(game)], check=True)


if __name__ == "__main__":
    main()
