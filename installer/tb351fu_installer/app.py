# SPDX-License-Identifier: GPL-2.0
"""GTK4 installer for postmarketOS on the Lenovo Tab Plus (TB351FU)."""

from __future__ import annotations

import dataclasses
import queue
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


@dataclasses.dataclass
class UiState:
    device: Optional[backend.Device] = None
    devices: list[backend.Device] = dataclasses.field(default_factory=list)
    partitions: list[backend.Partition] = dataclasses.field(default_factory=list)
    data: Optional[backend.DataSpace] = None
    images: backend.ImageSet = dataclasses.field(default_factory=backend.ImageSet)
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
        self.install_page = self._build_install_page()

        self.stack.add_named(self._scrollable(self.device_page), "device")
        self.stack.add_named(self._scrollable(self.partitions_page), "partitions")
        self.stack.add_named(self._scrollable(self.images_page), "images")
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

    def _build_install_page(self) -> Gtk.Widget:
        page = self._page()
        page.append(_label("Install", css="page-title"))
        page.append(_label(
            "Review the plan, then start. Slot A is never written.", wrap=True,
            css="dim"))

        summary = Gtk.Grid(column_spacing=18, row_spacing=8)
        self.summary_values: dict[str, Gtk.Label] = {}
        for row, name in enumerate(["Device", "RootFS image", "RootFS size",
                                    "Boot chain", "Switch slot", "Free after"]):
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
        return ["device", "partitions", "images", "install"]

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
            if self.state.device and self.state.device.mode == "adb" and not self.state.data:
                self._alert("Could not read /data",
                            "Rootfs size cannot be validated without free space information.")
                return
            if self.state.data:
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
            if devices:
                device = next((d for d in devices if d.is_target), devices[0])
                partitions = backend.list_partitions(device)
                space = backend.get_data_space(device)
            images = backend.find_images()
            GLib.idle_add(self._detect_done, devices, device, partitions, space,
                          images, None)
        except Exception as exc:  # noqa: BLE001 - shown to the user
            GLib.idle_add(self._detect_done, [], None, [], None,
                          backend.find_images(), str(exc))

    def _detect_done(self, devices, device, partitions, space, images, error) -> bool:
        self.device_spinner.stop()
        self.refresh_btn.set_sensitive(True)
        self.state.devices = devices or []
        self.state.device = device
        self.state.partitions = partitions or []
        self.state.data = space
        self.state.images = images
        self._render_device(error)
        self._render_partitions()
        self._apply_images(images)
        self._update_mode_controls()
        self._update_size_controls()
        self._on_images_changed()
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
        return backend.validate_images(
            images,
            need_rootfs=self.chk_rootfs.get_active(),
            need_boot=self.chk_flash.get_active(),
            need_firmware=(self.chk_flash.get_active()
                           and self.chk_firmware.get_active()),
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
        self.summary_values["Device"].set_text(
            f"{device.serial} ({device.mode}, slot {device.slot or '?'})"
            if device else "—")
        self.summary_values["RootFS image"].set_text(
            str(images.rootfs) if images.rootfs else "Not selected")
        self.summary_values["RootFS size"].set_text(
            f"{size_gib} GiB" if self.chk_rootfs.get_active() else "Skipped")
        self.summary_values["Boot chain"].set_text(
            "boot_b + init_boot_b + vbmeta_b + vendor_boot_b + dtbo_b"
            + (f" + {len(images.slot_b_firmware)} firmware images"
               if self.chk_firmware.get_active() else "")
            if self.chk_flash.get_active() else "Skipped")
        self.summary_values["Switch slot"].set_text(
            "Yes, set slot B active and reboot" if self.chk_switch.get_active() else "No")
        if data and self.chk_rootfs.get_active():
            left = data.free - size_gib * backend.GIB
            self.summary_values["Free after"].set_text(
                backend.human_size(max(0, left)))
        else:
            self.summary_values["Free after"].set_text("—")

    def _collect_options(self) -> backend.InstallOptions:
        device = self.state.device
        if not device:
            raise backend.InstallerError("no device detected")
        return backend.InstallOptions(
            device=device,
            images=self._current_images(),
            size_bytes=int(self.size_spin.get_value()) * backend.GIB,
            flash_boot=self.chk_flash.get_active(),
            flash_firmware=(self.chk_flash.get_active()
                            and self.chk_firmware.get_active()),
            install_rootfs=self.chk_rootfs.get_active(),
            switch_slot=self.chk_switch.get_active(),
            reboot_after_switch=self.chk_switch.get_active(),
        )

    def _confirm_install(self) -> None:
        if self.state.installing:
            return
        problems = self._validate_install()
        if problems:
            self._alert("Cannot start the installation",
                        "\n".join(f"• {p}" for p in problems))
            return
        options = self._collect_options()
        lines = [
            f"Device: {options.device.serial} ({options.device.mode})",
            "",
        ]
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
        if options.switch_slot:
            lines += ["", "Will set slot B active and reboot."]
        lines += ["", "Slot A (Android) and its system/vendor/boot are never written."]
        dialog = Gtk.AlertDialog()
        dialog.set_message("Start the installation?")
        dialog.set_detail("\n".join(lines))
        dialog.set_buttons(["Cancel", "Install"])
        dialog.set_cancel_button(0)
        dialog.set_default_button(1)
        dialog.choose(self, None, self._on_confirm_response)

    def _on_confirm_response(self, dialog: Gtk.AlertDialog, result) -> None:
        try:
            choice = dialog.choose_finish(result)
        except GLib.Error:
            return
        if choice == 1:
            self._start_install()

    def _validate_install(self) -> list[str]:
        problems: list[str] = []
        device = self.state.device
        if not device:
            problems.append("No device detected.")
            return problems
        if not device.is_target:
            problems.append(f"Device {device.codename or device.model} is not a TB351FU.")
        if not self.chk_flash.get_active() and not self.chk_rootfs.get_active():
            problems.append("Select at least one action.")
        images = self._current_images()
        if self.chk_flash.get_active():
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
        if self.chk_flash.get_active() and device.mode == "adb" and not device.root:
            problems.append(
                "Root access is required to write slot B over adb. Grant root "
                "access, or reboot into fastboot.")
        if self.chk_rootfs.get_active():
            if device.mode != "adb":
                problems.append("RootFS installation requires Android booted with adb.")
            elif not device.root:
                problems.append("Root access is required for rootfs installation.")
            if not images.rootfs:
                problems.append("postmarketOS rootfs image is not selected.")
            elif not images.rootfs.is_file():
                problems.append(f"rootfs image does not exist: {images.rootfs}")
            if not images.modules:
                problems.append("deploy/modules.tar.xz is missing.")
            data = self.state.data
            size = int(self.size_spin.get_value()) * backend.GIB
            if not data:
                problems.append("Could not read free space on /data.")
            elif size > data.free - backend.GIB:
                problems.append(
                    f"{size // backend.GIB} GiB would leave less than 1 GiB free on /data.")
            elif size < 2 * backend.GIB:
                problems.append("RootFS size must be at least 2 GiB.")
        return problems

    def _start_install(self) -> None:
        options = self._collect_options()
        self._options = options
        self.state.installing = True
        self.state.switched = False
        self._set_installing_ui(True)
        self.log_buffer.set_text("")
        self.progress.set_fraction(0.0)
        self.progress.set_text("0%")
        self.step_label.set_text("Starting…")
        self.install_status.set_text("")
        self._append_log(f"Installing postmarketOS on {options.device.serial}")
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
            self.chk_firmware.set_sensitive(False)
        else:
            self._update_firmware_status()
        if installing:
            self.chk_rootfs.set_sensitive(False)
        else:
            self._update_mode_controls()
        self.size_spin.set_sensitive(not installing and self.state.data is not None)

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
        self.state.switched = bool(self._options and self._options.switch_slot)
        self.progress.set_fraction(1.0)
        self.progress.set_text("100%")
        self.step_label.set_text("Installation complete")
        self.install_status.set_markup(
            "<span foreground='#26a269'><b>Installation complete.</b></span>"
            + (" The tablet is rebooting into postmarketOS."
               if self.state.switched else ""))
        self._set_installing_ui(False)
        self.finish_switch_btn.set_visible(not self.state.switched)
        if self.state.switched:
            self.finish_switch_btn.set_visible(False)

    def _install_failed(self, message: str) -> None:
        if not self.state.installing:
            return
        self.state.installing = False
        self.progress.set_fraction(1.0)
        self.progress.set_text("Failed")
        self.step_label.set_text("Installation failed")
        self.install_status.set_markup(
            f"<span foreground='#c01c28'><b>Installation failed:</b></span> {GLib.markup_escape_text(message)}")
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
