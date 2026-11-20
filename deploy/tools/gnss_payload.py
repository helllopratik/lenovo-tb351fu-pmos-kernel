#!/usr/bin/env python3
"""Assemble the TB351FU GNSS payload from stock Android slot A.

The vendor GNSS daemon (mnld) and its libraries are proprietary and are not
kept in this repository.  The installer runs this helper while Android is
still available and copies into the pmOS payload:

  * /vendor/bin/mnld and its ELF dependency closure (vendor libs first),
  * the Bionic runtime APEX, packed as runtime-apex.img,
  * the shim, scripts and systemd units from common/deploy/gnss,
  * the NMEA-only GeoClue configuration,
  * the host CA bundle used by mnld's EPO download.

Only read-only reads of slot A are performed.  The helper never writes to
Android, never reboots, and never touches misc or the boot chain.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from collections import deque

MNLD = "/vendor/bin/mnld"
# mnld constructs /system/vendor/etc/mpe.conf, so the payload needs Android's
# /system/vendor -> /vendor symlink.  MNL_Config.xml is the receiver config.
VENDOR_CONFIGS = ["/vendor/etc/MNL_Config.xml"]
OPTIONAL_VENDOR_CONFIGS = ["/vendor/etc/mpe.conf"]
VENDOR_DIRS = ["/vendor/lib64", "/vendor/lib64/mt6789"]
SYSTEM_DIRS = ["/system/lib64"]
APEX_DIRS = [
    "/apex/com.android.runtime/lib64/bionic",
    "/apex/com.android.runtime/lib64",
]
# Libraries that mnld loads with dlopen() rather than DT_NEEDED.  Missing
# entries are ignored: several are optional on non-MTK builds.
EXTRA_DLOPEN = ["libmbrainSDKv.so", "libcustom_nvram.so", "libmtkproperty.so"]

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
PT_LOAD = 1
PT_DYNAMIC = 2


def adb_shell(adb, command):
    return subprocess.run(
        [adb, "shell", "su -c " + command],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def remote_exists(adb, path):
    return adb_shell(adb, "test -e " + path).returncode == 0


def remote_readlink(adb, path):
    proc = adb_shell(adb, "readlink -f " + path)
    if proc.returncode != 0:
        return None
    return proc.stdout.strip() or None


def pull_file(adb, remotepath, destpath):
    os.makedirs(os.path.dirname(destpath), exist_ok=True)
    tmp = destpath + ".part"
    with open(tmp, "wb") as out:
        proc = subprocess.run(
            [adb, "exec-out", "su -c 'cat " + remotepath + "'"],
            stdout=out,
            stderr=subprocess.PIPE,
        )
    if proc.returncode != 0 or os.path.getsize(tmp) == 0:
        os.unlink(tmp)
        raise RuntimeError("failed to pull %s: %s" % (
            remotepath, proc.stderr.decode("utf-8", "replace").strip()))
    os.replace(tmp, destpath)


def _u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def _u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def _u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def elf_needed(path):
    """Return DT_NEEDED entries from an ELF32/ELF64 little-endian binary."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise RuntimeError("%s is not an ELF file" % path)
    elfclass, elfdata = data[4], data[5]
    if elfdata != 1:
        raise RuntimeError("%s is not little-endian" % path)

    if elfclass == 2:
        phoff = _u64(data, 0x20)
        phentsize = _u16(data, 0x36)
        phnum = _u16(data, 0x38)
        dyn_fmt = "<qQ"
        dyn_size = 16
    elif elfclass == 1:
        phoff = _u32(data, 0x1C)
        phentsize = _u16(data, 0x2A)
        phnum = _u16(data, 0x2C)
        dyn_fmt = "<iI"
        dyn_size = 8
    else:
        raise RuntimeError("%s has an unknown ELF class" % path)

    loads = []
    dynamic = None
    for i in range(phnum):
        off = phoff + i * phentsize
        if elfclass == 2:
            p_type = _u32(data, off)
            p_offset = _u64(data, off + 8)
            p_vaddr = _u64(data, off + 16)
            p_filesz = _u64(data, off + 32)
        else:
            p_type = _u32(data, off)
            p_offset = _u32(data, off + 4)
            p_vaddr = _u32(data, off + 8)
            p_filesz = _u32(data, off + 16)
        if p_type == PT_LOAD and p_filesz:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == PT_DYNAMIC:
            dynamic = (p_offset, p_filesz)

    if dynamic is None:
        return []

    entries = []
    offset, size = dynamic
    for pos in range(offset, offset + size, dyn_size):
        tag, val = struct.unpack_from(dyn_fmt, data, pos)
        if tag == DT_NULL:
            break
        entries.append((tag, val))

    strtab = None
    strsz = 0
    needed_offsets = []
    for tag, val in entries:
        if tag == DT_NEEDED:
            needed_offsets.append(val)
        elif tag == DT_STRTAB:
            strtab = val
        elif tag == DT_STRSZ:
            strsz = val
    if strtab is None or not needed_offsets:
        return []

    strtab_off = None
    for vaddr, p_offset, filesz in loads:
        if vaddr <= strtab < vaddr + filesz:
            strtab_off = p_offset + (strtab - vaddr)
            break
    if strtab_off is None:
        raise RuntimeError("cannot map the string table in " + path)

    strings = data[strtab_off:strtab_off + strsz]
    needed = []
    for value in needed_offsets:
        end = strings.find(b"\0", value)
        if end < 0:
            raise RuntimeError("unterminated DT_NEEDED in " + path)
        needed.append(strings[value:end].decode("utf-8", "replace"))
    return needed


