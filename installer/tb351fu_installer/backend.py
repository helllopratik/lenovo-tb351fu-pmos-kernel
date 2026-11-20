# SPDX-License-Identifier: GPL-2.0
"""Backend helpers for the TB351FU postmarketOS installer.

Everything here is deliberately plain Python + subprocess.  The GUI is a
thin layer on top, and the same functions are usable from the command line
for debugging (`python3 -m tb351fu_installer --detect`).
"""

from __future__ import annotations

import dataclasses
import json
import os
import re
import secrets
import shlex
import shutil
import string
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Callable, Iterable, Optional

GIB = 1024 ** 3
MIB = 1024 ** 2
KIB = 1024

BOOT_PART_SIZE = 64 * MIB
INIT_BOOT_PART_SIZE = 8 * MIB
VENDOR_BOOT_PART_SIZE = 64 * MIB
DTBO_PART_SIZE = 8 * MIB
# Leave Android enough room to keep running; the rootfs is a file inside
# /data, so this reserve is what keeps the system usable after installation.
DEFAULT_RESERVE = 8 * GIB
DEFAULT_CAP = 64 * GIB
DEFAULT_MINIMUM = 8 * GIB

# Firmware that belongs to slot B's boot chain.  The stock images come from
# the Lenovo TB351FU bundle and are written only to the *_b partitions.
SLOT_B_FIRMWARE = {
    "lk": "lk.img",
    "tee": "tee.img",
    "gz": "gz.img",
    "scp": "scp.img",
    "sspm": "sspm.img",
    "mcupm": "mcupm.img",
    "dpm": "dpm.img",
    "spmfw": "spmfw.img",
    "pi_img": "pi_img.img",
}


class InstallerError(RuntimeError):
    pass


@dataclasses.dataclass
class Device:
    serial: str
    mode: str  # "adb" or "fastboot"
    model: str = ""
    codename: str = ""
    slot: str = ""
    root: bool = False
    android: str = ""
    sdk: str = ""
    battery: Optional[int] = None

    @property
    def is_target(self) -> bool:
        return "tb351fu" in (self.codename + self.model).lower()


@dataclasses.dataclass
class Partition:
    name: str
    size: int
    node: str = ""
    role: str = ""
    slot: str = ""
    fstype: str = ""

    @property
    def size_text(self) -> str:
        return human_size(self.size)


@dataclasses.dataclass
class DataSpace:
    total: int
    used: int
    free: int

    @property
    def free_text(self) -> str:
        return human_size(self.free)


@dataclasses.dataclass
class ExistingInstall:
    """State of an existing pmOS installation inside Android /data."""
    checked: bool = False
    directory: bool = False
    marker: bool = False
    legacy: bool = False
    marker_text: str = ""
    error: str = ""

    @property
    def present(self) -> bool:
        return self.directory or self.marker or self.legacy


@dataclasses.dataclass
class ImageSet:
    boot: Optional[Path] = None
    init_boot: Optional[Path] = None
    vbmeta: Optional[Path] = None
    vendor_boot: Optional[Path] = None
    dtbo: Optional[Path] = None
    rootfs: Optional[Path] = None
    modules: Optional[Path] = None
    firmware: Optional[Path] = None
    bundle_dir: Optional[Path] = None
    slot_b_firmware: dict[str, Path] = dataclasses.field(default_factory=dict)


@dataclasses.dataclass
class InstallOptions:
    device: Device
    images: ImageSet
    size_bytes: int
    flash_boot: bool = True
    flash_firmware: bool = False
    install_rootfs: bool = True
    switch_slot: bool = True
    reboot_after_switch: bool = True
    mode: str = "dual"              # "dual" or "whole"
    whole_output: Optional[Path] = None
    source: str = "manual"          # "manual" or "auto"
    image_url: str = ""
    image_sha256: str = ""
    account_enabled: bool = False
    username: str = ""
    password_hash: str = ""
    existing_action: str = "none"   # "none", "replace" or "remove"
    remove_legacy: bool = False

    @property
    def size_gib(self) -> int:
        return self.size_bytes // GIB


# ---------------------------------------------------------------------------
# formatting / parsing helpers
# ---------------------------------------------------------------------------

def human_size(value: int) -> str:
    if value <= 0:
        return "0 B"
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    idx = 0
    val = float(value)
    while val >= 1024 and idx < len(units) - 1:
        val /= 1024
        idx += 1
    if idx == 0:
        return f"{int(val)} {units[idx]}"
    return f"{val:.1f} {units[idx]}"


def parse_size(text: str) -> int:
    text = text.strip().upper()
    m = re.fullmatch(r"(\d+)\s*([KMGT]?)", text)
    if not m:
        raise ValueError(f"invalid size: {text!r}")
    value = int(m.group(1))
    suffix = m.group(2)
    mult = {"": 1, "K": KIB, "M": MIB, "G": GIB, "T": 1024 ** 4}[suffix]
    return value * mult


def _clean(text: str) -> str:
    return text.replace("\r", "")


