#include "ShotCapture.hpp"
#include "MacShotSupport.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QWidget>
#include <cmath>

namespace ShotCapture {

namespace {

QRect globalRect(const QWidget* w) {
    return QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
}

bool runScreencapture(const QStringList& args, const QString& file, QString* error) {
#ifdef Q_OS_MACOS
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile::remove(file);
    QProcess proc;
    proc.start(QStringLiteral("/usr/sbin/screencapture"), QStringList(args) << file);
    if (!proc.waitForFinished(15000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 ||
        !QFileInfo(file).isFile() || QFileInfo(file).size() == 0) {
        *error = QStringLiteral("screencapture failed (%1). The app launching this run needs the Screen "
                                "Recording permission (System Settings > Privacy & Security).")
                     .arg(QString::fromLocal8Bit(proc.readAllStandardError()).trimmed());
        return false;
    }
    return true;
#else
    Q_UNUSED(args);
    Q_UNUSED(file);
    *error = QStringLiteral("'method: system' is only available on macOS");
    return false;
#endif
}

} // namespace

QList<QWidget*> visibleTransients(const QWidget* main) {
    QList<QWidget*> out;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w == main || !w->isVisible()) continue;
        const Qt::WindowType type = w->windowType();
        if (type == Qt::Popup || type == Qt::Dialog || type == Qt::Sheet || type == Qt::Tool ||
            qobject_cast<QDialog*>(w))
            out.append(w);
    }
    return out;
}

QRect compositeRect(const QWidget* target, const QList<QWidget*>& extras, int padding) {
    QRect area = globalRect(target);
    for (const QWidget* w : extras) area |= globalRect(w);
    return area.adjusted(-padding, -padding, padding, padding);
}

QImage renderComposite(QWidget* target, const QList<QWidget*>& extras, int padding, double scale) {
    const QRect area = compositeRect(target, extras, padding);

    QImage image(QSize(static_cast<int>(std::ceil(area.width() * scale)),
                       static_cast<int>(std::ceil(area.height() * scale))),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    auto draw = [&](QWidget* w) {
        w->render(&painter, globalRect(w).topLeft() - area.topLeft(), QRegion(),
                  QWidget::DrawWindowBackground | QWidget::DrawChildren);
    };
    draw(target);
    // Stacking order as listed by Qt: the popup opened last ends up on top.
    for (QWidget* w : extras) draw(w);
    painter.end();
    return image;
}

bool systemCaptureWindow(const QWidget* widget, const QString& file, QString* error) {
#ifdef Q_OS_MACOS
    const long number = macWindowNumber(widget);
    if (number == 0) {
        *error = QStringLiteral("the window has no on-screen window number");
        return false;
    }
    return runScreencapture({QStringLiteral("-x"), QStringLiteral("-o"), QStringLiteral("-l%1").arg(number)}, file,
                            error);
#else
    Q_UNUSED(widget);
    return runScreencapture({}, file, error);
#endif
}

bool systemCaptureRect(const QRect& r, const QString& file, QString* error) {
    if (r.isEmpty()) {
        *error = QStringLiteral("nothing to capture (empty screen region)");
        return false;
    }
    return runScreencapture({QStringLiteral("-x"), QStringLiteral("-R%1,%2,%3,%4")
                                                       .arg(r.x())
                                                       .arg(r.y())
                                                       .arg(r.width())
                                                       .arg(r.height())},
                            file, error);
}

bool savePng(const QImage& image, const QString& file, QString* error) {
    if (image.isNull()) {
        *error = QStringLiteral("nothing to capture (empty image)");
        return false;
    }
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!image.save(file, "PNG")) {
        *error = QStringLiteral("could not write %1").arg(file);
        return false;
    }
    return true;
}

} // namespace ShotCapture
