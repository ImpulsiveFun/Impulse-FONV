"""Create a private Android import ZIP from your installed PC game."""
import argparse
from pathlib import Path
from zipfile import ZIP_STORED, ZipFile

from android.prepare import ASSETS, validate

LOOSE_FOLDERS = {"music", "sound", "shaders", "video"}
OFFICIAL_ADD_ONS = (
    "DeadMoney", "HonestHearts", "OldWorldBlues", "LonesomeRoad", "GunRunnersArsenal",
    "CaravanPack", "ClassicPack", "MercenaryPack", "TribalPack",
)
GAME_DATA = {name.split("/", 1)[1].lower() for name in ASSETS} | {"update.bsa"}
GAME_DATA.update((pack + suffix).lower() for pack in OFFICIAL_ADD_ONS
                 for suffix in (".esm", " - Main.bsa", " - Sounds.bsa"))


def build(game, executable, output):
    if not game.is_dir() or output.is_relative_to(game):
        raise ValueError("Use an installed game folder and save the ZIP outside it.")
    validate(game, executable)
    ini = game / "Fallout_default.ini"
    if not ini.is_file():
        raise ValueError("Fallout_default.ini is missing from the installation.")
    data = game / "Data"
    files = [(executable or game / "FalloutNV.exe", "FalloutNV.exe"), (ini, ini.name)]
    for entry in sorted(data.iterdir()):
        if entry.is_file() and entry.name.lower() in GAME_DATA:
            files.append((entry, entry.relative_to(game).as_posix()))
        elif entry.is_dir() and entry.name.lower() in LOOSE_FOLDERS:
            for path in sorted(entry.rglob("*")):
                if path.is_file():
                    files.append((path, path.relative_to(game).as_posix()))
    if sum(path.stat().st_size for path, _ in files) > 32 * 1024**3:
        raise ValueError("The selected game files exceed the Android import limit of 32 GiB.")
    with output.open("xb") as destination:
        try:
            with ZipFile(destination, "w", compression=ZIP_STORED, allowZip64=True) as archive:
                for path, name in files:
                    print(f"Adding {name}")
                    archive.write(path, name)
        except BaseException:
            destination.close()
            output.unlink(missing_ok=True)
            raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path, help="Installed PC game directory")
    parser.add_argument("output", type=Path, help="Private ZIP destination outside the installation")
    parser.add_argument("--exe", type=Path, help="Same local executable passed to android/prepare.py")
    args = parser.parse_args()
    try:
        build(args.game.resolve(), args.exe, args.output.resolve())
    except (OSError, ValueError) as error:
        parser.exit(1, f"Could not create the private ZIP. {error}\n")
