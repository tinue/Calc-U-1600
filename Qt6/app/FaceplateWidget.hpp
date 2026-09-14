#pragma once
#include <QWidget>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <vector>

#include "MachineController.hpp"
#include "keylayout/KeyLayoutTypes.hpp"

class LcdWidget;

// Faceplate art + invisible key hit-regions + embedded LCD: a resizable
// widget that keeps the body art's aspect ratio (heightForWidth) and lets
// its child LcdWidget + a cached key-hit-rect table track every resize.
class FaceplateWidget : public QWidget {
    Q_OBJECT
public:
    explicit FaceplateWidget(QWidget* parent = nullptr);

    void setModel(Model model);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
    QSize sizeHint() const override;

    LcdWidget* lcdWidget() const { return m_lcd; }

signals:
    void keyPressed(QString name);
    void keyReleased(QString name);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    const KeyLayout::Layout* m_layout = nullptr;
    QPixmap m_pixmap;
    QRect m_fittedRect; // current letterboxed art rect, widget-local coords
    std::vector<QRect> m_keyRects; // parallel to m_layout->keys, widget-local
    LcdWidget* m_lcd = nullptr;
    QString m_pressedKey;
    int m_pressedIndex = -1; // index into m_keyRects/m_layout->keys, or -1; drives the press-highlight overlay

    void relayout();
    int hitTest(const QPoint& pos) const; // index into m_layout->keys, or -1
};
