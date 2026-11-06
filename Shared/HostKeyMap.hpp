#pragma once
#include <optional>
#include <string>

// Physical (host) keyboard -> calculator key-name resolution, for all three
// models. The frontend translates its own key event into a HostKey plus
// HostModifiers plus the one character the key typed (Qt: Qt::Key and
// QKeyEvent::text(); a browser: KeyboardEvent.key / .code), and gets back
// the calculator key to press.
//
// The PC-1600 divergence lives in `isPC1600`'s few branches; the bigger
// one, "every key uses real held press/release," is already how every
// frontend's press path works for every model, so it needs no branch here.
namespace shared {

// The non-character keys the map knows. Anything else is Other and is
// resolved from its typed character, if any.
enum class HostKey {
    Other,
    Backspace, Return, Enter, Tab, Delete,
    Up, Down, Left, Right,
    F1, F2, F3, F4, F5, F6,
    // Navigation cluster, only on an external/PC-style keyboard. A Mac's
    // Scroll Lock (kVK_ContextualMenu) is reported as ScrollLock. Insert
    // is never reported on a Mac host (it arrives as Help there).
    ScrollLock, Insert, Home, End, PageUp, PageDown,
};

// Modifiers by what the user pressed, not by how a toolkit names them
// (Qt on macOS calls Cmd "Control" and Control "Meta").
struct HostModifiers {
    bool shift = false;
    bool control = false;
    bool alt = false;      // Option on a Mac
    bool command = false;  // Mac Cmd
    bool meta = false;     // Windows / Super key
};

struct ResolvedKey {
    std::string baseKey;
    bool needsShift = false;
    // PC-1600 only: an accented character, typed as KBII, [SHIFT,] baseKey,
    // KBII (KBII latches; see Core/PC1600/PC1600TypedInput.hpp).
    bool needsKbii = false;

    // PC-1500 only: the ROM itself auto-repeats these when held (confirmed
    // on real hardware) -- they must bypass the live-typing keystroke queue
    // and go straight through as a continuous, unmodified press...hold...
    // release, or the queue's fixed tap/gap cadence would chop a physical
    // hold into synthetic taps and the ROM's own repeat could never engage.
    // Backspace resolves to base key "left" on PC-1500, so it's covered
    // here automatically.
    bool isPc1500RepeatKey() const {
        return baseKey == "left" || baseKey == "right" || baseKey == "up" || baseKey == "down";
    }
};

// Command modifiers never type: a key held with Cmd or Control (macHost),
// or with Control, Alt or the Windows/Super key (other hosts), resolves to
// nullopt -- so a shortcut that no action claims can't put a character on
// the display. Character-producing modifiers still type: Option on macOS,
// and AltGr elsewhere (Windows reports it as Control+Alt together, so that
// pair is let through).
//
// `typed` is the one character the key produced (a Unicode code point), 0
// for none or more than one. Only ASCII letters, digits and the Sharp
// punctuation map directly; other characters only as PC-1600 KBII accents.
std::optional<ResolvedKey> resolveHostKey(HostKey key, HostModifiers modifiers, char32_t typed, bool isPC1600,
                                          bool macHost);

} // namespace shared
