#!/usr/bin/env python3
import os
from pathlib import Path
import sys
import tempfile


SECTOR_SIZE = 512
MBR_SIGNATURE = b"\x55\xaa"
PARTITION_TABLE_OFFSET = 446
PARTITION_ENTRY_SIZE = 16
EFI_PARTITION_TYPE = 0xEF


def root_partition_index(mbr: bytes) -> int:
    first_type = mbr[PARTITION_TABLE_OFFSET + 4]
    return 2 if first_type == EFI_PARTITION_TYPE else 1


def partition_bounds(mbr: bytes, index: int) -> tuple[int, int]:
    entry = PARTITION_TABLE_OFFSET + index * PARTITION_ENTRY_SIZE
    start = int.from_bytes(mbr[entry + 8:entry + 12], "little")
    sectors = int.from_bytes(mbr[entry + 12:entry + 16], "little")
    return start * SECTOR_SIZE, sectors * SECTOR_SIZE


def shrink_image(src: Path, dst: Path) -> None:
    data = src.read_bytes()
    if len(data) < SECTOR_SIZE or data[510:512] != MBR_SIGNATURE:
        dst.write_bytes(data)
        return

    offset, length = partition_bounds(data, root_partition_index(data))
    if offset == 0 or length == 0:
        dst.write_bytes(data)
        return
    if offset >= len(data):
        raise SystemExit(f"root partition starts past end of image: {offset} >= {len(data)}")

    dst.write_bytes(data[offset:offset + min(length, len(data) - offset)])


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: shrink-image.py <input.img> <output.img>", file=sys.stderr)
        return 2

    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    if src.resolve() == dst.resolve():
        mode = src.stat().st_mode & 0o777
        with tempfile.NamedTemporaryFile(dir=dst.parent, prefix=f".{dst.name}.", delete=False) as f:
            tmp = Path(f.name)
        try:
            shrink_image(src, tmp)
            tmp.chmod(mode)
            os.replace(tmp, dst)
        finally:
            try:
                tmp.unlink()
            except FileNotFoundError:
                pass
    else:
        shrink_image(src, dst)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
