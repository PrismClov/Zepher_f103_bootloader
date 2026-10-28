#!/usr/bin/env python3
"""Windows GUI host for the STM32F103 USB IAP bootloader."""

from __future__ import annotations

import binascii
import ctypes
import queue
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import hid

from f103_uploader import (
    PID,
    SLOT_SIZE,
    USAGE_PAGE,
    VID,
    UploadError,
    perform_update,
    validate_image,
)


APP_TITLE = "F103 USB 固件升级工具"
ACCENT = "#167D68"
ACCENT_ACTIVE = "#116754"
BACKGROUND = "#F2F5F7"
CARD = "#FFFFFF"
TEXT = "#17212B"
MUTED = "#687784"
BORDER = "#DCE4E8"
SUCCESS = "#16835B"
WARNING = "#C47A18"
ERROR = "#C13C3C"
LOG_BG = "#101820"
LOG_FG = "#D5E3EA"


def application_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


class F103UpdaterApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("820x650")
        self.root.minsize(760, 600)
        self.root.configure(background=BACKGROUND)

        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.firmware_paths: dict[int, Path] = {}
        self.firmware_images: dict[int, bytes] = {}
        self.busy = False
        self.closed = False

        self._configure_style()
        self._build_ui()
        self._auto_select_firmware()
        self._poll_device()
        self.root.after(80, self._drain_events)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _configure_style(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")

        style.configure("App.TFrame", background=BACKGROUND)
        style.configure("Card.TFrame", background=CARD)
        style.configure(
            "Title.TLabel",
            background=BACKGROUND,
            foreground=TEXT,
            font=("Microsoft YaHei UI", 20, "bold"),
        )
        style.configure(
            "Subtitle.TLabel",
            background=BACKGROUND,
            foreground=MUTED,
            font=("Microsoft YaHei UI", 9),
        )
        style.configure(
            "Section.TLabel",
            background=CARD,
            foreground=TEXT,
            font=("Microsoft YaHei UI", 10, "bold"),
        )
        style.configure(
            "Body.TLabel",
            background=CARD,
            foreground=TEXT,
            font=("Microsoft YaHei UI", 9),
        )
        style.configure(
            "Muted.TLabel",
            background=CARD,
            foreground=MUTED,
            font=("Microsoft YaHei UI", 9),
        )
        style.configure(
            "Status.TLabel",
            background=CARD,
            foreground=TEXT,
            font=("Microsoft YaHei UI", 12, "bold"),
        )
        style.configure(
            "Accent.TButton",
            background=ACCENT,
            foreground="white",
            borderwidth=0,
            padding=(18, 11),
            font=("Microsoft YaHei UI", 10, "bold"),
        )
        style.map(
            "Accent.TButton",
            background=[("active", ACCENT_ACTIVE), ("disabled", "#9AB6AF")],
            foreground=[("disabled", "#EDF3F1")],
        )
        style.configure(
            "Secondary.TButton",
            background="#E8EFF2",
            foreground=TEXT,
            borderwidth=0,
            padding=(14, 9),
            font=("Microsoft YaHei UI", 9),
        )
        style.map("Secondary.TButton", background=[("active", "#D7E2E7")])
        style.configure(
            "Upgrade.Horizontal.TProgressbar",
            troughcolor="#E5ECEF",
            background=ACCENT,
            lightcolor=ACCENT,
            darkcolor=ACCENT,
            bordercolor="#E5ECEF",
            thickness=10,
        )
        style.configure(
            "TEntry",
            fieldbackground="#F8FAFB",
            foreground=TEXT,
            bordercolor=BORDER,
            lightcolor=BORDER,
            darkcolor=BORDER,
            padding=8,
            font=("Consolas", 9),
        )

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, style="App.TFrame", padding=(24, 20, 24, 18))
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)

        ttk.Label(outer, text=APP_TITLE, style="Title.TLabel").grid(
            row=0, column=0, sticky="w"
        )
        ttk.Label(
            outer,
            text="STM32F103CBT6 · USB HID IAP · 支持应用运行中直接升级",
            style="Subtitle.TLabel",
        ).grid(row=1, column=0, sticky="w", pady=(2, 16))

        status_card = tk.Frame(
            outer, background=CARD, highlightbackground=BORDER,
            highlightthickness=1, padx=18, pady=14
        )
        status_card.grid(row=2, column=0, sticky="ew")
        status_card.columnconfigure(1, weight=1)

        self.status_dot = tk.Label(
            status_card, text="●", background=CARD, foreground=MUTED,
            font=("Segoe UI Symbol", 22)
        )
        self.status_dot.grid(row=0, column=0, rowspan=2, padx=(0, 12))

        self.device_title = tk.Label(
            status_card, text="正在检测设备", background=CARD,
            foreground=TEXT, anchor="w",
            font=("Microsoft YaHei UI", 11, "bold")
        )
        self.device_title.grid(row=0, column=1, sticky="ew")

        self.device_detail = tk.Label(
            status_card, text="1209:F103 · Usage Page 0xFF00",
            background=CARD, foreground=MUTED, anchor="w",
            font=("Microsoft YaHei UI", 9)
        )
        self.device_detail.grid(row=1, column=1, sticky="ew", pady=(2, 0))

        ttk.Button(
            status_card, text="刷新", style="Secondary.TButton",
            command=self._poll_device
        ).grid(row=0, column=2, rowspan=2, padx=(12, 0))

        self._card_spacing(outer, 3)

        firmware_card = tk.Frame(
            outer, background=CARD, highlightbackground=BORDER,
            highlightthickness=1, padx=18, pady=14
        )
        firmware_card.grid(row=4, column=0, sticky="ew")
        firmware_card.columnconfigure(0, weight=1)

        ttk.Label(firmware_card, text="固件文件", style="Section.TLabel").grid(
            row=0, column=0, columnspan=2, sticky="w"
        )
        self.file_vars = {
            0: tk.StringVar(value="请选择 Slot A 固件"),
            1: tk.StringVar(value="请选择 Slot B 固件"),
        }
        ttk.Entry(
            firmware_card, textvariable=self.file_vars[0], state="readonly"
        ).grid(row=1, column=0, sticky="ew", pady=(9, 0), padx=(0, 8))
        ttk.Button(
            firmware_card, text="选择 A", style="Secondary.TButton",
            command=lambda: self._browse_slot(0)
        ).grid(row=1, column=1, sticky="e", pady=(9, 0))

        ttk.Entry(
            firmware_card, textvariable=self.file_vars[1], state="readonly"
        ).grid(row=2, column=0, sticky="ew", pady=(7, 0), padx=(0, 8))
        ttk.Button(
            firmware_card, text="选择 B", style="Secondary.TButton",
            command=lambda: self._browse_slot(1)
        ).grid(row=2, column=1, sticky="e", pady=(7, 0))

        self.file_info = ttk.Label(
            firmware_card, text=f"每槽最大允许 {SLOT_SIZE:,} 字节",
            style="Muted.TLabel"
        )
        self.file_info.grid(row=3, column=0, columnspan=2, sticky="w", pady=(7, 0))

        self._card_spacing(outer, 5)

        progress_card = tk.Frame(
            outer, background=CARD, highlightbackground=BORDER,
            highlightthickness=1, padx=18, pady=14
        )
        progress_card.grid(row=6, column=0, sticky="ew")
        progress_card.columnconfigure(0, weight=1)

        self.action_text = tk.StringVar(value="准备就绪")
        tk.Label(
            progress_card, textvariable=self.action_text, background=CARD,
            foreground=TEXT, anchor="w",
            font=("Microsoft YaHei UI", 10, "bold")
        ).grid(row=0, column=0, sticky="w")
        self.percent_text = tk.StringVar(value="0%")
        tk.Label(
            progress_card, textvariable=self.percent_text, background=CARD,
            foreground=ACCENT, anchor="e",
            font=("Consolas", 10, "bold")
        ).grid(row=0, column=1, sticky="e")

        self.progress = ttk.Progressbar(
            progress_card, style="Upgrade.Horizontal.TProgressbar",
            mode="determinate", maximum=100
        )
        self.progress.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(10, 12))

        self.upgrade_button = ttk.Button(
            progress_card, text="开始升级", style="Accent.TButton",
            command=self._start_update
        )
        self.upgrade_button.grid(row=2, column=0, columnspan=2, sticky="ew")

        self._card_spacing(outer, 7)

        log_header = tk.Frame(outer, background=BACKGROUND)
        log_header.grid(row=8, column=0, sticky="ew")
        log_header.columnconfigure(0, weight=1)
        ttk.Label(log_header, text="运行日志", style="Subtitle.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 6)
        )
        ttk.Button(
            log_header, text="清空", style="Secondary.TButton",
            command=self._clear_log
        ).grid(row=0, column=1, sticky="e", pady=(0, 6))

        log_frame = tk.Frame(outer, background=LOG_BG)
        log_frame.grid(row=9, column=0, sticky="nsew")
        outer.rowconfigure(9, weight=1)

        self.log = tk.Text(
            log_frame, height=10, wrap="word", relief="flat",
            background=LOG_BG, foreground=LOG_FG,
            insertbackground=LOG_FG, font=("Consolas", 9),
            padx=12, pady=10, state="disabled"
        )
        self.log.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(
            log_frame, orient="vertical", command=self.log.yview
        )
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=scrollbar.set)
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self._append_log("上位机已启动")
        self._append_log("请连接设备并选择固件；应用运行中也可直接升级")

    @staticmethod
    def _card_spacing(parent: ttk.Frame, row: int) -> None:
        tk.Frame(parent, background=BACKGROUND, height=12).grid(
            row=row, column=0, sticky="ew"
        )

    def _auto_select_firmware(self) -> None:
        for candidate in (
            application_root() / "app_template_slot_a.bin",
            application_root() / "dist" / "app_template_slot_a.bin",
        ):
            if candidate.is_file():
                counterpart = self._counterpart(candidate, 0)
                if counterpart is not None and counterpart.is_file():
                    self._set_firmware_pair(candidate, counterpart, quiet=True)
                return

    @staticmethod
    def _counterpart(path: Path, slot: int) -> Path | None:
        other = 1 - slot
        stem = path.stem
        lower = stem.lower()
        if "_slot_a" in lower:
            start = lower.index("_slot_a")
            name = stem[:start] + f"_slot_{'ab'[other]}" + stem[start + 7:]
            return path.with_name(name + path.suffix)
        if "_slot_b" in lower:
            start = lower.index("_slot_b")
            name = stem[:start] + f"_slot_{'ab'[other]}" + stem[start + 7:]
            return path.with_name(name + path.suffix)
        if lower.endswith("_a"):
            return path.with_name(stem[:-2] + f"_{'ab'[other]}" + path.suffix)
        if lower.endswith("_b"):
            return path.with_name(stem[:-2] + f"_{'ab'[other]}" + path.suffix)
        return path.with_name(f"{stem}_slot_{'ab'[other]}{path.suffix}")

    def _browse_slot(self, slot: int) -> None:
        selected = filedialog.askopenfilename(
            title=f"选择 Slot {'AB'[slot]} 固件",
            filetypes=[("Raw binary", "*.bin"), ("All files", "*.*")],
        )
        if selected:
            self._set_firmware(Path(selected), slot)

    def _set_firmware(self, path: Path, requested_slot: int,
                      quiet: bool = False) -> None:
        try:
            data = path.read_bytes()
            actual_slot = validate_image(data)
        except (OSError, UploadError) as exc:
            self.file_info.configure(text=f"文件错误：{exc}", foreground=ERROR)
            if not quiet:
                messagebox.showerror("固件无效", str(exc))
            return

        if actual_slot != requested_slot:
            self.file_info.configure(
                text=f"该文件实际链接到 Slot {'AB'[actual_slot]}",
                foreground=ERROR,
            )
            if not quiet:
                messagebox.showerror(
                    "槽位不匹配",
                    f"该固件链接到 Slot {'AB'[actual_slot]}，"
                    f"但选择的是 Slot {'AB'[requested_slot]}。",
                )
            return

        counterpart = self._counterpart(path, actual_slot)
        if counterpart is None or not counterpart.is_file():
            self.firmware_paths[actual_slot] = path
            self.firmware_images[actual_slot] = data
            self._refresh_firmware_display()
            self.file_info.configure(
                text=f"已选择 Slot {'AB'[actual_slot]}，"
                     f"未找到 Slot {'AB'[1 - actual_slot]} 配套文件",
                foreground=WARNING,
            )
            self._append_log(f"已选择 Slot {'AB'[actual_slot]}：{path.name}")
            return

        self._set_firmware_pair(
            path if actual_slot == 0 else counterpart,
            path if actual_slot == 1 else counterpart,
            quiet=quiet,
        )

    def _set_firmware_pair(self, path_a: Path, path_b: Path,
                           quiet: bool = False) -> None:
        try:
            data_a = path_a.read_bytes()
            data_b = path_b.read_bytes()
            validate_image(data_a, 0)
            validate_image(data_b, 1)
        except (OSError, UploadError) as exc:
            self.file_info.configure(text=f"固件对错误：{exc}", foreground=ERROR)
            if not quiet:
                messagebox.showerror("A/B 固件无效", str(exc))
            return

        self.firmware_paths = {0: path_a, 1: path_b}
        self.firmware_images = {0: data_a, 1: data_b}
        self._refresh_firmware_display()
        self._append_log(f"已选择 A/B 固件对：{path_a.name} + {path_b.name}")

    def _refresh_firmware_display(self) -> None:
        details = []
        for slot in (0, 1):
            path = self.firmware_paths.get(slot)
            data = self.firmware_images.get(slot)
            self.file_vars[slot].set(str(path) if path else f"请选择 Slot {'AB'[slot]} 固件")
            if data is not None:
                crc = binascii.crc32(data) & 0xFFFFFFFF
                details.append(
                    f"{'AB'[slot]}: {len(data):,} B / CRC 0x{crc:08X}"
                )
        if len(details) == 2:
            self.file_info.configure(
                text="  ·  ".join(details), foreground=MUTED
            )

    def _poll_device(self) -> None:
        if self.closed:
            return

        try:
            devices = [
                item for item in hid.enumerate(VID, PID)
                if item.get("usage_page", 0) == USAGE_PAGE
            ]
        except OSError:
            devices = []

        if not devices:
            if not self.busy:
                self._set_device_status(
                    MUTED, "设备未连接", "请连接 F103 USB 设备"
                )
        else:
            info = devices[0]
            product = info.get("product_string") or "F103 USB IAP Device"
            if "Application" in product:
                detail = "应用运行中 · 点击开始升级将自动切换到 Bootloader"
                color = SUCCESS
            elif "Bootloader" in product:
                detail = "Bootloader 已就绪 · 可直接下载固件"
                color = WARNING
            else:
                detail = "设备已连接"
                color = SUCCESS
            self._set_device_status(color, product, detail)

        self.root.after(1000, self._poll_device)

    def _set_device_status(self, color: str, title: str, detail: str) -> None:
        self.status_dot.configure(foreground=color)
        self.device_title.configure(text=title)
        self.device_detail.configure(text=detail)

    def _start_update(self) -> None:
        if self.busy:
            return
        for slot in (0, 1):
            if slot not in self.firmware_images:
                self._browse_slot(slot)
                if slot not in self.firmware_images:
                    return
        if len(self.firmware_images) != 2:
            if not self.firmware_images:
                return

        self.busy = True
        self.upgrade_button.configure(state="disabled")
        self.progress["value"] = 0
        self.percent_text.set("0%")
        self.action_text.set("正在连接设备...")
        self._append_log("")
        self._append_log("开始升级")

        data = dict(self.firmware_images)
        thread = threading.Thread(
            target=self._update_worker, args=(data,), daemon=True
        )
        thread.start()

    def _update_worker(self, data: bytes) -> None:
        try:
            perform_update(
                data,
                wait=15.0,
                run=True,
                log_cb=lambda message: self.events.put(("log", message)),
                progress_cb=lambda value: self.events.put(("progress", value)),
            )
            self.events.put(("success", None))
        except Exception as exc:
            self.events.put(("error", str(exc)))

    def _drain_events(self) -> None:
        if self.closed:
            return

        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "progress":
                    value = int(payload)
                    self.progress["value"] = value
                    self.percent_text.set(f"{value}%")
                    self.action_text.set("正在写入并校验固件...")
                elif kind == "success":
                    self._finish_update(True, "升级完成，应用正在启动")
                elif kind == "error":
                    self._finish_update(False, str(payload))
        except queue.Empty:
            pass

        self.root.after(80, self._drain_events)

    def _finish_update(self, success: bool, message: str) -> None:
        self.busy = False
        self.upgrade_button.configure(state="normal")
        if success:
            self.progress["value"] = 100
            self.percent_text.set("100%")
            self.action_text.set("升级成功")
            self._set_device_status(SUCCESS, "升级完成", "应用已重新启动")
            self._append_log(message)
        else:
            self.action_text.set("升级失败")
            self._set_device_status(ERROR, "升级失败", message)
            self._append_log(f"错误：{message}")
            messagebox.showerror("升级失败", message)

    def _append_log(self, message: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", f"{message}\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def _close(self) -> None:
        if self.busy:
            should_close = messagebox.askyesno(
                "升级进行中",
                "升级尚未完成，确定要退出吗？",
                icon="warning",
            )
            if not should_close:
                return
        self.closed = True
        self.root.destroy()


def main() -> None:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass

    root = tk.Tk()
    F103UpdaterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

