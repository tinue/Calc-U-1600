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
// release callbacks and the "back at the BASIC prompt" sample.
//
// Nothing is added and nothing is validated: the text is typed as is, up
// to (not including) its first line break -- ENTER is never pressed, so a
// paste never executes anything. A character with no key is silently
// skipped. (The feeder itself still handles Enter steps -- ENTER, then
// wait for the ROM to finish with the line -- for callers that build them.)

struct PasteStep {
    enum class Kind { Tap, Enter };
    Kind kind = Kind::Tap;
    std::string key;         // (Tap) key name in the machine's vocabulary
    bool needsShift = false; // (Tap) tap SHIFT (a one-shot latch) first
};

using TypedCharResolver = bool (*)(char c, std::string* baseKey, bool* needsShift);

/// Turns pasted text into steps. Only the text before the first line break
/// (CR or LF) is used; control characters, non-ASCII bytes and characters
/// `resolve` has no key for are skipped. Never emits an Enter step.
std::vector<PasteStep> buildPasteSteps(const std::string& text, TypedCharResolver resolve);

/// Per-model cadence, all in emulated 60 Hz frames.
struct PastePacing {
    int tapFrames = 4;              // key held down
    int gapFrames = 4;              // idle after release
    int shiftGapFrames = 0;         // extra idle between the SHIFT tap and its base key
    int postEnterFloorFrames = 12;  // fixed wait after ENTER before sampling the prompt
    int promptHoldFrames = 20;      // at-prompt frames in a row that count as "done"
    int postEnterCapFrames = 300;   // give up waiting after ~5 s (RUN, INPUT, ...)
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

    void setPacing(const PastePacing& pacing) { m_pacing = pacing; }

    /// Queues `steps` behind anything still being typed.
    void append(const std::vector<PasteStep>& steps);

    bool active() const { return m_hasCurrent || !m_queue.empty(); }

    /// Drops everything still queued; releases a key currently held down.
    void cancel(const KeyFn& release);

    /// Call once after every emulated frame. `atPrompt` is this frame's
    /// "ROM is in its BASIC command loop" sample (pc1500AtBasicPrompt /
    /// pc1600AtBasicPrompt); only consulted while waiting after ENTER.
    void onFrame(const KeyFn& press, const KeyFn& release, bool atPrompt);

private:
    struct Action {
        enum class Kind { Tap, Wait, WaitPrompt };
        Kind kind = Kind::Wait;
        std::string key; // Tap
        int frames = 0;  // Wait
    };
    enum class TapPhase { Hold, Gap };

    void begin(const Action& action, const KeyFn& press);
    bool elapse(bool atPrompt, const KeyFn& release); // true once the current action is done

    PastePacing m_pacing;
    std::deque<Action> m_queue;
    bool m_hasCurrent = false;
    Action m_current;
    TapPhase m_tapPhase = TapPhase::Hold;
    int m_framesLeft = 0;
    int m_promptRun = 0;
};
