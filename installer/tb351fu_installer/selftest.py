# SPDX-License-Identifier: GPL-2.0
"""Headless checks for the installer backend and the helper scripts.

Run with `python3 -m tb351fu_installer --self-test`.  No device is needed;
the GTK import is checked but no window is created.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import tarfile
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

        # A server download does not need a local rootfs path, but the payload
        # archives are still required.
        problems = backend.validate_images(
            backend.ImageSet(modules=modules, firmware=firmware),
            need_boot=False, need_local_rootfs=False,
        )
        assert problems == [], problems
        problems = backend.validate_images(
            backend.ImageSet(modules=modules, firmware=firmware),
            need_boot=False,
        )
        assert any("rootfs image not found" in p for p in problems), problems
    return ("size limits, vendor_boot/dtbo, local/server rootfs and payload "
            "checks work")


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


def check_boot_chain_selection() -> str:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        images = root / "images"
        artifacts = root / "artifacts"
        images.mkdir()
        artifacts.mkdir()
        packaged = images / "boot_b.img"
        latest = artifacts / "boot_latest.img"
        packaged.write_bytes(b"packaged")
        latest.write_bytes(b"latest")

        # A fresh local build must beat the packaged template.
        os.utime(packaged, (1000, 1000))
        os.utime(latest, (2000, 2000))
        chosen = backend._packaged_or_latest(
            images, artifacts, "boot_b.img", "boot_latest.img", [])
        assert chosen == latest, chosen

        # If the packaged bundle was refreshed deliberately, it wins.
        os.utime(packaged, (3000, 3000))
        chosen = backend._packaged_or_latest(
            images, artifacts, "boot_b.img", "boot_latest.img", [])
        assert chosen == packaged, chosen
    return "newer artifacts build wins; refreshed package keeps priority"


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
    assert "--password-hash-file" in proc.stdout
    assert "--source" in proc.stdout and "--url" in proc.stdout
    assert "--mode" in proc.stdout and "--whole-output" in proc.stdout
    assert "--replace-existing" in proc.stdout and "--remove-existing" in proc.stdout

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
    assert 'read -r -p "pmOS username: " PMOS_USERNAME' in rootfs_text
    assert 'die "could not read a pmOS username"' in rootfs_text
    assert "pmOS username [user]" not in rootfs_text
    # Prompts are captured through command substitution, so stdin must be the
    # terminal check.  Password newlines belong on stderr, not in the value.
    assert "[ -t 0 ]" in rootfs_text
    assert "echo >&2" in rootfs_text

    # Android compatibility regressions.  toybox has no `df -B1` and no zstd,
    # and its e2fsprogs 1.46 rejects the ext4 orphan_file feature.  A
    # read-write loop mount is only permitted under media_rw_data_file, so the
    # device helper must relabel temporarily and restore system_data_file.
    device = rootfs.parent / "deploy" / "installer" / "device_install.sh"
    device_text = device.read_text()
    assert "df -k /data" in rootfs_text
    assert "df -B1 /data" not in rootfs_text
    assert "zstd -d -f" in rootfs_text
    assert "tune2fs -O ^orphan_file" in rootfs_text
    assert "u:object_r:media_rw_data_file:s0" in device_text
    assert "u:object_r:system_data_file:s0" in device_text
    assert 'losetup -d "$LOOP" 2>/dev/null || true' in device_text
    assert "rm -f /data/pmos/rootfs.img\n" not in rootfs_text

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
    whole = rootfs.parent / "deploy" / "installer" / "whole_device_install.sh"
    proc = subprocess.run(["sh", "-n", str(whole)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, f"{whole}: {proc.stderr.strip()}"
    proc = subprocess.run(["sh", str(whole), "--help"], capture_output=True, text=True)
    assert proc.returncode == 0, (whole, proc.stderr)
    assert "ERASE ANDROID SLOT A" in proc.stdout
    assert "--remove-legacy" in proc.stdout
    assert "--dry-run" in proc.stdout
    assert "ERASE ANDROID SLOT A" in proc.stdout
    assert "--remove-legacy" in proc.stdout
    whole_text = whole.read_text()
    assert "ERASE ANDROID SLOT A" in whole_text
    assert "misc" in whole_text
    assert "pmos-dm-key" in whole_text
    assert "remove --force" in whole_text or "dmsetup remove" in whole_text
    assert "provision_slot_b_from_a vendor_boot" in whole_text
    assert "provision_slot_b_from_a dtbo" in whole_text
    assert "vbmeta_system" in whole_text and "vbmeta_vendor" in whole_text
    proc = subprocess.run(["sh", str(whole), "--bogus"],
                          capture_output=True, text=True)
    assert proc.returncode != 0, (proc.returncode, proc.stderr)
    return ("shell syntax, help, marker bytes, option errors, whole apply "
            "safety, slot-A guards and slot-B vendor_boot/dtbo provisioning")


def check_gnss_payload() -> str:
    paths = backend.repo_paths()
    common = paths["common"]
    gnss = paths["deploy"] / "gnss"
    tool = paths["deploy"] / "tools" / "gnss_payload.py"

    rootfs_text = (common / "install_pmos_rootfs.sh").read_text()
    assert "deploy/tools/gnss_payload.py" in rootfs_text
    assert "--repo-gnss" in rootfs_text

    tool_text = tool.read_text()
    assert 'MNLD = "/vendor/bin/mnld"' in tool_text
    assert "MNL_Config.xml" in tool_text
    assert "mpe.conf" in tool_text
    assert '"vendor", "bin"' in tool_text
    assert '"system", "vendor"' in tool_text
    assert "verify_payload(opt, tree)" in tool_text

    proc = subprocess.run([sys.executable, str(tool), "--help"],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    for flag in ("--payload", "--repo-gnss", "--adb"):
        assert flag in proc.stdout, (flag, proc.stdout)

    conf = (gnss / "geoclue.conf").read_text()
    assert "nmea-socket=/run/tablet-gps/nmea.sock" in conf
    for section in ("ip", "wifi", "static-source", "3g", "cdma",
                    "modem-gps"):
        match = re.search(
            rf"^\[{re.escape(section)}\]\s*$([\s\S]*?)(?=^\[|\Z)",
            conf, re.MULTILINE,
        )
        assert match, section
        assert re.search(r"^enable=false\s*$", match.group(1),
                         re.MULTILINE), section
    assert re.search(
        r"^\[network-nmea\]\s*$[\s\S]*?^enable=true\s*$",
        conf, re.MULTILINE,
    )

    for name in ("tablet-gnss-bridge.py", "run-gps.sh", "run-mnld.sh",
                 "gps-runtime-inner.sh", "ld.config.txt",
                 "tb351fu-gps.service", "tb351fu-gps.socket",
                 "modules/gps_pwr.ko", "modules/gps_drv_stp.ko"):
        assert (gnss / name).is_file(), name

    spec = importlib.util.spec_from_file_location("gnss_payload", tool)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    needed = module.elf_needed(sys.executable)
    assert needed, "ELF DT_NEEDED parser returned nothing"

    return ("GNSS builder wired into the installer, NMEA-only GeoClue config, "
            "units/modules present and ELF dependency parser works")


def check_charger_payload() -> str:
    """The shipped payload and installer must keep the charger stack intact."""
    paths = backend.repo_paths()
    rootfs = paths["common"] / "install_pmos_rootfs.sh"
    payload = paths["deploy"] / "modules.tar.xz"
    if not rootfs.is_file() or not payload.is_file():
        return f"SKIP: payload not found (deploy={paths['deploy']})"

    expected = {
        "charger/charger_class.ko",
        "charger/mtk_charger_algorithm_class.ko",
        "charger/adapter_class.ko",
        "charger/dev-info.ko",
        "charger/mt6375.ko",
        "charger/sc8989x_charger.ko",
        "charger/pmos_chg_stub.ko",
        "charger/pmos_charge_cap.ko",
        "charger/pmos_otg_vbus.ko",
    }
    with tarfile.open(payload, "r:xz") as archive:
        members = {
            member.name.removeprefix("./"): member
            for member in archive.getmembers()
            if member.isfile()
        }
    missing = expected - members.keys()
    assert not missing, f"missing from payload: {sorted(missing)}"

    # The rootfs installer extracts the archive straight into
    # /usr/lib/modules/extra, so the charger/ prefix must survive.
    with tempfile.TemporaryDirectory() as tmp:
        dest = Path(tmp) / "usr" / "lib" / "modules" / "extra"
        with tarfile.open(payload, "r:xz") as archive:
            for name in expected:
                source = archive.extractfile(members[name])
                assert source is not None, name
                target = dest / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read())
                assert target.stat().st_size > 0, name

    configs = paths["deploy"] / "configs"
    for name in ("pmos-charge-limit.desktop", "pmos-charge-toggle.desktop",
                 "pmos-charge-cap.conf", "sudoers-pmos-charge"):
        assert (configs / name).is_file(), name
    rootfs_text = rootfs.read_text()
    assert "pmos-charge-limit.desktop" in rootfs_text
    assert "sudoers-pmos-charge" in rootfs_text
    assert "pmos-charge-cap.conf" in rootfs_text
    assert "usr/lib/modules/extra" in rootfs_text
    device_text = (paths["deploy"] / "installer" / "device_install.sh").read_text()
    assert "chown -R 0:0 \"$MNT/etc/sudoers.d\"" in device_text
    assert "lenovo-tab-plus-tb351fu" in device_text
    assert "sshd.service" in rootfs_text
    init_text = (paths["deploy"] / "scripts" / "init_charger.sh").read_text()
    assert "LIMIT=80" in init_text
    return ("payload keeps charger/ under /usr/lib/modules/extra; desktop "
            "entry, 80% default and restricted sudoers helper are installed")


def check_widevine_helper() -> str:
    """The helper must find real users and both native and Flatpak profiles."""
    paths = backend.repo_paths()
    script = paths["deploy"] / "scripts" / "pmos-widevine-install.sh"
    docs = paths["deploy"] / "docs" / "DRM_WIDEVINE.md"
    rootfs = paths["common"] / "install_pmos_rootfs.sh"
    if not script.is_file():
        return f"SKIP: helper not found (deploy={paths['deploy']})"

    proc = subprocess.run(["sh", "-n", str(script)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, f"{script}: {proc.stderr.strip()}"

    proc = subprocess.run(["sh", str(script), "--help"],
                          capture_output=True, text=True)
    assert proc.returncode == 2, (proc.returncode, proc.stderr)
    for flag in ("--browser", "--user", "--uninstall"):
        assert flag in proc.stdout, (flag, proc.stdout)
    proc = subprocess.run(["sh", str(script), "--bogus"],
                          capture_output=True, text=True)
    assert proc.returncode == 2, (proc.returncode, proc.stderr)

    text = script.read_text()
    # Account discovery: SUDO_USER, --user and a passwd fallback, never the
    # old hard-coded /home/user.
    assert "SUDO_USER" in text
    assert "--user NAME" in text
    assert "/etc/passwd" in text
    assert "USER_HOME=/home/user" not in text
    assert "USER_NAME=user" not in text
    # Native and Flatpak profile layouts.
    for needle in (
        ".mozilla/firefox/*/prefs.js",
        ".var/app/org.mozilla.firefox/.mozilla/firefox/*/prefs.js",
        ".var/app/org.mozilla.firefox/config/mozilla/firefox/*/prefs.js",
        ".var/app/org.chromium.Chromium/config/chromium",
        "gmp-widevinecdm",
        "latest-component-updated-widevine-cdm",
        "ld-linux-aarch64",
        "ld-musl-aarch64",
    ):
        assert needle in text, needle
    # Reseeding a browser from its own component tree must not cp onto itself.
    assert "copy_file()" in text and '"$_src" -ef "$_dst"' in text
    # Chromium's hint file is JSON; a bare version string breaks registration.
    assert 'json.dumps({"Path": sys.argv[1]})' in text
    assert "latest-component-updated-widevine-cdm" in text

    # The rootfs payload copies every deploy/scripts helper to /usr/local/bin.
    assert rootfs.is_file(), rootfs
    rootfs_text = rootfs.read_text()
    assert ('cp -a "$DEPLOY_DIR/scripts/"* "$PAYLOAD_DIR/usr/local/bin/"'
            in rootfs_text)
    assert 'chmod 755 "$PAYLOAD_DIR/usr/local/bin/"*' in rootfs_text

    if docs.is_file():
        doc_text = docs.read_text()
        assert "Flatpak Firefox" in doc_text
        assert "Flatpak Chromium" in doc_text
        assert "musl" in doc_text and "glibc" in doc_text
        assert "4.10.3112.0" in doc_text

    return ("user auto-detection, native/Flatpak Firefox and Chromium paths, "
            "AArch64/libc guards and rootfs payload wiring")


def check_audio_display_helpers() -> str:
    """Jack/backlight helpers must follow real users and suffixed sinks."""
    paths = backend.repo_paths()
    jack = paths["deploy"] / "scripts" / "pmos-jack-profile.sh"
    guard = paths["deploy"] / "scripts" / "pmos-display-guard.py"
    migrate = paths["deploy"] / "scripts" / "pmos-audio-backend-migrate.sh"
    if not (jack.is_file() and guard.is_file() and migrate.is_file()):
        return "SKIP: helper scripts not found"

    for script in (jack, migrate):
        proc = subprocess.run(["sh", "-n", str(script)],
                              capture_output=True, text=True)
        assert proc.returncode == 0, f"{script}: {proc.stderr.strip()}"
    compile(guard.read_text(), str(guard), "exec")

    jack_text = jack.read_text()
    # module-alsa-card appends .N after a profile switch; matching only the
    # bare name made the watcher reset the card in a loop.
    assert "suffix ~ /^[0-9]+$/" in jack_text
    assert "sink_name()" in jack_text and "sink_id()" in jack_text
    assert "ensure_default()" in jack_text
    assert "pactl set-default-sink" in jack_text

    guard_text = guard.read_text()
    # The installer account is not necessarily "user"/10000.
    assert 'SESSION_USER = "user"' not in guard_text
    assert "SESSION_UID = 10000" not in guard_text
    assert "loginctl" in guard_text and "show-session" in guard_text
    assert "def discover_session()" in guard_text
    assert "def read_dpms()" in guard_text
    assert 'dpms == "Off"' in guard_text
    assert "DPMS = \"/sys/class/drm/card0-DSI-1/dpms\"" in guard_text

    migrate_text = migrate.read_text()
    # Restarting only pipewire-pulse.service unlinks pulse/native.
    assert "restart pipewire-pulse.socket" in migrate_text
    assert ("restart pipewire-pulse.socket" in migrate_text
            and "start pipewire-pulse.service" in migrate_text)
    assert (migrate_text.index("restart pipewire-pulse.socket")
            < migrate_text.index("start pipewire-pulse.service"))

    return ("suffixed-sink matching, default-sink follow, logind session "
            "discovery, DPMS-off blanking, and PipeWire-Pulse socket order")


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
    chain = (f"{images.boot.parent.name}/{images.boot.name}"
             if images.boot else "none")
    return (f"common={paths['common']}, found: {', '.join(found) or 'none'}, "
            f"boot: {chain}, "
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
        flash_args_file = common / "flash_args.txt"
        rootfs_args_file = common / "rootfs_args.txt"
        calls_file = common / "calls.txt"
        hash_seen = common / "hash.txt"
        flash.write_text(
            "#!/usr/bin/env bash\n"
            f"printf '%s\\n' \"$@\" > {shlex.quote(str(flash_args_file))}\n"
            f"printf 'flash\\n' >> {shlex.quote(str(calls_file))}\n"
            "echo '%%PMOS_STEP:write:Writing fake boot'\n"
            "echo '%%PMOS_PROGRESS:50:Halfway'\n"
            "echo '%%PMOS_DONE'\n"
        )
        rootfs.write_text(
            "#!/usr/bin/env bash\n"
            f"printf '%s\\n' \"$@\" > {shlex.quote(str(rootfs_args_file))}\n"
            f"printf 'rootfs\\n' >> {shlex.quote(str(calls_file))}\n"
            "prev=''\n"
            "for arg in \"$@\"; do\n"
            "  if [ \"$prev\" = '--password-hash-file' ]; then\n"
            f"    cat \"$arg\" > {shlex.quote(str(hash_seen))}\n"
            "  fi\n"
            "  prev=\"$arg\"\n"
            "done\n"
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

        image_paths = {
            "boot": common / "boot.img",
            "init_boot": common / "init_boot.img",
            "vbmeta": common / "vbmeta.img",
            "vendor_boot": common / "vendor_boot.img",
            "dtbo": common / "dtbo.img",
            "rootfs": common / "rootfs.img",
            "modules": common / "modules.tar.xz",
            "firmware": common / "firmware.tar.xz",
        }

        def run(options: backend.InstallOptions
                ) -> tuple[str, str, list[str], str, list[str]]:
            for path in (flash_args_file, rootfs_args_file, calls_file,
                         hash_seen):
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass
            lines: list[str] = []
            backend.install(options, lines.append)
            flash_args = (flash_args_file.read_text()
                          if flash_args_file.exists() else "")
            rootfs_args = (rootfs_args_file.read_text()
                           if rootfs_args_file.exists() else "")
            calls = (calls_file.read_text().split() if calls_file.exists() else [])
            seen = hash_seen.read_text() if hash_seen.exists() else ""
            return flash_args, rootfs_args, calls, seen, lines

        old = os.environ.get("TB351FU_COMMON_DIR")
        os.environ["TB351FU_COMMON_DIR"] = str(common)
        try:
            password_hash = "$6$rounds=120000$abcdefghijklmnop$" + "a" * 86
            manual = backend.InstallOptions(
                device=backend.Device(serial="FAKE", mode="adb", root=True),
                images=backend.ImageSet(
                    **image_paths, slot_b_firmware=firmware_images),
                size_bytes=16 * GIB,
                switch_slot=False,
                source="manual",
                mode="dual",
                account_enabled=True,
                username="alice",
                password_hash=password_hash,
                existing_action="replace",
            )
            manual_flash, manual_args, manual_calls, seen, manual_lines = run(manual)

            fw_options = backend.InstallOptions(
                device=backend.Device(serial="FAKE", mode="fastboot"),
                images=backend.ImageSet(
                    boot=image_paths["boot"],
                    init_boot=image_paths["init_boot"],
                    vbmeta=image_paths["vbmeta"],
                    vendor_boot=image_paths["vendor_boot"],
                    dtbo=image_paths["dtbo"],
                    slot_b_firmware=firmware_images,
                ),
                size_bytes=16 * GIB,
                flash_firmware=True,
                install_rootfs=False,
                switch_slot=False,
            )
            fw_flash_args, _, fw_calls, _, fw_lines = run(fw_options)

            whole_dir = common / "whole_out"
            whole = backend.InstallOptions(
                device=backend.Device(serial="FAKE", mode="adb", root=True),
                images=backend.ImageSet(
                    boot=image_paths["boot"],
                    init_boot=image_paths["init_boot"],
                    vbmeta=image_paths["vbmeta"],
                    modules=image_paths["modules"],
                    firmware=image_paths["firmware"]),
                size_bytes=0,
                source="auto",
                image_url="https://example.invalid/rootfs.img.zst",
                image_sha256="b" * 64,
                mode="whole",
                whole_output=whole_dir,
                account_enabled=False,
            )
            _, whole_args, whole_calls, _, whole_lines = run(whole)

            remove = backend.InstallOptions(
                device=backend.Device(serial="FAKE", mode="adb", root=True),
                images=backend.ImageSet(),
                size_bytes=0,
                source="manual",
                existing_action="remove",
                remove_legacy=True,
            )
            _, remove_args, remove_calls, _, remove_lines = run(remove)
        finally:
            if old is None:
                os.environ.pop("TB351FU_COMMON_DIR", None)
            else:
                os.environ["TB351FU_COMMON_DIR"] = old

    manual_tokens = manual_args.splitlines()
    manual_flash_tokens = manual_flash.splitlines()
    assert manual_calls == ["flash", "rootfs"], manual_calls
    for flag in ("--source", "manual", "--image", "--size", "16G",
                 "--mode", "dual", "--replace-existing", "--username",
                 "alice", "--password-hash-file", "--yes"):
        assert flag in manual_tokens, (flag, manual_tokens)
    assert "--no-account" not in manual_tokens, manual_tokens
    assert seen.strip() == password_hash, seen
    for flag in ("--vendor-boot", "--dtbo"):
        assert flag in manual_flash_tokens, (flag, manual_flash_tokens)

    done = [line for line in manual_lines if line == "%%PMOS_DONE"]
    assert len(done) == 1, done
    assert any(line.startswith("%%PMOS_STEP:boot:") for line in manual_lines), manual_lines
    assert any(line.startswith("%%PMOS_STEP:rootfs:") for line in manual_lines), manual_lines
    assert any(line.startswith("%%PMOS_STEP:unpack:") for line in manual_lines), manual_lines
    assert "%%PMOS_PROGRESS:100:Installation complete" in manual_lines, manual_lines
    assert done == ["%%PMOS_DONE"], manual_lines
    assert "--firmware-dir" not in manual_flash_tokens, manual_flash_tokens
    assert "--firmware-dir" in fw_flash_args.splitlines(), fw_flash_args
    assert str(firmware_dir.resolve()) in fw_flash_args.splitlines(), fw_flash_args
    assert fw_calls == ["flash"], fw_calls
    assert fw_lines.count("%%PMOS_DONE") == 1, fw_lines

    whole_tokens = whole_args.splitlines()
    assert whole_calls == ["rootfs"], whole_calls
    for flag in ("--source", "auto", "--url", "--sha256", "--mode", "whole",
                 "--whole-output", "--no-account"):
        assert flag in whole_tokens, (flag, whole_tokens)
    assert str(whole_dir) in whole_tokens, whole_tokens
    assert "--size" not in whole_tokens, whole_tokens
    assert "--image" not in whole_tokens, whole_tokens
    assert "--replace-existing" not in whole_tokens, whole_tokens
    assert "%%PMOS_PROGRESS:100:Whole-device package ready" in whole_lines, whole_lines

    remove_tokens = remove_args.splitlines()
    assert remove_calls == ["rootfs"], remove_calls
    for flag in ("--mode", "dual", "--remove-existing", "--remove-legacy", "--yes"):
        assert flag in remove_tokens, (flag, remove_tokens)
    assert "--size" not in remove_tokens, remove_tokens
    assert "--username" not in remove_tokens, remove_tokens
    assert "--source" not in remove_tokens, remove_tokens
    assert "%%PMOS_PROGRESS:100:Removal complete" in remove_lines, remove_lines
    return ("dual account/replace, auto whole-device, removal, firmware "
            "opt-in and one final marker each")


def check_existing_detection() -> str:
    device = backend.Device(serial="FAKE", mode="adb", root=True)
    calls: list[tuple[str, ...]] = []

    def fake_adb(serial, *args, **kwargs):
        calls.append(args)
        command = args[-1] if args else ""
        if "cat " in command:
            out = "PMOS_INSTALL=1\nPMOS_ACCOUNT=tester\n"
        else:
            out = "dir\nmarker\nlegacy\n"
        return subprocess.CompletedProcess(["adb"], 0, stdout=out, stderr="")

    original = backend._adb
    backend._adb = fake_adb
    try:
        state = backend.detect_existing_install(device)
        fastboot = backend.detect_existing_install(
            backend.Device(serial="FAKE", mode="fastboot"))
    finally:
        backend._adb = original

    assert state.checked and state.directory and state.marker and state.legacy
    assert state.present and "PMOS_INSTALL=1" in state.marker_text
    assert not fastboot.checked and not fastboot.present
    assert len(calls) == 2, calls
    assert all("su -c '" in call[-1] for call in calls), calls
    return "/data state parsed; su command is quoted; fastboot is read-free"


def check_password_hash() -> str:
    digest = backend.hash_password("correct horse battery staple")
    assert digest.startswith("$6$"), digest
    assert len(digest) <= 256, len(digest)
    assert digest.count("$") >= 4, digest
    assert "correct" not in digest, digest
    return f"SHA-512 crypt hash ({len(digest)} characters)"


def check_gui_import() -> str:
    try:
        import gi
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gtk  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        raise AssertionError(f"GTK4/PyGObject unavailable: {exc}") from exc
    from . import app  # noqa: F401
    source = Path(app.__file__).read_text()
    assert "Create a pmOS login account" not in source
    assert "self.username_entry" in source and "self.password_entry" in source
    assert "the pmOS username to create" in source
    return "GTK4 application imports with mandatory account fields"


CHECKS = [
    ("size parsing", check_sizes),
    ("GPT parsing", check_gpt),
    ("image magic", check_image_kinds),
    ("image validation", check_validate_images),
    ("bundle discovery", check_bundle_scan),
    ("boot chain selection", check_boot_chain_selection),
    ("partition roles", check_partition_roles),
    ("helper scripts", check_scripts),
    ("GNSS payload", check_gnss_payload),
    ("charger payload", check_charger_payload),
    ("Widevine helper", check_widevine_helper),
    ("audio/display helpers", check_audio_display_helpers),
    ("repository discovery", check_repo_paths),
    ("existing-install detection", check_existing_detection),
    ("install orchestration", check_orchestration),
    ("password hashing", check_password_hash),
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
