# SPDX-License-Identifier: GPL-2.0
"""GTK4 installer for postmarketOS on the Lenovo Tab Plus (TB351FU)."""

from __future__ import annotations

import dataclasses
import queue
import re
import threading
from pathlib import Path
from typing import Optional

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from . import backend  # noqa: E402

APP_ID = "org.postmarketos.TB351FUInstaller"
CSS = """
.dim { opacity: 0.65; }
.mono { font-family: monospace; }
.error { color: #c01c28; }
.success { color: #26a269; }
.warning { color: #e5a50a; }
.page-title { font-size: 1.45em; font-weight: bold; }
.section-title { font-size: 1.1em; font-weight: bold; }
.card { padding: 16px; }
.log { font-family: monospace; font-size: 0.9em; }
"""

USERNAME_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")
RESERVED_USERNAMES = {
    "root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail",
    "news", "uucp", "proxy", "www-data", "backup", "list", "irc", "gnats",
    "nobody",
}


def _username_problem(name: str) -> str:
    if not USERNAME_RE.fullmatch(name):
        return ("Use lowercase letters, digits, _ and -, starting with a letter "
                "or _ (maximum 32 characters).")
    if name in RESERVED_USERNAMES or name.startswith("systemd-"):
        return f"'{name}' is reserved by the system."
    return ""


@dataclasses.dataclass
class UiState:
    device: Optional[backend.Device] = None
    devices: list[backend.Device] = dataclasses.field(default_factory=list)
    partitions: list[backend.Partition] = dataclasses.field(default_factory=list)
    data: Optional[backend.DataSpace] = None
    images: backend.ImageSet = dataclasses.field(default_factory=backend.ImageSet)
    existing: backend.ExistingInstall = dataclasses.field(
        default_factory=backend.ExistingInstall)
    suggested_gib: int = 16
    installing: bool = False
    switched: bool = False


def _label(text: str = "", *, xalign: float = 0.0, wrap: bool = False,
           css: Optional[str] = None) -> Gtk.Label:
    label = Gtk.Label(label=text, xalign=xalign)
    label.set_wrap(wrap)
    if wrap:
        label.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
    if css:
        label.add_css_class(css)
    return label


def _section(title: str, subtitle: str = "") -> Gtk.Box:
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
    box.append(_label(title, css="section-title"))
    if subtitle:
        box.append(_label(subtitle, wrap=True, css="dim"))
    return box


def _card(child: Gtk.Widget) -> Gtk.Frame:
    frame = Gtk.Frame()
    frame.add_css_class("card")
    frame.set_child(child)
    return frame


