#include "KeyPaste.hpp"

std::vector<PasteStep> buildPasteSteps(const std::string& text, TypedCharResolver resolve) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            normalized.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            normalized.push_back(text[i]);
        }
    }
    if (!normalized.empty() && normalized.back() == '\n') normalized.pop_back();

    std::vector<PasteStep> steps;
    for (char c : normalized) {
        if (c == '\n') {
            PasteStep step;
            step.kind = PasteStep::Kind::Enter;
            step.key = "enter";
            steps.push_back(step);
            continue;
        }
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
        if (step.kind == PasteStep::Kind::Enter) {
            tap(step.key);
            wait(m_pacing.postEnterFloorFrames);
            Action a;
            a.kind = Action::Kind::WaitPrompt;
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
        case Action::Kind::WaitPrompt:
            m_framesLeft = m_pacing.postEnterCapFrames;
            m_promptRun = 0;
            break;
    }
}

bool KeyPasteFeeder::elapse(bool atPrompt, const KeyFn& release) {
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
        case Action::Kind::WaitPrompt:
            m_promptRun = atPrompt ? m_promptRun + 1 : 0;
            return m_promptRun >= m_pacing.promptHoldFrames || --m_framesLeft <= 0;
    }
    return true;
}

void KeyPasteFeeder::onFrame(const KeyFn& press, const KeyFn& release, bool atPrompt) {
    if (m_hasCurrent) {
        if (!elapse(atPrompt, release)) return;
        m_hasCurrent = false;
    }
    if (m_queue.empty()) return;
    // Start the next action at this frame boundary. Only a Tap/Wait/
    // WaitPrompt with time left can be current, so one begin() suffices.
    const Action next = m_queue.front();
    m_queue.pop_front();
    begin(next, press);
}
