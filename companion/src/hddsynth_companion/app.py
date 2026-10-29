"""The tray (menu bar) app: finds the HDD Synth, sends it the disk activity, and offers
its pack, volume and balance."""

from __future__ import annotations

import math
import sys
import time
from typing import Optional

from PySide6.QtCore import QLockFile, QObject, QPointF, QRectF, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QAction, QActionGroup, QColor, QIcon, QPainter, QPen, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QMessageBox,
    QSlider,
    QSystemTrayIcon,
    QVBoxLayout,
    QWidget,
)

from . import __version__, autostart, config, device, disks, protocol, supercap
from .about import AboutDialog
from .activity import MAX_THRESHOLD_KB, Activity, ActivityDetector, LinkSender

POLL_MS = 20
RECONNECT_MS = 2000
HANDSHAKE_MS = 3000        # a port must answer $HDQ STATUS this soon to count as an HDD Synth
NOT_A_SYNTH_RETRY_S = 60   # then leave that port alone this long (another Pico, say)
STATUS_REFRESH_MS = 5000   # the supercap voltage isn't pushed on change
SLIDER_THROTTLE_MS = 120   # while dragging, send at most this often

ORANGE = QColor(255, 120, 0)


def _drive_icon(led: Optional[QColor], dimmed: bool = False) -> QIcon:
    """A little 3.5" drive with its activity LED."""
    pm = QPixmap(64, 64)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    body = QColor(150, 150, 150) if dimmed else QColor(70, 70, 75)
    p.setPen(QPen(QColor(30, 30, 30), 3))
    p.setBrush(body)
    p.drawRoundedRect(QRectF(6, 14, 52, 36), 6, 6)
    p.setPen(QPen(QColor(200, 200, 200) if not dimmed else QColor(220, 220, 220), 3))
    p.drawLine(QPointF(14, 40), QPointF(38, 40))
    if led is not None:
        p.setPen(Qt.NoPen)
        p.setBrush(led)
        p.drawEllipse(QPointF(48, 40), 6, 6)
    p.end()
    return QIcon(pm)


def _bring_to_front(w: QWidget) -> None:
    w.show()
    w.raise_()
    w.activateWindow()
    if sys.platform == "darwin":
        try:
            from AppKit import NSApplication

            NSApplication.sharedApplication().activateIgnoringOtherApps_(True)
        except ImportError:
            pass


def _hide_dock_icon() -> None:
    """macOS: a menu bar app, without a Dock icon (needs pyobjc, installed on macOS)."""
    if sys.platform != "darwin":
        return
    try:
        from AppKit import NSApplication, NSApplicationActivationPolicyAccessory

        NSApplication.sharedApplication().setActivationPolicy_(NSApplicationActivationPolicyAccessory)
    except ImportError:
        pass


class _Bridge(QObject):
    """DeviceLink calls back from its reader thread; signals move that to the GUI thread."""

    reply = Signal(object)
    closed = Signal()


class ActivityMeter(QWidget):
    """How busy the disk is, as a bar (log scale), with the sensitivity threshold as a red
    line across it: orange when activity crosses the line and counts.

    The scale matches the sensitivity curve, so the line sits at (100 - sensitivity)%
    of the width: right under the handle of the (inverted) sensitivity slider above it.
    The bar holds its peaks briefly (falling ~3 dB a tick) so it doesn't flicker.
    """

    DECAY = 0.7

    def __init__(self) -> None:
        super().__init__()
        self.setFixedHeight(16)
        self.setMinimumWidth(120)
        self._level = 0.0  # 0..1 along the bar
        self._threshold = 0.0
        self._busy = False

    def sizeHint(self) -> QSize:
        return QSize(260, 16)

    @staticmethod
    def _fraction(kb: float) -> float:
        return min(1.0, math.log10(1 + max(0.0, kb)) / math.log10(1 + MAX_THRESHOLD_KB))

    def show_activity(self, a: Optional[Activity]) -> None:
        if a is None:
            return
        self._level = max(self._fraction(a.window_kb), self._level * self.DECAY)
        self._threshold = self._fraction(a.threshold_kb)
        self._busy = a.busy
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QColor(120, 120, 120))
        p.setBrush(QColor(40, 40, 40))
        p.drawRoundedRect(r, 3, 3)
        inner = r.adjusted(2, 2, -2, -2)
        if self._level > 0.002:
            p.setPen(Qt.NoPen)
            p.setBrush(ORANGE if self._busy else QColor(120, 120, 120))
            p.drawRoundedRect(QRectF(inner.left(), inner.top(), inner.width() * self._level, inner.height()), 2, 2)
        x = inner.left() + inner.width() * self._threshold
        p.setPen(QPen(QColor(230, 50, 50), 2))
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
        p.end()