def _run(argv: list[str], timeout: float = 30, check: bool = True,
         env: Optional[dict] = None, cwd: Optional[Path] = None
         ) -> subprocess.CompletedProcess:
    try:
        proc = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
            env=env,
            cwd=str(cwd) if cwd else None,
        )
    except FileNotFoundError as exc:
        raise InstallerError(f"missing tool: {argv[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        raise InstallerError(f"timed out: {' '.join(argv)}") from exc
    if check and proc.returncode != 0:
        detail = _clean(proc.stderr or proc.stdout).strip()
        raise InstallerError(f"{' '.join(argv)} failed: {detail}")
    return proc


def _stream(argv: list[str], on_line: Callable[[str], None],
            env: Optional[dict] = None, cwd: Optional[Path] = None) -> int:
    try:
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env,
            cwd=str(cwd) if cwd else None,
        )
    except FileNotFoundError as exc:
        raise InstallerError(f"missing tool: {argv[0]}") from exc
    assert proc.stdout is not None
    try:
        for raw in proc.stdout:
            # adb shell can emit CRLF, and adb push rewrites its progress
            # line with bare carriage returns.  Normalise both.
            for line in raw.replace("\r", "\n").split("\n"):
                if line:
                    on_line(line)
    finally:
        proc.stdout.close()
    return proc.wait()


# ---------------------------------------------------------------------------
# repository / image discovery
# ---------------------------------------------------------------------------

def repo_paths() -> dict[str, Path]:
    """Locate the pmOS checkout.

    In a source tree the package lives at common/installer/tb351fu_installer,
    so parents[2] is `common`.  An installed copy cannot rely on that, so
    TB351FU_COMMON_DIR wins, then the source layout, then the usual checkout
    locations used during development.
    """
    candidates: list[Path] = []
    env_common = os.environ.get("TB351FU_COMMON_DIR")
    if env_common:
        candidates.append(Path(env_common).expanduser())
    candidates.append(Path(__file__).resolve().parents[2])
    candidates.append(Path.home() / "Desktop/pmos/common")
    candidates.append(Path("/mnt/Nvme/pmos/common"))
    common = next(
        (c.resolve() for c in candidates
         if (c / "install_pmos_rootfs.sh").is_file()),
        candidates[0].resolve(),
    )
    pmos = common.parent
    return {
        "common": common,
        "pmos": pmos,
        "artifacts": pmos / "artifacts",
        "deploy": common / "deploy",
        "installer": common / "installer",
    }


def default_image_url() -> str:
    """Return the configured server rootfs URL, or an empty string."""
    env_url = os.environ.get("PMOS_IMAGE_URL", "").strip()
    if env_url:
        return env_url
    path = repo_paths()["deploy"] / "configs" / "pmos_image_url"
    try:
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                return line
    except OSError:
        pass
    return ""


def hash_password(password: str) -> str:
    """Hash a password with SHA-512 crypt, rounds=120000 when possible.

    The caller must keep the plaintext in memory only.  The returned value is
    what the shell installer accepts through --password-hash-file.
    """
    if not password:
        raise InstallerError("password must not be empty")
    alphabet = string.ascii_letters + string.digits + "./"
    salt = "".join(secrets.choice(alphabet) for _ in range(16))
    setting = f"$6$rounds=120000${salt}$"
    result = ""
    perl = shutil.which("perl")
    if perl:
        try:
            proc = subprocess.run(
                [perl, "-e",
                 "my $p=<STDIN>; chomp $p; print crypt($p, $ARGV[0]);",
                 setting],
                input=password + "\n", text=True, capture_output=True,
                timeout=60,
            )
            if proc.returncode == 0:
                result = proc.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            result = ""
    if not result.startswith("$6$"):
        openssl = shutil.which("openssl")
        if openssl:
            try:
                proc = subprocess.run(
                    [openssl, "passwd", "-6", "-salt", salt, "-stdin"],
                    input=password + "\n", text=True, capture_output=True,
                    timeout=60,
                )
                if proc.returncode == 0:
                    result = proc.stdout.strip()
            except (OSError, subprocess.SubprocessError):
                result = ""
    if not result.startswith("$6$"):
        raise InstallerError("perl or openssl is required to hash the password")
    if len(result) > 256 or not re.fullmatch(r"\$6\$[A-Za-z0-9./=$]+", result):
        raise InstallerError("the password hash has an unexpected format")
    return result


def _first_existing(paths: Iterable[Path]) -> Optional[Path]:
    for path in paths:
        if path and path.is_file():
            return path
    return None


# Names used by the Lenovo slot-B bundle and by the repository's images/ dir.
# Stock boot.img/init_boot.img are deliberately excluded: they are Android
# images, not the postmarketOS boot chain.
BUNDLE_IMAGES = {
    "boot": ("boot_pmos.img", "boot_b.img"),
    "init_boot": ("init_boot_pmos.img", "init_boot_b.img"),
    "vbmeta": ("vbmeta_disabled.img", "vbmeta_b.img", "vbmeta.img"),
    "vendor_boot": ("vendor_boot_b.img", "vendor_boot.img"),
    "dtbo": ("dtbo_b.img", "dtbo.img"),
}


def _first_in(directory: Path, names: Iterable[str]) -> Optional[Path]:
    for name in names:
        path = directory / name
        if path.is_file():
            return path
    return None


