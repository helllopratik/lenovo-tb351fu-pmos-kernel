#!/usr/bin/env python3
"""Patch the vendor hf_manager.ko for this kernel's task_struct layout.

The vendor module (built for Android) reads current->comm, current->pid and
current->group_leader->pid at offsets 0x910/0x708/0x748.  This kernel's
task_struct has them at 0x850/0x648/0x688 (uniform -0xC0).  Without the patch
the first /dev/hf_manager open NULL-derefs in hf_client_create.

Usage:
    ./patch_hf_manager.py <vendor-in.ko> <patched-out.ko>

File offset of a .text virtual address is 0x2000 + VA (section sh_offset).
"""

import struct
import sys

TEXT_OFF = 0x2000

# virtual address -> (expected original word, patched word)
PATCHES = {
    0x0EE4: (0x91244280, 0x91214280),  # add x0, x20, #0x910 -> #0x850 (comm)
    0x0F0C: (0x91244281, 0x91214281),  # add x1, x20, #0x910 -> #0x850 (comm)
    0x0F14: (0xF943A688, 0xF9434688),  # ldr x8, [x20,#0x748] -> #0x688 (group_leader)
    0x0F30: (0xB9470908, 0xB9464908),  # ldr w8, [x8,#0x708]  -> #0x648 (pid)
    0x0F3C: (0xB9470A88, 0xB9464A88),  # ldr w8, [x20,#0x708] -> #0x648 (pid)
    0x2D28: (0xB9470908, 0xB9464908),  # poll: ldr w8,[x8,#0x708] -> #0x648
}


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1

    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())

    for va, (old, new) in PATCHES.items():
        off = TEXT_OFF + va
        cur = struct.unpack_from("<I", data, off)[0]
        if cur != old:
            raise SystemExit(
                f"{va:#06x}: unexpected bytes {cur:#010x} (want {old:#010x})"
            )
        struct.pack_into("<I", data, off, new)

    open(dst, "wb").write(data)
    print(f"patched {len(PATCHES)} words -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
