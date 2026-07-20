"""Stats panel: per-channel statistics and an RGB/luminance histogram."""
import numpy as np
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

RGB_COLORS = [QColor(255, 80, 80), QColor(80, 220, 80), QColor(90, 130, 255)]
GRAY_COLOR = QColor(210, 210, 210)


def human_size(n):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return f'{n:.0f} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024


class HistogramWidget(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._hists = []      # list of 256-bin arrays
        self._colors = []
        self.setMinimumHeight(130)

    def set_histograms(self, hists, colors):
        self._hists = hists
        self._colors = colors
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(28, 28, 30))
        if not self._hists:
            p.setPen(QColor(120, 120, 120))
            p.drawText(self.rect(), Qt.AlignCenter, 'no image')
            return
        m = 6
        w, h = self.width() - 2 * m, self.height() - 2 * m
        peak = max(int(hist.max()) for hist in self._hists) or 1
        p.setRenderHint(QPainter.Antialiasing)
        p.setCompositionMode(QPainter.CompositionMode_Plus)
        for hist, color in zip(self._hists, self._colors):
            pts = QPolygonF([
                QPointF(m + i / 255 * w, m + h - (int(v) / peak) * h)
                for i, v in enumerate(hist)
            ])
            p.setPen(QPen(color, 1.2))
            p.drawPolyline(pts)


class StatsPanel(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.hist = HistogramWidget()
        self.info = QLabel('no image')
        self.info.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.info.setWordWrap(True)
        self.info.setAlignment(Qt.AlignTop | Qt.AlignLeft)
        lay = QVBoxLayout(self)
        lay.addWidget(self.hist)
        lay.addWidget(self.info, 1)

    def clear(self):
        self.hist.set_histograms([], [])
        self.info.setText('no image')

    def set_image(self, arr, path, mode, file_size):
        """arr: uint8 HxW (grayscale) or HxWx3 (RGB); mode: original PIL mode."""
        if arr is None:
            self.clear()
            return
        if arr.ndim == 2:
            names, chans, colors = ['L'], [arr], [GRAY_COLOR]
        else:
            names = ['R', 'G', 'B']
            chans = [arr[..., i] for i in range(3)]
            colors = RGB_COLORS
        hists = [np.bincount(c.reshape(-1), minlength=256)[:256] for c in chans]
        self.hist.set_histograms(hists, colors)

        h, w = arr.shape[:2]
        lines = [
            f'<b>{path.name}</b>',
            f'{w} × {h} px &nbsp; {mode} &nbsp; {human_size(file_size)}',
            f'{w * h:,} pixels',
            '',
        ]
        for name, c in zip(names, chans):
            lines.append(
                f'<b>{name}</b>&nbsp; mean {c.mean():.1f} &nbsp; std {c.std():.1f}'
                f' &nbsp; min {c.min()} &nbsp; max {c.max()}'
            )
        if arr.ndim == 3:
            lum = (0.2126 * chans[0] + 0.7152 * chans[1] + 0.0722 * chans[2])
            lines.append(f'<b>Y</b>&nbsp; mean {lum.mean():.1f} &nbsp; std {lum.std():.1f}')
        self.info.setText('<br>'.join(lines))