def bundle_directories() -> list[Path]:
    """Directories that may hold the stock slot-B bundle."""
    dirs: list[Path] = []
    env_bundle = os.environ.get("TB351FU_BUNDLE_DIR")
    if env_bundle:
        dirs.append(Path(env_bundle).expanduser())
    dirs.append(Path("/mnt/Nvme/slot_b_bundle"))
    paths = repo_paths()
    dirs.append(paths["pmos"] / "slot_b_bundle")
    dirs.append(paths["common"] / "images")
    dirs.append(paths["artifacts"])

    result: list[Path] = []
    seen: set[Path] = set()
    for directory in dirs:
        try:
            resolved = directory.resolve()
        except OSError:
            continue
        if resolved in seen or not resolved.is_dir():
            continue
        seen.add(resolved)
        result.append(resolved)
    return result


def scan_bundle(directory: Path) -> ImageSet:
    """Discover the boot-chain and firmware images inside one bundle."""
    directory = directory.expanduser()
    images = ImageSet(
        boot=_first_in(directory, BUNDLE_IMAGES["boot"]),
        init_boot=_first_in(directory, BUNDLE_IMAGES["init_boot"]),
        vbmeta=_first_in(directory, BUNDLE_IMAGES["vbmeta"]),
        vendor_boot=_first_in(directory, BUNDLE_IMAGES["vendor_boot"]),
        dtbo=_first_in(directory, BUNDLE_IMAGES["dtbo"]),
        bundle_dir=directory if directory.is_dir() else None,
    )
    images.slot_b_firmware = {
        part: directory / name
        for part, name in SLOT_B_FIRMWARE.items()
        if (directory / name).is_file()
    }
    return images


def slot_b_firmware_dir(images: ImageSet) -> Optional[Path]:
    """Return a single directory containing every selected firmware image."""
    if not images.slot_b_firmware:
        return None
    parents = {path.parent.resolve() for path in images.slot_b_firmware.values()}
    if len(parents) == 1:
        directory = next(iter(parents))
        if all((directory / name).is_file()
               for name in SLOT_B_FIRMWARE.values()):
            return directory
    return None


def find_images() -> ImageSet:
    paths = repo_paths()
    artifacts = paths["artifacts"]
    images_dir = paths["common"] / "images"
    pmos = paths["pmos"]
    deploy = paths["deploy"]
    bundles = bundle_directories()

    def bundle_image(names: Iterable[str]) -> Optional[Path]:
        for directory in bundles:
            path = _first_in(directory, names)
            if path:
                return path
        return None

    # Prefer the directory that carries the complete slot-B firmware set, then
    # one with vendor_boot/dtbo, so the GUI's bundle field points somewhere
    # useful.
    bundle_dir = next(
        (d for d in bundles
         if all((d / name).is_file() for name in SLOT_B_FIRMWARE.values())),
        None,
    )
    if bundle_dir is None:
        bundle_dir = next(
            (d for d in bundles
             if (d / "vendor_boot.img").is_file()
             or (d / "vendor_boot_b.img").is_file()),
            None,
        )
    if bundle_dir is None and bundles:
        scored = [(sum((d / name).is_file()
                       for name in SLOT_B_FIRMWARE.values()), d)
                  for d in bundles]
        best_score, best_dir = max(scored, key=lambda item: item[0])
        if best_score:
            bundle_dir = best_dir
    if bundle_dir is None and bundles:
        bundle_dir = bundles[0]

    firmware: dict[str, Path] = {}
    for part, name in SLOT_B_FIRMWARE.items():
        path = (bundle_dir / name) if bundle_dir else None
        if not path or not path.is_file():
            path = bundle_image((name,))
        if path:
            firmware[part] = path

    rootfs_candidates = [
        Path(os.environ["TB351FU_ROOTFS_IMAGE"]) if os.environ.get("TB351FU_ROOTFS_IMAGE") else None,
        images_dir / "rootfs_base.img.zst",
        images_dir / "rootfs_base.img",
        pmos / "pmos-phosh-aarch64.img",
        Path("/mnt/Nvme/pmos_rootfs_base.img"),
        Path("/mnt/Nvme/pmos_rootfs_base.img.zst"),
        pmos / "pmos_rootfs_base.img",
        pmos / "pmos_rootfs_base.img.zst",
    ]

    return ImageSet(
        boot=_first_existing([
            images_dir / "boot_b.img",
            images_dir / "boot_latest.img",
            artifacts / "boot_latest.img",
            artifacts / "boot_pmos.img",
            bundle_image(BUNDLE_IMAGES["boot"]),
        ]),
        init_boot=_first_existing([
            images_dir / "init_boot_b.img",
            images_dir / "init_boot_latest.img",
            artifacts / "init_boot_latest.img",
            artifacts / "init_boot_pmos.img",
            bundle_image(BUNDLE_IMAGES["init_boot"]),
        ]),
        vbmeta=_first_existing([
            images_dir / "vbmeta_b.img",
            images_dir / "vbmeta_latest.img",
            artifacts / "vbmeta_latest.img",
            bundle_image(BUNDLE_IMAGES["vbmeta"]),
        ]),
        vendor_boot=_first_existing([
            images_dir / "vendor_boot_b.img",
            images_dir / "vendor_boot.img",
            artifacts / "vendor_boot_latest.img",
            bundle_image(BUNDLE_IMAGES["vendor_boot"]),
        ]),
        dtbo=_first_existing([
            images_dir / "dtbo_b.img",
            images_dir / "dtbo.img",
            artifacts / "dtbo_latest.img",
            bundle_image(BUNDLE_IMAGES["dtbo"]),
        ]),
        rootfs=_first_existing([p for p in rootfs_candidates if p]),
        modules=_first_existing([deploy / "modules.tar.xz"]),
        firmware=_first_existing([deploy / "firmware.tar.xz"]),
        bundle_dir=bundle_dir,
        slot_b_firmware=firmware,
    )