def resolve(adb, name):
    for directory in VENDOR_DIRS:
        path = directory + "/" + name
        if remote_exists(adb, path):
            return path, "vendor"
    return resolve_system(adb, name)


def resolve_system(adb, name):
    """Resolve inside the chroot's system namespace.

    The linker keeps the system namespace isolated from /vendor/lib64, so a
    library loaded there may only depend on /system/lib64 or the Bionic APEX.
    """
    for directory in SYSTEM_DIRS:
        path = directory + "/" + name
        if remote_exists(adb, path):
            target = remote_readlink(adb, path)
            if target and target.startswith("/apex/com.android.runtime/"):
                return target, "apex"
            return path, "system"
    for directory in APEX_DIRS:
        path = directory + "/" + name
        if remote_exists(adb, path):
            return path, "apex"
    return None, None


def pull_closure(adb, opt, pulled, queue):
    """Pull every queued library and follow its DT_NEEDED closure."""
    while queue:
        remote, kind = queue.popleft()
        name = os.path.basename(remote)
        if (kind, name) in pulled:
            continue
        pulled[(kind, name)] = remote
        if kind == "apex":
            continue
        dest = os.path.join(opt,
                            "vendor" if kind == "vendor" else "system",
                            "lib64", name)
        if remote.startswith("/vendor/bin/"):
            dest = os.path.join(opt, "vendor", "bin", name)
        if not os.path.isfile(dest):
            pull_file(adb, remote, dest)
            os.chmod(dest, 0o644)
        with open(dest, "rb") as handle:
            if handle.read(4) != b"\x7fELF":
                raise RuntimeError("%s is not an ELF library" % remote)
        for needed in elf_needed(dest):
            if kind == "system":
                found, found_kind = resolve_system(adb, needed)
            else:
                found, found_kind = resolve(adb, needed)
            if not found:
                raise RuntimeError("cannot resolve %s (needed by %s)" %
                                   (needed, remote))
            if (found_kind, needed) not in pulled:
                queue.append((found, found_kind))


