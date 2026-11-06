#include "FaceplateWidget.hpp"
#include "LcdWidget.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QResizeEvent>

namespace {
QRect toQRect(const KeyLayout::PaneRect& r) { return QRect(r.x, r.y, r.w, r.h); }
} // namespace

FaceplateWidget::FaceplateWidget(QWidget* parent) : QWidget(parent) {
    m_lcd = new LcdWidget(this);
    setModel(Model::PC1500A);
}

void FaceplateWidget::setModel(Model model) {
    switch (model) {
    case Model::PC1500: m_pixmap.load(":/faceplates/pc1500.png"); break;
    case Model::PC1500A: m_pixmap.load(":/faceplates/pc1500A.png"); break;
    case Model::PC1600: m_pixmap.load(":/faceplates/pc1600.png"); break;
    }
    m_layout = &KeyLayout::layoutFor(model);
    m_lcd->setModel(model);
    m_pressedKey.clear();
    m_pressedIndex = -1;
    updateGeometry();
    relayout();
    update();
}

int FaceplateWidget::heightForWidth(int w) const {
    return m_layout ? KeyLayout::heightForWidth(*m_layout, w) : w;
}

QSize FaceplateWidget::sizeHint() const {
    const int w = 800;
    return QSize(w, heightForWidth(w));
}

void FaceplateWidget::relayout() {
    m_fitted = m_layout ? KeyLayout::fitLayout(*m_layout, width(), height()) : KeyLayout::FittedLayout{};
    if (m_fitted.art.valid()) m_lcd->setGeometry(toQRect(m_fitted.lcd));
}

void FaceplateWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (!m_pixmap.isNull() && m_fitted.art.valid()) {
        // Without this, scaling the 4000px source art down to the widget's
        // size uses a fast/non-smooth transform -- most visible on
        // non-integer display scale factors (e.g. Windows' common 125%/
        // 150%), where it reads noticeably softer/rougher than a smooth
        // scale would.
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(toQRect(m_fitted.art), m_pixmap);
    }

    // Press-visual-feedback flash: a translucent white overlay on the
    // pressed key's own hit-rect.
    if (m_pressedIndex >= 0 && m_pressedIndex < static_cast<int>(m_fitted.keys.size())) {
        painter.fillRect(toQRect(m_fitted.keys[static_cast<std::size_t>(m_pressedIndex)]), QColor(255, 255, 255, 64));
    }
}

void FaceplateWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void FaceplateWidget::pressIndex(int idx) {
    m_pressedKey = QString::fromUtf8(m_layout->keys[idx].name);
    m_pressedIndex = idx;
    update();
    emit keyPressed(m_pressedKey);
}

void FaceplateWidget::mousePressEvent(QMouseEvent* event) {
    const int idx = KeyLayout::hitTest(m_fitted, event->pos().x(), event->pos().y());
    if (idx < 0 || !m_layout) return;
    pressIndex(idx);
}

void FaceplateWidget::mouseReleaseEvent(QMouseEvent*) {
    // Release whatever was pressed regardless of where the mouse ended up --
    // a drag-off-widget release must still clear the key, or it's stuck
    // down in the matrix.
    releasePressedKey();
}

bool FaceplateWidget::pressKeyByName(const QString& name) {
    if (!m_layout) return false;
    const int idx = KeyLayout::keyIndex(*m_layout, name.toUtf8().constData());
    if (idx < 0) return false;
    releasePressedKey();
    pressIndex(idx);
    return true;
}

void FaceplateWidget::releasePressedKey() {
    if (m_pressedKey.isEmpty()) return;
    emit keyReleased(m_pressedKey);
    m_pressedKey.clear();
    m_pressedIndex = -1;
    update();
}