def image_kind(path: Path) -> str:
    try:
        with open(path, "rb") as fh:
            magic = fh.read(4)
            if magic == b"\x28\xb5\x2f\xfd":
                return "zstd"
            fh.seek(512)
            if fh.read(8) == b"EFI PART":
                return "gpt"
            fh.seek(1024 + 56)
            if fh.read(2) == b"\x53\xef":
                return "ext4"
    except OSError:
        return "error"
    return "unknown"


def gpt_partitions(path: Path) -> list[tuple[int, int, int]]:
    """Return [(index, first_lba, last_lba), ...] from a GPT image."""
    result: list[tuple[int, int, int]] = []
    try:
        with open(path, "rb") as fh:
            fh.seek(512)
            header = fh.read(92)
            if len(header) < 92 or header[:8] != b"EFI PART":
                return result
            entries_lba = struct.unpack_from("<Q", header, 72)[0]
            count = struct.unpack_from("<I", header, 80)[0]
            entry_size = struct.unpack_from("<I", header, 84)[0]
            if entry_size < 128 or count > 256:
                return result
            fh.seek(entries_lba * 512)
            for i in range(count):
                entry = fh.read(entry_size)
                if len(entry) < 128:
                    break
                if entry[:16] == b"\x00" * 16:
                    continue
                first = struct.unpack_from("<Q", entry, 32)[0]
                last = struct.unpack_from("<Q", entry, 40)[0]
                result.append((i + 1, first, last))
    except OSError:
        return result
    return result


def rootfs_content_size(path: Path) -> Optional[int]:
    """Best-effort size of the ext4 filesystem inside a rootfs image."""
    kind = image_kind(path)
    if kind == "ext4":
        return path.stat().st_size
    if kind == "gpt":
        parts = gpt_partitions(path)
        if len(parts) >= 2:
            _, first, last = parts[1]
            return (last - first + 1) * 512
        if parts:
            _, first, last = parts[0]
            return (last - first + 1) * 512
    return None


def validate_images(images: ImageSet, need_rootfs: bool = True,
                    need_boot: bool = True,
                    need_firmware: bool = False,
                    need_local_rootfs: bool = True) -> list[str]:
    problems: list[str] = []
    if need_boot:
        if not images.boot:
            problems.append("boot image not found (artifacts/boot_latest.img)")
        elif not images.boot.is_file():
            problems.append(f"boot image not found: {images.boot}")
        elif images.boot.stat().st_size > BOOT_PART_SIZE:
            problems.append("boot image is larger than the 64 MiB boot partition")
        if not images.init_boot:
            problems.append("init_boot image not found (artifacts/init_boot_latest.img)")
        elif not images.init_boot.is_file():
            problems.append(f"init_boot image not found: {images.init_boot}")
        elif images.init_boot.stat().st_size > INIT_BOOT_PART_SIZE:
            problems.append("init_boot image is larger than the 8 MiB partition")
        if not images.vbmeta:
            problems.append("vbmeta image not found (artifacts/vbmeta_latest.img)")
        elif not images.vbmeta.is_file():
            problems.append(f"vbmeta image not found: {images.vbmeta}")
        if not images.vendor_boot:
            problems.append(
                "vendor_boot image not found (slot_b_bundle/vendor_boot.img)")
        elif not images.vendor_boot.is_file():
            problems.append(f"vendor_boot image not found: {images.vendor_boot}")
        elif images.vendor_boot.stat().st_size > VENDOR_BOOT_PART_SIZE:
            problems.append(
                "vendor_boot image is larger than the 64 MiB partition")
        if not images.dtbo:
            problems.append("dtbo image not found (slot_b_bundle/dtbo.img)")
        elif not images.dtbo.is_file():
            problems.append(f"dtbo image not found: {images.dtbo}")
        elif images.dtbo.stat().st_size > DTBO_PART_SIZE:
            problems.append("dtbo image is larger than the 8 MiB partition")
    if need_firmware:
        missing = [part for part in SLOT_B_FIRMWARE
                   if part not in images.slot_b_firmware]
        if missing:
            problems.append(
                "slot-B firmware image(s) missing: "
                + ", ".join(f"{part}.img" for part in missing))
    if need_rootfs:
        if need_local_rootfs and not images.rootfs:
            problems.append("postmarketOS rootfs image not found")
        if not images.modules:
            problems.append("deploy/modules.tar.xz is missing; run the deploy build first")
        if not images.firmware:
            problems.append("deploy/firmware.tar.xz is missing; run the deploy build first")
    return problems


# ---------------------------------------------------------------------------
# device detection
# ---------------------------------------------------------------------------

def list_adb_serials() -> list[str]:
    try:
        out = _run(["adb", "devices"], timeout=10, check=False).stdout
    except InstallerError:
        return []
    serials = []
    for line in _clean(out).splitlines()[1:]:
        if not line.strip() or line.startswith("*"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            serials.append(parts[0])
    return serials


def list_fastboot_serials() -> list[str]:
    try:
        out = _run(["fastboot", "devices"], timeout=10, check=False).stdout
    except InstallerError:
        return []
    serials = []
    for line in _clean(out).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] in ("fastboot", "fastbootd"):
            serials.append(parts[0])
    return serials