def pull_apex(adb, tree, payload):
    os.makedirs(tree, exist_ok=True)
    proc = subprocess.run(
        [adb, "shell", "su -c 'tar -cf - -C /apex/com.android.runtime .'"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        raise RuntimeError("cannot read the runtime APEX: " +
                           proc.stderr.decode("utf-8", "replace").strip())
    with tempfile.NamedTemporaryFile(suffix=".tar") as archive:
        archive.write(proc.stdout)
        archive.flush()
        with tarfile.open(archive.name) as tar:
            try:
                tar.extractall(tree, filter="fully_trusted")
            except TypeError:  # Python < 3.12
                tar.extractall(tree)

    # The APEX contains absolute symlinks such as
    # lib64/libbase.so -> /system/lib64/libbase.so.  Inside the chroot /system
    # is the payload copy, so ship those targets too.  Return them so the
    # caller can follow their own dependencies (for example libunwindstack
    # needs libz, which the APEX does not contain).
    copied = []
    for root, dirs, files in os.walk(tree):
        for name in files + dirs:
            path = os.path.join(root, name)
            if not os.path.islink(path):
                continue
            target = os.readlink(path)
            if not target.startswith(("/system/", "/vendor/")):
                continue
            local = os.path.join(payload, "opt", "tb351fu-gps",
                                 target.lstrip("/"))
            if os.path.exists(local) or not remote_exists(adb, target):
                continue
            pull_file(adb, target, local)
            os.chmod(local, 0o644)
            copied.append((target, "system"))
    return copied


def copy_repo_files(repo_gnss, payload):
    opt = os.path.join(payload, "opt", "tb351fu-gps")
    files = [
        "tablet-gnss-bridge.py", "run-gps.sh", "run-mnld.sh",
        "gps-runtime-inner.sh", "logd-sink.py", "mnl-probe.py",
        "ld.config.txt", "shim/props.txt", "shim/lib64/libgpsprops.so",
        "modules/gps_pwr.ko", "modules/gps_drv_stp.ko",
    ]
    for name in files:
        src = os.path.join(repo_gnss, name)
        if not os.path.isfile(src):
            raise RuntimeError("missing GNSS payload file: " + src)
        if name.startswith("modules/"):
            dst = os.path.join(payload, "usr", "lib", "modules", "extra",
                               os.path.basename(name))
        else:
            dst = os.path.join(opt, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        executable = name.endswith((".py", ".sh"))
        os.chmod(dst, 0o755 if executable else 0o644)

    for unit in ("tb351fu-gps.service", "tb351fu-gps.socket"):
        src = os.path.join(repo_gnss, unit)
        dst = os.path.join(payload, "etc", "systemd", "system", unit)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        os.chmod(dst, 0o644)

    wants = os.path.join(payload, "etc", "systemd", "system",
                         "sockets.target.wants")
    os.makedirs(wants, exist_ok=True)
    link = os.path.join(wants, "tb351fu-gps.socket")
    if os.path.lexists(link):
        os.unlink(link)
    os.symlink("../tb351fu-gps.socket", link)

    conf = os.path.join(repo_gnss, "geoclue.conf")
    dst = os.path.join(payload, "etc", "geoclue", "geoclue.conf")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(conf, dst)
    os.chmod(dst, 0o644)

    # /system/bin/linker64 is the interpreter recorded in mnld's PT_INTERP.
    # Inside the chroot it resolves through the mounted runtime APEX.
    bindir = os.path.join(opt, "system", "bin")
    os.makedirs(bindir, exist_ok=True)
    linker = os.path.join(bindir, "linker64")
    if os.path.lexists(linker):
        os.unlink(linker)
    os.symlink("/apex/com.android.runtime/bin/linker64", linker)

    # Android has /system/vendor -> /vendor; mnld opens
    # /system/vendor/etc/mpe.conf through it.
    vendor_link = os.path.join(opt, "system", "vendor")
    if os.path.lexists(vendor_link):
        os.unlink(vendor_link)
    os.symlink("/vendor", vendor_link)


def copy_ca_bundle(payload):
    src = "/etc/ssl/certs"
    dst = os.path.join(payload, "opt", "tb351fu-gps", "system", "etc",
                       "security", "cacerts")
    os.makedirs(dst, exist_ok=True)
    count = 0
    if os.path.isdir(src):
        for name in sorted(os.listdir(src)):
            if not name.endswith(".0"):
                continue
            path = os.path.join(src, name)
            if not os.path.isfile(path):
                continue
            shutil.copyfile(path, os.path.join(dst, name))
            os.chmod(os.path.join(dst, name), 0o644)
            count += 1
    if count == 0:
        print("WARNING: no host CA certificates found; EPO downloads may fail",
              file=sys.stderr)
    else:
        print("Installed %d CA certificates for mnld EPO downloads" % count)


def copy_vendor_configs(adb, payload):
    """Copy the stock receiver configuration into the vendor tree."""
    opt = os.path.join(payload, "opt", "tb351fu-gps")
    for remote in VENDOR_CONFIGS:
        if not remote_exists(adb, remote):
            raise RuntimeError("%s is missing; cannot build the GNSS payload" %
                               remote)
        local = os.path.join(opt, remote.lstrip("/"))
        pull_file(adb, remote, local)
        os.chmod(local, 0o644)
    for remote in OPTIONAL_VENDOR_CONFIGS:
        if not remote_exists(adb, remote):
            print("WARNING: %s is absent; mnld will use its built-in defaults" %
                  remote, file=sys.stderr)
            continue
        local = os.path.join(opt, remote.lstrip("/"))
        pull_file(adb, remote, local)
        os.chmod(local, 0o644)


def verify_payload(opt, apex_tree):
    """Check that each linker namespace can resolve every shipped library."""
    vendor_dir = os.path.join(opt, "vendor", "lib64")
    system_dir = os.path.join(opt, "system", "lib64")
    shim_dir = os.path.join(opt, "shim", "lib64")
    vendor = set(os.listdir(vendor_dir))
    system = set(os.listdir(system_dir))
    shim = set(os.listdir(shim_dir)) if os.path.isdir(shim_dir) else set()

    # Files that actually exist in the runtime APEX image.  Absolute symlinks
    # are skipped: they only resolve inside the chroot through /system.
    apex = set()
    for sub in ("lib64", os.path.join("lib64", "bionic")):
        directory = os.path.join(apex_tree, sub)
        if not os.path.isdir(directory):
            continue
        for name in os.listdir(directory):
            path = os.path.join(directory, name)
            if os.path.isfile(path) and not os.path.islink(path):
                apex.add(name)
    visible = system | apex

    shared = set()
    with open(os.path.join(opt, "ld.config.txt"), encoding="utf-8") as handle:
        for line in handle:
            if "link.system.shared_libs" in line:
                shared.update(line.split("=", 1)[1].strip().split(":"))
                break

    problems = []
    for name in sorted(vendor):
        if name == "mnld":
            continue
        for needed in elf_needed(os.path.join(vendor_dir, name)):
            if needed in vendor or needed in shim:
                continue
            if needed in shared and needed in visible:
                continue
            problems.append("%s needs %s (not in vendor or shared libs)" %
                            (name, needed))
    mnld = os.path.join(opt, "vendor", "bin", "mnld")
    if not os.path.isfile(mnld):
        problems.append("vendor/bin/mnld is missing")
    else:
        for needed in elf_needed(mnld):
            if needed in vendor or needed in shim:
                continue
            if needed in shared and needed in visible:
                continue
            problems.append("mnld needs %s (not in vendor or shared libs)" %
                            needed)
    for name in sorted(system):
        for needed in elf_needed(os.path.join(system_dir, name)):
            if needed not in visible:
                problems.append("%s needs %s (not in system or APEX)" %
                                (name, needed))
    if problems:
        raise RuntimeError("linker namespace closure: " + "; ".join(problems[:6]))


def build_apex_image(tree, output):
    used = 0
    for root, _dirs, files in os.walk(tree):
        for name in files:
            path = os.path.join(root, name)
            if not os.path.islink(path):
                used += os.path.getsize(path)
    size = max(12 * 1024 * 1024,
               ((used + 3 * 1024 * 1024) // 4096) * 4096)
    with open(output, "wb") as out:
        out.truncate(size)
    subprocess.run([
        "mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
        "-d", tree, "-E", "root_owner=0:0", output,
        str(size // 4096),
    ], check=True)
    subprocess.run(["e2fsck", "-fn", output], check=True,
                   stdout=subprocess.DEVNULL)
    return size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--payload", required=True,
                        help="installer payload root")
    parser.add_argument("--repo-gnss", required=True,
                        help="common/deploy/gnss")
    parser.add_argument("--adb", default=os.environ.get("ADB", "adb"))
    args = parser.parse_args()

    payload = os.path.abspath(args.payload)
    if not remote_exists(args.adb, MNLD):
        raise RuntimeError("%s is missing; cannot build the GNSS payload" % MNLD)

    copy_repo_files(os.path.abspath(args.repo_gnss), payload)
    copy_ca_bundle(payload)
    copy_vendor_configs(args.adb, payload)

    opt = os.path.join(payload, "opt", "tb351fu-gps")
    print("Resolving the mnld library closure from stock slot A...")
    pulled = {}
    queue = deque([(MNLD, "vendor")])
    for extra in EXTRA_DLOPEN:
        path, kind = resolve(args.adb, extra)
        if path:
            queue.append((path, kind))
    pull_closure(args.adb, opt, pulled, queue)

    tree = os.path.join(opt, "runtime-apex")
    if os.path.isdir(tree):
        shutil.rmtree(tree)
    copied = pull_apex(args.adb, tree, payload)

    # APEX symlinks point at /system/lib64 copies that were not part of the
    # mnld closure.  Follow their dependencies too, then verify against the
    # APEX contents that actually exist.
    queue.extend(copied)
    pull_closure(args.adb, opt, pulled, queue)
    verify_payload(opt, tree)

    vendor = sum(1 for kind, name in pulled
                 if kind == "vendor" and name != "mnld")
    system = sum(1 for kind, _name in pulled if kind == "system")
    print("Pulled %d libraries plus mnld (%d vendor, %d system)" %
          (vendor + system, vendor, system))

    image = os.path.join(opt, "runtime-apex.img")
    size = build_apex_image(tree, image)
    shutil.rmtree(tree)
    os.chmod(image, 0o644)
    print("Built runtime-apex.img (%d bytes)" % size)

    ca = os.path.join(opt, "system", "etc", "security", "cacerts")
    if not os.path.isdir(ca) or not os.listdir(ca):
        raise RuntimeError("GNSS payload has no CA certificates")
    if not os.path.isfile(os.path.join(opt, "vendor", "bin", "mnld")):
        raise RuntimeError("GNSS payload is missing mnld")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print("GNSS_PAYLOAD_ERROR: %s" % exc, file=sys.stderr)
        sys.exit(1)
