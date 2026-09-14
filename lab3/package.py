#!/usr/bin/env python3
"""Create a reproducible submission without binaries, logs or test replacements."""
import gzip
import hashlib
import io
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "pa3"
NAMES = ["bank.c", "bank_robbery.c", "clock.c", "history.c", "ipc.c", "main.c",
         "node.h", "banking.h", "common.h", "ipc.h", "pa2345.h", "Makefile", "libruntime.so"]


def main():
    destination = ROOT / "pa3.tar.gz"
    with destination.open("wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                directory = tarfile.TarInfo("pa3")
                directory.type = tarfile.DIRTYPE
                directory.mode = 0o755
                archive.addfile(directory)
                for name in sorted(NAMES):
                    data = (SOURCE / name).read_bytes()
                    entry = tarfile.TarInfo("pa3/" + name)
                    entry.size = len(data)
                    entry.mode = 0o644
                    archive.addfile(entry, io.BytesIO(data))
    with tarfile.open(destination) as archive:
        assert set(archive.getnames()) == {"pa3"} | {"pa3/" + name for name in NAMES}
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    (ROOT / "pa3.tar.gz.sha256").write_text(digest + "  pa3.tar.gz\n", encoding="ascii")
    print(str(destination))
    print("SHA256: " + digest)


if __name__ == "__main__":
    main()
