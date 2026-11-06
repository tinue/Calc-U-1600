#pragma once
#include <QString>
#include <Qt>
#include <optional>

#include "Shared/HostKeyMap.hpp"

// Qt adapter for Shared/HostKeyMap: turns a Qt key event's parts into a
// shared::HostKey + HostModifiers + typed character and resolves it.
namespace PC1500KeyboardMap {

using shared::ResolvedKey;

// `key`/`modifiers`/`text` are taken as plain values (not QKeyEvent*) so
// this stays a small, directly testable function.
//
// `nativeVirtualKey` is QKeyEvent::nativeVirtualKey() -- only consulted on
// macOS, for keys Qt has no Qt::Key for (a PC keyboard's Scroll Lock).
std::optional<ResolvedKey> resolve(Qt::Key key, Qt::KeyboardModifiers modifiers,
                                    const QString& text, bool isPC1600,
                                    quint32 nativeVirtualKey = 0);

} // namespace PC1500KeyboardMap
