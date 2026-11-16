#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Map Android super-partition logical partitions to raw byte ranges.

The TB351FU uses a single "super" physical partition that hosts the dynamic
Android partitions (system, system_ext, product, vendor, *_dlkm).  pmOS cannot
mount those directly: they are logical partitions described by Android's
liblp metadata, not by the GPT.  This helper parses the metadata read-only and
prints one line per populated partition:

    name|offset_bytes|size_bytes|group

The offset/size pair is suitable for `losetup --offset/--sizelimit`, which is
all this device needs: every slot-A partition is a single contiguous linear
extent (verified with Android's lpdump on 2026-11-16).

Nothing is written to the super partition.  Metadata checksums are verified so
a corrupt/empty A/B metadata slot is never mistaken for a real one.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys

LP_PARTITION_RESERVED_BYTES = 4096
LP_METADATA_GEOMETRY_SIZE = 4096
# liblp writes the geometry twice (primary + backup) before the metadata slots.
LP_GEOMETRY_COPIES = 2
LP_SECTOR_SIZE = 512

LP_METADATA_GEOMETRY_MAGIC = 0x616C4467
LP_METADATA_HEADER_MAGIC = 0x414C5030

LP_TARGET_TYPE_LINEAR = 0
LP_PARTITION_NAME_LEN = 36
LP_GROUP_NAME_LEN = 36

# Geometry fields (little endian):
#   u32 magic, u32 struct_size, u8 checksum[32],
#   u32 metadata_max_size, u32 metadata_slot_count, u32 logical_block_size
GEOMETRY_STRUCT = "<II32sIII"

# Header fields common to v1.0-v1.2:
#   u32 magic, u16 major, u16 minor, u32 header_size,
#   u8 header_checksum[32], u32 tables_size, u8 tables_checksum[32],
#   descriptor[4] { u32 offset, u32 num_entries, u32 entry_size }
HEADER_STRUCT = "<IHHI32sI32s"
HEADER_DESC_SIZE = 12
HEADER_DESC_COUNT = 4
HEADER_FIXED_SIZE = struct.calcsize(HEADER_STRUCT)  # 80

PARTITION_STRUCT = "<36sIIII"  # name, attributes, first_extent, num_extents, group
EXTENT_STRUCT = "<QIQI"        # num_sectors, target_type, target_data, target_source
GROUP_STRUCT = "<36sI"         # name, flags
BLOCK_DEVICE_STRUCT = "<QQII36s"  # first_logical_sector, alignment, align_offset, flags, name


class LpError(RuntimeError):
    pass


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def parse_geometry(fh) -> dict:
    fh.seek(LP_PARTITION_RESERVED_BYTES)
    raw = fh.read(LP_METADATA_GEOMETRY_SIZE)
    if len(raw) != LP_METADATA_GEOMETRY_SIZE:
        raise LpError("short read while reading liblp geometry")

    magic, struct_size, checksum, max_size, slot_count, block_size = struct.unpack_from(
        GEOMETRY_STRUCT, raw, 0
    )
    if magic != LP_METADATA_GEOMETRY_MAGIC:
        raise LpError(f"bad liblp geometry magic 0x{magic:08x}")

    if struct_size < struct.calcsize(GEOMETRY_STRUCT) or struct_size > LP_METADATA_GEOMETRY_SIZE:
        raise LpError(f"implausible geometry struct size {struct_size}")

    # The checksum covers the structure with the checksum field zeroed.
    body = bytearray(raw[:struct_size])
    body[8:40] = b"\x00" * 32
    if sha256(bytes(body)) != checksum:
        # Old liblp builds used a different checksum?  Be strict, but say why.
        raise LpError("liblp geometry checksum mismatch")

    if max_size == 0 or slot_count == 0:
        raise LpError("liblp geometry has no metadata slots")
    if max_size > 4 * 1024 * 1024:
        raise LpError(f"implausible metadata_max_size {max_size}")
    if slot_count > 16:
        raise LpError(f"implausible metadata_slot_count {slot_count}")

    return {
        "struct_size": struct_size,
        "metadata_max_size": max_size,
        "metadata_slot_count": slot_count,
        "logical_block_size": block_size or 4096,
    }


def parse_metadata_slot(blob: bytes) -> dict | None:
    if len(blob) < HEADER_FIXED_SIZE:
        return None

    magic, major, minor, header_size = struct.unpack_from("<IHHI", blob, 0)
    if magic != LP_METADATA_HEADER_MAGIC:
        return None
    if header_size < HEADER_FIXED_SIZE or header_size > len(blob):
        return None

    header_checksum = blob[12:44]
    tables_size = struct.unpack_from("<I", blob, 44)[0]
    tables_checksum = blob[48:80]

    body = bytearray(blob[:header_size])
    body[12:44] = b"\x00" * 32
    if sha256(bytes(body)) != header_checksum:
        return None

    descs = []
    for i in range(HEADER_DESC_COUNT):
        off, num, size = struct.unpack_from(
            "<III", blob, HEADER_FIXED_SIZE + i * HEADER_DESC_SIZE
        )
        descs.append((off, num, size))

    tables_start = header_size
    tables_end = tables_start + tables_size
    if tables_end > len(blob):
        return None
    tables = blob[tables_start:tables_end]
    if sha256(tables) != tables_checksum:
        return None

    # The descriptor offsets are relative to the start of the tables area.
    partitions_desc, extents_desc, groups_desc, block_devices_desc = descs

    def table(desc, struct_fmt, what):
        off, num, entry_size = desc
        need = struct.calcsize(struct_fmt)
        if num and entry_size < need:
            raise LpError(f"{what} entry size {entry_size} < {need}")
        if off + num * entry_size > len(tables):
            raise LpError(f"{what} table runs past metadata tables")
        return tables[off : off + num * entry_size], num, entry_size

    ptab, pnum, psize = table(partitions_desc, PARTITION_STRUCT, "partition")
    etab, enum, esize = table(extents_desc, EXTENT_STRUCT, "extent")
    gtab, gnum, gsize = table(groups_desc, GROUP_STRUCT, "group")
    btab, bnum, bsize = table(block_devices_desc, BLOCK_DEVICE_STRUCT, "block device")

    def name_at(tab, index, entry_size, what, name_offset=0):
        if entry_size < name_offset + LP_PARTITION_NAME_LEN:
            raise LpError(f"{what} name field missing")
        start = index * entry_size + name_offset
        raw = tab[start : start + LP_PARTITION_NAME_LEN]
        return raw.split(b"\x00", 1)[0].decode("ascii", "replace")

    groups = [name_at(gtab, i, gsize, "group") for i in range(gnum)]
    # LpMetadataBlockDevice stores first_logical_sector/alignment/... before
    # the partition name, unlike partitions and groups.
    block_devices = [
        name_at(btab, i, bsize, "block device", name_offset=24) for i in range(bnum)
    ]

    partitions = []
    for i in range(pnum):
        base = i * psize
        (raw_name, attributes, first_extent, num_extents, group_index) = struct.unpack_from(
            PARTITION_STRUCT, ptab, base
        )
        name = raw_name.split(b"\x00", 1)[0].decode("ascii", "replace")
        if not name:
            continue
        if group_index >= len(groups) and num_extents:
            raise LpError(f"partition {name} references missing group {group_index}")
        extents = []
        if num_extents:
            if first_extent + num_extents > enum:
                raise LpError(f"partition {name} extents run past extent table")
            for j in range(num_extents):
                ebase = (first_extent + j) * esize
                num_sectors, target_type, target_data, target_source = struct.unpack_from(
                    EXTENT_STRUCT, etab, ebase
                )
                if target_source >= len(block_devices):
                    raise LpError(
                        f"partition {name} extent references missing block device {target_source}"
                    )
                extents.append(
                    {
                        "num_sectors": num_sectors,
                        "target_type": target_type,
                        "target_data": target_data,
                        "target_source": target_source,
                    }
                )
        partitions.append(
            {
                "name": name,
                "attributes": attributes,
                "group": groups[group_index] if group_index < len(groups) else "",
                "extents": extents,
            }
        )

    return {
        "major": major,
        "minor": minor,
        "partitions": partitions,
        "block_devices": block_devices,
    }


def read_metadata_slots(fh, geometry: dict) -> list[dict]:
    slots = []
    max_size = geometry["metadata_max_size"]
    for slot in range(geometry["metadata_slot_count"]):
        off = (
            LP_PARTITION_RESERVED_BYTES
            + LP_GEOMETRY_COPIES * LP_METADATA_GEOMETRY_SIZE
            + slot * max_size
        )
        fh.seek(off)
        blob = fh.read(max_size)
        if len(blob) != max_size:
            break
        parsed = parse_metadata_slot(blob)
        if parsed:
            parsed["slot"] = slot
            slots.append(parsed)
    return slots


def device_size(path: str) -> int | None:
    try:
        st = os.stat(path)
    except OSError:
        return None
    if st.st_size > 0:
        return st.st_size
    # Block device: BLKGETSIZE64 = _IOR(0x12, 114, size_t)
    try:
        import fcntl

        with open(path, "rb", buffering=0) as fh:
            buf = fcntl.ioctl(fh.fileno(), 0x80081272, b"\x00" * 8)
        return struct.unpack("<Q", buf)[0]
    except (OSError, ImportError):
        return None


def find_super_device() -> str | None:
    for candidate in ("/dev/disk/by-partlabel/super", "/dev/block/by-name/super"):
        if os.path.exists(candidate):
            return os.path.realpath(candidate)

    # udev may not have created by-partlabel yet (or the GPT label differs).
    # Scan the kernel's uevents for PARTNAME=super instead of shelling out.
    try:
        entries = os.listdir("/sys/class/block")
    except OSError:
        entries = []
    for entry in entries:
        uevent = f"/sys/class/block/{entry}/uevent"
        try:
            with open(uevent, "r", encoding="ascii", errors="replace") as fh:
                lines = fh.read().splitlines()
        except OSError:
            continue
        if "PARTNAME=super" not in lines:
            continue
        devname = next((ln.split("=", 1)[1] for ln in lines if ln.startswith("DEVNAME=")), None)
        if devname:
            return f"/dev/{devname}"

    try:
        out = subprocess.run(
            ["blkid", "-t", "PARTLABEL=super", "-o", "device"],
            check=False,
            capture_output=True,
            text=True,
        )
        for line in out.stdout.splitlines():
            line = line.strip()
            if line:
                return os.path.realpath(line)
    except OSError:
        pass
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Print android super partition extents as name|offset|size|group"
    )
    parser.add_argument(
        "--device",
        help="super partition (default: auto-detect via by-partlabel/uevent)",
    )
    parser.add_argument(
        "--slot",
        type=int,
        default=None,
        help="force a metadata slot number instead of choosing the populated one",
    )
    parser.add_argument(
        "--print-device",
        action="store_true",
        help="print the resolved super device path and exit",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="print metadata diagnostics to stderr",
    )
    args = parser.parse_args()

    device = args.device or find_super_device()
    if not device:
        print("pmos-supermap: no super partition found", file=sys.stderr)
        return 1
    if not os.path.exists(device):
        print(f"pmos-supermap: {device} does not exist", file=sys.stderr)
        return 1

    if args.print_device:
        print(device)
        return 0

    try:
        with open(device, "rb", buffering=0) as fh:
            geometry = parse_geometry(fh)
            slots = read_metadata_slots(fh, geometry)
    except (OSError, LpError) as exc:
        print(f"pmos-supermap: {exc}", file=sys.stderr)
        return 1

    if not slots:
        print("pmos-supermap: no valid liblp metadata slot found", file=sys.stderr)
        return 1

    if args.slot is not None:
        chosen = next((s for s in slots if s["slot"] == args.slot), None)
        if not chosen:
            print(f"pmos-supermap: metadata slot {args.slot} is empty/invalid", file=sys.stderr)
            return 1
    else:
        # Pick the slot with the most populated partitions.  For virtual-A/B
        # super partitions one metadata slot describes slot A (populated) and
        # the other describes the inactive slot B (all extents empty).
        chosen = max(
            slots,
            key=lambda s: (
                sum(1 for p in s["partitions"] if p["extents"]),
                -s["slot"],
            ),
        )
        if not any(p["extents"] for p in chosen["partitions"]):
            print("pmos-supermap: every metadata slot is empty", file=sys.stderr)
            return 1

    if args.verbose:
        print(
            "pmos-supermap: "
            f"device={device} slot={chosen['slot']} "
            f"version={chosen['major']}.{chosen['minor']} "
            f"partitions={len(chosen['partitions'])} "
            f"block_devices={','.join(chosen['block_devices']) or 'none'}",
            file=sys.stderr,
        )

    total_bytes = device_size(device)
    emitted = 0
    for part in chosen["partitions"]:
        name = part["name"]
        extents = part["extents"]
        if not extents:
            continue
        if len(extents) != 1:
            print(
                f"pmos-supermap: skipping {name}: {len(extents)} extents "
                "(only single linear extents are supported)",
                file=sys.stderr,
            )
            continue
        extent = extents[0]
        if extent["target_type"] != LP_TARGET_TYPE_LINEAR:
            print(
                f"pmos-supermap: skipping {name}: target type {extent['target_type']} "
                "is not linear",
                file=sys.stderr,
            )
            continue
        if extent["target_source"] >= len(chosen["block_devices"]):
            print(f"pmos-supermap: skipping {name}: bad block device index", file=sys.stderr)
            continue
        target = chosen["block_devices"][extent["target_source"]]
        if target != "super":
            print(
                f"pmos-supermap: skipping {name}: backed by {target}, not super",
                file=sys.stderr,
            )
            continue

        offset = extent["target_data"] * LP_SECTOR_SIZE
        size = extent["num_sectors"] * LP_SECTOR_SIZE
        if size == 0:
            continue
        if total_bytes is not None and offset + size > total_bytes:
            print(
                f"pmos-supermap: skipping {name}: extent {offset}+{size} "
                f"exceeds {device} ({total_bytes} bytes)",
                file=sys.stderr,
            )
            continue

        print(f"{name}|{offset}|{size}|{part['group']}")
        emitted += 1

    if args.verbose:
        print(f"pmos-supermap: emitted {emitted} partitions", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