def _adb(serial: str, *args: str, timeout: float = 20,
         check: bool = False) -> subprocess.CompletedProcess:
    return _run(["adb", "-s", serial, *args], timeout=timeout, check=check)


def _fastboot(serial: str, *args: str, timeout: float = 60,
              check: bool = False) -> subprocess.CompletedProcess:
    return _run(["fastboot", "-s", serial, *args], timeout=timeout, check=check)


def _getprops(serial: str) -> dict[str, str]:
    props: dict[str, str] = {}
    try:
        out = _clean(_adb(serial, "shell", "getprop", timeout=12).stdout)
    except InstallerError:
        return props
    for line in out.splitlines():
        m = re.match(r"\[([^\]]+)\]:\s*\[(.*)\]", line)
        if m:
            props[m.group(1)] = m.group(2)
    return props


def _root_ok(serial: str) -> bool:
    try:
        out = _clean(_adb(serial, "shell", "su", "-c", "id", timeout=12).stdout)
    except InstallerError:
        return False
    return "uid=0" in out


def _battery(serial: str) -> Optional[int]:
    try:
        out = _clean(_adb(serial, "shell", "dumpsys", "battery", timeout=10).stdout)
    except InstallerError:
        return None
    m = re.search(r"level:\s*(\d+)", out)
    return int(m.group(1)) if m else None


def detect_devices() -> list[Device]:
    devices: list[Device] = []
    for serial in list_adb_serials():
        props = _getprops(serial)
        devices.append(Device(
            serial=serial,
            mode="adb",
            model=props.get("ro.product.model", ""),
            codename=props.get("ro.product.device", ""),
            slot=props.get("ro.boot.slot_suffix", "").lstrip("_"),
            android=props.get("ro.build.version.release", ""),
            sdk=props.get("ro.build.version.sdk", ""),
            root=_root_ok(serial),
            battery=_battery(serial),
        ))
    for serial in list_fastboot_serials():
        slot = ""
        product = ""
        for key in ("current-slot", "product"):
            try:
                proc = _fastboot(serial, "getvar", key, timeout=10)
                out = _clean(proc.stdout + "\n" + proc.stderr)
            except InstallerError:
                continue
            m = re.search(rf"{re.escape(key)}:\s*(\S+)", out)
            if m:
                if key == "current-slot":
                    slot = m.group(1).lstrip("_")
                else:
                    product = m.group(1)
        devices.append(Device(
            serial=serial,
            mode="fastboot",
            model=product,
            codename=product,
            slot=slot,
        ))
    return devices


def detect_existing_install(device: Device) -> ExistingInstall:
    """Read the existing pmOS state from /data without modifying anything."""
    state = ExistingInstall()
    if device.mode != "adb" or not device.root:
        return state
    command = (
        "[ -d /data/pmos ] && echo dir; "
        "[ -f /data/pmos_install.env ] && echo marker; "
        "[ -f /data/pmos_legacy/rootfs.img ] && echo legacy"
    )
    try:
        out = _clean(_adb(device.serial, "shell",
                          f"su -c {shlex.quote(command)}",
                          timeout=20).stdout)
    except InstallerError as exc:
        state.error = str(exc)
        return state
    state.checked = True
    for token in out.split():
        if token == "dir":
            state.directory = True
        elif token == "marker":
            state.marker = True
        elif token == "legacy":
            state.legacy = True
    if state.marker:
        try:
            state.marker_text = _clean(
                _adb(device.serial, "shell",
                     "su -c " + shlex.quote(
                         "cat /data/pmos_install.env 2>/dev/null"),
                     timeout=20).stdout
            )[:4000]
        except InstallerError:
            pass
    return state


# ---------------------------------------------------------------------------
# partition / free-space detection
# ---------------------------------------------------------------------------

def _role_for(name: str) -> tuple[str, str]:
    slot = ""
    base = name
    if name.endswith("_a") or name.endswith("_b"):
        base = name[:-2]
        slot = name[-1]
    roles = {
        "boot": "Kernel + AVB boot image",
        "init_boot": "pmOS initramfs",
        "vbmeta": "AVB metadata",
        "vbmeta_system": "AVB system metadata",
        "vbmeta_vendor": "AVB vendor metadata",
        "vendor_boot": "Vendor boot / DTB",
        "dtbo": "Device tree overlays",
        "userdata": "Android /data (rootfs loop image)",
        "super": "Android dynamic partitions",
        "metadata": "Android metadata",
        "persist": "Modem / sensor calibration",
        "nvdata": "NV data",
        "nvcfg": "NV config",
    }
    return roles.get(base, ""), slot


def list_partitions(device: Device) -> list[Partition]:
    if device.mode == "fastboot":
        return _list_partitions_fastboot(device)
    return _list_partitions_adb(device)


