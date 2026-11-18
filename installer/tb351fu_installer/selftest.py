# SPDX-License-Identifier: GPL-2.0
"""Headless checks for the installer backend and the helper scripts.

Run with `python3 -m tb351fu_installer --self-test`.  No device is needed;
the GTK import is checked but no window is created.
"""

from __future__ import annotations

import json
import os
import re
import shlex
import struct
import subprocess
import tempfile
from pathlib import Path

from . import backend

GIB = backend.GIB
MIB = backend.MIB


# ---------------------------------------------------------------------------
# synthetic images
# ---------------------------------------------------------------------------

def make_gpt(path: Path) -> None:
    """Write a tiny two-partition GPT disk image."""
    sector = 512
    total = 400
    data = bytearray(sector * total)

    # Protective MBR.
    struct.pack_into(
        "<B3sB3sII", data, 446,
        0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, total - 1,
    )
    data[510:512] = b"\x55\xaa"

    # GPT header at LBA 1.
    header = bytearray(92)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)   # revision
    struct.pack_into("<I", header, 12, 92)          # header size
    struct.pack_into("<Q", header, 24, 1)           # current LBA
    struct.pack_into("<Q", header, 32, total - 1)   # backup LBA
    struct.pack_into("<Q", header, 40, 34)          # first usable LBA
    struct.pack_into("<Q", header, 48, total - 34)  # last usable LBA
    header[56:72] = bytes(range(16))                # disk GUID
    struct.pack_into("<Q", header, 72, 2)           # entries LBA
    struct.pack_into("<I", header, 80, 128)         # entry count
    struct.pack_into("<I", header, 84, 128)         # entry size
    data[sector:sector + len(header)] = header

    # Partition entries at LBA 2.
    entries = bytearray(128 * 128)
    type_guid = bytes(reversed(range(16)))
    struct.pack_into("<16s16sQQQ", entries, 0,
                     type_guid, bytes(range(16)), 34, 99, 0)
    struct.pack_into("<16s16sQQQ", entries, 128,
                     type_guid, bytes(reversed(range(16))), 100, 299, 0)
    data[2 * sector:2 * sector + len(entries)] = entries

    path.write_bytes(data)


# ---------------------------------------------------------------------------
# checks
# ---------------------------------------------------------------------------

def check_sizes() -> str:
    assert backend.parse_size("64G") == 64 * GIB
    assert backend.parse_size(" 512m ") == 512 * MIB
    assert backend.parse_size("4096") == 4096
    try:
        backend.parse_size("nope")
    except ValueError:
        pass
    else:
        raise AssertionError("parse_size accepted an invalid value")

    assert backend.human_size(0) == "0 B"
    assert backend.human_size(1024) == "1.0 KiB"
    assert backend.human_size(64 * GIB) == "64.0 GiB"

    suggested, fits = backend.suggest_rootfs_size(100 * GIB)
    assert suggested == 64 * GIB and fits
    tight, tight_fits = backend.suggest_rootfs_size(10 * GIB)
    assert tight >= 4 * GIB and not tight_fits
    return (f"64G parses; 100 GiB free suggests {suggested // GIB} GiB; "
            f"10 GiB free is rejected as too tight")


def check_gpt() -> str:
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / "synthetic.img"
        make_gpt(image)
        parts = backend.gpt_partitions(image)
        assert parts == [(1, 34, 99), (2, 100, 299)], parts
        assert backend.image_kind(image) == "gpt"
        assert backend.rootfs_content_size(image) == 200 * 512
    return "GPT header and partition entries parsed"


def check_image_kinds() -> str:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        zstd = root / "image.img.zst"
        zstd.write_bytes(b"\x28\xb5\x2f\xfd" + b"\x00" * 64)
        assert backend.image_kind(zstd) == "zstd"

        ext4 = root / "ext4.img"
        ext4.write_bytes(b"\x00" * 1080 + b"\x53\xef" + b"\x00" * 1024)
        assert backend.image_kind(ext4) == "ext4"

        unknown = root / "unknown.bin"
        unknown.write_bytes(b"\x00" * 4096)
        assert backend.image_kind(unknown) == "unknown"
    return "zstd, ext4 and unknown magic detected"


