#include "KeyPaste.hpp"

#include "Utf8.hpp"

std::vector<PasteStep> buildPasteSteps(const std::string& text, TypedCharResolver resolve,
                                       KbiiCharResolver resolveKbii) {
    // Only the first line is typed, and never entered: the paste stops at
    // the first line break (CR or LF), so nothing executes on its own.
    const std::string firstLine = text.substr(0, text.find_first_of("\r\n"));

    std::vector<PasteStep> steps;
    for (std::size_t i = 0; i < firstLine.size();) {
        char32_t cp = 0;
        if (!decodeUtf8(firstLine, i, cp)) continue; // malformed byte
        if (cp < 0x20 || cp == 0x7F) continue;       // tabs, other controls
        PasteStep step;
        if (cp < 0x80) {
            if (!resolve(static_cast<char>(cp), &step.key, &step.needsShift)) continue;
        } else {
            if (!resolveKbii || !resolveKbii(cp, &step.key, &step.needsShift)) continue;
            step.needsKbii = true;
        }
        steps.push_back(step);
    }
    return steps;
}

void KeyPasteFeeder::append(const std::vector<PasteStep>& steps) {
    auto tap = [this](const std::string& key, bool closesKbii = false) {
        Action a;
        a.kind = Action::Kind::Tap;
        a.key = key;
        a.closesKbii = closesKbii;
        m_queue.push_back(a);
    };
    auto wait = [this](int frames) {
        if (frames <= 0) return;
        Action a;
        a.kind = Action::Kind::Wait;
        a.frames = frames;
        m_queue.push_back(a);
    };
    for (const PasteStep& step : steps) {
        if (step.needsKbii) {
            // Expanded only when it's next (expandKbiiChar()): what it takes
            // depends on what is latched on the machine by then.
            Action a;
            a.kind = Action::Kind::KbiiChar;
            a.key = step.key;
            a.shift = step.needsShift;
            m_queue.push_back(a);
            continue;
        }
        if (step.needsShift) {
            // SHIFT is a one-shot latch: tapped, not held, and consumed by
            // the next key -- never explicitly un-latched afterward.
            tap("shift");
            wait(m_pacing.shiftGapFrames);
        }
        tap(step.key);
    }
}

void KeyPasteFeeder::expandKbiiChar(const KeyLatches& latched) {
    const Action ch = m_queue.front();
    m_queue.pop_front();
    const std::vector<KbiiTap> taps = kbiiSequence(ch.key, ch.shift, latched);
    std::deque<Action> seq;
    for (std::size_t i = 0; i < taps.size(); ++i) {
        Action a;
        a.kind = Action::Kind::Tap;
        a.key = taps[i].key;
        a.closesKbii = i + 1 == taps.size();
        seq.push_back(a);
        if (taps[i].gapAfter && m_pacing.shiftGapFrames > 0) {
            Action w;
            w.kind = Action::Kind::Wait;
            w.frames = m_pacing.shiftGapFrames;
            seq.push_back(w);
        }
    }
    m_queue.insert(m_queue.begin(), seq.begin(), seq.end());
}

void KeyPasteFeeder::cancel(const KeyFn& release, bool finishKbii) {
    const bool holdingOpeningKbii = m_hasCurrent && m_current.kind == Action::Kind::Tap &&
                                    m_current.key == "kbii" && !m_current.closesKbii &&
                                    m_tapPhase == TapPhase::Hold;
    if (finishKbii && (m_kbiiLatched || holdingOpeningKbii)) {
        // Stopped inside an accented character. Finish its KBII sequence:
        // an opening KBII tap still held runs to the end (a cut-short tap
        // may or may not have been scanned), then only the closing tap
        // follows, so the next key typed isn't turned into an accented one.
        m_queue.clear();
        if (!holdingOpeningKbii) {
            if (m_hasCurrent && m_current.kind == Action::Kind::Tap && m_tapPhase == TapPhase::Hold && release) {
                release(m_current.key);
            }
            m_hasCurrent = false;
        }
        Action close;
        close.kind = Action::Kind::Tap;
        close.key = "kbii";
        close.closesKbii = true;
        m_queue.push_back(close);
        return;
    }
    if (m_hasCurrent && m_current.kind == Action::Kind::Tap && m_tapPhase == TapPhase::Hold && release) {
        release(m_current.key);
    }
    m_hasCurrent = false;
    m_queue.clear();
    m_kbiiLatched = false;
}

void KeyPasteFeeder::begin(const Action& action, const KeyFn& press) {
    m_current = action;
    m_hasCurrent = true;
    switch (action.kind) {
        case Action::Kind::Tap:
            press(action.key);
            if (action.closesKbii) m_kbiiLatched = false;
            m_tapPhase = TapPhase::Hold;
            m_framesLeft = m_pacing.tapFrames;
            break;
        case Action::Kind::Wait:
            m_framesLeft = action.frames;
            break;
        case Action::Kind::KbiiChar:
            break; // expanded before it can start (onFrame())
    }
}

bool KeyPasteFeeder::elapse(const KeyFn& release) {
    switch (m_current.kind) {
        case Action::Kind::Tap:
            if (--m_framesLeft > 0) return false;
            if (m_tapPhase == TapPhase::Hold) {
                release(m_current.key);
                if (m_current.key == "kbii" && !m_current.closesKbii) m_kbiiLatched = true;
                m_tapPhase = TapPhase::Gap;
                m_framesLeft = m_pacing.gapFrames;
                return m_framesLeft <= 0;
            }
            return true;
        case Action::Kind::Wait:
            return --m_framesLeft <= 0;
        case Action::Kind::KbiiChar:
            break;
    }
    return true;
}

void KeyPasteFeeder::onFrame(const KeyFn& press, const KeyFn& release, const LatchFn& latches) {
    if (m_hasCurrent) {
        if (!elapse(release)) return;
        m_hasCurrent = false;
    }
    while (!m_queue.empty() && m_queue.front().kind == Action::Kind::KbiiChar) {
        expandKbiiChar(latches ? latches() : KeyLatches{});
    }
    if (m_queue.empty()) return;
    // Start the next action at this frame boundary. Only a Tap/Wait with
    // time left can be current, so one begin() suffices.
    const Action next = m_queue.front();
    m_queue.pop_front();
    begin(next, press);
}
