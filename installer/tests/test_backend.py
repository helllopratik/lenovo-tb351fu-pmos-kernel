# SPDX-License-Identifier: GPL-2.0
"""Unit tests for the TB351FU installer backend."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
