#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "MachineController.hpp"

// The cassette bay on the control bar: which tapes there are and what is in
// the recorder -- the tape counterpart to FloppyDiskManager. Tapes are the
// WAVs in AppPaths::tapeDir() (Settings ▸ Storage ▸ Tapes).
//
// A tape's role is fixed when it goes in: one picked from the list plays
// (CLOAD), one made with Save records (CSAVE) and is never played back from
// the bay until it is picked again. The recorder itself (Core TapeDeck)
// saves a recording each time the motor stops, so a tape only has to be
// taken out ("–empty–" or another pick) to un-arm it. What's in the bay is
// read back from the recorder (its path), so a machine rebuild that ejects
// it needs no bookkeeping here.
class TapeManager : public QObject {
    Q_OBJECT
public:
    explicit TapeManager(MachineController* controller, QObject* parent = nullptr);

    /// The tapes in the folder: "<name>.wav" files, sorted, without ".wav".
    QStringList tapeNames() const;
    /// The tape in the bay ("" for none), whichever role.
    QString currentName() const;
    bool exists(const QString& name) const;

    /// Puts `name` in to play, rewound; "" takes the tape out. False with a
    /// reason if the WAV can't be read.
    bool selectForPlay(const QString& nameOrEmpty, QString* error);
    /// Puts a blank tape in to record into "<name>.wav" (replacing a tape of
    /// that name when the first CSAVE stops the motor). False if the name
    /// is empty.
    bool recordNew(const QString& name, QString* error);

    /// Once per frame: reports a failed automatic save, and tapesChanged()
    /// when a recording has just been saved (its file may be new).
    void onFrameTick();

signals:
    void tapesChanged();
    void errorMessage(QString text);

private:
    MachineController* m_controller;
    bool m_recordingMotor = false; // the motor was running on a recording
};
