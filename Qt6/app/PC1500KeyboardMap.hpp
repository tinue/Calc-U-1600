#pragma once
#include <QString>
#include <Qt>
#include <optional>
#include <string>

// Physical (host) keyboard -> calculator key-name resolution. Shared by all
// three models -- the PC-1600 divergence lives entirely in `isPC1600`'s one
// Backspace branch; the bigger PC-1600 divergence, "every key uses real
// held press/release," is already how this whole app's press path works
// for every model, so it needs no branch here at all -- see MainWindow's
// own key-event handling.
namespace PC1500KeyboardMap {

struct ResolvedKey {
    std::string baseKey;
    bool needsShift = false;

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

// `key`/`modifiers`/`text` are taken as plain values (not QKeyEvent*) so
// this stays a small, dependency-free, directly testable function.
//
// Command modifiers never type: a key held with Cmd or Ctrl (macOS), or
// with Ctrl, Alt or the Windows/Super key (Linux/Windows), resolves to
// nullopt -- so a shortcut that no action claims (or one that reached us
// anyway) can't put a character on the display. Character-producing
// modifiers still type: Option on macOS, and AltGr on Linux/Windows
// (Windows reports it as Ctrl+Alt together, so that pair is let through).
// Anything else that doesn't match a case below falls through to nullopt
// via the matchers themselves.
std::optional<ResolvedKey> resolve(Qt::Key key, Qt::KeyboardModifiers modifiers,
                                    const QString& text, bool isPC1600);

} // namespace PC1500KeyboardMap
