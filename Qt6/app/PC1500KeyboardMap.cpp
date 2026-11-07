#include "PC1500KeyboardMap.hpp"

namespace PC1500KeyboardMap {

namespace {

using shared::HostKey;

HostKey hostKey(Qt::Key key, quint32 nativeVirtualKey) {
#ifdef Q_OS_MACOS
    // macOS reports a PC keyboard's Scroll Lock as kVK_ContextualMenu
    // (0x6E), which Qt has no Qt::Key for (it hands over the raw 0x10
    // character code instead) -- match the physical key code directly.
    if (nativeVirtualKey == 0x6E) return HostKey::ScrollLock;
#else
    (void)nativeVirtualKey;
#endif
    switch (key) {
    case Qt::Key_Backspace: return HostKey::Backspace;
    case Qt::Key_Return: return HostKey::Return;
    case Qt::Key_Enter: return HostKey::Enter;
    case Qt::Key_Tab: return HostKey::Tab;
    case Qt::Key_Delete: return HostKey::Delete;
    case Qt::Key_Up: return HostKey::Up;
    case Qt::Key_Down: return HostKey::Down;
    case Qt::Key_Left: return HostKey::Left;
    case Qt::Key_Right: return HostKey::Right;
    case Qt::Key_F1: return HostKey::F1;
    case Qt::Key_F2: return HostKey::F2;
    case Qt::Key_F3: return HostKey::F3;
    case Qt::Key_F4: return HostKey::F4;
    case Qt::Key_F5: return HostKey::F5;
    case Qt::Key_F6: return HostKey::F6;
    case Qt::Key_ScrollLock: return HostKey::ScrollLock;
#ifndef Q_OS_MACOS
    // On macOS, Insert arrives as Key_Help and stays unmapped.
    case Qt::Key_Insert: return HostKey::Insert;
#endif
    case Qt::Key_Home: return HostKey::Home;
    case Qt::Key_End: return HostKey::End;
    case Qt::Key_PageUp: return HostKey::PageUp;
    case Qt::Key_PageDown: return HostKey::PageDown;
    default: return HostKey::Other;
    }
}

} // namespace

std::optional<ResolvedKey> resolve(Qt::Key key, Qt::KeyboardModifiers modifiers,
                                    const QString& text, bool isPC1600, quint32 nativeVirtualKey) {
    shared::HostModifiers mods;
    mods.shift = modifiers & Qt::ShiftModifier;
    mods.alt = modifiers & Qt::AltModifier;
#ifdef Q_OS_MACOS
    // Qt maps Cmd to ControlModifier and the Control key to MetaModifier.
    mods.command = modifiers & Qt::ControlModifier;
    mods.control = modifiers & Qt::MetaModifier;
    constexpr bool macHost = true;
#else
    mods.control = modifiers & Qt::ControlModifier;
    mods.meta = modifiers & Qt::MetaModifier;
    constexpr bool macHost = false;
#endif
    const char32_t typed = text.size() == 1 ? text.at(0).unicode() : 0;
    return shared::resolveHostKey(hostKey(key, nativeVirtualKey), mods, typed, isPC1600, macHost);
}

} // namespace PC1500KeyboardMap