def check_validate_images() -> str:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        boot = root / "boot.img"
        init_boot = root / "init_boot.img"
        vbmeta = root / "vbmeta.img"
        vendor_boot = root / "vendor_boot.img"
        dtbo = root / "dtbo.img"
        rootfs = root / "rootfs.img"
        modules = root / "modules.tar.xz"
        firmware = root / "firmware.tar.xz"
        boot.write_bytes(b"\x00" * MIB)
        init_boot.write_bytes(b"\x00" * MIB)
        vbmeta.write_bytes(b"\x00" * 4096)
        vendor_boot.write_bytes(b"\x00" * MIB)
        dtbo.write_bytes(b"\x00" * MIB)
        rootfs.write_bytes(b"\x00" * MIB)
        modules.write_bytes(b"\x00" * 16)
        firmware.write_bytes(b"\x00" * 16)
        firmware_images = {}
        for part in backend.SLOT_B_FIRMWARE:
            image = root / f"{part}.img"
            image.write_bytes(b"\x00" * 16)
            firmware_images[part] = image

        images = backend.ImageSet(
            boot=boot, init_boot=init_boot, vbmeta=vbmeta,
            vendor_boot=vendor_boot, dtbo=dtbo,
            rootfs=rootfs, modules=modules, firmware=firmware,
            slot_b_firmware=firmware_images,
        )
        assert backend.validate_images(images) == []
        assert backend.validate_images(images, need_firmware=True) == []

        os.truncate(boot, backend.BOOT_PART_SIZE + 1)
        problems = backend.validate_images(images)
        assert any("64 MiB boot partition" in p for p in problems), problems

        # vendor_boot and dtbo are required for a complete slot-B boot chain.
        problems = backend.validate_images(
            backend.ImageSet(boot=boot, init_boot=init_boot, vbmeta=vbmeta,
                             dtbo=dtbo, rootfs=rootfs, modules=modules,
                             firmware=firmware))
        assert any("vendor_boot image not found" in p for p in problems), problems
        problems = backend.validate_images(
            backend.ImageSet(boot=boot, init_boot=init_boot, vbmeta=vbmeta,
                             vendor_boot=vendor_boot, rootfs=rootfs,
                             modules=modules, firmware=firmware))
        assert any("dtbo image not found" in p for p in problems), problems

        partial = dict(firmware_images)
        partial.pop("lk")
        problems = backend.validate_images(
            backend.ImageSet(boot=boot, init_boot=init_boot, vbmeta=vbmeta,
                             vendor_boot=vendor_boot, dtbo=dtbo,
                             slot_b_firmware=partial),
            need_rootfs=False, need_firmware=True,
        )
        assert any("lk.img" in p for p in problems), problems

        # Boot-chain checks are skipped when only the rootfs is wanted.
        problems = backend.validate_images(
            backend.ImageSet(rootfs=rootfs, modules=modules,
                             firmware=firmware),
            need_boot=False,
        )
        assert problems == [], problems

        problems = backend.validate_images(
            backend.ImageSet(rootfs=rootfs, modules=modules),
            need_boot=False,
        )
        assert any("firmware.tar.xz" in p for p in problems), problems
    return ("size limits, vendor_boot/dtbo and rootfs payload checks work")


def check_bundle_scan() -> str:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name in ("boot_pmos.img", "init_boot_pmos.img", "vbmeta.img",
                     "vendor_boot.img", "dtbo.img"):
            (root / name).write_bytes(b"\x00" * MIB)
        for name in backend.SLOT_B_FIRMWARE.values():
            (root / name).write_bytes(b"\x00" * 16)

        images = backend.scan_bundle(root)
        assert images.boot == root / "boot_pmos.img", images.boot
        assert images.init_boot == root / "init_boot_pmos.img"
        assert images.vbmeta == root / "vbmeta.img"
        assert images.vendor_boot == root / "vendor_boot.img"
        assert images.dtbo == root / "dtbo.img"
        assert images.bundle_dir == root
        assert set(images.slot_b_firmware) == set(backend.SLOT_B_FIRMWARE)
        assert backend.slot_b_firmware_dir(images) == root.resolve()
    return "slot-B bundle names and firmware directory resolved"


def check_partition_roles() -> str:
    assert backend._role_for("boot_b") == ("Kernel + AVB boot image", "b")
    assert backend._role_for("vbmeta_system_a") == ("AVB system metadata", "a")
    assert backend._role_for("userdata") == ("Android /data (rootfs loop image)", "")
    return "slot suffix and roles resolved"


def _script_paths() -> tuple[Path, Path] | None:
    paths = backend.repo_paths()
    rootfs = paths["common"] / "install_pmos_rootfs.sh"
    flash = paths["deploy"] / "scripts" / "flash_slot_b.sh"
    if rootfs.is_file() and flash.is_file():
        return rootfs, flash
    return None