def _list_partitions_adb(device: Device) -> list[Partition]:
    sizes: dict[str, int] = {}
    try:
        out = _clean(_adb(device.serial, "shell", "cat", "/proc/partitions").stdout)
    except InstallerError:
        out = ""
    for line in out.splitlines():
        m = re.match(r"\s*\d+\s+\d+\s+(\d+)\s+(\S+)\s*$", line)
        if m:
            sizes[m.group(2)] = int(m.group(1)) * 1024

    def ls_dir(path: str) -> str:
        text = ""
        try:
            text = _clean(_adb(device.serial, "shell", "ls", "-l", path).stdout)
        except InstallerError:
            text = ""
        if "Permission denied" in text or "No such file" in text or not text.strip():
            try:
                text = _clean(_adb(device.serial, "shell", "su", "-c",
                                   f"ls -l {path}", timeout=20).stdout)
            except InstallerError:
                text = ""
        return text

    parts: list[Partition] = []
    seen: set[str] = set()

    def add_listing(text: str, logical: bool) -> None:
        for line in text.splitlines():
            if "->" not in line:
                continue
            left, target = line.split("->", 1)
            name = left.split()[-1]
            node = target.strip().split("/")[-1]
            if not name or name in seen:
                continue
            seen.add(name)
            role, slot = _role_for(name)
            if logical and not role:
                role = "Android logical partition (read-only)"
            parts.append(Partition(
                name=name,
                size=sizes.get(node, 0),
                node=node,
                role=role,
                slot=slot,
            ))

    add_listing(ls_dir("/dev/block/by-name"), logical=False)
    # Dynamic partitions are informational only; the installer never writes
    # them, but showing them makes the list complete.
    add_listing(ls_dir("/dev/block/mapper"), logical=True)

    # Fallback: expose raw partitions when by-name is unavailable.
    if not parts:
        for node, size in sizes.items():
            if node.startswith(("sd", "mmcblk", "nvme")):
                parts.append(Partition(name=node, size=size, node=node))
    parts.sort(key=lambda p: (p.name.endswith(("_b",)), p.name))
    return parts


def _list_partitions_fastboot(device: Device) -> list[Partition]:
    try:
        proc = _fastboot(device.serial, "getvar", "all", timeout=30)
    except InstallerError:
        return []
    text = _clean(proc.stdout + "\n" + proc.stderr)
    sizes: dict[str, int] = {}
    types: dict[str, str] = {}
    for line in text.splitlines():
        line = re.sub(r"^\(bootloader\)\s*", "", line).strip()
        m = re.match(r"partition-size:([^:]+):\s*(0x[0-9a-fA-F]+|\d+)", line)
        if m:
            sizes[m.group(1)] = int(m.group(2), 0)
            continue
        m = re.match(r"partition-type:([^:]+):\s*(\S+)", line)
        if m:
            types[m.group(1)] = m.group(2)
    parts = []
    for name, size in sizes.items():
        role, slot = _role_for(name)
        parts.append(Partition(name=name, size=size, role=role, slot=slot,
                               fstype=types.get(name, "")))
    parts.sort(key=lambda p: (p.name.endswith("_b"), p.name))
    return parts


def get_data_space(device: Device) -> Optional[DataSpace]:
    if device.mode != "adb":
        return None
    # Android toybox does not support `df -B1`; -k is available on both
    # toybox and GNU coreutils.
    try:
        out = _clean(_adb(device.serial, "shell", "df", "-k", "/data").stdout)
    except InstallerError:
        out = ""
    if not out.strip():
        try:
            out = _clean(_adb(device.serial, "shell", "df", "/data").stdout)
        except InstallerError:
            return None
    lines = [ln for ln in out.splitlines() if ln.strip()]
    if len(lines) < 2:
        return None
    header, row = lines[0], lines[-1]
    fields = row.split()
    if len(fields) < 4:
        return None
    try:
        total, used, free = int(fields[1]), int(fields[2]), int(fields[3])
    except ValueError:
        return None
    if "1K-blocks" in header:
        total, used, free = total * 1024, used * 1024, free * 1024
    return DataSpace(total=total, used=used, free=free)


