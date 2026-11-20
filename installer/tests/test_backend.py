# SPDX-License-Identifier: GPL-2.0
"""Unit tests for the TB351FU installer backend."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tb351fu_installer import backend, selftest  # noqa: E402


class SizeTests(unittest.TestCase):
    def test_parse_size(self):
        self.assertEqual(backend.parse_size("64G"), 64 * backend.GIB)
        self.assertEqual(backend.parse_size("512m"), 512 * backend.MIB)
        self.assertEqual(backend.parse_size("4096"), 4096)
        with self.assertRaises(ValueError):
            backend.parse_size("64GB")

    def test_human_size(self):
        self.assertEqual(backend.human_size(0), "0 B")
        self.assertEqual(backend.human_size(1024), "1.0 KiB")
        self.assertEqual(backend.human_size(8 * backend.GIB), "8.0 GiB")

    def test_suggest(self):
        suggested, fits = backend.suggest_rootfs_size(100 * backend.GIB)
        self.assertEqual(suggested, 64 * backend.GIB)
        self.assertTrue(fits)
        _, fits = backend.suggest_rootfs_size(10 * backend.GIB)
        self.assertFalse(fits)

    def test_options_size_gib(self):
        options = backend.InstallOptions(
            device=backend.Device(serial="x", mode="adb"),
            images=backend.ImageSet(),
            size_bytes=32 * backend.GIB,
        )
        self.assertEqual(options.size_gib, 32)


class WholeModeTests(unittest.TestCase):
    def test_whole_requires_output_and_boot_chain(self):
        device = backend.Device(serial="x", mode="adb")
        options = backend.InstallOptions(
            device=device,
            images=backend.ImageSet(rootfs=Path("rootfs.img"),
                                    modules=Path("modules.tar.xz"),
                                    firmware=Path("firmware.tar.xz")),
            size_bytes=64 * backend.GIB,
            mode="whole",
            source="manual",
            install_rootfs=True,
        )
        with mock.patch.object(backend, "repo_paths",
                               return_value={"artifacts": Path("/tmp")}):
            with self.assertRaisesRegex(backend.InstallerError, "output directory"):
                backend.install(options, lambda _line: None)
            options.whole_output = Path("/tmp/whole-test")
            with self.assertRaisesRegex(backend.InstallerError, "boot chain"):
                backend.install(options, lambda _line: None)

    def test_whole_does_not_flash_from_android(self):
        device = backend.Device(serial="x", mode="adb")
        options = backend.InstallOptions(
            device=device,
            images=backend.ImageSet(rootfs=Path("rootfs.img"),
                                    modules=Path("modules.tar.xz"),
                                    firmware=Path("firmware.tar.xz"),
                                    boot=Path("boot.img"),
                                    init_boot=Path("init_boot.img"),
                                    vbmeta=Path("vbmeta.img")),
            size_bytes=64 * backend.GIB,
            mode="whole",
            source="manual",
            install_rootfs=True,
            flash_boot=True,
            switch_slot=True,
            whole_output=Path("/tmp/whole-test"),
            username="alice",
            password_hash="$6$rounds=120000$abcdefghijklmnop$" + "a" * 86,
        )
        seen = {}

        def fake_stream(argv, emit, env=None, cwd=None):
            seen["argv"] = argv
            emit("%%PMOS_DONE")
            return 0

        with mock.patch.object(backend, "repo_paths",
                               return_value={"common": Path(__file__).resolve().parents[2],
                                             "deploy": Path(__file__).resolve().parents[2] / "deploy",
                                             "artifacts": Path("/tmp")}), \
             mock.patch.object(backend, "_stream", side_effect=fake_stream):
            backend.install(options, lambda _line: None)
        argv = seen["argv"]
        self.assertNotIn("flash_slot_b.sh", " ".join(argv))
        self.assertIn("--mode", argv)
        self.assertEqual(argv[argv.index("--mode") + 1], "whole")
        self.assertIn("--whole-output", argv)
        self.assertIn("--whole-boot-image", argv)
        self.assertIn("--whole-init-boot-image", argv)
        self.assertIn("--whole-vbmeta-image", argv)
        self.assertIn("--username", argv)
        self.assertIn("alice", argv)
        self.assertIn("--password-hash-file", argv)
        self.assertNotIn("--no-account", argv)
        self.assertNotIn("--set-active", argv)

    def test_rootfs_requires_the_entered_account(self):
        device = backend.Device(serial="x", mode="adb")
        options = backend.InstallOptions(
            device=device,
            images=backend.ImageSet(rootfs=Path("rootfs.img"),
                                    modules=Path("modules.tar.xz"),
                                    firmware=Path("firmware.tar.xz")),
            size_bytes=16 * backend.GIB,
            flash_boot=False,
            install_rootfs=True,
            source="manual",
        )
        with mock.patch.object(backend, "repo_paths",
                               return_value={"artifacts": Path("/tmp")}):
            with self.assertRaisesRegex(backend.InstallerError,
                                        "username is required"):
                backend.install(options, lambda _line: None)


class SpaceTests(unittest.TestCase):
    def test_data_space_uses_df_k(self):
        """Android toybox rejects df -B1, so the backend must query -k."""
        device = backend.Device(serial="x", mode="adb")
        calls = []

        def fake_adb(serial, *args, **kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(
                ["adb"], 0,
                stdout=("Filesystem 1K-blocks Used Available Use% Mounted on\n"
                        "/dev/block/dm-50 239056864 4667788 233852208 2% /data\n"),
                stderr="")

        with mock.patch.object(backend, "_adb", side_effect=fake_adb):
            space = backend.get_data_space(device)

        self.assertIsNotNone(space)
        self.assertEqual(space.free, 233852208 * 1024)
        self.assertEqual(space.used, 4667788 * 1024)
        self.assertEqual(calls[0], ("shell", "df", "-k", "/data"))


class ImageTests(unittest.TestCase):
    def test_gpt(self):
        self.assertIn("GPT", selftest.check_gpt())

    def test_magic(self):
        selftest.check_image_kinds()

    def test_validation(self):
        selftest.check_validate_images()

    def test_bundle_scan(self):
        selftest.check_bundle_scan()

    def test_roles(self):
        selftest.check_partition_roles()


class ScriptTests(unittest.TestCase):
    def test_scripts(self):
        result = selftest.check_scripts()
        if result.startswith("SKIP:"):
            self.skipTest(result)
        self.assertIn("slot-A guards", result)

    def test_orchestration(self):
        selftest.check_orchestration()

    def test_gnss_payload(self):
        result = selftest.check_gnss_payload()
        self.assertIn("NMEA-only", result)


if __name__ == "__main__":
    unittest.main()
