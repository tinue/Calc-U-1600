#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "MachineController.hpp"
#include "Shared/TapeBay.hpp"

// The cassette bay on the control bar: the session's shared::TapeBay (Shared/TapeBay.hpp
// has the rules) with Qt signals and QStrings.
class TapeManager : public QObject {
    Q_OBJECT
public:
    explicit TapeManager(MachineController* controller, QObject* parent = nullptr);

    QStringList tapeNames() const;
    QString currentName() const { return QString::fromStdString(m_bay->currentName()); }
    bool exists(const QString& name) const { return m_bay->exists(name.toStdString()); }
    bool selectForPlay(const QString& nameOrEmpty, QString* error);
    bool recordNew(const QString& name, QString* error);

signals:
    void tapesChanged();
    void errorMessage(QString text);

private:
    shared::TapeBay* m_bay;  // the session's
};