def suggest_rootfs_size(free: int, cap: int = DEFAULT_CAP,
                        reserve: int = DEFAULT_RESERVE,
                        minimum: int = DEFAULT_MINIMUM) -> tuple[int, bool]:
    """Return (suggested bytes rounded down to GiB, fits)."""
    usable = free - reserve
    fits = usable >= minimum
    suggested = min(cap, usable)
    suggested = max(0, (suggested // GIB) * GIB)
    if suggested < minimum:
        suggested = minimum
    return suggested, fits


# ---------------------------------------------------------------------------
# install orchestration
# ---------------------------------------------------------------------------

def _prefix(stream_name: str, line: str) -> str:
    return f"[{stream_name}] {line}"


def install(options: InstallOptions, emit: Callable[[str], None],
            log_path: Optional[Path] = None) -> None:
    """Run the requested flash, rootfs install, removal or whole-device prep."""
    paths = repo_paths()
    env = os.environ.copy()
    env["ANDROID_SERIAL"] = options.device.serial
    env["ARTIFACTS_DIR"] = str(paths["artifacts"])

    if options.mode not in ("dual", "whole"):
        raise InstallerError(f"unknown installation mode: {options.mode}")
    if options.source not in ("manual", "auto"):
        raise InstallerError(f"unknown image source: {options.source}")
    if options.existing_action not in ("none", "replace", "remove"):
        raise InstallerError(
            f"unknown existing-install action: {options.existing_action}")

    removing = options.existing_action == "remove"
    whole = options.mode == "whole"
    if removing and whole:
        raise InstallerError("removal applies to a dual-boot installation")

    flash_boot = options.flash_boot and not whole and not removing
    install_rootfs = True if (whole or removing) else options.install_rootfs
    switch_slot = options.switch_slot and not whole and not removing
    account_enabled = options.account_enabled and not removing

    if flash_boot:
        missing = [name for name in ("boot", "init_boot", "vbmeta",
                                     "vendor_boot", "dtbo")
                   if not getattr(options.images, name)]
        if missing:
            raise InstallerError(
                "missing boot-chain image(s): " + ", ".join(missing))
        if options.flash_firmware:
            missing_fw = [part for part in SLOT_B_FIRMWARE
                          if part not in options.images.slot_b_firmware]
            if missing_fw:
                raise InstallerError(
                    "missing slot-B firmware image(s): "
                    + ", ".join(f"{part}.img" for part in missing_fw))
            if not slot_b_firmware_dir(options.images):
                raise InstallerError(
                    "slot-B firmware images are not in one directory")

    if install_rootfs and not removing:
        if options.source == "manual" and not options.images.rootfs:
            raise InstallerError("missing postmarketOS rootfs image")
        if options.source == "auto" and not (
                options.image_url or default_image_url()):
            raise InstallerError("no server rootfs image URL is configured")
        if not options.images.modules:
            raise InstallerError("missing deploy/modules.tar.xz payload")
        if whole:
            if not options.whole_output:
                raise InstallerError(
                    "whole-device mode needs an output directory")
            missing_boot = [name for name in ("boot", "init_boot", "vbmeta")
                            if not getattr(options.images, name)]
            if missing_boot:
                raise InstallerError(
                    "whole-device mode needs a matching pmOS boot chain: "
                    + ", ".join(missing_boot))
        if account_enabled:
            if not options.username:
                raise InstallerError("an account username is required")
            if not options.password_hash:
                raise InstallerError("an account password hash is required")

    switched_in_flash = (
        options.device.mode == "fastboot"
        and flash_boot
        and switch_slot
    )

    phases: list[str] = []
    if flash_boot:
        phases.append("boot")
    if install_rootfs:
        phases.append("rootfs")

    phase_start = 0.0
    phase_span = 100.0 / max(1, len(phases))
    phase_idx = 0

    def phase_emit(line: str, name: str, start: float, end: float) -> None:
        # Each helper script reports its own 0..100 progress and its own
        # completion marker.  Scale those into this run's overall progress
        # and let only the orchestrator emit the final DONE.
        if line.startswith("%%PMOS_DONE"):
            return
        if line.startswith("%%PMOS_PROGRESS:"):
            m = re.match(r"%%PMOS_PROGRESS:(\d+):(.*)", line)
            if m:
                scaled = start + (end - start) * int(m.group(1)) / 100.0
                emit(f"%%PMOS_PROGRESS:{scaled:.0f}:{m.group(2)}")
                return
        if line.startswith(("%%PMOS_STEP:", "%%PMOS_ERROR:")):
            # Keep helper step/error markers machine-readable; they drive the
            # wizard status line instead of being buried in the log.
            emit(line)
            return
        emit(_prefix(name, line))

    if flash_boot:
        script = paths["deploy"] / "scripts" / "flash_slot_b.sh"
        if not script.is_file():
            raise InstallerError(f"missing flash script: {script}")
        argv = [
            "bash", str(script),
            "--boot", str(options.images.boot),
            "--init-boot", str(options.images.init_boot),
            "--vbmeta", str(options.images.vbmeta),
            "--vendor-boot", str(options.images.vendor_boot),
            "--dtbo", str(options.images.dtbo),
            "--serial", options.device.serial,
        ]
        if options.flash_firmware:
            firmware_dir = slot_b_firmware_dir(options.images)
            if not firmware_dir:
                raise InstallerError(
                    "slot-B firmware images are not in one directory")
            argv += ["--firmware-dir", str(firmware_dir)]
        if options.device.mode == "fastboot":
            if switch_slot:
                argv.append("--set-active")
            if options.reboot_after_switch:
                argv.append("--reboot")
        emit("%%PMOS_STEP:boot:Flashing the slot-B boot chain")
        rc = _stream(
            argv,
            lambda line: phase_emit(line, "flash",
                                    phase_start + phase_idx * phase_span,
                                    phase_start + (phase_idx + 1) * phase_span),
            env=env,
        )
        if rc != 0:
            raise InstallerError(f"slot-B flash failed (exit {rc})")
        phase_idx += 1

    if install_rootfs:
        if options.device.mode != "adb":
            raise InstallerError(
                "rootfs installation requires Android booted with adb")
        script = paths["common"] / "install_pmos_rootfs.sh"
        if not script.is_file():
            raise InstallerError(f"missing rootfs installer: {script}")

        tmpdir: Optional[str] = None
        if removing:
            argv = [
                "bash", str(script),
                "--mode", "dual",
                "--remove-existing",
                "--yes",
            ]
            if options.remove_legacy:
                argv.append("--remove-legacy")
            emit("%%PMOS_STEP:remove:Removing the existing pmOS installation")
            step_name = "remove"
        else:
            argv = ["bash", str(script)]
            if options.source == "auto":
                argv += ["--source", "auto"]
                if options.image_url:
                    argv += ["--url", options.image_url]
                if options.image_sha256:
                    argv += ["--sha256", options.image_sha256]
            else:
                argv += ["--source", "manual", "--image",
                         str(options.images.rootfs)]
            argv += ["--mode", options.mode]
            if options.size_bytes > 0:
                argv += ["--size", f"{options.size_gib}G"]
            if whole:
                argv += ["--whole-output", str(options.whole_output)]
                argv += [
                    "--whole-boot-image", str(options.images.boot),
                    "--whole-init-boot-image", str(options.images.init_boot),
                    "--whole-vbmeta-image", str(options.images.vbmeta),
                ]
                if options.images.vendor_boot:
                    argv += ["--whole-vendor-boot-image",
                             str(options.images.vendor_boot)]
                if options.images.dtbo:
                    argv += ["--whole-dtbo-image",
                             str(options.images.dtbo)]
            if options.existing_action == "replace":
                argv.append("--replace-existing")
            if account_enabled:
                tmpdir = tempfile.mkdtemp(prefix="pmos-gui-")
                hash_path = Path(tmpdir) / "password.hash"
                fd = os.open(hash_path,
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w") as handle:
                    handle.write(options.password_hash + "\n")
                argv += ["--username", options.username,
                         "--password-hash-file", str(hash_path)]
            else:
                argv.append("--no-account")
            argv.append("--yes")
            emit("%%PMOS_STEP:rootfs:"
                 + ("Preparing the whole-device rootfs package"
                    if whole else "Installing the postmarketOS rootfs"))
            step_name = "rootfs"

        try:
            rc = _stream(
                argv,
                lambda line: phase_emit(
                    line, step_name,
                    phase_start + phase_idx * phase_span,
                    phase_start + (phase_idx + 1) * phase_span),
                env=env,
            )
        finally:
            if tmpdir:
                shutil.rmtree(tmpdir, ignore_errors=True)
        if rc != 0:
            raise InstallerError(
                f"{'removal' if removing else 'rootfs installation'} "
                f"failed (exit {rc})")
        phase_idx += 1

    if switch_slot and not switched_in_flash:
        emit("%%PMOS_STEP:switch:Switching the active slot to B")
        switch_to_slot_b(options.device, emit,
                         reboot=options.reboot_after_switch)
    elif switch_slot:
        emit("Slot B was activated by the fastboot flash step.")

    if removing:
        final_text = "Removal complete"
    elif whole:
        final_text = "Whole-device package ready"
    else:
        final_text = "Installation complete"
    emit(f"%%PMOS_PROGRESS:100:{final_text}")
    emit("%%PMOS_DONE")


def wait_for_fastboot(serial: str, timeout: float = 120,
                      emit: Optional[Callable[[str], None]] = None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if serial in list_fastboot_serials():
            return True
        if emit:
            emit("%%PMOS_PROGRESS:98:Waiting for fastboot")
        time.sleep(2)
    return False


def switch_to_slot_b(device: Device, emit: Callable[[str], None],
                     reboot: bool = True) -> None:
    serial = device.serial
    if device.mode == "adb":
        emit("Rebooting to the bootloader...")
        _run(["adb", "-s", serial, "reboot", "bootloader"], timeout=20, check=False)
        if not wait_for_fastboot(serial, emit=emit):
            # Fastboot sometimes reports a different USB serial; use the only
            # connected device when the original serial does not reappear.
            candidates = list_fastboot_serials()
            if len(candidates) == 1:
                serial = candidates[0]
            else:
                raise InstallerError(
                    "device did not enter fastboot; switch the slot manually")
    emit("Setting slot B active...")
    _run(["fastboot", "-s", serial, "set_active", "b"], timeout=60, check=True)

    if reboot:
        emit("Rebooting into slot B...")
        _run(["fastboot", "-s", serial, "reboot"], timeout=30, check=False)
    else:
        emit("Slot B is active. Reboot when ready.")


def reboot_to_bootloader(device: Device) -> None:
    if device.mode == "adb":
        _run(["adb", "-s", device.serial, "reboot", "bootloader"], timeout=20, check=False)
    else:
        _run(["fastboot", "-s", device.serial, "reboot-bootloader"], timeout=20, check=False)


def reboot_device(device: Device) -> None:
    if device.mode == "adb":
        _run(["adb", "-s", device.serial, "reboot"], timeout=20, check=False)
    else:
        _run(["fastboot", "-s", device.serial, "reboot"], timeout=20, check=False)


def _images_json(images: ImageSet) -> dict:
    payload = {}
    for key, value in dataclasses.asdict(images).items():
        if isinstance(value, dict):
            payload[key] = {name: str(path) for name, path in value.items()}
        else:
            payload[key] = str(value) if value else None
    return payload


def detect_json() -> str:
    devices = detect_devices()
    payload = []
    for dev in devices:
        parts = list_partitions(dev)
        space = get_data_space(dev)
        payload.append({
            "device": dataclasses.asdict(dev),
            "data": dataclasses.asdict(space) if space else None,
            "partitions": [dataclasses.asdict(p) for p in parts],
            "images": _images_json(find_images()),
        })
    return json.dumps(payload, indent=2)
