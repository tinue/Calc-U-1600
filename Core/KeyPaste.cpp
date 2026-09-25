#include "KeyPaste.hpp"

std::vector<PasteStep> buildPasteSteps(const std::string& text, TypedCharResolver resolve) {
    // Only the first line is typed, and never entered: the paste stops at
    // the first line break (CR or LF), so nothing executes on its own.
    const std::string firstLine = text.substr(0, text.find_first_of("\r\n"));

    std::vector<PasteStep> steps;
    for (char c : firstLine) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc >= 0x7F) continue; // tabs, other controls, UTF-8 bytes
        PasteStep step;
        if (!resolve(c, &step.key, &step.needsShift)) continue;
        steps.push_back(step);
    }
    return steps;
}

void KeyPasteFeeder::append(const std::vector<PasteStep>& steps) {
    auto tap = [this](const std::string& key) {
        Action a;
        a.kind = Action::Kind::Tap;
        a.key = key;
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
        if (step.needsShift) {
            // SHIFT is a one-shot latch: tapped, not held, and consumed by
            // the next key -- never explicitly un-latched afterward.
            tap("shift");
            wait(m_pacing.shiftGapFrames);
        }
        tap(step.key);
    }
}

void KeyPasteFeeder::cancel(const KeyFn& release) {
    if (m_hasCurrent && m_current.kind == Action::Kind::Tap && m_tapPhase == TapPhase::Hold && release) {
        release(m_current.key);
    }
    m_hasCurrent = false;
    m_queue.clear();
}

void KeyPasteFeeder::begin(const Action& action, const KeyFn& press) {
    m_current = action;
    m_hasCurrent = true;
    switch (action.kind) {
        case Action::Kind::Tap:
            press(action.key);
            m_tapPhase = TapPhase::Hold;
            m_framesLeft = m_pacing.tapFrames;
            break;
        case Action::Kind::Wait:
            m_framesLeft = action.frames;
            break;
    }
}

bool KeyPasteFeeder::elapse(const KeyFn& release) {
    switch (m_current.kind) {
        case Action::Kind::Tap:
            if (--m_framesLeft > 0) return false;
            if (m_tapPhase == TapPhase::Hold) {
                release(m_current.key);
                m_tapPhase = TapPhase::Gap;
                m_framesLeft = m_pacing.gapFrames;
                return m_framesLeft <= 0;
            }
            return true;
        case Action::Kind::Wait:
            return --m_framesLeft <= 0;
    }
    return true;
}

void KeyPasteFeeder::onFrame(const KeyFn& press, const KeyFn& release) {
    if (m_hasCurrent) {
        if (!elapse(release)) return;
        m_hasCurrent = false;
    }
    if (m_queue.empty()) return;
    // Start the next action at this frame boundary. Only a Tap/Wait with
    // time left can be current, so one begin() suffices.
    const Action next = m_queue.front();
    m_queue.pop_front();
    begin(next, press);
}
