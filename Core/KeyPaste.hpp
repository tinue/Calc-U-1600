#pragma once
#include <deque>
#include <functional>
#include <string>
#include <vector>

// ── Clipboard paste: host text -> paced live keystrokes ──────────────────
//
// The GUI's Edit > Paste Text types the clipboard's text into the running
// emulator. Unlike PC1500BasicTyper/PC1600BasicTyper (which run the CPU
// themselves, synchronously, for presets), this has to ride along with the
// live, wall-clock-paced emulation loop: KeyPasteFeeder is a small state
// machine the host pokes once per emulated 60 Hz frame (see
// MachineController::advance()). It uses the typers' proven cadence, so a
// paste can't outrun the ROM's key-scan loop and lose characters.
//
// Model-agnostic: the caller supplies the per-model character resolver
// (pc1500ResolveTypedChar / pc1600ResolveTypedChar), pacing, key press /
// release callbacks.
//
// Nothing is added and nothing is validated: the text is typed as is, up
// to (not including) its first line break -- ENTER is never pressed, so a
// paste never executes anything. A character with no key is silently
// skipped.

struct PasteStep {
    std::string key;         // key name in the machine's vocabulary
    bool needsShift = false; // tap SHIFT (a one-shot latch) first
    bool needsKbii = false;  // PC-1600: wrap in KBII taps (latch on, off again)
};

using TypedCharResolver = bool (*)(char c, std::string* baseKey, bool* needsShift);
/// Resolves a non-ASCII character to a KBII key (pc1600ResolveTypedKbiiChar).
using KbiiCharResolver = bool (*)(char32_t cp, std::string* baseKey, bool* needsShift);

/// Turns pasted (UTF-8) text into steps. Only the text before the first
/// line break (CR or LF) is used; control characters, characters `resolve`
/// has no key for, and non-ASCII characters `resolveKbii` has no key for
/// (all of them without one) are skipped.
std::vector<PasteStep> buildPasteSteps(const std::string& text, TypedCharResolver resolve,
                                       KbiiCharResolver resolveKbii = nullptr);

/// SHIFT / KBII as latched on the machine when an accented character's
/// KBII sequence is about to start (pc1600ReadLatches):
///   nothing latched  KBII, [SHIFT,] key, KBII
///   SHIFT            SHIFT (un-latch -- with SHIFT on, the KBII key is the
///                    key-click toggle), then the same
///   KBII (+/- SHIFT) nothing: the character is dropped silently
struct KeyLatches {
    bool shift = false;
    bool kbii = false;
};

/// Per-model cadence, all in emulated 60 Hz frames.
struct PastePacing {
    int tapFrames = 4;              // key held down
    int gapFrames = 4;              // idle after release
    int shiftGapFrames = 0;         // extra idle between the SHIFT tap and its base key
};

/// Same 4+4 frame cadence as PC1500BasicTyper's tapKey(); SHIFT then base
/// key with no extra gap, as its typeLine() does.
inline PastePacing pc1500PastePacing() { return PastePacing{}; }

/// PC1600BasicTyper's cadence: 4+4 frames, plus a 6-frame gap after the
/// SHIFT tap so the key-scan notices and latches it.
inline PastePacing pc1600PastePacing() {
    PastePacing p;
    p.shiftGapFrames = 6;
    return p;
}

class KeyPasteFeeder {
public:
    using KeyFn = std::function<void(const std::string&)>;
    using LatchFn = std::function<KeyLatches()>;

    void setPacing(const PastePacing& pacing) { m_pacing = pacing; }

    /// Queues `steps` behind anything still being typed.
    void append(const std::vector<PasteStep>& steps);

    bool active() const { return m_hasCurrent || !m_queue.empty(); }

    /// Drops everything still queued; releases a key currently held down.
    /// With `finishKbii` (a live keystroke interrupting a paste -- not a
    /// reset, which drops everything), a KBII sequence already under way is
    /// finished instead: an opening KBII tap still held runs to the end and
    /// the closing tap stays queued, so KBII isn't left latched.
    void cancel(const KeyFn& release, bool finishKbii = false);

    /// Call once after every emulated frame. `latches` reports the machine's
    /// SHIFT / KBII latches when an accented character starts (see
    /// KeyLatches); without it nothing counts as latched.
    void onFrame(const KeyFn& press, const KeyFn& release, const LatchFn& latches = {});

private:
    struct Action {
        enum class Kind { Tap, Wait, KbiiChar };
        Kind kind = Kind::Wait;
        std::string key;          // Tap, KbiiChar
        int frames = 0;           // Wait
        bool closesKbii = false;  // Tap: the KBII tap that un-latches a sequence's KBII
        bool shift = false;       // KbiiChar: from the SHIFT+KBII table
    };
    enum class TapPhase { Hold, Gap };

    void begin(const Action& action, const KeyFn& press);
    // Replaces the KbiiChar at the queue's front by its taps, per the
    // machine's latches (KeyLatches) -- or drops it.
    void expandKbiiChar(const KeyLatches& latched);
    bool elapse(const KeyFn& release); // true once the current action is done

    PastePacing m_pacing;
    std::deque<Action> m_queue;
    bool m_hasCurrent = false;
    Action m_current;
    TapPhase m_tapPhase = TapPhase::Hold;
    int m_framesLeft = 0;
    bool m_kbiiLatched = false; // an opening KBII tap has been typed, its closing one not yet
};
