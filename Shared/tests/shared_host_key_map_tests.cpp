// Shared/HostKeyMap.

#include <cstdio>

#include "Shared/HostKeyMap.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

using shared::HostKey;
using shared::HostModifiers;
using shared::resolveHostKey;

HostModifiers shift() { HostModifiers m; m.shift = true; return m; }

bool is(const std::optional<shared::ResolvedKey>& r, const char* key, bool shifted = false, bool kbii = false) {
    return r && r->baseKey == key && r->needsShift == shifted && r->needsKbii == kbii;
}

void test_special_keys() {
    for (bool mac : {false, true}) {
        CHECK(is(resolveHostKey(HostKey::Backspace, {}, 0, false, mac), "left"));
        CHECK(is(resolveHostKey(HostKey::Backspace, {}, 0, true, mac), "bs"));
        CHECK(is(resolveHostKey(HostKey::F3, {}, 0, false, mac), "f3"));
        CHECK(is(resolveHostKey(HostKey::Return, {}, '\r', false, mac), "enter"));
        CHECK(is(resolveHostKey(HostKey::Tab, {}, '\t', false, mac), "mode"));
        CHECK(is(resolveHostKey(HostKey::Delete, shift(), 0, false, mac), "cl", true));
        CHECK(is(resolveHostKey(HostKey::Home, shift(), 0, true, mac), "ctrl"));
        CHECK(is(resolveHostKey(HostKey::Home, shift(), 0, false, mac), "rcl"));
        CHECK(is(resolveHostKey(HostKey::End, shift(), 0, true, mac), "kbii"));
        CHECK(is(resolveHostKey(HostKey::End, {}, 0, true, mac), "sml"));
        CHECK(is(resolveHostKey(HostKey::PageUp, {}, 0, false, mac), "left", true));
        CHECK(is(resolveHostKey(HostKey::PageUp, shift(), 0, false, mac), "def"));
        CHECK(is(resolveHostKey(HostKey::PageDown, {}, 0, false, mac), "right", true));
        CHECK(is(resolveHostKey(HostKey::ScrollLock, {}, 0, false, mac), "rsv"));
    }
}

void test_characters() {
    CHECK(is(resolveHostKey(HostKey::Other, {}, 'A', false, true), "a"));
    CHECK(is(resolveHostKey(HostKey::Other, {}, 'z', false, true), "z"));
    CHECK(is(resolveHostKey(HostKey::Other, {}, '7', false, true), "7"));
    CHECK(is(resolveHostKey(HostKey::Other, {}, ' ', false, true), "space"));
    CHECK(is(resolveHostKey(HostKey::Other, {}, '+', false, true), "+"));
    CHECK(resolveHostKey(HostKey::Other, {}, 0, false, true) == std::nullopt);
    // A second-legend punctuation character: shifted base key.
    const auto q = resolveHostKey(HostKey::Other, {}, '"', false, true);
    CHECK(q && q->needsShift && !q->baseKey.empty());
    // PC-1600 accents go through KBII; the PC-1500 has none.
    const auto e = resolveHostKey(HostKey::Other, {}, U'é', true, true);
    CHECK(e && e->needsKbii);
    CHECK(resolveHostKey(HostKey::Other, {}, U'é', false, true) == std::nullopt);
}

void test_modifiers() {
    HostModifiers cmd; cmd.command = true;
    HostModifiers ctrl; ctrl.control = true;
    HostModifiers alt; alt.alt = true;
    HostModifiers altGr; altGr.control = true; altGr.alt = true;
    HostModifiers meta; meta.meta = true;
    // macOS: Cmd and Control never type; Option does.
    CHECK(resolveHostKey(HostKey::Other, cmd, 'a', false, true) == std::nullopt);
    CHECK(resolveHostKey(HostKey::Other, ctrl, 'a', false, true) == std::nullopt);
    CHECK(is(resolveHostKey(HostKey::Other, alt, 'a', false, true), "a"));
    // Elsewhere: Control or Alt alone and the Windows key never type; AltGr does.
    CHECK(resolveHostKey(HostKey::Other, ctrl, 'a', false, false) == std::nullopt);
    CHECK(resolveHostKey(HostKey::Other, alt, 'a', false, false) == std::nullopt);
    CHECK(resolveHostKey(HostKey::Other, meta, 'a', false, false) == std::nullopt);
    CHECK(is(resolveHostKey(HostKey::Other, altGr, '+', false, false), "+"));
}

void test_repeat_keys() {
    CHECK(resolveHostKey(HostKey::Backspace, {}, 0, false, false)->isPc1500RepeatKey());
    CHECK(!resolveHostKey(HostKey::Return, {}, 0, false, false)->isPc1500RepeatKey());
}

} // namespace

int run_shared_host_key_map_tests() {
    test_special_keys();
    test_characters();
    test_modifiers();
    test_repeat_keys();
    std::printf("shared_host_key_map_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
