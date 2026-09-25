#include "FaceplateWidget.hpp"
#include "LcdWidget.hpp"
#include "keylayout/PC1500KeyLayout.hpp"
#include "keylayout/PC1600KeyLayout.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QResizeEvent>

FaceplateWidget::FaceplateWidget(QWidget* parent) : QWidget(parent) {
    m_lcd = new LcdWidget(this);
    setModel(Model::PC1500A);
}

void FaceplateWidget::setModel(Model model) {
    switch (model) {
    case Model::PC1500:
        m_pixmap.load(":/faceplates/pc1500.png");
        m_layout = &KeyLayout::pc1500Layout();
        break;
    case Model::PC1500A:
        m_pixmap.load(":/faceplates/pc1500A.png");
        m_layout = &KeyLayout::pc1500Layout();
        break;
    case Model::PC1600:
        m_pixmap.load(":/faceplates/pc1600.png");
        m_layout = &KeyLayout::pc1600Layout();
        break;
    }
    m_lcd->setModel(model);
    m_pressedKey.clear();
    m_pressedIndex = -1;
    updateGeometry();
    relayout();
    update();
}

int FaceplateWidget::heightForWidth(int w) const {
    if (!m_layout || m_layout->imageWidth <= 0) return w;
    return static_cast<int>(w * (m_layout->imageHeight / m_layout->imageWidth));
}

QSize FaceplateWidget::sizeHint() const {
    const int w = 800;
    return QSize(w, heightForWidth(w));
}

void FaceplateWidget::relayout() {
    if (!m_layout || width() <= 0 || height() <= 0) {
        m_fittedRect = QRect();
        m_keyRects.clear();
        return;
    }

    // Letterbox-fit: largest centered rect preserving the model's aspect
    // ratio within the widget's current size.
    const double imageAspect = m_layout->imageWidth / m_layout->imageHeight;
    const double widgetAspect = static_cast<double>(width()) / height();

    int fitW, fitH;
    if (widgetAspect > imageAspect) {
        fitH = height();
        fitW = static_cast<int>(fitH * imageAspect);
    } else {
        fitW = width();
        fitH = static_cast<int>(fitW / imageAspect);
    }
    const int x0 = (width() - fitW) / 2;
    const int y0 = (height() - fitH) / 2;
    m_fittedRect = QRect(x0, y0, fitW, fitH);

    const auto toWidgetRect = [this](const KeyLayout::KeyRect& r) {
        const double fx = r.x / m_layout->imageWidth;
        const double fy = r.y / m_layout->imageHeight;
        const double fw = r.w / m_layout->imageWidth;
        const double fh = r.h / m_layout->imageHeight;
        return QRect(m_fittedRect.x() + static_cast<int>(fx * m_fittedRect.width()),
                     m_fittedRect.y() + static_cast<int>(fy * m_fittedRect.height()),
                     static_cast<int>(fw * m_fittedRect.width()),
                     static_cast<int>(fh * m_fittedRect.height()));
    };

    m_keyRects.clear();
    m_keyRects.reserve(m_layout->keyCount);
    for (std::size_t i = 0; i < m_layout->keyCount; ++i) {
        m_keyRects.push_back(toWidgetRect(m_layout->keys[i]));
    }

    m_lcd->setGeometry(toWidgetRect(m_layout->lcdRect));
}

void FaceplateWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (!m_pixmap.isNull() && m_fittedRect.isValid()) {
        // Without this, scaling the 4000px source art down to the widget's
        // size uses a fast/non-smooth transform -- most visible on
        // non-integer display scale factors (e.g. Windows' common 125%/
        // 150%), where it reads noticeably softer/rougher than a smooth
        // scale would.
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(m_fittedRect, m_pixmap);
    }

    // Press-visual-feedback flash: a translucent white overlay on the
    // pressed key's own hit-rect.
    if (m_pressedIndex >= 0 && m_pressedIndex < static_cast<int>(m_keyRects.size())) {
        painter.fillRect(m_keyRects[static_cast<std::size_t>(m_pressedIndex)], QColor(255, 255, 255, 64));
    }
}

void FaceplateWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

int FaceplateWidget::hitTest(const QPoint& pos) const {
    for (std::size_t i = 0; i < m_keyRects.size(); ++i) {
        if (m_keyRects[i].contains(pos)) return static_cast<int>(i);
    }
    return -1;
}

void FaceplateWidget::pressIndex(int idx) {
    m_pressedKey = QString::fromUtf8(m_layout->keys[idx].name);
    m_pressedIndex = idx;
    update();
    emit keyPressed(m_pressedKey);
}

void FaceplateWidget::mousePressEvent(QMouseEvent* event) {
    const int idx = hitTest(event->pos());
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
    for (std::size_t i = 0; i < m_layout->keyCount; ++i) {
        if (name == QString::fromUtf8(m_layout->keys[i].name)) {
            releasePressedKey();
            pressIndex(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

void FaceplateWidget::releasePressedKey() {
    if (m_pressedKey.isEmpty()) return;
    emit keyReleased(m_pressedKey);
    m_pressedKey.clear();
    m_pressedIndex = -1;
    update();
}
