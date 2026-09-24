#pragma once
#include <cstdint>
#include <string>
#include <vector>

class PC1600Machine;

// ── Keystroke-injection primitives for scripted PC-1600 input ───────────
//
// The PC-1600 analog of Core/PC1500/PC1500BasicTyper. Shared by the
// preset loader's `type:` steps and its `program:` (basic-text) sections
// (PC1600PresetLoader.cpp) -- both drive the ROM's own line editor via
// simulated keystrokes and let the ROM store the line.
//
// How the PC-1600 program path works (confirmed against the real ROM set
// and hardware testing):
//
//   * The PC-1600's BASIC keyword-tokenizes on ENTER, like the PC-1500's,
//     and the program-end pointer BASPRG_END lives at the same place --
//     SC-7852-view F867H, big-endian (PC-1600-Work-Area-Map.md: "Block C
//     is used exactly as on the PC-1500/1500A"). It advances by each
//     stored line's size. typeBasicProgramText() waits for it to settle
//     after each line and uses "did the program change?" (BASPRG_END or
//     the program bytes -- a same-length replacement moves only the
//     latter) to tell a stored line from an unstored one.
//   * BUT F867 only moves in PRO mode. After ALL RESET the machine is in
//     RUN mode, where a typed line is a direct command, not stored.
//     Getting into PRO mode is the PRESET AUTHOR's job (a `key: mode` step
//     before the `program:` block). typeBasicProgramText() assumes PRO +
//     ready, types the lines, leaves the machine in PRO mode, and does NOT
//     clear a resident program (put `type: NEW` before the block for a
//     clean load). If it sees no line get stored at all it reports that
//     as a probable "not in PRO mode".
//   * The editor's line-length check is on the RAW typed line (keyword
//     tokenization shrinks what's stored, but doesn't change what the
//     editor will accept), so an over-length line is caught up front by
//     kMaxBasicLineLength before any keystroke rather than typed and
//     truncated.
//
// Case: PC-1600 `type:` is case-sensitive. A lowercase letter is typed as
// a SHIFT-tap before its uppercase base key -- SHIFT is a one-shot latch
// (tap, don't hold; the next key consumes it), the same mechanism the
// digit-row second legends and the shared punctuation table use. This is
// NOT the SML lock (a separate persistent lowercase/kana toggle).

/// The longest source line typeBasicProgramText() will send. A longer line
/// is collected as rejected without typing it (the editor would truncate
/// and store it wrong). 79 matches the classic Sharp BASIC editor limit;
/// pending a hardware-confirmed PC-1600 figure it is deliberately
/// generous -- an over-limit line just means the preset author splits it.
inline constexpr int kMaxBasicLineLength = 79;

/// Presses and releases the named key (PC1600Keyboard's vocabulary),
/// pacing the CPU through a real keystroke's hold/idle cycle. An unmapped
/// name is silently a no-op, matching PC1600Machine::pressKey/releaseKey.
void tapKey(PC1600Machine& machine, const std::string& name);

/// Steps the CPU forward, a 60 Hz frame at a time, while the LCD's BUSY
/// status symbol is lit or `maxTStates` is exhausted (a safety cap).
/// Returns the T-states actually consumed.
uint64_t waitIdle(PC1600Machine& machine, uint64_t maxTStates);

/// Runs frames until a running BASIC program / plot has finished -- the
/// SC7852 is back spinning in the BASIC command loop (~$92B3, held for a
/// third of a second) -- or `maxTStates` is consumed. Returns the T-states
/// consumed. A parameterless `- wait:` preset step uses this with a
/// generous cap. NB: an INPUT wait / plot pen-move / FOR-NEXT delay are
/// each their own tight loop elsewhere, so this deliberately checks the
/// command-loop address, not just "small stable PC span".
uint64_t waitUntilBasicIdle(PC1600Machine& machine, uint64_t maxTStates);

/// Runs frames until the ROM's keyboard-scan idle loop is sweeping the key
/// matrix again -- i.e. it has returned from boot / line tokenisation /
/// peripheral work and will actually register the next keystroke: CK0, the
/// LCD clock (on only once the machine is fully up), together with the
/// SC7852 sitting in the BASIC command loop (`pc1600AtBasicPrompt`) or
/// reading the key matrix (`PC1600Keyboard::scanCount()` advancing), held
/// for ~0.25 s. Cap-bounded so a stuck ROM can't hang the caller.
///
/// **No-op unless a plotter is attached** (CE-1600P *or* CE-150). Without
/// one the ROM re-engages the loop fast enough that the existing BUSY /
/// program-pointer settles already cover it. The CE-1600P's power-on init
/// (LCD dark until it finishes) and post-line housekeeping open the gap on
/// the SC7852 side; the CE-150 opens it via the boot's plotter self-test,
/// which hands off to the LH5803 to home the colour turret.
void waitForKeyboardScanLoop(PC1600Machine& machine);

/// Runs a machine that was just reset flat out through its boot until it
/// waits at the prompt for keys: a fixed 2 s past the ROM's power-on
/// sequence, then the BUSY tail (waitIdle, 5 s cap), then -- with a plotter
/// attached -- the peripheral's power-on init (waitForKeyboardScanLoop).
/// Used by the preset loader and by the GUI's Reset.
void runBootToPrompt(PC1600Machine& machine);

/// Types `line` character by character (a lowercase letter and the
/// shifted punctuation / digit-row symbols each get a SHIFT-tap first),
/// optionally tapping ENTER afterward, then a short settle. Returns false
/// only for a character with no PC-1600 key (`error` names it).
bool typeLine(PC1600Machine& machine, const std::string& line, bool pressEnter, std::string* error);

struct PC1600BasicTypeResult {
    bool ok = false;
    std::vector<std::string> rejectedLines; // lines too long for the editor (see kMaxBasicLineLength)
    std::string error; // set when ok is false for a reason other than a rejection (e.g. an unmapped character)
};

/// Types a whole BASIC program's source text through the ROM's own line
/// editor, one line at a time -- assumes the machine is already in PRO
/// mode and ready (see the file comment). Blank source lines are skipped.
/// A line longer than kMaxBasicLineLength is put in `rejectedLines`
/// without being typed; a line that is typed but doesn't advance
/// BASPRG_END (F867) -- i.e. wasn't stored, typically because the machine
/// isn't in PRO mode -- is put there too. Does not clear a resident
/// program and does not change mode.
PC1600BasicTypeResult typeBasicProgramText(PC1600Machine& machine, const std::string& text);
