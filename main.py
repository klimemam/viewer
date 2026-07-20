"""Image viewer with folder browsing, zoom/pan, thumbnails, A/B compare and stats."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from PySide6.QtCore import QObject, QRunnable, QSize, Qt, QThreadPool, Signal
from PySide6.QtGui import QAction, QIcon, QImage, QPixmap
from PySide6.QtWidgets import (
    QApplication, QDialog, QDockWidget, QFileDialog, QHBoxLayout, QLabel,
    QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QSplitter,
    QVBoxLayout, QWidget,
)

from image_view import ImageView
from stats_panel import StatsPanel

EXTS = {'.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp', '.tif', '.tiff'}
THUMB_SIZE = 144
DIFF_GAIN = 4  # amplification for the diff heat image


def load_array(path):
    """Return (uint8 numpy array HxW or HxWx3, original PIL mode string)."""
    with Image.open(path) as im:
        mode = im.mode
        if im.mode not in ('RGB', 'L'):
            im = im.convert('RGB')
        return np.ascontiguousarray(im), mode


def qimage_from_array(arr):
    if arr.ndim == 2:
        h, w = arr.shape
        return QImage(arr.tobytes(), w, h, w, QImage.Format_Grayscale8).copy()
    h, w, _ = arr.shape
    return QImage(arr.tobytes(), w, h, w * 3, QImage.Format_RGB888).copy()


def to_rgb(arr):
    return np.stack([arr] * 3, axis=-1) if arr.ndim == 2 else arr


class ThumbSignals(QObject):
    done = Signal(int, int, QImage)  # generation, row, thumbnail


class ThumbTask(QRunnable):
    def __init__(self, generation, row, path, signals):
        super().__init__()
        self.generation, self.row, self.path, self.signals = generation, row, path, signals

    def run(self):
        try:
            with Image.open(self.path) as im:
                im.thumbnail((THUMB_SIZE, THUMB_SIZE))
                im = im.convert('RGBA')
                qimg = QImage(im.tobytes(), im.width, im.height,
                              QImage.Format_RGBA8888).copy()
        except Exception:
            return
        self.signals.done.emit(self.generation, self.row, qimg)


class DiffDialog(QDialog):
    def __init__(self, arr_a, arr_b, name_a, name_b, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f'Diff — {name_a} vs {name_b}')
        self.resize(900, 650)

        d = np.abs(arr_a.astype(np.int16) - arr_b.astype(np.int16))
        per_pixel = d.max(axis=2)
        mae = d.mean()
        rmse = np.sqrt((d.astype(np.float64) ** 2).mean())
        pct = (per_pixel > 0).mean() * 100
        heat = np.clip(per_pixel * DIFF_GAIN, 0, 255).astype(np.uint8)
        heat = np.ascontiguousarray(heat)

        self.metrics = (f'MAE {mae:.3f}   RMSE {rmse:.3f}   max |Δ| {int(d.max())}   '
                        f'{pct:.2f}% of pixels differ')
        label = QLabel(self.metrics + f'   (heat image gain ×{DIFF_GAIN})')
        self.view = ImageView()
        self.view.set_image(QPixmap.fromImage(qimage_from_array(heat)))

        lay = QVBoxLayout(self)
        lay.addWidget(label)
        lay.addWidget(self.view, 1)

    def showEvent(self, event):
        super().showEvent(event)
        self.view.fit_to_window()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('Viewer')
        self.resize(1280, 800)

        self.files = []
        self.index = -1
        self.arr = None            # current image as numpy array
        self.mode = ''
        self.pm_current = QPixmap()
        self.arr_a = None          # pinned reference for compare
        self.path_a = None
        self.pm_a = QPixmap()

        self.pool = QThreadPool.globalInstance()
        self.thumb_signals = ThumbSignals()
        self.thumb_signals.done.connect(self._thumb_done)
        self.generation = 0

        # --- central: [pinned A pane | main pane] ---
        self.view_a = ImageView()
        self.view_main = ImageView()
        self.header_a = QLabel('A')
        self.header_main = QLabel('')
        for lbl in (self.header_a, self.header_main):
            lbl.setStyleSheet('padding: 3px 6px; background: #202022; color: #ddd;')

        self.pane_a = self._pane(self.header_a, self.view_a)
        pane_main = self._pane(self.header_main, self.view_main)
        splitter = QSplitter(Qt.Horizontal)
        splitter.addWidget(self.pane_a)
        splitter.addWidget(pane_main)
        self.setCentralWidget(splitter)
        self.pane_a.hide()
        self.header_main.hide()

        self.view_a.viewChanged.connect(lambda: self.view_main.sync_from(self.view_a))
        self.view_main.viewChanged.connect(lambda: self.view_a.sync_from(self.view_main))
        self.view_main.viewChanged.connect(self._update_zoom_label)
        self.view_a.pixelHovered.connect(self._on_hover)
        self.view_main.pixelHovered.connect(self._on_hover)

        # --- gallery dock ---
        self.list = QListWidget()
        self.list.setViewMode(QListWidget.IconMode)
        self.list.setIconSize(QSize(THUMB_SIZE, THUMB_SIZE))
        self.list.setResizeMode(QListWidget.Adjust)
        self.list.setMovement(QListWidget.Static)
        self.list.setSpacing(8)
        self.list.setWordWrap(True)
        self.list.currentRowChanged.connect(self._row_changed)
        gallery = QDockWidget('Gallery', self)
        gallery.setWidget(self.list)
        self.addDockWidget(Qt.LeftDockWidgetArea, gallery)

        # --- stats dock ---
        self.stats = StatsPanel()
        stats_dock = QDockWidget('Stats', self)
        stats_dock.setWidget(self.stats)
        self.addDockWidget(Qt.RightDockWidgetArea, stats_dock)

        # --- toolbar / actions ---
        tb = self.addToolBar('Main')
        tb.setMovable(False)

        def act(text, shortcut, slot, checkable=False, enabled=True):
            a = QAction(text, self)
            a.setShortcut(shortcut)
            a.setCheckable(checkable)
            a.setEnabled(enabled)
            a.triggered.connect(slot)
            tb.addAction(a)
            return a

        act('Open Folder', 'Ctrl+O', self.open_folder)
        act('Prev', 'Left', lambda: self.show_index(self.index - 1))
        act('Next', 'Right', lambda: self.show_index(self.index + 1))
        act('Fit', 'F', self.view_main.fit_to_window)
        act('100%', '1', self.view_main.zoom_100)
        act('Pin as A', 'A', self.pin_a)
        self.compare_act = act('Compare', 'C', self.toggle_compare,
                               checkable=True, enabled=False)
        self.diff_act = act('Diff', 'D', self.show_diff, enabled=False)

        # --- status bar ---
        self.pixel_label = QLabel('')
        self.zoom_label = QLabel('')
        self.statusBar().addPermanentWidget(self.pixel_label)
        self.statusBar().addPermanentWidget(self.zoom_label)

        self.setAcceptDrops(True)

    @staticmethod
    def _pane(header, view):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)
        lay.addWidget(header)
        lay.addWidget(view, 1)
        return w

    # --- folder / navigation ------------------------------------------------
    def open_folder(self):
        d = QFileDialog.getExistingDirectory(self, 'Open image folder')
        if d:
            self.load_folder(Path(d))

    def load_folder(self, folder, select=None):
        self.files = sorted(p for p in folder.iterdir() if p.suffix.lower() in EXTS)
        self.generation += 1
        self.index = -1
        self.list.clear()
        for i, p in enumerate(self.files):
            self.list.addItem(QListWidgetItem(p.name))
            self.pool.start(ThumbTask(self.generation, i, p, self.thumb_signals))
        self.setWindowTitle(f'Viewer — {folder}')
        if not self.files:
            self.arr = None
            self.view_main.set_image(None)
            self.stats.clear()
            self.statusBar().showMessage('No images found in this folder')
            return
        start = self.files.index(select) if select in self.files else 0
        self.show_index(start)

    def show_index(self, i):
        if not (0 <= i < len(self.files)):
            return
        self.index = i
        path = self.files[i]
        try:
            self.arr, self.mode = load_array(path)
        except Exception as e:
            self.arr, self.mode = None, ''
            self.statusBar().showMessage(f'Failed to load {path.name}: {e}')
        pm = QPixmap(str(path))
        if pm.isNull() and self.arr is not None:
            pm = QPixmap.fromImage(qimage_from_array(self.arr))
        self.pm_current = pm
        self.view_main.set_image(pm)
        self.stats.set_image(self.arr, path, self.mode, path.stat().st_size)
        if self.list.currentRow() != i:
            self.list.setCurrentRow(i)
        self.header_main.setText(f'Current: {path.name}')
        self.statusBar().showMessage(f'{i + 1}/{len(self.files)}   {path.name}')
        self._update_zoom_label()

    def _row_changed(self, row):
        if row >= 0 and row != self.index:
            self.show_index(row)

    def _thumb_done(self, gen, row, qimg):
        if gen == self.generation and row < self.list.count():
            self.list.item(row).setIcon(QIcon(QPixmap.fromImage(qimg)))

    # --- compare --------------------------------------------------------------
    def pin_a(self):
        if self.arr is None:
            return
        self.arr_a = self.arr
        self.path_a = self.files[self.index]
        self.pm_a = self.pm_current
        self.view_a.set_image(self.pm_a)
        self.header_a.setText(f'A (pinned): {self.path_a.name}')
        self.compare_act.setEnabled(True)
        self.statusBar().showMessage(f'Pinned {self.path_a.name} as A — press C to compare')

    def toggle_compare(self, on):
        show = on and self.arr_a is not None
        self.pane_a.setVisible(show)
        self.header_main.setVisible(show)
        self.diff_act.setEnabled(show)
        if show:
            self.view_a.fit_to_window()
            self.view_main.fit_to_window()

    def show_diff(self):
        if self.arr_a is None or self.arr is None:
            return
        a, b = to_rgb(self.arr_a), to_rgb(self.arr)
        if a.shape != b.shape:
            QMessageBox.information(
                self, 'Diff',
                f'Images have different dimensions:\n'
                f'A: {a.shape[1]}×{a.shape[0]}   current: {b.shape[1]}×{b.shape[0]}')
            return
        dlg = DiffDialog(a, b, self.path_a.name, self.files[self.index].name, self)
        dlg.exec()

    # --- status readouts -------------------------------------------------------
    def _on_hover(self, x, y):
        if x < 0 or self.arr is None:
            self.pixel_label.setText('')
            return
        parts = [f'({x}, {y})']
        compare = self.compare_act.isChecked()
        if compare and self.arr_a is not None:
            v = self._sample(self.arr_a, x, y)
            if v:
                parts.append(f'A: {v}')
        v = self._sample(self.arr, x, y)
        if v:
            parts.append(f'B: {v}' if compare else v)
        self.pixel_label.setText('   '.join(parts) + '  ')

    @staticmethod
    def _sample(arr, x, y):
        h, w = arr.shape[:2]
        if not (0 <= x < w and 0 <= y < h):
            return ''
        px = arr[y, x]
        return str(int(px)) if arr.ndim == 2 else ','.join(str(int(v)) for v in px)

    def _update_zoom_label(self):
        self.zoom_label.setText(f'{self.view_main.scale_factor() * 100:.0f}%  ')

    # --- drag & drop -------------------------------------------------------------
    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event):
        for url in event.mimeData().urls():
            p = Path(url.toLocalFile())
            if p.is_dir():
                self.load_folder(p)
                return
            if p.suffix.lower() in EXTS:
                self.load_folder(p.parent, select=p)
                return


def main():
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
        if target.is_dir():
            win.load_folder(target)
        elif target.is_file():
            win.load_folder(target.parent, select=target)
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
