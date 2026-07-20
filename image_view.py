"""Zoomable, pannable image view based on QGraphicsView."""
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QPainter, QPixmap
from PySide6.QtWidgets import QGraphicsPixmapItem, QGraphicsScene, QGraphicsView

MIN_SCALE = 0.02
MAX_SCALE = 64.0


class ImageView(QGraphicsView):
    pixelHovered = Signal(int, int)   # image coords; (-1, -1) when leaving the image
    viewChanged = Signal()            # zoom or pan happened (used to sync A/B views)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._scene = QGraphicsScene(self)
        self._item = QGraphicsPixmapItem()
        self._scene.addItem(self._item)
        self.setScene(self._scene)
        self.setDragMode(QGraphicsView.ScrollHandDrag)
        self.setTransformationAnchor(QGraphicsView.AnchorUnderMouse)
        self.setRenderHints(QPainter.SmoothPixmapTransform | QPainter.Antialiasing)
        self.setMouseTracking(True)
        self.setBackgroundBrush(Qt.darkGray)
        self._syncing = False

    def set_image(self, pixmap):
        self._item.setPixmap(pixmap if pixmap is not None else QPixmap())
        self._scene.setSceneRect(self._item.boundingRect())
        self.fit_to_window()

    def has_image(self):
        return not self._item.pixmap().isNull()

    def scale_factor(self):
        return self.transform().m11()

    def zoom_by(self, factor):
        if not self.has_image():
            return
        new = self.scale_factor() * factor
        if new < MIN_SCALE or new > MAX_SCALE:
            return
        self.scale(factor, factor)
        self._emit_changed()

    def fit_to_window(self):
        if not self.has_image():
            return
        self.fitInView(self._item, Qt.KeepAspectRatio)
        self._emit_changed()

    def zoom_100(self):
        if not self.has_image():
            return
        self.resetTransform()
        self._emit_changed()

    def sync_from(self, other):
        """Copy zoom + pan from another view (used by A/B compare)."""
        if self._syncing:
            return
        self._syncing = True
        try:
            self.setTransform(other.transform())
            self.horizontalScrollBar().setValue(other.horizontalScrollBar().value())
            self.verticalScrollBar().setValue(other.verticalScrollBar().value())
        finally:
            self._syncing = False

    def _emit_changed(self):
        if not self._syncing:
            self.viewChanged.emit()

    # --- events -----------------------------------------------------------
    def wheelEvent(self, event):
        self.zoom_by(1.25 if event.angleDelta().y() > 0 else 0.8)

    def mouseDoubleClickEvent(self, event):
        self.fit_to_window()

    def scrollContentsBy(self, dx, dy):
        super().scrollContentsBy(dx, dy)
        self._emit_changed()

    def mouseMoveEvent(self, event):
        super().mouseMoveEvent(event)
        if self.has_image():
            pos = self._item.mapFromScene(self.mapToScene(event.position().toPoint()))
            x, y = int(pos.x()), int(pos.y())
            pm = self._item.pixmap()
            if 0 <= x < pm.width() and 0 <= y < pm.height():
                self.pixelHovered.emit(x, y)
                return
        self.pixelHovered.emit(-1, -1)

    def leaveEvent(self, event):
        super().leaveEvent(event)
        self.pixelHovered.emit(-1, -1)