class InstallerWindow(Gtk.ApplicationWindow):
    def __init__(self, app: Gtk.Application):
        super().__init__(application=app)
        self.set_title("TB351FU pmOS Installer")
        self.set_default_size(1000, 720)

        self.state = UiState()
        self.queue: queue.Queue[str] = queue.Queue()
        self._install_thread: Optional[threading.Thread] = None
        self._detect_thread: Optional[threading.Thread] = None
        self._options: Optional[backend.InstallOptions] = None
        self._pending_options: Optional[backend.InstallOptions] = None
        self._source_touched = False
        self._syncing_options = False

        self._install_css()
        self._build_header()
        self._build_body()
        self.connect("close-request", self._on_close)
        GLib.timeout_add(100, self._drain_queue)
        GLib.idle_add(self.detect_async)

    # ------------------------------------------------------------------
    # construction
    # ------------------------------------------------------------------

    def _install_css(self) -> None:
        provider = Gtk.CssProvider()
        provider.load_from_string(CSS)
        display = Gdk.Display.get_default()
        if display:
            Gtk.StyleContext.add_provider_for_display(
                display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def _build_header(self) -> None:
        header = Gtk.HeaderBar()
        title = Gtk.Label()
        title.set_markup("<b>TB351FU pmOS Installer</b>")
        header.set_title_widget(title)
        self.refresh_btn = Gtk.Button.new_from_icon_name("view-refresh-symbolic")
        self.refresh_btn.set_tooltip_text("Detect device again")
        self.refresh_btn.connect("clicked", lambda *_: self.detect_async())
        header.pack_end(self.refresh_btn)
        self.set_titlebar(header)

    def _build_body(self) -> None:
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.stack = Gtk.Stack()
        self.stack.set_transition_type(Gtk.StackTransitionType.SLIDE_LEFT_RIGHT)
        self.stack.set_vexpand(True)

        self.device_page = self._build_device_page()
        self.partitions_page = self._build_partitions_page()
        self.images_page = self._build_images_page()
        self.options_page = self._build_options_page()
        self.install_page = self._build_install_page()

        self.stack.add_named(self._scrollable(self.device_page), "device")
        self.stack.add_named(self._scrollable(self.partitions_page), "partitions")
        self.stack.add_named(self._scrollable(self.images_page), "images")
        self.stack.add_named(self._scrollable(self.options_page), "options")
        self.stack.add_named(self.install_page, "install")
        root.append(self.stack)

        nav = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        nav.set_margin_start(18)
        nav.set_margin_end(18)
        nav.set_margin_top(10)
        nav.set_margin_bottom(14)
        self.back_btn = Gtk.Button(label="Back")
        self.back_btn.connect("clicked", lambda *_: self._go_back())
        self.next_btn = Gtk.Button(label="Continue")
        self.next_btn.add_css_class("suggested-action")
        self.next_btn.connect("clicked", lambda *_: self._go_next())
        spacer = Gtk.Box()
        spacer.set_hexpand(True)
        nav.append(self.back_btn)
        nav.append(spacer)
        nav.append(self.next_btn)
        root.append(Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL))
        root.append(nav)
        self.set_child(root)
        self._update_nav()

    def _page(self) -> Gtk.Box:
        page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14)
        page.set_margin_top(18)
        page.set_margin_bottom(8)
        page.set_margin_start(22)
        page.set_margin_end(22)
        return page

    def _scrollable(self, child: Gtk.Widget) -> Gtk.ScrolledWindow:
        scrolled = Gtk.ScrolledWindow()
        scrolled.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scrolled.set_child(child)
        return scrolled

    def _build_device_page(self) -> Gtk.Widget:
        page = self._page()
        page.append(_label("Connect your tablet", css="page-title"))
        page.append(_label(
            "Boot Android on slot A with USB debugging enabled, or enter fastboot. "
            "The installer only writes slot B and a rootfs file inside /data.",
            wrap=True, css="dim"))

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        status_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        self.device_spinner = Gtk.Spinner()
        self.device_status = _label("Looking for a device…", css="section-title")
        status_row.append(self.device_spinner)
        status_row.append(self.device_status)
        box.append(status_row)

        self.device_grid = Gtk.Grid(column_spacing=18, row_spacing=8)
        self.device_values: dict[str, Gtk.Label] = {}
        fields = ["Mode", "Serial", "Model", "Codename", "Active slot",
                  "Root access", "Android", "Battery"]
        for row, name in enumerate(fields):
            key = _label(f"{name}:", css="dim")
            value = _label("—")
            value.set_selectable(True)
            self.device_grid.attach(key, 0, row, 1, 1)
            self.device_grid.attach(value, 1, row, 1, 1)
            self.device_values[name] = value
        box.append(self.device_grid)

        self.device_hint = _label("", wrap=True, css="dim")
        box.append(self.device_hint)

        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        detect = Gtk.Button(label="Detect device")
        detect.connect("clicked", lambda *_: self.detect_async())
        bootloader = Gtk.Button(label="Reboot to bootloader")
        bootloader.connect("clicked", lambda *_: self._reboot("bootloader"))
        android = Gtk.Button(label="Reboot device")
        android.connect("clicked", lambda *_: self._reboot("device"))
        buttons.append(detect)
        buttons.append(bootloader)
        buttons.append(android)
        box.append(buttons)

        page.append(_card(box))
        page.append(_label(
            "Full installation needs Android + root adb (Magisk/KernelSU). "
            "Fastboot mode can only flash the boot chain, not the rootfs.",
            wrap=True, css="dim"))
        return page

    def _build_partitions_page(self) -> Gtk.Widget:
        page = self._page()
        page.append(_label("Partitions and rootfs size", css="page-title"))
        page.append(_label(
            "All by-name and dynamic partitions are detected read-only. The pmOS "
            "rootfs is a loopback image inside Android /data, so no UFS "
            "repartitioning is needed.",
            wrap=True, css="dim"))

        summary = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.part_count = _label("No partitions detected", css="section-title")
        self.space_summary = _label("", wrap=True)
        summary.append(self.part_count)
        summary.append(self.space_summary)
        page.append(_card(summary))

        self.part_store = Gtk.ListStore(str, str, str, str, str)
        tree = Gtk.TreeView(model=self.part_store)
        for idx, title in enumerate(["Partition", "Size", "Slot", "Role", "Block node"]):
            renderer = Gtk.CellRendererText()
            if title == "Size":
                renderer.set_property("xalign", 1.0)
            column = Gtk.TreeViewColumn(title, renderer, text=idx)
            column.set_resizable(True)
            tree.append_column(column)
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.set_min_content_height(250)
        scroll.set_child(tree)
        page.append(scroll)

        size_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        size_box.append(_section(
            "RootFS image size",
            "A larger image leaves more room for apps. Keep at least 8 GiB free "
            "for Android."))
        size_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        self.size_spin = Gtk.SpinButton.new_with_range(4, 256, 1)
        self.size_spin.set_value(16)
        self.size_spin.set_digits(0)
        self.size_spin.connect("value-changed", lambda *_: self._on_size_changed())
        suggested_btn = Gtk.Button(label="Use suggested")
        suggested_btn.connect("clicked", lambda *_: self._apply_suggested())
        size_row.append(self.size_spin)
        size_row.append(_label("GiB", css="dim"))
        size_row.append(suggested_btn)
        size_box.append(size_row)
        self.size_hint = _label("", wrap=True)
        size_box.append(self.size_hint)
        page.append(_card(size_box))
        return page

    def _build_images_page(self) -> Gtk.Widget:
        page = self._page()
        page.append(_label("Installation images", css="page-title"))
        page.append(_label(
            "Auto-detect looks in the slot-B bundle, images/ and artifacts/. "
            "The bundle supplies the stock vendor_boot, dtbo and slot-B firmware "
            "needed for boot_b to boot properly.",
            wrap=True, css="dim"))

        bundle_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        bundle_box.append(_section(
            "Slot-B image bundle",
            "Folder containing boot_pmos.img, init_boot_pmos.img, vbmeta.img, "
            "vendor_boot.img, dtbo.img and the slot-B firmware images."))
        bundle_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.bundle_entry = Gtk.Entry()
        self.bundle_entry.set_hexpand(True)
        self.bundle_entry.set_placeholder_text("/mnt/Nvme/slot_b_bundle")
        self.bundle_entry.connect("changed", lambda *_: self._on_bundle_changed())
        self.bundle_browse_btn = Gtk.Button(label="Browse…")
        self.bundle_browse_btn.connect("clicked", lambda *_: self._browse_bundle())
        self.bundle_use_btn = Gtk.Button(label="Use bundle images")
        self.bundle_use_btn.connect("clicked", lambda *_: self._use_bundle())
        bundle_row.append(self.bundle_entry)
        bundle_row.append(self.bundle_browse_btn)
        bundle_row.append(self.bundle_use_btn)
        bundle_box.append(bundle_row)
        self.bundle_status = _label("", wrap=True, css="dim")
        bundle_box.append(self.bundle_status)
        page.append(_card(bundle_box))

        grid = Gtk.Grid(column_spacing=10, row_spacing=10)
        self.image_entries: dict[str, Gtk.Entry] = {}
        self.image_status: dict[str, Gtk.Label] = {}
        rows = [
            ("boot", "Boot image (boot_b)", "boot_b.img"),
            ("init_boot", "Init_boot image (init_boot_b)", "init_boot_b.img"),
            ("vbmeta", "VBMeta image", "vbmeta_b.img"),
            ("vendor_boot", "Vendor boot (vendor_boot_b)", "vendor_boot.img"),
            ("dtbo", "DTB overlays (dtbo_b)", "dtbo.img"),
            ("rootfs", "postmarketOS rootfs", "rootfs_base.img.zst"),
        ]
        for row, (key, title, hint) in enumerate(rows):
            label = _label(title)
            entry = Gtk.Entry()
            entry.set_hexpand(True)
            entry.set_placeholder_text(hint)
            entry.connect("changed", lambda *_: self._on_images_changed())
            browse = Gtk.Button(label="Browse…")
            browse.connect("clicked", lambda _b, k=key, t=title: self._browse_image(k, t))
            if key == "rootfs":
                self.rootfs_browse = browse
            status = _label("", wrap=True, css="dim")
            grid.attach(label, 0, row * 2, 1, 1)
            grid.attach(entry, 1, row * 2, 1, 1)
            grid.attach(browse, 2, row * 2, 1, 1)
            grid.attach(status, 1, row * 2 + 1, 2, 1)
            self.image_entries[key] = entry
            self.image_status[key] = status
        page.append(_card(grid))

        self.chk_firmware = Gtk.CheckButton(
            label="Provision the slot-B firmware from the bundle "
                  "(lk, tee, gz, scp, sspm, mcupm, dpm, spmfw, pi_img)")
        self.chk_firmware.set_active(False)
        self.chk_firmware.set_sensitive(False)
        self.chk_firmware.connect("toggled", lambda *_: self._on_images_changed())
        page.append(self.chk_firmware)
        self.firmware_status = _label("", wrap=True, css="dim")
        page.append(self.firmware_status)

        options = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.chk_flash = Gtk.CheckButton(label="Flash the pmOS boot chain to slot B")
        self.chk_flash.set_active(True)
        self.chk_rootfs = Gtk.CheckButton(label="Install the postmarketOS rootfs in /data")
        self.chk_rootfs.set_active(True)
        self.chk_switch = Gtk.CheckButton(
            label="Switch to slot B and reboot when finished")
        self.chk_switch.set_active(True)
        for chk in (self.chk_flash, self.chk_rootfs, self.chk_switch):
            chk.connect("toggled", lambda *_: self._on_images_changed())
            options.append(chk)
        page.append(_card(options))
        self.rootfs_hint = _label("", wrap=True, css="dim")
        page.append(self.rootfs_hint)

        detect = Gtk.Button(label="Auto-detect images")
        detect.connect("clicked", lambda *_: self._auto_images())
        page.append(detect)
        self.image_problems = _label("", wrap=True, css="error")
        page.append(self.image_problems)
        return page

    def _build_options_page(self) -> Gtk.Widget:
        page = self._page()
        page.append(_label("Installation options", css="page-title"))
        page.append(_label(
            "Choose what happens to an existing installation, whether Android "
            "stays, where the rootfs image comes from, and the pmOS login. "
            "Nothing is written until you confirm on the next page.",
            wrap=True, css="dim"))

        existing_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        existing_box.append(_section(
            "Existing installation",
            "Read-only check of /data/pmos, its marker and the legacy image."))
        self.existing_status = _label("Not checked yet.", wrap=True)
        existing_box.append(self.existing_status)
        self.radio_replace = Gtk.CheckButton(
            label="Replace the existing pmOS rootfs (Android is kept)")
        self.radio_remove = Gtk.CheckButton(
            label="Remove the existing pmOS installation and exit")
        self.radio_remove.set_group(self.radio_replace)
        self.radio_replace.set_active(True)
        for radio in (self.radio_replace, self.radio_remove):
            radio.connect("toggled", lambda *_: self._on_options_changed())
            existing_box.append(radio)
        self.chk_remove_legacy = Gtk.CheckButton(
            label="Also delete the legacy image /data/pmos_legacy/rootfs.img")
        self.chk_remove_legacy.connect(
            "toggled", lambda *_: self._on_options_changed())
        existing_box.append(self.chk_remove_legacy)
        existing_box.append(_label(
            "The legacy image is never deleted implicitly. It is only removed "
            "when this box is checked together with Remove.",
            wrap=True, css="dim"))
        page.append(_card(existing_box))

        mode_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        mode_box.append(_section(
            "Installation mode",
            "Dual boot keeps Android and is the normal choice. Whole device "
            "prepares a destructive offline package: applying it from recovery "
            "erases Android and replaces the boot chain in slots A and B."))
        self.radio_dual = Gtk.CheckButton(
            label="Dual boot — keep Android and install postmarketOS into /data/pmos")
        self.radio_whole = Gtk.CheckButton(
            label="Whole device — prepare a destructive package (erases Android when applied)")
        self.radio_whole.set_group(self.radio_dual)
        self.radio_dual.set_active(True)
        for radio in (self.radio_dual, self.radio_whole):
            radio.connect("toggled", lambda *_: self._on_options_changed())
            mode_box.append(radio)
        self.whole_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.whole_entry = Gtk.Entry()
        self.whole_entry.set_hexpand(True)
        self.whole_entry.set_text(str(
            backend.repo_paths()["artifacts"] / "whole_device_prep"))
        self.whole_entry.connect("changed", lambda *_: self._on_options_changed())
        self.whole_browse_btn = Gtk.Button(label="Browse…")
        self.whole_browse_btn.connect("clicked", lambda *_: self._browse_whole_output())
        self.whole_row.append(_label("Output folder:"))
        self.whole_row.append(self.whole_entry)
        self.whole_row.append(self.whole_browse_btn)
        mode_box.append(self.whole_row)
        self.mode_hint = _label("", wrap=True, css="dim")
        mode_box.append(self.mode_hint)
        page.append(_card(mode_box))

        source_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        source_box.append(_section(
            "RootFS image source",
            "Download the published image from the server, or select a local "
            "image on the previous page."))
        self.radio_manual = Gtk.CheckButton(label="Use a local image file")
        self.radio_auto = Gtk.CheckButton(label="Download from the server")
        self.radio_auto.set_group(self.radio_manual)
        self.radio_manual.set_active(True)
        for radio in (self.radio_manual, self.radio_auto):
            radio.connect("toggled", self._on_source_choice)
            source_box.append(radio)
        self.auto_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        url_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.url_entry = Gtk.Entry()
        self.url_entry.set_hexpand(True)
        self.url_entry.set_placeholder_text(backend.default_image_url()
                                            or "https://…/rootfs.img.zst")
        self.url_entry.set_text(backend.default_image_url())
        self.url_entry.connect("changed", lambda *_: self._on_options_changed())
        url_row.append(_label("URL:"))
        url_row.append(self.url_entry)
        self.auto_box.append(url_row)
        sha_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.sha_entry = Gtk.Entry()
        self.sha_entry.set_hexpand(True)
        self.sha_entry.set_placeholder_text("Optional SHA-256 of the downloaded file")
        self.sha_entry.connect("changed", lambda *_: self._on_options_changed())
        sha_row.append(_label("SHA-256:"))
        sha_row.append(self.sha_entry)
        self.auto_box.append(sha_row)
        source_box.append(self.auto_box)
        self.manual_hint = _label(
            "Select the local rootfs image in Installation images → postmarketOS "
            "rootfs. Compressed .img.zst, .xz and .gz files are accepted.",
            wrap=True, css="dim")
        source_box.append(self.manual_hint)
        self.source_hint = _label("", wrap=True, css="dim")
        source_box.append(self.source_hint)
        page.append(_card(source_box))

        account_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        account_box.append(_section(
            "pmOS login",
            "The password is hashed on this computer with SHA-512 crypt; only "
            "the hash is sent to the tablet."))
        self.chk_account = Gtk.CheckButton(label="Create a pmOS login account")
        self.chk_account.set_active(True)
        self.chk_account.connect("toggled", lambda *_: self._on_options_changed())
        account_box.append(self.chk_account)
        account_grid = Gtk.Grid(column_spacing=10, row_spacing=8)
        self.username_entry = Gtk.Entry()
        self.username_entry.set_text("user")
        self.username_entry.connect("changed", lambda *_: self._on_options_changed())
        self.password_entry = Gtk.Entry()
        self.password_entry.set_visibility(False)
        self.password_entry.set_input_purpose(Gtk.InputPurpose.PASSWORD)
        self.confirm_entry = Gtk.Entry()
        self.confirm_entry.set_visibility(False)
        self.confirm_entry.set_input_purpose(Gtk.InputPurpose.PASSWORD)
        account_grid.attach(_label("Username:"), 0, 0, 1, 1)
        account_grid.attach(self.username_entry, 1, 0, 1, 1)
        account_grid.attach(_label("Password:"), 0, 1, 1, 1)
        account_grid.attach(self.password_entry, 1, 1, 1, 1)
        account_grid.attach(_label("Confirm:"), 0, 2, 1, 1)
        account_grid.attach(self.confirm_entry, 1, 2, 1, 1)
        account_box.append(account_grid)
        self.account_hint = _label("", wrap=True, css="dim")
        account_box.append(self.account_hint)
        page.append(_card(account_box))
        return page

    def _build_install_page(self) -> Gtk.Widget:
        page = self._page()
        page.append(_label("Install", css="page-title"))
        page.append(_label(
            "Review the plan, then start. Slot A is never written.", wrap=True,
            css="dim"))

        summary = Gtk.Grid(column_spacing=18, row_spacing=8)
        self.summary_values: dict[str, Gtk.Label] = {}
        for row, name in enumerate(["Device", "Action", "Image source",
                                    "RootFS image", "RootFS size", "Boot chain",
                                    "Switch slot", "Account", "Free after"]):
            summary.attach(_label(f"{name}:", css="dim"), 0, row, 1, 1)
            value = _label("—", wrap=True)
            summary.attach(value, 1, row, 1, 1)
            self.summary_values[name] = value
        page.append(_card(summary))

        self.progress = Gtk.ProgressBar()
        self.progress.set_show_text(True)
        self.progress.set_fraction(0.0)
        self.progress.set_text("Ready")
        page.append(self.progress)
        self.step_label = _label("Ready")
        page.append(self.step_label)

        log_scroll = Gtk.ScrolledWindow()
        log_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        log_scroll.set_min_content_height(260)
        log_scroll.set_vexpand(True)
        self.log_view = Gtk.TextView()
        self.log_view.set_editable(False)
        self.log_view.set_cursor_visible(False)
        self.log_view.set_monospace(True)
        self.log_view.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self.log_view.add_css_class("log")
        self.log_buffer = self.log_view.get_buffer()
        log_scroll.set_child(self.log_view)
        page.append(log_scroll)

        action_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.install_btn = Gtk.Button(label="Start installation")
        self.install_btn.add_css_class("suggested-action")
        self.install_btn.connect("clicked", lambda *_: self._confirm_install())
        self.finish_switch_btn = Gtk.Button(label="Switch to slot B and reboot")
        self.finish_switch_btn.set_visible(False)
        self.finish_switch_btn.connect("clicked", lambda *_: self._finish_switch())
        action_row.append(self.install_btn)
        action_row.append(self.finish_switch_btn)
        page.append(action_row)
        self.install_status = _label("", wrap=True)
        page.append(self.install_status)
        return page

    # ------------------------------------------------------------------
    # navigation
    # ------------------------------------------------------------------

    def _page_names(self) -> list[str]:
        return ["device", "partitions", "images", "options", "install"]

    def _current_page(self) -> str:
        return self.stack.get_visible_child_name() or "device"

    def _update_nav(self) -> None:
        current = self._current_page()
        index = self._page_names().index(current)
        self.back_btn.set_sensitive(index > 0 and not self.state.installing)
        if current == "install":
            self.next_btn.set_visible(False)
        else:
            self.next_btn.set_visible(True)
            self.next_btn.set_label("Continue")
            self.next_btn.set_sensitive(not self.state.installing)
        if current == "install":
            self._refresh_summary()
        elif current == "options":
            self._refresh_summary()

    def _go_back(self) -> None:
        if self.state.installing:
            return
        names = self._page_names()
        index = names.index(self._current_page())
        if index > 0:
            self.stack.set_visible_child_name(names[index - 1])
            self._update_nav()

    def _go_next(self) -> None:
        if self.state.installing:
            return
        current = self._current_page()
        if current == "device":
            if not self.state.device:
                self._alert("No device detected",
                            "Boot Android with USB debugging (or enter fastboot) and press Detect device.")
                return
            if not self.state.device.is_target:
                self._alert("Unexpected device",
                            f"Detected {self.state.device.codename or self.state.device.model}. "
                            "This installer is for the Lenovo TB351FU.")
                return
        elif current == "partitions":
            if (self.state.device and self.state.device.mode == "adb"
                    and not self.state.data and not self._is_whole()):
                self._alert("Could not read /data",
                            "Rootfs size cannot be validated without free space information.")
                return
            if (self.state.data and not self._is_whole()
                    and not self._is_removing()):
                size = int(self.size_spin.get_value()) * backend.GIB
                if size > self.state.data.free:
                    self._alert("RootFS too large",
                                "The selected size is larger than the free space on /data.")
                    return
        elif current == "images":
            problems = self._validate_images()
            if problems:
                self._alert("Check the installation images",
                            "\n".join(f"• {p}" for p in problems))
                return
        elif current == "options":
            problems = self._validate_install()
            if problems:
                self._alert("Check the installation options",
                            "\n".join(f"• {p}" for p in problems))
                return
        names = self._page_names()
        index = names.index(current)
        if index < len(names) - 1:
            self.stack.set_visible_child_name(names[index + 1])
            self._update_nav()

    # ------------------------------------------------------------------
    # detection
    # ------------------------------------------------------------------

    def detect_async(self) -> None:
        if self.state.installing:
            return
        if self._detect_thread and self._detect_thread.is_alive():
            return
        self.device_spinner.start()
        self.device_status.set_text("Looking for a device…")
        self.refresh_btn.set_sensitive(False)
        self._detect_thread = threading.Thread(target=self._detect_worker, daemon=True)
        self._detect_thread.start()

    def _detect_worker(self) -> None:
        try:
            devices = backend.detect_devices()
            device = None
            partitions: list[backend.Partition] = []
            space = None
            existing = backend.ExistingInstall()
            if devices:
                device = next((d for d in devices if d.is_target), devices[0])
                partitions = backend.list_partitions(device)
                space = backend.get_data_space(device)
                existing = backend.detect_existing_install(device)
            images = backend.find_images()
            GLib.idle_add(self._detect_done, devices, device, partitions, space,
                          existing, images, None)
        except Exception as exc:  # noqa: BLE001 - shown to the user
            GLib.idle_add(self._detect_done, [], None, [], None,
                          backend.ExistingInstall(), backend.find_images(), str(exc))

    def _detect_done(self, devices, device, partitions, space, existing,
                     images, error) -> bool:
        self.device_spinner.stop()
        self.refresh_btn.set_sensitive(True)
        self.state.devices = devices or []
        self.state.device = device
        self.state.partitions = partitions or []
        self.state.data = space
        self.state.existing = existing or backend.ExistingInstall()
        self.state.images = images
        self._render_device(error)
        self._render_partitions()
        self._apply_images(images)
        self._choose_default_source()
        self._update_existing_ui()
        self._update_mode_controls()
        self._update_size_controls()
        self._on_images_changed()
        self._update_options_controls()
        self._update_nav()
        return GLib.SOURCE_REMOVE

    def _update_mode_controls(self) -> None:
        device = self.state.device
        adb_root = bool(device and device.mode == "adb" and device.root)
        self.chk_rootfs.set_sensitive(adb_root and not self.state.installing)
        if not adb_root:
            if self.chk_rootfs.get_active():
                self.chk_rootfs.set_active(False)
            self.rootfs_hint.set_text(
                "RootFS installation needs Android booted with root adb. "
                "In fastboot mode only the boot chain can be flashed.")
            self.rootfs_hint.add_css_class("warning")
        else:
            self.rootfs_hint.set_text(
                "The rootfs will be created as /data/pmos/rootfs.img. "
                "Android /data is never repartitioned.")
            self.rootfs_hint.remove_css_class("warning")
        self._update_options_controls()

    def _is_whole(self) -> bool:
        radio = getattr(self, "radio_whole", None)
        return bool(radio and radio.get_active())

    def _is_removing(self) -> bool:
        radio = getattr(self, "radio_remove", None)
        return bool(self.state.existing.present and not self._is_whole()
                    and radio and radio.get_active())

    def _is_source_auto(self) -> bool:
        radio = getattr(self, "radio_auto", None)
        return bool(radio and radio.get_active())

    def _will_install_rootfs(self) -> bool:
        return (not self._is_removing()
                and (self._is_whole() or self.chk_rootfs.get_active()))

    def _choose_default_source(self) -> None:
        if self._source_touched:
            return
        if self.state.images.rootfs:
            target = self.radio_manual
        elif backend.default_image_url():
            target = self.radio_auto
        else:
            return
        if target.get_active():
            return
        self._syncing_options = True
        target.set_active(True)
        self._syncing_options = False

    def _update_existing_ui(self) -> None:
        if not hasattr(self, "existing_status"):
            return
        existing = self.state.existing
        if existing.error:
            self.existing_status.set_text(
                "Could not check /data: " + existing.error)
            self.existing_status.add_css_class("warning")
            return
        self.existing_status.remove_css_class("warning")
        if existing.present:
            parts = []
            if existing.directory:
                parts.append("/data/pmos")
            if existing.marker:
                parts.append("/data/pmos_install.env")
            if existing.legacy:
                parts.append("/data/pmos_legacy/rootfs.img (legacy)")
            text = "Existing pmOS installation found: " + ", ".join(parts)
            if existing.marker_text:
                marker = " ".join(existing.marker_text.split())[:300]
                text += "\nMarker: " + marker
            self.existing_status.set_text(text)
            if not (self.radio_replace.get_active()
                    or self.radio_remove.get_active()):
                self.radio_replace.set_active(True)
        elif existing.checked:
            self.existing_status.set_text(
                "No existing pmOS installation was found in /data.")
        else:
            self.existing_status.set_text(
                "Existing installations can only be checked with Android "
                "running and root adb available.")

    def _on_source_choice(self, *_args) -> None:
        if not self._syncing_options:
            self._source_touched = True
        self._on_options_changed()

    def _on_options_changed(self) -> None:
        if getattr(self, "_syncing_options", False):
            return
        self._update_options_controls()
        self._on_images_changed()

    def _update_options_controls(self) -> None:
        if not hasattr(self, "radio_whole"):
            return
        busy = self.state.installing
        device = self.state.device
        adb_root = bool(device and device.mode == "adb" and device.root)
        existing = self.state.existing
        present = existing.present and not existing.error
        whole = self._is_whole()
        removing = self._is_removing()
        auto = self._is_source_auto()

        for radio in (self.radio_replace, self.radio_remove):
            radio.set_sensitive(present and not busy and not whole)
        self.chk_remove_legacy.set_sensitive(
            present and existing.legacy and removing and not busy)

        self.radio_dual.set_sensitive(not busy)
        self.radio_whole.set_sensitive(not busy and adb_root)
        self.whole_entry.set_sensitive(whole and not busy)
        self.whole_browse_btn.set_sensitive(whole and not busy)
        self.chk_flash.set_sensitive(not busy and not whole and not removing)
        self.chk_switch.set_sensitive(not busy and not whole and not removing)
        self.chk_rootfs.set_sensitive(
            not busy and adb_root and not whole and not removing)
        if whole or removing:
            self.chk_firmware.set_sensitive(False)

        self.radio_manual.set_sensitive(not busy and not removing)
        self.radio_auto.set_sensitive(not busy and not removing)
        self.auto_box.set_visible(auto)
        self.manual_hint.set_visible(not auto)
        self.url_entry.set_sensitive(auto and not busy and not removing)
        self.sha_entry.set_sensitive(auto and not busy and not removing)
        if hasattr(self, "image_entries"):
            self.image_entries["rootfs"].set_sensitive(
                not auto and not busy and not removing)
            self.rootfs_browse.set_sensitive(
                not auto and not busy and not removing)

        account = (self.chk_account.get_active()
                   and self._will_install_rootfs() and not removing)
        self.chk_account.set_sensitive(
            not busy and not removing and self._will_install_rootfs())
        for widget in (self.username_entry, self.password_entry,
                       self.confirm_entry):
            widget.set_sensitive(account and not busy)
        self.size_spin.set_sensitive(
            not busy and self.state.data is not None
            and not whole and not removing)

        if removing:
            self.mode_hint.set_text(
                "The existing installation will be removed. Nothing is "
                "installed, flashed or switched; Android is not modified.")
        elif whole:
            if adb_root:
                self.mode_hint.set_text(
                    "The package is assembled in temporary encrypted staging "
                    "under /data and copied here; Android is not changed until "
                    "you apply the package from recovery. Slot B is not flashed "
                    "or activated. Applying it erases super, metadata, frp and "
                    "userdata, then writes pmOS directly to userdata and "
                    "replaces the boot chain in slots A and B.")
            else:
                self.mode_hint.set_text(
                    "Whole-device preparation needs Android booted with root adb.")
        else:
            self.mode_hint.set_text(
                "Android and slot A stay installed. postmarketOS lives in "
                "/data/pmos and slot B is used for its boot chain.")

        if removing:
            self.source_hint.set_text("Not used while removing an installation.")
        elif auto:
            self.source_hint.set_text(
                "The image is downloaded on this computer, verified if a "
                "SHA-256 is given, then pushed to the tablet.")
        else:
            self.source_hint.set_text(
                "The selected local image is used; nothing is downloaded.")

        if removing:
            self.account_hint.set_text("Not used while removing an installation.")
        elif not self._will_install_rootfs():
            self.account_hint.set_text(
                "Account setup is only used when a postmarketOS rootfs is installed.")
        elif self.chk_account.get_active():
            self.account_hint.set_text(
                "Only the SHA-512 crypt hash leaves this computer. The tablet "
                "never receives the plaintext password.")
        else:
            self.account_hint.set_text(
                "The image's default account is kept.")

    def _browse_whole_output(self) -> None:
        dialog = Gtk.FileDialog(title="Select the whole-device package folder")
        dialog.select_folder(self, None, self._on_whole_folder_chosen)

    def _on_whole_folder_chosen(self, dialog: Gtk.FileDialog, result) -> None:
        try:
            folder = dialog.select_folder_finish(result)
        except GLib.Error:
            return
        if folder and folder.get_path():
            self.whole_entry.set_text(folder.get_path())

    def _render_device(self, error: Optional[str]) -> None:
        device = self.state.device
        if error:
            self.device_status.set_markup("<span foreground='#c01c28'>Detection error</span>")
            self.device_hint.set_text(error)
        elif not device:
            self.device_status.set_text("No device detected")
            self.device_hint.set_text(
                "Connect the tablet over USB. In Android, enable Developer options → "
                "USB debugging and grant root access to the installer.")
        else:
            mode = "Android (adb)" if device.mode == "adb" else "Fastboot"
            self.device_status.set_markup(f"<b>Device ready</b> — {mode}")
            if device.mode == "fastboot":
                self.device_hint.set_text(
                    "Fastboot mode: the slot-B boot chain can be flashed. Boot "
                    "Android with root adb to install the rootfs.")
            elif device.root:
                self.device_hint.set_text(
                    "Root access confirmed. The installer can flash slot B and "
                    "install the rootfs.")
            else:
                self.device_hint.set_text(
                    "Root access is not available. Boot flash only; rootfs "
                    "installation requires root adb.")
        values = {
            "Mode": (device.mode if device else "—"),
            "Serial": (device.serial if device else "—"),
            "Model": (device.model if device else "—"),
            "Codename": (device.codename if device else "—"),
            "Active slot": (device.slot.upper() if device and device.slot else "—"),
            "Root access": (
                "n/a" if device and device.mode == "fastboot"
                else "Yes" if device and device.root
                else "No" if device else "—"),
            "Android": (device.android if device and device.android else "—"),
            "Battery": (f"{device.battery}%" if device and device.battery is not None else "—"),
        }
        for key, text in values.items():
            self.device_values[key].set_text(text)

    def _render_partitions(self) -> None:
        self.part_store.clear()
        for part in self.state.partitions:
            self.part_store.append([
                part.name,
                part.size_text,
                part.slot.upper() if part.slot else "—",
                part.role or "—",
                part.node or "—",
            ])
        count = len(self.state.partitions)
        self.part_count.set_text(f"{count} partition{'s' if count != 1 else ''} detected")
        data = self.state.data
        if data:
            self.space_summary.set_text(
                f"Android /data: {backend.human_size(data.used)} used of "
                f"{backend.human_size(data.total)} — {backend.human_size(data.free)} free")
        elif self.state.device and self.state.device.mode == "adb":
            self.space_summary.set_text("/data free space could not be read.")
        else:
            self.space_summary.set_text(
                "Rootfs size can only be chosen while Android is running (adb).")

    def _update_size_controls(self) -> None:
        data = self.state.data
        if not data:
            self.size_spin.set_sensitive(False)
            self.size_hint.set_text(
                "Connect in Android mode to choose the rootfs size.")
            return
        suggested, fits = backend.suggest_rootfs_size(data.free)
        self.state.suggested_gib = max(4, suggested // backend.GIB)
        max_gib = max(4, (data.free - backend.GIB) // backend.GIB)
        max_gib = min(256, max_gib)
        self.size_spin.set_range(4, max_gib)
        self.size_spin.set_sensitive(data.free >= 5 * backend.GIB)
        self.size_spin.set_value(min(self.state.suggested_gib, max_gib))
        if fits:
            self.size_hint.set_text(
                f"Suggested: {self.state.suggested_gib} GiB, leaving about "
                f"{backend.human_size(data.free - self.state.suggested_gib * backend.GIB)} "
                "free for Android.")
        else:
            self.size_hint.set_text(
                f"Only {backend.human_size(data.free)} is free. The suggested "
                "size leaves less than 8 GiB for Android; lower it if needed, "
                "but keep at least 1 GiB free.")
        self._on_size_changed()

    def _apply_suggested(self) -> None:
        if self.state.data:
            self.size_spin.set_value(self.state.suggested_gib)

    def _on_size_changed(self) -> None:
        data = self.state.data
        if not data:
            return
        size = int(self.size_spin.get_value()) * backend.GIB
        left = data.free - size
        self.size_hint.remove_css_class("error")
        self.size_hint.remove_css_class("warning")
        if left < backend.GIB:
            self.size_hint.set_text(
                "Selected size would leave less than 1 GiB free on /data. "
                "Android may stop working.")
            self.size_hint.add_css_class("error")
        elif left < backend.DEFAULT_RESERVE:
            self.size_hint.set_text(
                f"Selected: {int(self.size_spin.get_value())} GiB — only "
                f"{backend.human_size(left)} left for Android (8 GiB recommended).")
            self.size_hint.add_css_class("warning")
        else:
            self.size_hint.set_text(
                f"Selected: {int(self.size_spin.get_value())} GiB — "
                f"{backend.human_size(left)} left on /data after creation.")
        self._refresh_summary()

    # ------------------------------------------------------------------
    # images
    # ------------------------------------------------------------------

    def _apply_images(self, images: backend.ImageSet) -> None:
        if images.bundle_dir and not self.bundle_entry.get_text().strip():
            self.bundle_entry.set_text(str(images.bundle_dir))
        mapping = {
            "boot": images.boot,
            "init_boot": images.init_boot,
            "vbmeta": images.vbmeta,
            "vendor_boot": images.vendor_boot,
            "dtbo": images.dtbo,
            "rootfs": images.rootfs,
        }
        for key, path in mapping.items():
            entry = self.image_entries[key]
            if path and not entry.get_text().strip():
                entry.set_text(str(path))
        self._update_image_status()
        self._update_firmware_status()
        if (self.chk_flash.get_active()
                and len(images.slot_b_firmware) == len(backend.SLOT_B_FIRMWARE)):
            self.chk_firmware.set_active(True)

    def _auto_images(self) -> None:
        images = backend.find_images()
        if images.bundle_dir:
            self.bundle_entry.set_text(str(images.bundle_dir))
        for key, path in {
            "boot": images.boot, "init_boot": images.init_boot,
            "vbmeta": images.vbmeta, "vendor_boot": images.vendor_boot,
            "dtbo": images.dtbo, "rootfs": images.rootfs,
        }.items():
            if path:
                self.image_entries[key].set_text(str(path))
        self._update_image_status()
        self._update_firmware_status()
        if (self.chk_flash.get_active()
                and len(images.slot_b_firmware) == len(backend.SLOT_B_FIRMWARE)):
            self.chk_firmware.set_active(True)
        self._choose_default_source()

    def _bundle_path(self) -> Optional[Path]:
        text = (self.bundle_entry.get_text().strip()
                if hasattr(self, "bundle_entry") else "")
        return Path(text).expanduser() if text else None

    def _bundle_images(self) -> backend.ImageSet:
        path = self._bundle_path()
        if path is None:
            return self.state.images
        if not path.is_dir():
            return backend.ImageSet()
        return backend.scan_bundle(path)

    def _browse_bundle(self) -> None:
        dialog = Gtk.FileDialog(title="Select the slot-B image bundle")
        dialog.select_folder(self, None, self._on_bundle_chosen)

    def _on_bundle_chosen(self, dialog: Gtk.FileDialog, result) -> None:
        try:
            folder = dialog.select_folder_finish(result)
        except GLib.Error:
            return
        if folder and folder.get_path():
            self.bundle_entry.set_text(folder.get_path())

    def _on_bundle_changed(self) -> None:
        if not hasattr(self, "bundle_status"):
            return
        path = self._bundle_path()
        if not path:
            self.bundle_status.set_text(
                "No bundle selected. vendor_boot, dtbo and the slot-B firmware "
                "are required for a complete boot_b.")
            self.bundle_status.remove_css_class("error")
        elif not path.is_dir():
            self.bundle_status.set_text(f"Not a directory: {path}")
            self.bundle_status.add_css_class("error")
        else:
            images = backend.scan_bundle(path)
            found = [name for name, value in (
                ("boot", images.boot), ("init_boot", images.init_boot),
                ("vbmeta", images.vbmeta),
                ("vendor_boot", images.vendor_boot), ("dtbo", images.dtbo),
            ) if value]
            firmware = len(images.slot_b_firmware)
            self.bundle_status.set_text(
                f"{len(found)}/5 boot images and {firmware}/"
                f"{len(backend.SLOT_B_FIRMWARE)} firmware images found.")
            self.bundle_status.remove_css_class("error")
        self._update_firmware_status()
        self._refresh_summary()

    def _use_bundle(self) -> None:
        path = self._bundle_path()
        if not path or not path.is_dir():
            self._alert(
                "Bundle not found",
                "Select the folder containing the slot-B images, for example "
                "/mnt/Nvme/slot_b_bundle.")
            return
        images = backend.scan_bundle(path)
        if not any((images.boot, images.init_boot, images.vbmeta,
                    images.vendor_boot, images.dtbo)):
            self._alert(
                "No boot images in this folder",
                "Expected boot_pmos.img, init_boot_pmos.img, vbmeta.img, "
                "vendor_boot.img and dtbo.img.")
            return
        for key, value in {
            "boot": images.boot, "init_boot": images.init_boot,
            "vbmeta": images.vbmeta, "vendor_boot": images.vendor_boot,
            "dtbo": images.dtbo,
        }.items():
            if value:
                self.image_entries[key].set_text(str(value))
        self._update_image_status()
        self._update_firmware_status()
        self._on_images_changed()

    def _update_firmware_status(self) -> None:
        if not hasattr(self, "firmware_status"):
            return
        if self._is_whole() or self._is_removing():
            self.chk_firmware.set_sensitive(False)
            self.firmware_status.set_text(
                "Slot-B firmware is not used in this mode.")
            self.firmware_status.remove_css_class("warning")
            return
        images = self._bundle_images()
        if not self.chk_flash.get_active():
            self.chk_firmware.set_active(False)
            self.chk_firmware.set_sensitive(False)
            self.firmware_status.set_text(
                "Slot-B firmware is only written together with the boot chain.")
            self.firmware_status.remove_css_class("warning")
            return
        missing = [part for part in backend.SLOT_B_FIRMWARE
                   if part not in images.slot_b_firmware]
        if missing:
            self.chk_firmware.set_active(False)
            self.chk_firmware.set_sensitive(False)
            self.firmware_status.set_text(
                "Firmware incomplete; missing "
                + ", ".join(f"{part}.img" for part in missing) + ".")
            self.firmware_status.add_css_class("warning")
            return
        self.chk_firmware.set_sensitive(True)
        self.firmware_status.set_text(
            f"{len(images.slot_b_firmware)} firmware images ready: "
            + ", ".join(f"{part}_b" for part in backend.SLOT_B_FIRMWARE) + ".")
        self.firmware_status.remove_css_class("warning")

    def _browse_image(self, key: str, title: str) -> None:
        dialog = Gtk.FileDialog(title=title)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        images = Gtk.FileFilter()
        images.set_name("Disk images")
        for pattern in ("*.img", "*.zst", "*.raw"):
            images.add_pattern(pattern)
        filters.append(images)
        every = Gtk.FileFilter()
        every.set_name("All files")
        every.add_pattern("*")
        filters.append(every)
        dialog.set_filters(filters)
        dialog.set_default_filter(images)
        dialog.open(self, None, self._on_file_chosen, key)

    def _on_file_chosen(self, dialog: Gtk.FileDialog, result, key: str) -> None:
        try:
            file = dialog.open_finish(result)
        except GLib.Error:
            return
        if file and file.get_path():
            self.image_entries[key].set_text(file.get_path())

    def _image_path(self, key: str) -> Optional[Path]:
        text = self.image_entries[key].get_text().strip()
        return Path(text) if text else None

    def _on_images_changed(self) -> None:
        self._update_image_status()
        self._update_firmware_status()
        self._update_options_controls()
        problems = self._validate_images()
        if problems:
            self.image_problems.set_text("\n".join(f"• {p}" for p in problems))
            self.image_problems.add_css_class("error")
        else:
            self.image_problems.set_text("")
            self.image_problems.remove_css_class("error")
        self._refresh_summary()

    def _update_image_status(self) -> None:
        for key, status in self.image_status.items():
            path = self._image_path(key)
            if not path:
                status.set_text("Not selected")
                continue
            if not path.is_file():
                status.set_text(f"Not found: {path}")
                status.add_css_class("error")
                continue
            status.remove_css_class("error")
            size = backend.human_size(path.stat().st_size)
            kind = backend.image_kind(path)
            extra = ""
            if key == "rootfs":
                content = backend.rootfs_content_size(path)
                if content:
                    extra = f", filesystem {backend.human_size(content)}"
            status.set_text(f"{size}, {kind}{extra}")

    def _current_images(self) -> backend.ImageSet:
        bundle_dir = self._bundle_path()
        bundle_images = self._bundle_images()
        return backend.ImageSet(
            boot=self._image_path("boot"),
            init_boot=self._image_path("init_boot"),
            vbmeta=self._image_path("vbmeta"),
            vendor_boot=self._image_path("vendor_boot"),
            dtbo=self._image_path("dtbo"),
            rootfs=self._image_path("rootfs"),
            modules=self.state.images.modules,
            firmware=self.state.images.firmware,
            bundle_dir=bundle_dir if bundle_dir and bundle_dir.is_dir() else None,
            slot_b_firmware=bundle_images.slot_b_firmware,
        )

    def _validate_images(self) -> list[str]:
        images = self._current_images()
        removing = self._is_removing()
        whole = self._is_whole()
        return backend.validate_images(
            images,
            need_rootfs=self._will_install_rootfs(),
            need_boot=(self.chk_flash.get_active()
                       and not whole and not removing),
            need_firmware=(self.chk_flash.get_active()
                           and self.chk_firmware.get_active()
                           and not whole and not removing),
            need_local_rootfs=not self._is_source_auto(),
        )

    # ------------------------------------------------------------------
    # summary / install
    # ------------------------------------------------------------------

    def _refresh_summary(self) -> None:
        if not hasattr(self, "summary_values"):
            return
        device = self.state.device
        images = self._current_images()
        size_gib = int(self.size_spin.get_value()) if hasattr(self, "size_spin") else 0
        data = self.state.data
        existing = self.state.existing
        whole = self._is_whole()
        removing = self._is_removing()
        auto = self._is_source_auto()
        self.summary_values["Device"].set_text(
            f"{device.serial} ({device.mode}, slot {device.slot or '?'})"
            if device else "—")
        if removing:
            action = "Remove the existing pmOS installation"
            if self.chk_remove_legacy.get_active() and existing.legacy:
                action += " and the legacy image"
        elif whole:
            action = "Prepare a destructive whole-device package"
        elif existing.present and not existing.error:
            action = "Dual boot — replace the existing pmOS rootfs"
        else:
            action = "Dual boot — keep Android"
        self.summary_values["Action"].set_text(action)
        if removing:
            source_text = "—"
        elif auto:
            source_text = ("Server: " + (self.url_entry.get_text().strip()
                                         or backend.default_image_url()
                                         or "URL not set"))
        else:
            source_text = "Local image file"
        self.summary_values["Image source"].set_text(source_text)
        if removing:
            rootfs_text = "—"
        elif auto:
            rootfs_text = "Downloaded from the server"
        else:
            rootfs_text = str(images.rootfs) if images.rootfs else "Not selected"
        self.summary_values["RootFS image"].set_text(rootfs_text)
        if removing:
            size_text = "—"
        elif whole:
            size_text = "Full userdata size (automatic)"
        elif self.chk_rootfs.get_active():
            size_text = f"{size_gib} GiB"
        else:
            size_text = "Skipped"
        self.summary_values["RootFS size"].set_text(size_text)
        if removing or whole:
            boot_text = ("Package includes pmOS boot/init_boot/vbmeta for slots A and B"
                         if whole else "Not touched")
        elif self.chk_flash.get_active():
            boot_text = ("boot_b + init_boot_b + vbmeta_b + vendor_boot_b + dtbo_b"
                         + (f" + {len(images.slot_b_firmware)} firmware images"
                            if self.chk_firmware.get_active() else ""))
        else:
            boot_text = "Skipped"
        self.summary_values["Boot chain"].set_text(boot_text)
        if removing or whole:
            switch_text = "No"
        else:
            switch_text = ("Yes, set slot B active and reboot"
                           if self.chk_switch.get_active() else "No")
        self.summary_values["Switch slot"].set_text(switch_text)
        if removing:
            account_text = "—"
        elif not self._will_install_rootfs():
            account_text = "—"
        elif self.chk_account.get_active():
            account_text = self.username_entry.get_text().strip() or "user"
        else:
            account_text = "No account (image default kept)"
        self.summary_values["Account"].set_text(account_text)
        if data and self.chk_rootfs.get_active() and not whole and not removing:
            left = data.free - size_gib * backend.GIB
            self.summary_values["Free after"].set_text(
                backend.human_size(max(0, left)))
        elif whole:
            self.summary_values["Free after"].set_text(
                "Temporary staging only; freed afterwards")
        else:
            self.summary_values["Free after"].set_text("—")

        if removing:
            self.install_btn.set_label("Remove installation")
        elif whole:
            self.install_btn.set_label("Prepare destructive whole-device package")
        else:
            self.install_btn.set_label("Start installation")

    def _collect_options(self) -> backend.InstallOptions:
        device = self.state.device
        if not device:
            raise backend.InstallerError("no device detected")
        whole = self._is_whole()
        removing = self._is_removing()
        source = "auto" if self._is_source_auto() else "manual"
        existing_action = "none"
        if (self.state.existing.present and not self.state.existing.error
                and not whole):
            existing_action = ("remove" if self.radio_remove.get_active()
                               else "replace")
        account = (self.chk_account.get_active()
                   and self._will_install_rootfs() and not removing)
        password_hash = ""
        if account:
            password = self.password_entry.get_text()
            password_hash = backend.hash_password(password)
        whole_text = self.whole_entry.get_text().strip()
        size_bytes = 0 if (whole or removing) else (
            int(self.size_spin.get_value()) * backend.GIB)
        return backend.InstallOptions(
            device=device,
            images=self._current_images(),
            size_bytes=size_bytes,
            flash_boot=(self.chk_flash.get_active() and not whole and not removing),
            flash_firmware=(self.chk_flash.get_active()
                            and self.chk_firmware.get_active()
                            and not whole and not removing),
            install_rootfs=(removing or whole or self.chk_rootfs.get_active()),
            switch_slot=(self.chk_switch.get_active() and not whole and not removing),
            reboot_after_switch=(self.chk_switch.get_active()
                                 and not whole and not removing),
            mode="whole" if whole else "dual",
            whole_output=(Path(whole_text).expanduser()
                          if whole and whole_text else None),
            source=source,
            image_url=(self.url_entry.get_text().strip() if source == "auto" else ""),
            image_sha256=(self.sha_entry.get_text().strip().lower()
                          if source == "auto" else ""),
            account_enabled=account,
            username=(self.username_entry.get_text().strip() if account else ""),
            password_hash=password_hash,
            existing_action=existing_action,
            remove_legacy=(removing and self.chk_remove_legacy.get_active()),
        )

    def _confirm_install(self) -> None:
        if self.state.installing:
            return
        problems = self._validate_install()
        if problems:
            self._alert("Cannot start the installation",
                        "\n".join(f"• {p}" for p in problems))
            return
        try:
            options = self._collect_options()
        except backend.InstallerError as exc:
            self._alert("Cannot start the installation", str(exc))
            return
        self._pending_options = options
        lines = [f"Device: {options.device.serial} ({options.device.mode})", ""]
        if options.existing_action == "remove":
            lines += [
                "Will remove the existing pmOS installation:",
                "  /data/pmos/rootfs.img and /data/pmos_install.env",
            ]
            if options.remove_legacy:
                lines.append("  /data/pmos_legacy/rootfs.img (legacy)")
            else:
                lines.append("  the legacy image, if any, is kept")
            lines += ["", "Android, slot A and slot B are not modified."]
            message = "Remove the pmOS installation?"
            button = "Remove"
        elif options.mode == "whole":
            lines += [
                "Will prepare a destructive whole-device package in:",
                f"  {options.whole_output}",
                "",
                "The rootfs is assembled in temporary encrypted staging under "
                "/data and removed afterwards. Android is not changed while the "
                "package is being prepared.",
                "",
                "Applying the package from recovery (with /data unmounted) will:",
                "  erase super, metadata, frp and userdata",
                "  write pmOS directly to userdata with dm-default-key",
                "  flash the included pmOS boot/init_boot/vbmeta to slots A and B",
                "  leave slot B active, preserve low-level firmware and calibration,",
                "    and never write misc",
                "",
                "The package needs a matching pmOS boot chain and the existing "
                "Android metadata key. It is not a secure erase.",
            ]
            message = "Prepare the destructive whole-device package?"
            button = "Prepare"
        else:
            if options.flash_boot:
                lines += [
                    "Will write slot B only:",
                    "  boot_b, init_boot_b, vbmeta_b, vbmeta_system_b, vbmeta_vendor_b",
                    "  vendor_boot_b, dtbo_b",
                ]
                if options.flash_firmware:
                    lines.append(
                        "  " + ", ".join(f"{part}_b"
                                         for part in backend.SLOT_B_FIRMWARE))
            if options.install_rootfs:
                lines += [
                    "Will create inside Android /data:",
                    f"  /data/pmos/rootfs.img ({options.size_gib} GiB)",
                    "  plus temporary files in /data/local/tmp",
                ]
                if options.existing_action == "replace":
                    lines.append("  the existing pmOS rootfs is replaced")
            if options.account_enabled:
                lines.append(f"pmOS login: {options.username} (hash only)")
            else:
                lines.append("pmOS login: image default (no account change)")
            if options.switch_slot:
                lines += ["", "Will set slot B active and reboot."]
            lines += ["", "Slot A (Android) and its system/vendor/boot are never written."]
            message = "Start the installation?"
            button = "Install"
        dialog = Gtk.AlertDialog()
        dialog.set_message(message)
        dialog.set_detail("\n".join(lines))
        dialog.set_buttons(["Cancel", button])
        dialog.set_cancel_button(0)
        dialog.set_default_button(1)
        dialog.choose(self, None, self._on_confirm_response)

    def _on_confirm_response(self, dialog: Gtk.AlertDialog, result) -> None:
        try:
            choice = dialog.choose_finish(result)
        except GLib.Error:
            return
        if choice == 1 and self._pending_options:
            options = self._pending_options
            self._pending_options = None
            self._start_install(options)

    def _validate_install(self) -> list[str]:
        problems: list[str] = []
        device = self.state.device
        if not device:
            problems.append("No device detected.")
            return problems
        if not device.is_target:
            problems.append(f"Device {device.codename or device.model} is not a TB351FU.")
        existing = self.state.existing
        whole = self._is_whole()
        removing = self._is_removing()
        auto = self._is_source_auto()
        if removing:
            if not existing.present:
                problems.append(
                    "No existing pmOS installation was detected; there is "
                    "nothing to remove.")
            return problems
        if whole:
            if device.mode != "adb":
                problems.append("Whole-device preparation requires Android booted with adb.")
            elif not device.root:
                problems.append("Whole-device preparation requires root adb.")
            images = self._current_images()
            missing = [name for name, path in (
                ("pmOS boot", images.boot),
                ("pmOS init_boot", images.init_boot),
                ("pmOS vbmeta", images.vbmeta),
            ) if not path or not Path(path).is_file()]
            if missing:
                problems.append(
                    "Whole-device mode needs a matching pmOS boot chain: "
                    + ", ".join(missing) + ".")
            output_text = self.whole_entry.get_text().strip()
            if not output_text:
                problems.append("Choose an output folder for the whole-device package.")
            else:
                output = Path(output_text).expanduser()
                if output.exists() and not output.is_dir():
                    problems.append(f"The whole-device output is not a folder: {output}")
                elif output.is_dir():
                    try:
                        if any(output.iterdir()):
                            problems.append(
                                f"The whole-device output folder is not empty: {output}")
                    except OSError as exc:
                        problems.append(f"Cannot read the output folder: {exc}")
        elif not self.chk_flash.get_active() and not self.chk_rootfs.get_active():
            problems.append("Select at least one action.")
        images = self._current_images()
        flash = (self.chk_flash.get_active() and not whole and not removing)
        if flash:
            for key, path, limit in (
                ("boot", images.boot, backend.BOOT_PART_SIZE),
                ("init_boot", images.init_boot, backend.INIT_BOOT_PART_SIZE),
                ("vbmeta", images.vbmeta, 1024 * 1024),
                ("vendor_boot", images.vendor_boot,
                 backend.VENDOR_BOOT_PART_SIZE),
                ("dtbo", images.dtbo, backend.DTBO_PART_SIZE),
            ):
                if not path:
                    problems.append(f"{key} image is not selected.")
                elif not path.is_file():
                    problems.append(f"{key} image does not exist: {path}")
                elif path.stat().st_size > limit:
                    problems.append(f"{key} image is too large for its partition.")
            if self.chk_firmware.get_active():
                missing = [part for part in backend.SLOT_B_FIRMWARE
                           if part not in images.slot_b_firmware]
                if missing:
                    problems.append(
                        "Slot-B firmware image(s) missing: "
                        + ", ".join(f"{part}.img" for part in missing))
        if flash and device.mode == "adb" and not device.root:
            problems.append(
                "Root access is required to write slot B over adb. Grant root "
                "access, or reboot into fastboot.")
        installing_rootfs = whole or self.chk_rootfs.get_active()
        if installing_rootfs:
            if device.mode != "adb":
                problems.append("RootFS installation requires Android booted with adb.")
            elif not device.root:
                problems.append("Root access is required for rootfs installation.")
            if auto:
                url = (self.url_entry.get_text().strip()
                       or backend.default_image_url())
                if not url:
                    problems.append("Enter the server image URL.")
                sha = self.sha_entry.get_text().strip()
                if sha and not re.fullmatch(r"[0-9a-fA-F]{64}", sha):
                    problems.append("The SHA-256 must be 64 hexadecimal characters.")
            else:
                if not images.rootfs:
                    problems.append("postmarketOS rootfs image is not selected.")
                elif not images.rootfs.is_file():
                    problems.append(f"rootfs image does not exist: {images.rootfs}")
            if not images.modules:
                problems.append("deploy/modules.tar.xz is missing.")
            if not images.firmware:
                problems.append("deploy/firmware.tar.xz is missing.")
            data = self.state.data
            size = int(self.size_spin.get_value()) * backend.GIB
            if not whole:
                if not data:
                    problems.append("Could not read free space on /data.")
                elif size > data.free - backend.GIB:
                    problems.append(
                        f"{size // backend.GIB} GiB would leave less than 1 GiB free on /data.")
                elif size < 2 * backend.GIB:
                    problems.append("RootFS size must be at least 2 GiB.")
        if self.chk_account.get_active() and installing_rootfs:
            username = self.username_entry.get_text().strip()
            if not username:
                problems.append("Enter a pmOS username.")
            else:
                problem = _username_problem(username)
                if problem:
                    problems.append(f"Username: {problem}")
            password = self.password_entry.get_text()
            if not password:
                problems.append("Enter a pmOS password.")
            elif password != self.confirm_entry.get_text():
                problems.append("The passwords do not match.")
        return problems

    def _start_install(self, options: Optional[backend.InstallOptions] = None) -> None:
        if options is None:
            try:
                options = self._collect_options()
            except backend.InstallerError as exc:
                self._alert("Cannot start the installation", str(exc))
                return
        self._options = options
        self.state.installing = True
        self.state.switched = False
        self._set_installing_ui(True)
        self.log_buffer.set_text("")
        self.progress.set_fraction(0.0)
        self.progress.set_text("0%")
        self.step_label.set_text("Starting…")
        self.install_status.set_text("")
        if options.existing_action == "remove":
            action = "Removing the pmOS installation"
        elif options.mode == "whole":
            action = "Preparing the whole-device package"
        else:
            action = "Installing postmarketOS"
        self._append_log(f"{action} on {options.device.serial}")
        self.password_entry.set_text("")
        self.confirm_entry.set_text("")
        self._install_thread = threading.Thread(
            target=self._install_worker, args=(options,), daemon=True)
        self._install_thread.start()

    def _install_worker(self, options: backend.InstallOptions) -> None:
        try:
            backend.install(options, self.queue.put)
        except Exception as exc:  # noqa: BLE001 - shown in the log
            self.queue.put(f"%%PMOS_FATAL:{exc}")
        finally:
            self.queue.put("%%PMOS_FINISHED")

    def _set_installing_ui(self, installing: bool) -> None:
        self.install_btn.set_sensitive(not installing)
        self.back_btn.set_sensitive(not installing)
        self.next_btn.set_sensitive(not installing)
        self.refresh_btn.set_sensitive(not installing)
        self.chk_flash.set_sensitive(not installing)
        self.chk_switch.set_sensitive(not installing)
        self.bundle_entry.set_sensitive(not installing)
        self.bundle_browse_btn.set_sensitive(not installing)
        self.bundle_use_btn.set_sensitive(not installing)
        if installing:
            for widget in (
                self.radio_replace, self.radio_remove, self.chk_remove_legacy,
                self.radio_dual, self.radio_whole, self.whole_entry,
                self.whole_browse_btn, self.radio_manual, self.radio_auto,
                self.url_entry, self.sha_entry, self.chk_account,
                self.username_entry, self.password_entry, self.confirm_entry,
                self.image_entries["rootfs"], self.rootfs_browse,
            ):
                widget.set_sensitive(False)
            self.chk_firmware.set_sensitive(False)
            self.chk_rootfs.set_sensitive(False)
        else:
            self._update_firmware_status()
            self._update_mode_controls()
            self._update_options_controls()
        self.size_spin.set_sensitive(
            not installing and self.state.data is not None
            and not self._is_whole() and not self._is_removing())

    def _drain_queue(self) -> bool:
        finished = False
        while True:
            try:
                line = self.queue.get_nowait()
            except queue.Empty:
                break
            if line == "%%PMOS_FINISHED":
                finished = True
                continue
            self._handle_line(line)
        if finished:
            self._install_finished()
        return GLib.SOURCE_CONTINUE

    def _handle_line(self, line: str) -> None:
        if line.startswith("%%PMOS_STEP:"):
            parts = line.split(":", 2)
            if len(parts) == 3:
                self.step_label.set_text(parts[2])
            return
        if line.startswith("%%PMOS_PROGRESS:"):
            parts = line.split(":", 2)
            if len(parts) == 3:
                try:
                    pct = max(0, min(100, int(parts[1])))
                except ValueError:
                    return
                self.progress.set_fraction(pct / 100.0)
                self.progress.set_text(f"{pct}%")
                if parts[2]:
                    self.step_label.set_text(parts[2])
            return
        if line.startswith("%%PMOS_ERROR:"):
            self._append_log(f"Script reported error code {line.rsplit(':', 1)[-1]}")
            return
        if line.startswith("%%PMOS_FATAL:"):
            self._install_failed(line.split(":", 1)[1])
            return
        if line.startswith("%%PMOS_DONE"):
            self._install_succeeded()
            return
        if line == "%%TB_SWITCHED":
            self._append_log("Slot B is active. The tablet is rebooting.")
            self.finish_switch_btn.set_sensitive(True)
            return
        if line.startswith("%%TB_SWITCH_FAILED:"):
            self.finish_switch_btn.set_sensitive(True)
            self._alert("Could not switch slot", line.split(":", 1)[1])
            return
        self._append_log(line)

    def _append_log(self, text: str) -> None:
        if not text:
            return
        end = self.log_buffer.get_end_iter()
        self.log_buffer.insert(end, text + "\n")
        if self.log_buffer.get_line_count() > 4000:
            start = self.log_buffer.get_start_iter()
            cut = self.log_buffer.get_iter_at_line(500)
            self.log_buffer.delete(start, cut)
        mark = self.log_buffer.create_mark(None, self.log_buffer.get_end_iter(), False)
        self.log_view.scroll_to_mark(mark, 0.0, True, 0.0, 1.0)

    def _install_succeeded(self) -> None:
        if not self.state.installing:
            return
        self.state.installing = False
        options = self._options
        removed = bool(options and options.existing_action == "remove")
        whole = bool(options and options.mode == "whole")
        switched = bool(options and options.switch_slot
                        and not whole and not removed)
        self.state.switched = switched
        self.progress.set_fraction(1.0)
        self.progress.set_text("100%")
        if removed:
            self.step_label.set_text("Removal complete")
            self.install_status.set_markup(
                "<span foreground='#26a269'><b>Existing pmOS installation "
                "removed.</b></span> Android was not modified.")
        elif whole:
            self.step_label.set_text("Whole-device package ready")
            output = (GLib.markup_escape_text(str(options.whole_output))
                      if options and options.whole_output else "the output folder")
            self.install_status.set_markup(
                "<span foreground='#26a269'><b>Destructive whole-device package "
                f"prepared in {output}.</b></span> Android was not changed while "
                "preparing it. Apply the package only from recovery with /data "
                "unmounted; it erases Android and replaces the boot chain in "
                "both slots.")
        else:
            self.step_label.set_text("Installation complete")
            self.install_status.set_markup(
                "<span foreground='#26a269'><b>Installation complete.</b></span>"
                + (" The tablet is rebooting into postmarketOS."
                   if switched else ""))
        self._set_installing_ui(False)
        self.finish_switch_btn.set_visible(not switched and not removed and not whole)
        if switched:
            self.finish_switch_btn.set_visible(False)
        if removed or whole:
            # Re-read /data so the existing-install controls match reality.
            GLib.idle_add(self.detect_async)

    def _install_failed(self, message: str) -> None:
        if not self.state.installing:
            return
        self.state.installing = False
        options = self._options
        if options and options.existing_action == "remove":
            action = "Removal"
        elif options and options.mode == "whole":
            action = "Whole-device preparation"
        else:
            action = "Installation"
        self.progress.set_fraction(1.0)
        self.progress.set_text("Failed")
        self.step_label.set_text(f"{action} failed")
        self.install_status.set_markup(
            f"<span foreground='#c01c28'><b>{action} failed:</b></span> "
            + GLib.markup_escape_text(message))
        self._append_log(f"ERROR: {message}")
        self._set_installing_ui(False)

    def _install_finished(self) -> None:
        # The final state is normally set by DONE or FATAL.  If neither was
        # seen (e.g. the helper crashed), surface a failure rather than
        # leaving the UI stuck.
        if self.state.installing:
            self._install_failed("the installer stopped without reporting completion")

    def _finish_switch(self) -> None:
        device = self.state.device
        if not device:
            return
        dialog = Gtk.AlertDialog()
        dialog.set_message("Switch to slot B and reboot?")
        dialog.set_detail("The tablet will reboot into postmarketOS.")
        dialog.set_buttons(["Cancel", "Switch and reboot"])
        dialog.set_cancel_button(0)
        dialog.set_default_button(1)
        dialog.choose(self, None, self._on_switch_response)

    def _on_switch_response(self, dialog: Gtk.AlertDialog, result) -> None:
        try:
            choice = dialog.choose_finish(result)
        except GLib.Error:
            return
        if choice != 1:
            return
        device = self.state.device
        if not device:
            return
        self.finish_switch_btn.set_sensitive(False)
        self._append_log("Switching slot B active and rebooting…")
        threading.Thread(target=self._switch_worker, args=(device,),
                         daemon=True).start()

    def _switch_worker(self, device: backend.Device) -> None:
        try:
            backend.switch_to_slot_b(device, self.queue.put, reboot=True)
            self.queue.put("%%TB_SWITCHED")
        except Exception as exc:  # noqa: BLE001 - shown to the user
            self.queue.put(f"%%TB_SWITCH_FAILED:{exc}")

    # ------------------------------------------------------------------
    # misc
    # ------------------------------------------------------------------

    def _reboot(self, target: str) -> None:
        device = self.state.device
        if not device:
            self._alert("No device", "Detect a device first.")
            return
        try:
            if target == "bootloader":
                backend.reboot_to_bootloader(device)
            else:
                backend.reboot_device(device)
        except Exception as exc:  # noqa: BLE001
            self._alert("Reboot failed", str(exc))

    def _alert(self, message: str, detail: str) -> None:
        dialog = Gtk.AlertDialog()
        dialog.set_message(message)
        dialog.set_detail(detail)
        dialog.set_buttons(["OK"])
        dialog.set_cancel_button(0)
        dialog.set_default_button(0)
        dialog.choose(self, None, lambda *_: None)

    def _on_close(self, *_args) -> bool:
        if self.state.installing:
            self._alert("Installation in progress",
                        "Wait for the current step to finish before closing the installer.")
            return True
        return False


class InstallerApp(Gtk.Application):
    def __init__(self, smoke_test: bool = False) -> None:
        super().__init__(application_id=APP_ID,
                         flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.window: Optional[InstallerWindow] = None
        self.smoke_test = smoke_test

    def do_activate(self) -> None:
        if not self.window:
            self.window = InstallerWindow(self)
        if self.smoke_test:
            # Build every page and run detection, but never map the window.
            # This validates the widget tree on a headless CI/desktop.
            GLib.timeout_add(1200, self.quit)
            return
        self.window.present()


def main(argv: Optional[list[str]] = None, smoke_test: bool = False) -> int:
    app = InstallerApp(smoke_test=smoke_test)
    return app.run(argv if argv is not None else [])