class SupercapMeter(QWidget):
    """The supercap's charge as a bar from SCALE_MIN to SCALE_MAX: red, orange or green for
    whether it could carry a spindown (supercap.py), with a mark at the voltage a typical
    spindown needs and the scale's ends labelled underneath."""

    COLOURS = {supercap.RED: QColor(220, 50, 50), supercap.ORANGE: ORANGE, supercap.GREEN: QColor(60, 180, 75)}

    def __init__(self) -> None:
        super().__init__()
        self.setFixedHeight(30)
        self.setMinimumWidth(160)  # room for the three labels under the bar
        self.mv: Optional[int] = None

    def sizeHint(self) -> QSize:
        return QSize(260, 30)

    def set_millivolts(self, mv: Optional[int]) -> None:
        self.mv = mv
        self.setToolTip(supercap.describe(mv) if mv is not None else "")
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        bar = QRectF(0.5, 0.5, self.width() - 1, 15)
        p.setPen(QColor(120, 120, 120))
        p.setBrush(QColor(40, 40, 40))
        p.drawRoundedRect(bar, 3, 3)
        inner = bar.adjusted(2, 2, -2, -2)
        if self.mv is not None:
            p.setPen(Qt.NoPen)
            p.setBrush(self.COLOURS[supercap.level(self.mv)])
            p.drawRoundedRect(QRectF(inner.left(), inner.top(), inner.width() * supercap.fraction(self.mv),
                                     inner.height()), 2, 2)
        x = inner.left() + inner.width() * supercap.fraction(supercap.SPINDOWN_TYPICAL_MV)
        p.setPen(QPen(QColor(235, 235, 235), 2))
        p.drawLine(QPointF(x, bar.top()), QPointF(x, bar.bottom()))

        font = p.font()
        font.setPixelSize(10)
        p.setFont(font)
        p.setPen(QColor(128, 128, 128))
        text = QRectF(0, bar.bottom() + 1, self.width(), self.height() - bar.bottom() - 1)
        p.drawText(text, Qt.AlignLeft | Qt.AlignVCenter, f"{supercap.SCALE_MIN_MV / 1000:.1f} V")
        p.drawText(text, Qt.AlignRight | Qt.AlignVCenter, f"{supercap.SCALE_MAX_MV / 1000:.1f} V")
        mark = QRectF(x - 60, text.top(), 120, text.height())
        p.drawText(mark, Qt.AlignHCenter | Qt.AlignVCenter, f"spindown {supercap.SPINDOWN_TYPICAL_MV / 1000:.1f} V")
        p.end()


