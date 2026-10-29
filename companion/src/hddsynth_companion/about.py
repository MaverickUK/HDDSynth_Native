"""The About box, after HDDSynth_Win32's: the logo, the app and its version, what it's
running on (here: the OS and the connected HDD Synth's firmware), the author, and a link
to the project."""

from __future__ import annotations

import platform
import sys
from importlib import resources
from typing import Optional

from PySide6.QtCore import Qt
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QDialog, QDialogButtonBox, QLabel, QVBoxLayout, QWidget

from . import __version__

APP_NAME = "HDD Synth Companion"
AUTHOR = "By Peter Bridger"
URL = "https://github.com/MaverickUK/HDDSynth_Native/tree/main/companion"
LOGO_WIDTH = 220


def os_description() -> str:
    if sys.platform == "darwin":
        return f"Running on macOS {platform.mac_ver()[0]}"
    if sys.platform == "win32":
        build = sys.getwindowsversion().build
        return f"Running on Windows {'11' if build >= 22000 else platform.release()}"
    return f"Running on {platform.system()} {platform.release()}"


class AboutDialog(QDialog):
    def __init__(self, firmware: Optional[str], parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"About {APP_NAME}")
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(24, 18, 24, 14)

        # The logo is black on transparent: on a white card, as Win32 flattens it, so it
        # stays visible in dark mode.
        logo = QLabel()
        pm = QPixmap()
        pm.loadFromData(resources.files(__package__).joinpath("resources/logo.png").read_bytes())
        if not pm.isNull():
            ratio = self.devicePixelRatioF()
            pm = pm.scaledToWidth(int(LOGO_WIDTH * ratio), Qt.SmoothTransformation)
            pm.setDevicePixelRatio(ratio)
            logo.setPixmap(pm)
        logo.setAlignment(Qt.AlignCenter)
        logo.setStyleSheet("background: white; border-radius: 8px; padding: 10px")
        layout.addWidget(logo, 0, Qt.AlignHCenter)
        layout.addSpacing(10)

        def line(text: str, bold: bool = False) -> QLabel:
            lab = QLabel(text)
            lab.setAlignment(Qt.AlignCenter)
            if bold:
                f = lab.font()
                f.setBold(True)
                f.setPointSizeF(f.pointSizeF() * 1.2)
                lab.setFont(f)
            layout.addWidget(lab)
            return lab

        line(APP_NAME, bold=True)
        line(f"Version {__version__}")
        line(os_description())
        line(f"HDD Synth firmware v{firmware}" if firmware else "No HDD Synth connected")
        line(AUTHOR)
        link = line(f'<a href="{URL}">{URL[len("https://"):]}</a>')
        link.setTextFormat(Qt.RichText)
        link.setOpenExternalLinks(True)  # opens the browser, like Win32's ShellExecute

        buttons = QDialogButtonBox(QDialogButtonBox.Ok)
        buttons.accepted.connect(self.accept)
        layout.addSpacing(8)
        layout.addWidget(buttons, 0, Qt.AlignHCenter)