def check_scripts() -> str:
    found = _script_paths()
    if not found:
        return "SKIP: no pmOS checkout found (TB351FU_COMMON_DIR unset)"
    rootfs, flash = found

    for script in (rootfs, flash):
        proc = subprocess.run(["bash", "-n", str(script)],
                              capture_output=True, text=True)
        assert proc.returncode == 0, f"{script}: {proc.stderr.strip()}"

    proc = subprocess.run([str(rootfs), "--help"],
                          capture_output=True, text=True,
                          env={**os.environ, "ANDROID_SERIAL": ""})
    assert proc.returncode == 0, proc.stderr
    assert "--size" in proc.stdout and "--image" in proc.stdout

    proc = subprocess.run([str(rootfs), "--size", "nope"],
                          capture_output=True, text=True)
    assert proc.returncode == 2, (proc.returncode, proc.stderr)

    proc = subprocess.run([str(flash), "--help"],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert "--dry-run" in proc.stdout
    assert "--vendor-boot" in proc.stdout
    assert "--dtbo" in proc.stdout
    assert "--firmware-dir" in proc.stdout

    proc = subprocess.run([str(flash), "--bogus"],
                          capture_output=True, text=True)
    assert proc.returncode == 2, (proc.returncode, proc.stderr)

    text = flash.read_text()
    assert "%%PMOS_DONE" in text
    assert "/dev/block/by-name/boot_a" not in text
    assert "flash boot_a" not in text
    assert "vendor_boot_b" in text and "dtbo_b" in text
    assert "FIRMWARE_PARTS" in text and "${part}_b" in text
    for target in re.findall(r"of=/dev/block/by-name/([^\s'\";]+)", text):
        assert not target.endswith("_a"), f"slot-A write target: {target}"

    rootfs_text = rootfs.read_text()
    assert "%%PMOS_PROGRESS:" in rootfs_text
    assert "%%PMOS_DONE" in rootfs_text

    # The markers must contain two literal percent signs on the wire; printf
    # consumes %% otherwise.  The scripts expose a tiny hook for this test.
    expected = [
        "%%PMOS_STEP:selftest:ok",
        "%%PMOS_PROGRESS:50:selftest",
        "%%PMOS_DONE",
    ]
    for script in (rootfs, flash):
        proc = subprocess.run([str(script)], capture_output=True, text=True,
                              env={**os.environ, "PMOS_MARKER_TEST": "1"})
        assert proc.returncode == 0, (script, proc.stderr)
        assert proc.stdout.splitlines() == expected, (script, proc.stdout)
        proc = subprocess.run([str(script)], capture_output=True, text=True,
                              env={**os.environ, "PMOS_MARKER_TEST": "error"})
        assert proc.returncode == 1, (script, proc.returncode)
        assert "%%PMOS_ERROR:1" in proc.stdout, (script, proc.stdout)
    return "shell syntax, help, marker bytes, option errors and slot-A guards"


def check_repo_paths() -> str:
    paths = backend.repo_paths()
    if not (paths["common"] / "install_pmos_rootfs.sh").is_file():
        return f"SKIP: checkout not found (common={paths['common']})"
    images = backend.find_images()
    found = [name for name in ("boot", "init_boot", "vbmeta", "vendor_boot",
                               "dtbo", "rootfs", "modules", "firmware")
             if getattr(images, name)]
    payload = backend._images_json(images)
    assert json.loads(json.dumps(payload))["bundle_dir"] == (
        str(images.bundle_dir) if images.bundle_dir else None)
    return (f"common={paths['common']}, found: {', '.join(found) or 'none'}, "
            f"firmware: {len(images.slot_b_firmware)}/"
            f"{len(backend.SLOT_B_FIRMWARE)}")


def check_orchestration() -> str:
    """Run backend.install against fake helper scripts."""
    with tempfile.TemporaryDirectory() as tmp:
        common = Path(tmp) / "common"
        scripts = common / "deploy" / "scripts"
        scripts.mkdir(parents=True)
        flash = scripts / "flash_slot_b.sh"
        rootfs = common / "install_pmos_rootfs.sh"
        args_file = common / "flash_args.txt"
        flash.write_text(
            "#!/usr/bin/env bash\n"
            f"printf '%s\\n' \"$@\" > {shlex.quote(str(args_file))}\n"
            "echo '%%PMOS_STEP:write:Writing fake boot'\n"
            "echo '%%PMOS_PROGRESS:50:Halfway'\n"
            "echo '%%PMOS_DONE'\n"
        )
        rootfs.write_text(
            "#!/usr/bin/env bash\n"
            "echo '%%PMOS_STEP:unpack:Fake unpack'\n"
            "echo '%%PMOS_PROGRESS:25:Quarter'\n"
            "echo '%%PMOS_DONE'\n"
        )
        firmware_dir = common / "firmware"
        firmware_dir.mkdir()
        firmware_images = {}
        for part, name in backend.SLOT_B_FIRMWARE.items():
            image = firmware_dir / name
            image.write_bytes(b"\x00" * 16)
            firmware_images[part] = image

        old = os.environ.get("TB351FU_COMMON_DIR")
        os.environ["TB351FU_COMMON_DIR"] = str(common)
        try:
            lines: list[str] = []
            options = backend.InstallOptions(
                device=backend.Device(serial="FAKE", mode="adb", root=True),
                images=backend.ImageSet(
                    boot=common / "boot.img",
                    init_boot=common / "init_boot.img",
                    vbmeta=common / "vbmeta.img",
                    vendor_boot=common / "vendor_boot.img",
                    dtbo=common / "dtbo.img",
                    rootfs=common / "rootfs.img",
                ),
                size_bytes=16 * GIB,
                switch_slot=False,
            )
            backend.install(options, lines.append)
            args_text = args_file.read_text()

            fw_lines: list[str] = []
            fw_options = backend.InstallOptions(
                device=backend.Device(serial="FAKE", mode="adb", root=True),
                images=backend.ImageSet(
                    boot=common / "boot.img",
                    init_boot=common / "init_boot.img",
                    vbmeta=common / "vbmeta.img",
                    vendor_boot=common / "vendor_boot.img",
                    dtbo=common / "dtbo.img",
                    slot_b_firmware=firmware_images,
                ),
                size_bytes=16 * GIB,
                flash_firmware=True,
                install_rootfs=False,
                switch_slot=False,
            )
            backend.install(fw_options, fw_lines.append)
            fw_args_text = args_file.read_text()
        finally:
            if old is None:
                os.environ.pop("TB351FU_COMMON_DIR", None)
            else:
                os.environ["TB351FU_COMMON_DIR"] = old

    done = [line for line in lines if line == "%%PMOS_DONE"]
    assert len(done) == 1, done
    assert any(line.startswith("%%PMOS_STEP:boot:") for line in lines), lines
    assert any(line.startswith("%%PMOS_STEP:rootfs:") for line in lines), lines
    assert any(line.startswith("%%PMOS_STEP:unpack:") for line in lines), lines
    assert "%%PMOS_PROGRESS:100:Installation complete" in lines, lines
    # Child DONE markers must not leak through.
    assert done == ["%%PMOS_DONE"], lines
    for flag in ("--vendor-boot", "--dtbo"):
        assert flag in args_text.splitlines(), args_text
    assert "--firmware-dir" not in args_text.splitlines(), args_text
    assert "--firmware-dir" in fw_args_text.splitlines(), fw_args_text
    assert str(firmware_dir.resolve()) in fw_args_text.splitlines(), fw_args_text
    assert fw_lines.count("%%PMOS_DONE") == 1, fw_lines
    return "two helper phases, bundle args, firmware opt-in, one final DONE"


def check_gui_import() -> str:
    try:
        import gi
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gtk  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        raise AssertionError(f"GTK4/PyGObject unavailable: {exc}") from exc
    from . import app  # noqa: F401
    return "GTK4 application imports"


CHECKS = [
    ("size parsing", check_sizes),
    ("GPT parsing", check_gpt),
    ("image magic", check_image_kinds),
    ("image validation", check_validate_images),
    ("bundle discovery", check_bundle_scan),
    ("partition roles", check_partition_roles),
    ("helper scripts", check_scripts),
    ("repository discovery", check_repo_paths),
    ("install orchestration", check_orchestration),
    ("GTK4 import", check_gui_import),
]


def run() -> int:
    failures = 0
    skips = 0
    for name, check in CHECKS:
        try:
            detail = check() or ""
        except Exception as exc:  # noqa: BLE001 - this is the test runner
            failures += 1
            print(f"[FAIL] {name}: {exc}")
            continue
        if detail.startswith("SKIP:"):
            skips += 1
            print(f"[SKIP] {name}: {detail[5:].strip()}")
        else:
            print(f"[PASS] {name}: {detail}")
    total = len(CHECKS)
    print(f"\n{total - failures - skips} passed, {failures} failed, "
          f"{skips} skipped")
    return 0 if failures == 0 else 1
