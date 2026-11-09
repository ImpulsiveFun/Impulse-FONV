"""Download the pinned Xiph.Org source dependencies for the local game build."""
import hashlib
import io
from http.client import HTTPException
from pathlib import Path
import tarfile
from tempfile import TemporaryDirectory
from urllib.request import urlopen

CODECS = (
    ("libogg-1.3.6", "ogg", "83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638"),
    ("libvorbis-1.3.7", "vorbis", "0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab"),
)


def download_codec(name, project, checksum):
    version = name.rsplit("-", 1)[1]
    sources = (
        f"https://github.com/xiph/{project}/releases/download/v{version}/{name}.tar.gz",
        f"https://downloads.xiph.org/releases/{project}/{name}.tar.gz",
    )
    failures = []
    for url in sources:
        try:
            with urlopen(url, timeout=30) as response:
                payload = response.read(4 * 1024 * 1024)
        except (OSError, HTTPException) as error:
            failures.append(f"{url}: {error}")
            continue
        if hashlib.sha256(payload).hexdigest() == checksum:
            return payload
        failures.append(f"{url}: checksum mismatch")
    raise SystemExit(f"Could not download a verified {name} archive.\n" + "\n".join(failures))


def main():
    third_party = Path(__file__).resolve().parents[1] / "third_party"
    third_party.mkdir(exist_ok=True)
    for name, project, checksum in CODECS:
        target = third_party / name
        if (target / "CMakeLists.txt").is_file():
            print(f"Using existing {target.name}")
            continue
        if target.exists():
            raise SystemExit(f"Remove or move the incomplete directory first: {target}")
        payload = download_codec(name, project, checksum)
        with TemporaryDirectory(dir=third_party) as temporary:
            with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
                archive.extractall(temporary, filter="data")
            (Path(temporary) / name).rename(target)
        print(f"Verified and unpacked {name}")


if __name__ == "__main__":
    main()
