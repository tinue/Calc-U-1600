#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Tape/TapeDeck.hpp"

namespace shared {

class AppPaths;
class MachineSession;

// ── TapeBay: the cassette bay on the control bar ─────────────────────────
//
// Which tapes there are and what is in the recorder -- the tape
// counterpart to the floppy bay. Tapes are the WAVs in AppPaths::tapeDir().
//
// A tape's role is fixed when it goes in: one picked from the list plays
// (CLOAD), one made with recordNew() records (CSAVE) and is never played
// back from the bay until it is picked again. The recorder itself (Core
// TapeDeck) saves a recording each time the motor stops, so a tape only has
// to be taken out to un-arm it. What's in the bay is read back from the
// recorder (its path), so a machine rebuild that ejects it needs no
// bookkeeping here.
class TapeBay {
public:
    struct Events {
        // A recording was just saved (its file may be new): relist.
        std::function<void()> tapesChanged;
        // A failed automatic save.
        std::function<void(const std::string&)> error;
    };

    TapeBay(MachineSession& session, const AppPaths& paths, Events events);

    /// The tapes in the folder: "<name>.wav" files, sorted case-
    /// insensitively, without ".wav".
    std::vector<std::string> tapeNames() const;
    /// The tape picker's list: tapeNames() plus the tape in the bay if it
    /// has no file yet (a recording before its first save).
    std::vector<std::string> pickerNames() const;
    /// The tape in the bay ("" for none), whichever role.
    std::string currentName() const;
    bool exists(const std::string& name) const;

    /// Puts `name` in to play, rewound; "" takes the tape out. False with a
    /// reason if the WAV can't be read.
    bool selectForPlay(const std::string& nameOrEmpty, std::string* error);
    /// Puts a blank tape in to record into "<name>.wav" (replacing a tape of
    /// that name when the first CSAVE stops the motor). False with a
    /// reason if the name is empty (after trimming).
    bool recordNew(const std::string& name, std::string* error);

    /// Once per frame: reports a failed automatic save, and tapesChanged
    /// when a recording has just been saved.
    void onFrameTick(const TapeDeck::Status& status);

private:
    MachineSession& m_session;
    const AppPaths& m_paths;
    Events m_events;
    bool m_recordingMotor = false;  // the motor was running on a recording
};

} // namespace shared
