#!/usr/bin/env python3
"""Pack only the allocated extents of a sparse image.

The result is understood by deploy/tools/sparse_pack.c.  Keeping holes out
of the transfer avoids adb expanding a sparse 64 GiB rootfs into 64 GiB of
allocated data on the tablet.

Usage: make_sparse_pack.py SRC DST
"""

import errno
import os
import struct
import sys
import zlib

MAGIC = b"PMOSPK01"
CHUNK = 1 << 20


def iter_extents(fd, size):
    off = 0
    while off < size:
        try:
            data = os.lseek(fd, off, os.SEEK_DATA)
        except OSError as exc:
            if exc.errno == errno.ENXIO:  # no more data
                return
            raise
        hole = os.lseek(fd, data, os.SEEK_HOLE)
        if hole <= data:
            return
        yield data, hole - data
        off = hole


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make_sparse_pack.py SRC DST")
    src, dst = sys.argv[1], sys.argv[2]
    sfd = os.open(src, os.O_RDONLY)
    try:
        size = os.fstat(sfd).st_size
        extents = list(iter_extents(sfd, size))
        # Some filesystems do not implement SEEK_DATA.  Fall back to one
        # full extent rather than producing an empty pack.
        if not extents and size:
            extents = [(0, size)]

        data_bytes = 0
        with open(dst, "wb") as out:
            out.write(MAGIC)
            out.write(struct.pack("<QQ", size, len(extents)))
            for off, length in extents:
                out.write(struct.pack("<QQ", off, length))
                pos = off
                end = off + length
                crc = 0
                while pos < end:
                    chunk = os.pread(sfd, min(CHUNK, end - pos), pos)
                    if not chunk:
                        raise IOError("short read at %d" % pos)
                    out.write(chunk)
                    crc = zlib.crc32(chunk, crc)
                    pos += len(chunk)
                out.write(struct.pack("<I", crc & 0xFFFFFFFF))
                data_bytes += length
            out.flush()
            os.fsync(out.fileno())
    finally:
        os.close(sfd)

    print(f"packed_size={os.path.getsize(dst)}")
    print(f"logical_size={size}")
    print(f"data_bytes={data_bytes}")
    print(f"extents={len(extents)}")


if __name__ == "__main__":
    main()
