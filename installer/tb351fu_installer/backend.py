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
import shutil
import struct
import subprocess
import time
from pathlib import Path
from typing import Callable, Iterable, Optional

GIB = 1024 ** 3
MIB = 1024 ** 2
KIB = 1024

BOOT_PART_SIZE = 64 * MIB
INIT_BOOT_PART_SIZE = 8 * MIB
# Leave Android enough room to keep running; the rootfs is a file inside
# /data, so this reserve is what keeps the system usable after installation.
DEFAULT_RESERVE = 8 * GIB
DEFAULT_CAP = 64 * GIB
DEFAULT_MINIMUM = 8 * GIB


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
class ImageSet:
    boot: Optional[Path] = None
    init_boot: Optional[Path] = None
    vbmeta: Optional[Path] = None
    rootfs: Optional[Path] = None
    modules: Optional[Path] = None
    firmware: Optional[Path] = None


@dataclasses.dataclass
class InstallOptions:
    device: Device
    images: ImageSet
    size_bytes: int
    flash_boot: bool = True
    install_rootfs: bool = True
    switch_slot: bool = True
    reboot_after_switch: bool = True

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


def _first_existing(paths: Iterable[Path]) -> Optional[Path]:
    for path in paths:
        if path and path.is_file():
            return path
    return None


def find_images() -> ImageSet:
    paths = repo_paths()
    artifacts = paths["artifacts"]
    pmos = paths["pmos"]
    deploy = paths["deploy"]

    rootfs_candidates = [
        Path(os.environ["TB351FU_ROOTFS_IMAGE"]) if os.environ.get("TB351FU_ROOTFS_IMAGE") else None,
        pmos / "pmos-phosh-aarch64.img",
        Path("/mnt/Nvme/pmos_rootfs_base.img"),
        Path("/mnt/Nvme/pmos_rootfs_base.img.zst"),
        pmos / "pmos_rootfs_base.img",
        pmos / "pmos_rootfs_base.img.zst",
    ]

    return ImageSet(
        boot=_first_existing([
            artifacts / "boot_latest.img",
            artifacts / "boot_pmos.img",
            Path("/mnt/Nvme/slot_b_bundle/boot_pmos.img"),
        ]),
        init_boot=_first_existing([
            artifacts / "init_boot_latest.img",
            artifacts / "init_boot_pmos.img",
            Path("/mnt/Nvme/slot_b_bundle/init_boot_pmos.img"),
        ]),
        vbmeta=_first_existing([
            artifacts / "vbmeta_latest.img",
            Path("/mnt/Nvme/slot_b_bundle/vbmeta.img"),
        ]),
        rootfs=_first_existing([p for p in rootfs_candidates if p]),
        modules=_first_existing([deploy / "modules.tar.xz"]),
        firmware=_first_existing([deploy / "firmware.tar.xz"]),
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
                    need_boot: bool = True) -> list[str]:
    problems: list[str] = []
    if need_boot:
        if not images.boot:
            problems.append("boot image not found (artifacts/boot_latest.img)")
        elif images.boot.stat().st_size > BOOT_PART_SIZE:
            problems.append("boot image is larger than the 64 MiB boot partition")
        if not images.init_boot:
            problems.append("init_boot image not found (artifacts/init_boot_latest.img)")
        elif images.init_boot.stat().st_size > INIT_BOOT_PART_SIZE:
            problems.append("init_boot image is larger than the 8 MiB partition")
        if not images.vbmeta:
            problems.append("vbmeta image not found (artifacts/vbmeta_latest.img)")
    if need_rootfs:
        if not images.rootfs:
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
    try:
        out = _clean(_adb(device.serial, "shell", "df", "-B1", "/data").stdout)
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
    """Run the flash + rootfs install.  `emit` receives output lines."""
    paths = repo_paths()
    env = os.environ.copy()
    env["ANDROID_SERIAL"] = options.device.serial
    env["ARTIFACTS_DIR"] = str(paths["artifacts"])

    if options.flash_boot:
        missing = [name for name in ("boot", "init_boot", "vbmeta")
                   if not getattr(options.images, name)]
        if missing:
            raise InstallerError(
                "missing boot-chain image(s): " + ", ".join(missing))
    if options.install_rootfs and not options.images.rootfs:
        raise InstallerError("missing postmarketOS rootfs image")

    switched_in_flash = (
        options.device.mode == "fastboot"
        and options.flash_boot
        and options.switch_slot
    )

    phases: list[str] = []
    if options.flash_boot:
        phases.append("boot")
    if options.install_rootfs:
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

    if options.flash_boot:
        script = paths["deploy"] / "scripts" / "flash_slot_b.sh"
        if not script.is_file():
            raise InstallerError(f"missing flash script: {script}")
        argv = [
            "bash", str(script),
            "--boot", str(options.images.boot),
            "--init-boot", str(options.images.init_boot),
            "--vbmeta", str(options.images.vbmeta),
            "--serial", options.device.serial,
        ]
        if options.device.mode == "fastboot":
            if options.switch_slot:
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

    if options.install_rootfs:
        if options.device.mode != "adb":
            raise InstallerError("rootfs installation requires Android booted with adb")
        script = paths["common"] / "install_pmos_rootfs.sh"
        if not script.is_file():
            raise InstallerError(f"missing rootfs installer: {script}")
        argv = [
            "bash", str(script),
            "--image", str(options.images.rootfs),
            "--size", f"{options.size_gib}G",
            "--yes",
        ]
        emit("%%PMOS_STEP:rootfs:Installing the postmarketOS rootfs")
        rc = _stream(
            argv,
            lambda line: phase_emit(line, "rootfs",
                                    phase_start + phase_idx * phase_span,
                                    phase_start + (phase_idx + 1) * phase_span),
            env=env,
        )
        if rc != 0:
            raise InstallerError(f"rootfs installation failed (exit {rc})")
        phase_idx += 1

    if options.switch_slot and not switched_in_flash:
        emit("%%PMOS_STEP:switch:Switching the active slot to B")
        switch_to_slot_b(options.device, emit,
                         reboot=options.reboot_after_switch)
    elif options.switch_slot:
        emit("Slot B was activated by the fastboot flash step.")

    emit("%%PMOS_PROGRESS:100:Installation complete")
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
            "images": {k: str(v) if v else None for k, v in dataclasses.asdict(find_images()).items()},
        })
    return json.dumps(payload, indent=2)
