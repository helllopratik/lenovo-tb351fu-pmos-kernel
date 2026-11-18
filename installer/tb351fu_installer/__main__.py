# SPDX-License-Identifier: GPL-2.0
"""Command line entry point for the TB351FU installer."""

from __future__ import annotations

import argparse
import sys

from . import __version__, backend, selftest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tb351fu-installer",
        description="Install postmarketOS on the Lenovo Tab Plus (TB351FU).",
    )
    parser.add_argument("--version", action="version",
                        version=f"%(prog)s {__version__}")
    parser.add_argument("--detect", action="store_true",
                        help="print detected devices, partitions and images as JSON")
    parser.add_argument("--self-test", action="store_true",
                        help="run headless checks for the backend and scripts")
    parser.add_argument("--gui-smoke-test", action="store_true",
                        help="build the GTK window and quit without showing it")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.detect:
        print(backend.detect_json())
        return 0
    if args.self_test:
        return selftest.run()
    if args.gui_smoke_test:
        from .app import main as gui_main
        return gui_main(argv=[], smoke_test=True)

    from .app import main as gui_main
    return gui_main(argv=[], smoke_test=False)


if __name__ == "__main__":
    sys.exit(main())
