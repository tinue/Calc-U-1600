#pragma once
#include <QWidget>
#include <QColor>
#include "MachineController.hpp"

// Dot-matrix LCD + status-indicator renderer. PC-1500/1500A: an indicator
// bar above the dot matrix, both sized off the panel's own height.
// PC-1600: a fixed-legend status strip above a 156x32 graphics area, both
// positioned as fractions of the panel's own width/height. Blanks entirely
// while the frame reports !poweredOn rather than freeze-framing the last
// image.
//
// Press-and-hold anywhere on this widget to fast-forward: emits
// turboRequested(true) on press, turboRequested(false) on release (Qt
// implicitly grabs the mouse for the widget that received the press, so the
// release still reaches us even if the pointer drifted off first).
class LcdWidget : public QWidget {
    Q_OBJECT
public:
    explicit LcdWidget(QWidget* parent = nullptr);

    void setModel(Model model);
    void setFrame(const DisplayFrame& frame);

signals:
    void turboRequested(bool active);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    Model m_model = Model::PC1500A;
    DisplayFrame m_frame;

    bool flag(const char* name) const;
    void paintPC1500(class QPainter& painter);
    void paintPC1600(class QPainter& painter);
    void paintDotMatrix(class QPainter& painter, const QRectF& area, const QColor& ink);
};