class SettingsWindow(QWidget):
    def __init__(self, app: "Companion") -> None:
        super().__init__()
        self.app = app
        self.setWindowTitle("HDD Synth Companion")
        self.setMinimumWidth(420)

        self.device_label = QLabel()
        self.device_label.setWordWrap(True)
        self.supercap = SupercapMeter()
        self.supercap_label = QLabel()
        # Wide enough for "4.20 V" so the bar doesn't move as the value changes.
        self.supercap_label.setFixedWidth(self.supercap_label.fontMetrics().horizontalAdvance("0.00 V") + 6)
        self.supercap_label.setAlignment(Qt.AlignRight | Qt.AlignTop)
        self.pack_combo = QComboBox()
        self.pack_combo.activated.connect(self._pack_chosen)

        self.volume = self._slider()
        self.volume_label = QLabel()
        self.balance = self._slider()
        self.balance_label = QLabel()
        self._pending: dict[str, int] = {}
        self._throttle = QTimer(self, singleShot=True, interval=SLIDER_THROTTLE_MS)
        self._throttle.timeout.connect(self._flush_levels)
        self.volume.valueChanged.connect(lambda v: self._level_changed("vol", v))
        self.balance.valueChanged.connect(lambda v: self._level_changed("bal", v))
        self.volume.sliderReleased.connect(self._flush_levels)
        self.balance.sliderReleased.connect(self._flush_levels)

        self.send_activity = QCheckBox("Play this computer's disk activity")
        self.send_activity.setChecked(app.cfg.send_activity)
        self.send_activity.toggled.connect(self._send_activity_toggled)
        # Inverted (more sensitive on the left) so the handle sits over the meter's
        # threshold line below it.
        self.sensitivity = self._slider()
        self.sensitivity.setInvertedAppearance(True)
        self.sensitivity.setValue(app.cfg.sensitivity)
        self.sensitivity.valueChanged.connect(self._sensitivity_changed)
        self.meter = ActivityMeter()
        ends = QWidget()
        e = QHBoxLayout(ends)
        e.setContentsMargins(0, 0, 0, 0)
        for text, align in (("more sensitive", Qt.AlignLeft), ("less sensitive", Qt.AlignRight)):
            lab = QLabel(text)
            lab.setStyleSheet("color: gray; font-size: 10px")
            e.addWidget(lab, 1, align)
        sensitivity_box = QWidget()
        sb = QVBoxLayout(sensitivity_box)
        sb.setContentsMargins(0, 0, 0, 0)
        sb.setSpacing(2)
        sb.addWidget(self.sensitivity)
        sb.addWidget(self.meter)
        sb.addWidget(ends)

        self.disk_list = QListWidget()
        self.disk_list.setMaximumHeight(90)
        self.disk_list.itemChanged.connect(self._disks_changed)
        self.port_combo = QComboBox()
        self.port_combo.activated.connect(self._port_chosen)

        form = QFormLayout()
        # macOS's default keeps fields at their preferred width; stretch them as elsewhere.
        form.setFieldGrowthPolicy(QFormLayout.AllNonFixedFieldsGrow)
        form.addRow("Device", self.device_label)
        form.addRow("Supercap", self._row(self.supercap, self.supercap_label))
        form.addRow("Sample pack", self.pack_combo)
        form.addRow("Volume", self._row(self.volume, self.volume_label))
        form.addRow("Balance", self._row(self.balance, self.balance_label))
        form.addRow("", self.send_activity)
        form.addRow("Sensitivity", sensitivity_box)
        form.addRow("Disks", self.disk_list)
        form.addRow("Port", self.port_combo)
        hint = QLabel(
            "The bar shows disk activity and the red line the threshold: activity past the line "
            "(orange) plays the access sound. Move the slider until background activity stays left "
            "of the line but real work crosses it. Balance mixes the idle sound (left) against the "
            "access sound (right)."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color: gray")
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(hint)

        self._meter_timer = QTimer(self, interval=50)
        self._meter_timer.timeout.connect(self._update_meter)
        self._fill_disks()
        self._fill_ports()
        self.refresh()

    @staticmethod
    def _slider() -> QSlider:
        s = QSlider(Qt.Horizontal)
        s.setRange(0, 100)
        return s

    @staticmethod
    def _row(*widgets: QWidget) -> QWidget:
        w = QWidget()
        h = QHBoxLayout(w)
        h.setContentsMargins(0, 0, 0, 0)
        for x in widgets:
            h.addWidget(x)
        if widgets[-1].maximumWidth() > 1000:  # not given a fixed width
            widgets[-1].setMinimumWidth(80)
        return w

    # ---- showing ----

    def showEvent(self, e) -> None:
        self._meter_timer.start()
        self._fill_ports()
        super().showEvent(e)

    def hideEvent(self, e) -> None:
        self._meter_timer.stop()
        super().hideEvent(e)

    def refresh(self) -> None:
        """Bring everything up to date with the app's view of the device."""
        a = self.app
        st = a.status
        if not a.link.connected:
            self.device_label.setText("Not connected. Plug the HDD Synth into USB.")
        elif st is None:
            self.device_label.setText(f"Connected on {a.link.port}")
        else:
            self.device_label.setText(f"Firmware v{st.firmware}, {st.state.lower()}, on {a.link.port}")
        self.supercap.set_millivolts(st.supercap_mv if a.link.connected and st is not None else None)
        self.supercap_label.setText(f"{st.supercap_mv / 1000:.2f} V" if a.link.connected and st is not None else "")
        enabled = a.link.connected and st is not None
        for w in (self.pack_combo, self.volume, self.balance):
            w.setEnabled(enabled)

        self.pack_combo.blockSignals(True)
        self.pack_combo.clear()
        for i in sorted(a.packs):
            self.pack_combo.addItem(a.packs[i].name, i)
        if st is not None and st.pack_index in a.packs:
            self.pack_combo.setCurrentIndex(sorted(a.packs).index(st.pack_index))
        self.pack_combo.blockSignals(False)

        if st is not None:
            self._show_level(self.volume, st.volume)
            self._show_level(self.balance, st.balance)
        self.volume_label.setText(f"{self.volume.value()}%")
        self.balance_label.setText(f"{100 - self.balance.value()} / {self.balance.value()}")

    def _show_level(self, slider: QSlider, value: int) -> None:
        if slider.isSliderDown():
            return  # don't fight the user's drag
        slider.blockSignals(True)
        slider.setValue(value)
        slider.blockSignals(False)

    def _update_meter(self) -> None:
        self.meter.show_activity(self.app.last_activity)

    def _fill_disks(self) -> None:
        self.disk_list.blockSignals(True)
        self.disk_list.clear()
        chosen = set(self.app.cfg.disks)
        for name in disks.list_disks():
            item = QListWidgetItem(name)
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(Qt.Checked if not chosen or name in chosen else Qt.Unchecked)
            self.disk_list.addItem(item)
        self.disk_list.blockSignals(False)

    def _fill_ports(self) -> None:
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        self.port_combo.addItem("Automatic", "")
        for p in device.all_ports():
            self.port_combo.addItem(p, p)
        i = self.port_combo.findData(self.app.cfg.port)
        if i < 0:
            self.port_combo.addItem(self.app.cfg.port, self.app.cfg.port)
            i = self.port_combo.count() - 1
        self.port_combo.setCurrentIndex(i)
        self.port_combo.blockSignals(False)

    # ---- user changes ----

    def _pack_chosen(self, row: int) -> None:
        self.app.choose_pack(self.pack_combo.itemData(row))

    def _level_changed(self, which: str, value: int) -> None:
        self._pending[which] = value
        self.volume_label.setText(f"{self.volume.value()}%")
        self.balance_label.setText(f"{100 - self.balance.value()} / {self.balance.value()}")
        if self.volume.isSliderDown() or self.balance.isSliderDown():
            if not self._throttle.isActive():
                self._throttle.start()
        else:
            self._flush_levels()  # keyboard, or a click on the groove

    def _flush_levels(self) -> None:
        for which, value in self._pending.items():
            self.app.link.send(protocol.set_volume(value) if which == "vol" else protocol.set_balance(value))
        self._pending.clear()

    def _sensitivity_changed(self, value: int) -> None:
        self.app.cfg.sensitivity = value
        self.app.detector.sensitivity = value
        self.app.save_config()

    def _send_activity_toggled(self, on: bool) -> None:
        self.app.cfg.send_activity = on
        self.app.save_config()

    def _disks_changed(self, _item) -> None:
        names = [self.disk_list.item(i).text() for i in range(self.disk_list.count())]
        checked = [n for i, n in enumerate(names) if self.disk_list.item(i).checkState() == Qt.Checked]
        self.app.cfg.disks = [] if len(checked) == len(names) else checked
        self.app.detector.reset()
        self.app.save_config()

    def _port_chosen(self, row: int) -> None:
        self.app.cfg.port = self.port_combo.itemData(row) or ""
        self.app.save_config()
        self.app.reconnect()


class Companion(QObject):
    def __init__(self, qapp: QApplication) -> None:
        super().__init__()
        self.qapp = qapp
        self.cfg = config.load()
        self.detector = ActivityDetector(self.cfg.sensitivity)
        self.sender = LinkSender()
        self.last_activity: Optional[Activity] = None
        self.status: Optional[protocol.Status] = None
        self.packs: dict[int, protocol.PackEntry] = {}

        self._bridge = _Bridge()
        self._bridge.reply.connect(self._on_reply)
        self._bridge.closed.connect(self._on_closed)
        self.link = device.DeviceLink(self._bridge.reply.emit, self._bridge.closed.emit)

        self.icons = {
            "off": _drive_icon(None, dimmed=True),
            "idle": _drive_icon(QColor(60, 30, 0)),
            "busy": _drive_icon(ORANGE),
        }
        self._icon_key = ""
        self.tray = QSystemTrayIcon(self.icons["off"])
        self.tray.setToolTip("HDD Synth Companion")
        self.menu = QMenu()
        self.status_action = self.menu.addAction("Not connected")
        self.status_action.setEnabled(False)
        self.packs_menu = self.menu.addMenu("Sample pack")
        self.pack_group = QActionGroup(self.menu, exclusive=True)
        self.menu.addSeparator()
        self.settings_action = self.menu.addAction("Settings…", self.show_settings)
        self.autostart_action = self.menu.addAction("Start at login")
        self.autostart_action.setCheckable(True)
        self.autostart_action.setVisible(autostart.supported())
        if autostart.supported():
            self.autostart_action.setChecked(autostart.enabled())
        self.autostart_action.toggled.connect(self._autostart_toggled)
        self.menu.addSeparator()
        self.about_action = self.menu.addAction("About HDD Synth Companion…", self.show_about)
        self.quit_action = self.menu.addAction("Quit", self.quit)
        # Keep macOS from moving these into an application menu (this app has none).
        for a in (self.settings_action, self.about_action, self.quit_action):
            a.setMenuRole(QAction.NoRole)
        self.tray.setContextMenu(self.menu)
        self.tray.activated.connect(self._tray_activated)
        self.tray.show()

        self.window: Optional[SettingsWindow] = None
        self.about: Optional[AboutDialog] = None

        self._poll = QTimer(self, interval=POLL_MS)
        self._poll.timeout.connect(self._poll_disks)
        self._poll.start()
        self._not_a_synth: dict[str, float] = {}
        self._handshake = QTimer(self, singleShot=True, interval=HANDSHAKE_MS)
        self._handshake.timeout.connect(self._check_handshake)
        self._reconnect = QTimer(self, interval=RECONNECT_MS)
        self._reconnect.timeout.connect(self._try_connect)
        self._reconnect.start()
        self._status_timer = QTimer(self, interval=STATUS_REFRESH_MS)
        self._status_timer.timeout.connect(lambda: self.link.send(protocol.query_status()))
        self._status_timer.start()
        self._try_connect()
        self._refresh_ui()

    # ---- connection ----

    def _try_connect(self) -> None:
        if self.link.connected:
            return
        now = time.monotonic()
        candidates = [self.cfg.port] if self.cfg.port else device.candidate_ports()
        for port in candidates:
            if self._not_a_synth.get(port, 0) > now:
                continue
            if self.link.connect(port):
                self.sender.reset()
                self.link.send(protocol.query_status())
                self.link.send(protocol.query_packs())
                self._handshake.start()
                self._refresh_ui()
                return

    def _check_handshake(self) -> None:
        """No status reply: not an HDD Synth (or one with old firmware). Try elsewhere."""
        if self.link.connected and self.status is None:
            port = self.link.port
            if not self.cfg.port:  # a port chosen by hand is always retried
                self._not_a_synth[port] = time.monotonic() + NOT_A_SYNTH_RETRY_S
            self.link.close()
            self._on_closed()
            self.tray.setToolTip(f"HDD Synth Companion\n{port} didn't answer as an HDD Synth (firmware v0.19.0+ needed)")

    def reconnect(self) -> None:
        self.link.close()
        self._on_closed()
        self._try_connect()

    def _on_closed(self) -> None:
        self.status = None
        self.packs.clear()
        self._refresh_ui()

    def _on_reply(self, reply: protocol.Reply) -> None:
        if isinstance(reply, protocol.Status):
            previous = self.status
            self.status = reply
            if reply.pack_count != len(self.packs) or (previous and previous.firmware != reply.firmware):
                self.packs.clear()
                self.link.send(protocol.query_packs())
        elif isinstance(reply, protocol.PackEntry):
            self.packs[reply.index] = reply
        elif isinstance(reply, protocol.Ack) and not reply.ok:
            why = {"busy": "it isn't running yet", "demo": "demo mode is on", "range": "that pack isn't on the card"}
            self.tray.showMessage("HDD Synth", f"Couldn't change the {reply.command.lower()}: "
                                  f"{why.get(reply.reason, reply.reason)}.")
        self._refresh_ui()

    # ---- activity ----

    def _poll_disks(self) -> None:
        s = disks.sample(self.cfg.disks)
        if s is None:
            return
        a = self.detector.update(s)
        self.last_activity = a
        if self.link.connected and self.cfg.send_activity and self.sender.should_send(a, s.t):
            self.link.send(protocol.activity(a.busy, a.cls, a.op))
        self._set_icon("off" if not self.link.connected else "busy" if a.busy and self.cfg.send_activity else "idle")

    def _set_icon(self, key: str) -> None:
        if key != self._icon_key:
            self._icon_key = key
            self.tray.setIcon(self.icons[key])

    # ---- commands ----

    def choose_pack(self, index: Optional[int]) -> None:
        if index is not None:
            self.link.send(protocol.set_pack(index))

    # ---- UI ----

    def _refresh_ui(self) -> None:
        st = self.status
        if not self.link.connected:
            text = "Not connected"
        elif st is None:
            text = f"Connected on {self.link.port}"
        else:
            pack = self.packs.get(st.pack_index)
            text = f"HDD Synth v{st.firmware}: {pack.name if pack else '…'}"
        self.status_action.setText(text)
        self.tray.setToolTip(f"HDD Synth Companion\n{text}")

        self.packs_menu.clear()
        for a in self.pack_group.actions():
            self.pack_group.removeAction(a)
        for i in sorted(self.packs):
            act = QAction(self.packs[i].name, self.packs_menu, checkable=True)
            act.setChecked(st is not None and st.pack_index == i)
            act.triggered.connect(lambda _checked=False, i=i: self.choose_pack(i))
            self.pack_group.addAction(act)
            self.packs_menu.addAction(act)
        self.packs_menu.setEnabled(bool(self.packs))
        if self.window is not None:
            self.window.refresh()

    def show_settings(self) -> None:
        if self.window is None:
            self.window = SettingsWindow(self)
        self.window.refresh()
        _bring_to_front(self.window)

    def show_about(self) -> None:
        if self.about is not None:
            self.about.close()
        self.about = AboutDialog(self.status.firmware if self.link.connected and self.status else None)
        _bring_to_front(self.about)

    def _tray_activated(self, reason) -> None:
        # Windows: a click on the tray icon opens the settings (the menu is on right-click).
        if sys.platform == "win32" and reason == QSystemTrayIcon.Trigger:
            self.show_settings()

    def _autostart_toggled(self, on: bool) -> None:
        try:
            autostart.set_enabled(on)
        except OSError as e:
            QMessageBox.warning(None, "HDD Synth Companion", f"Couldn't change start at login: {e}")

    def save_config(self) -> None:
        try:
            config.save(self.cfg)
        except OSError:
            pass  # not worth interrupting for; the defaults come back next time

    def quit(self) -> None:
        self.link.close()
        self.tray.hide()
        self.qapp.quit()


def main() -> int:
    qapp = QApplication(sys.argv)
    qapp.setApplicationName("HDD Synth Companion")
    qapp.setQuitOnLastWindowClosed(False)  # closing the settings window leaves the tray icon
    if not QSystemTrayIcon.isSystemTrayAvailable():
        QMessageBox.critical(None, "HDD Synth Companion", "This desktop has no system tray to live in.")
        return 1
    # One copy at a time: two would fight over the serial port.
    lock = QLockFile(str(config.path().with_name("app.lock")))
    config.path().parent.mkdir(parents=True, exist_ok=True)
    if not lock.tryLock(100):
        QMessageBox.information(None, "HDD Synth Companion", "HDD Synth Companion is already running.")
        return 0
    _hide_dock_icon()
    _companion = Companion(qapp)
    return qapp.exec()
