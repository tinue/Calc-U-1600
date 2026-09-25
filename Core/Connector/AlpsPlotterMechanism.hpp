#pragma once
#include <cstdint>
#include <deque>
#include <iterator>
#include <string>
#include <vector>

// ── ALPS plotter mechanism (CE-150 / CE-1600P shared hardware) ──────────
//
// Sharp's CE-150 (PC-1500) and CE-1600P (PC-1600) plotter/printers are the
// same ALPS pen-plotter mechanism behind two different 60-pin bus wrappers,
// sharing all pen/motor/color logic. This class is the host- and
// bus-agnostic half of that split: pure mechanism state, fed raw
// stepper-motor phase writes and exposing nothing but the resulting pen
// strokes. A `CE1600PCard` (and, if ever built, a `CE150Card`) owns one of
// these and does only its own bus's port/address decode, forwarding motor
// writes into it -- mirroring the project's existing "connector knows the
// host, card doesn't" split (ExpansionCard.hpp), one layer deeper.
//
// The real hardware has no "move to (x,y)" or "select color N" command at
// all -- the peripheral ROM drives three raw 4-bit stepper phases (X =
// carriage, Y = paper feed, Z = pen lift + color-turret rotation) and the
// pen's position/state is simply the running integral of those steps. This
// class reproduces that stepper simulation exactly (same phase table, same
// Z-position thresholds) rather than inventing a higher-level vector
// protocol, and turns the result into strokes: this project only wants
// "correct lines, correctly colored, no motion animation" as output, not
// per-step pixels.
class AlpsPlotterMechanism {
public:
    enum class PenColor : uint8_t { Black = 0, Blue = 1, Green = 2, Red = 3 };

    struct Point { int x = 0; int y = 0; };

    // One continuous pen-down run: a polyline of step-space points, all in
    // one color (the pen physically cannot change color without first
    // lifting, so a color change never happens mid-stroke). Rendered as a
    // single accumulated Path per color by the paper view.
    struct Stroke {
        PenColor color = PenColor::Black;
        std::vector<Point> points;
    };

    /// Ports 0x82 (Z, low nibble) / 0x83 (X low nibble, Y high nibble) on
    /// the real CE-1600P hardware. Callers pass the already-isolated 4-bit
    /// phase.
    void writeMotorX(uint8_t phase) { stepXY(m_motorX, phase, /*isX=*/true); }
    void writeMotorY(uint8_t phase) { stepXY(m_motorY, phase, /*isX=*/false); }
    void writeMotorZ(uint8_t phase) { stepZ(phase); }

    // ── CE-150 wiring ─────────────────────────────────────────────────
    //
    // The CE-150 (PC-1500 sibling) drives the same ALPS X/Y steppers, but
    // has no Z motor: pen lift is a pair of discrete LH5810 Port B signals
    // and the colour turret rotates mechanically when the carriage is
    // driven to its physical left stop. These entry points reuse the
    // shared X/Y integration + stroke accumulator above; the CE-1600P
    // `writeMotorX/Y/Z` paths are untouched.

    // The CE-150 carriage's turret geometry, in this class's own `m_penX`
    // units, which count a full step as 2 (a half step as 1). The turret
    // arms at half of the physical left-stop distance and the carriage
    // clamps at the stop itself; the firmware homes into the stop then
    // backs off a fixed number of steps, so an incorrect stop position
    // shows up as a constant offset on every CE-150 plot.
    static constexpr int kCE150TurretArmX  = -32;
    static constexpr int kCE150LeftStopX   = -90;

    /// CE-150 carriage (X) motor -- LH5810 Port C low nibble. Same stepper
    /// as `writeMotorX`, plus the left-stop turret mechanism: arm on the
    /// way past `kCE150TurretArmX`, clamp at `kCE150LeftStopX`, and every
    /// 3rd armed arrival at the stop advances the pen colour. The `<=`
    /// comparisons tolerate a full step (delta 2) jumping past the exact
    /// coordinate.
    void writeCarriageMotorCE150(uint8_t phase) {
        const int delta = m_motorX.sendPhase(phase & 0x0F);
        if (delta == 0) return;
        const int prevX = m_penX;
        m_penX += delta;
        if (delta < 0) { // carriage moving left, toward the turret stop
            // Arm on the falling edge past the arm threshold. Not re-armed
            // while the carriage sits against the stop, so holding it
            // there doesn't ratchet the turret.
            if (prevX > kCE150TurretArmX && m_penX <= kCE150TurretArmX) m_ce150StartRot = true;
            if (m_penX < kCE150LeftStopX) m_penX = kCE150LeftStopX;
            if (m_penX <= kCE150LeftStopX && m_ce150StartRot) {
                m_ce150StartRot = false;
                if (++m_ce150Rot >= 3) { m_ce150Rot = 0; nextColor(); }
            }
        }
        extendStroke();
    }

    /// CE-150 paper-feed (Y) motor -- LH5810 Port C high nibble.
    /// Identical to `writeMotorY` (no CE-150-specific behaviour on Y);
    /// named for call-site symmetry with `writeCarriageMotorCE150`.
    void writePaperMotorCE150(uint8_t phase) { stepXY(m_motorY, phase, /*isX=*/false); }

    /// CE-150 pen lift -- LH5810 Port B bit 0 (ascending -> pen up) / bit 1
    /// (descending -> pen down). Ascending is checked first;
    /// `setPenDown`'s edge guard makes a repeated signal idempotent.
    void applyPenSignals(bool ascending, bool descending) {
        if (ascending) setPenDown(false);
        if (descending) setPenDown(true);
    }

    /// CE-150 colour-detect magnet, read back by the ROM on LH5810 Port B
    /// bit 2 to confirm the turret is home. A pure level condition -- the
    /// carriage sitting at the left stop with the turret un-rotated in
    /// COLOR0. This must stay a level condition rather than an
    /// edge/"moved this tick" flag: the ROM's homing loop polls it without
    /// necessarily moving on every poll, and gating it on motion would
    /// leave that poll spinning forever.
    bool colorMagnet() const {
        return m_penColor == PenColor::Black && m_ce150Rot == 0 && m_penX <= kCE150LeftStopX;
    }

    /// Re-anchor the mechanism to its power-on state: steppers to the ring
    /// phase the tests anchor on, pen up, colour Black, all turret state
    /// cleared. Deliberately KEEPS `m_strokes` -- a chip reset is not a
    /// fresh sheet of paper, and real ink stays put. Bumps `revision()`.
    void reset() {
        m_motorX = Stepper{};
        m_motorY = Stepper{};
        m_motorZ = Stepper{};
        m_penX = 0;
        m_penY = 0;
        m_penZ = 6;
        m_penDown = false;
        m_penColor = PenColor::Black;
        m_colorRotateArmed = false;
        m_colorRotateCount = 0;
        m_ce150StartRot = false;
        m_ce150Rot = 0;
        ++m_revision;
    }

    bool penDown() const { return m_penDown; }
    int penX() const { return m_penX; }
    int penY() const { return m_penY; }
    int penZ() const { return m_penZ; } // debug/test access; 0-50, see stepZ()
    PenColor penColor() const { return m_penColor; }

    /// Committed strokes plus, if a pen-down run is in progress, that run
    /// too (so the paper view sees a line grow live rather than only on
    /// pen-up) -- the in-progress run is always `m_strokes.back()` while
    /// `m_penDown` is true, since a new Stroke is opened the instant the pen
    /// goes down (see stepZ()).
    ///
    /// Reference into live mutable state -- callers crossing a thread
    /// boundary must not use this; use `flatPoints()` (a value copy) via
    /// `PC1600Machine`'s locked accessor instead.
    const std::vector<Stroke>& strokes() const { return m_strokes; }

    /// Bumped every time the plotted geometry changes -- a stroke opened, a
    /// point appended, or the paper cleared. A GUI-thread caller polls this
    /// O(1) value (under `PC1600Machine::m_mutex`) and only pays for
    /// `flatPoints()`'s full copy + the downstream GUI-widget repaint when
    /// it has actually moved. Monotonic across `clearPaper()` too, so "same
    /// point count after a cut" still reads as a change.
    uint64_t revision() const { return m_revision; }

    /// One point of every stroke, flattened, with `newStroke = 1` on each
    /// stroke's first point. A self-contained value copy -- this is what a
    /// GUI-thread caller reads (under `PC1600Machine::m_mutex`), never the
    /// live `strokes()` above.
    struct FlatPoint {
        int32_t x = 0;
        int32_t y = 0;
        uint8_t color = 0;
        uint8_t newStroke = 0;
    };
    std::vector<FlatPoint> flatPoints() const {
        std::vector<FlatPoint> out;
        size_t total = 0;
        for (const Stroke& stroke : m_strokes) total += stroke.points.size();
        out.reserve(total);
        for (const Stroke& stroke : m_strokes) {
            bool first = true;
            for (const Point& p : stroke.points) {
                out.push_back(FlatPoint{static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                                        static_cast<uint8_t>(stroke.color),
                                        static_cast<uint8_t>(first ? 1 : 0)});
                first = false;
            }
        }
        return out;
    }

    void clearPaper() { m_strokes.clear(); ++m_revision; }

    /// Human-readable pen-down/up/color-change events, for diagnosing a
    /// "nothing drew" report without having to reverse-engineer a raw CPU
    /// trace to find the handful of real motor writes among a ROM's
    /// unrelated periodic housekeeping. Bounded (oldest dropped past
    /// kMaxEvents) so a caller that never drains this can't leak memory.
    /// Draining consumes -- repeated calls only return what's new.
    std::vector<std::string> drainEvents() {
        std::vector<std::string> out(std::make_move_iterator(m_events.begin()),
                                     std::make_move_iterator(m_events.end()));
        m_events.clear();
        return out;
    }

private:
    // Fixed stepper phase table: each 4-bit phase write is compared against
    // the motor's last phase, yielding one of {none, ±1 half-step, ±2
    // full-step}. This is a physical stepper wiring table, not something to
    // "improve".
    struct Stepper {
        // Starts at an arbitrary *valid* ring phase (0x9, the same one this
        // project's tests anchor on) rather than 0, so the very first
        // genuine phase write already registers as a step -- matching the
        // real CE-1600P, which performs an automatic pen-holder startup
        // spin that reliably resets to COLOR0.
        uint8_t currentPhase = 0x9;
        // Returns the step delta: 0 (no/unrecognized transition), or
        // ±1 (half-step) / ±2 (full-step).
        //
        // currentPhase only ever latches onto a write that produced a real,
        // table-recognized step -- a write of phase 0 (coils off) or 0xF
        // (all coils on -- a hold/brake pattern the real ROM uses between
        // moves) leaves it untouched. A brake/hold write simply holds the
        // motor's current position, it doesn't relocate it, so the next
        // real step must still be computed relative to wherever the motor
        // was *before* the brake, not from the brake pattern itself; if
        // currentPhase latched onto 0xF, the next genuine step would look
        // up an unrecognized transition and be silently dropped.
        int sendPhase(uint8_t phase) {
            if (phase == currentPhase) return 0;
            const int delta = deltaFor(currentPhase, phase);
            if (delta != 0) currentPhase = phase;
            return delta;
        }

    private:
        // The stepper's phase-transition table, collapsed to a signed delta:
        // a full step in either direction moves two units, a half step one
        // unit.
        //
        // Every recognized transition in that table is a hop along one
        // 8-phase drive ring: advancing one slot is a half-step (+1), two
        // slots a full step (+2), and the same backwards is -1 / -2. So
        // rather than transcribe all 32 arms by hand, look both phases up
        // in the ring and map the forward distance (mod 8). Any phase not
        // on the ring -- 0x0 (coils off), 0xF (brake/hold), etc. -- has no
        // index and yields 0 (no move).
        static int deltaFor(uint8_t oldPhase, uint8_t newPhase) {
            static constexpr uint8_t kRing[8] = {0x9, 0x8, 0xc, 0x4, 0x6, 0x2, 0x3, 0x1};
            auto indexOf = [](uint8_t phase) -> int {
                for (int i = 0; i < 8; ++i)
                    if (kRing[i] == phase) return i;
                return -1;
            };
            const int oldIdx = indexOf(oldPhase), newIdx = indexOf(newPhase);
            if (oldIdx < 0 || newIdx < 0) return 0;
            switch ((newIdx - oldIdx + 8) % 8) {
                case 1: return +1; // one slot forward  -> half-step
                case 2: return +2; // two slots forward -> full step
                case 6: return -2; // two slots back    -> full step
                case 7: return -1; // one slot back     -> half-step
                default: return 0; // no move / unrecognized transition
            }
        }
    };

    // Stepper::sendPhase()'s delta directly encodes the X motor's own
    // direction convention (right = positive, left = negative). The Y and Z
    // motors are wired to the same phase table and raw numeric codes, but
    // their direction convention is the opposite sign (up decrements, down
    // increments) -- so their effective delta is `-sendPhase(...)`, not the
    // raw value.
    void stepXY(Stepper& motor, uint8_t phase, bool isX) {
        const int delta = motor.sendPhase(phase & 0x0F);
        if (delta == 0) return;
        if (isX) m_penX += delta; else m_penY -= delta;
        extendStroke();
    }

    void stepZ(uint8_t phase) {
        const int delta = -m_motorZ.sendPhase(phase & 0x0F);
        if (delta == 0) return;
        m_penZ += delta;
        if (m_penZ < 0) m_penZ = 0;
        if (m_penZ > 50) m_penZ = 50;

        if (m_penZ == 6) setPenDown(false);
        if (m_penZ == 0) setPenDown(true);

        if (m_penZ == 20) m_colorRotateArmed = true;
        if (m_penZ == 50 && m_colorRotateArmed) {
            m_colorRotateArmed = false;
            // CE-1600P: 8 detents per full color-turret click (vs. 3 for
            // the CE-150's carriage-driven turret, see
            // writeCarriageMotorCE150) -- the two mechanisms rotate the
            // turret by different means and have independent thresholds.
            if (++m_colorRotateCount >= 8) {
                m_colorRotateCount = 0;
                nextColor();
            }
        }
    }

    void setPenDown(bool down) {
        if (down == m_penDown) return;
        m_penDown = down;
        if (down) {
            m_strokes.push_back(Stroke{m_penColor, {Point{m_penX, m_penY}}});
            ++m_revision;
            logEvent("PEN DOWN @ (" + std::to_string(m_penX) + "," + std::to_string(m_penY) +
                     ") color=" + std::to_string(static_cast<int>(m_penColor)) +
                     " stroke#" + std::to_string(m_strokes.size() - 1));
        } else if (!m_strokes.empty()) {
            logEvent("PEN UP stroke#" + std::to_string(m_strokes.size() - 1) +
                     " points=" + std::to_string(m_strokes.back().points.size()));
        }
    }

    void extendStroke() {
        if (!m_penDown) return;
        m_strokes.back().points.push_back(Point{m_penX, m_penY});
        ++m_revision;
    }

    void nextColor() {
        m_penColor = static_cast<PenColor>((static_cast<uint8_t>(m_penColor) + 1) & 0x03);
        logEvent("COLOR -> " + std::to_string(static_cast<int>(m_penColor)));
    }

    static constexpr size_t kMaxEvents = 30000;
    void logEvent(std::string s) {
        if (m_events.size() >= kMaxEvents) m_events.pop_front();
        m_events.push_back(std::move(s));
    }

    Stepper m_motorX, m_motorY, m_motorZ;
    int m_penX = 0, m_penY = 0;
    // Real CE-1600P power-on state: the pen holder's automatic startup spin
    // leaves the pen UP, at the left border, in COLOR0/black. `m_penZ`'s
    // own threshold encoding (see stepZ()) treats exactly 0 as "pen down"
    // -- so the resting *up* position must start at a genuinely different
    // value (6, this class's own "pen up" threshold), not 0, or the very
    // first real pen-down command has nothing to transition *from*: it
    // would find penZ already sitting at 0 and never re-cross into it, so
    // setPenDown(true) never fires and the shape silently fails to draw.
    int m_penZ = 6;
    bool m_penDown = false;
    PenColor m_penColor = PenColor::Black;
    bool m_colorRotateArmed = false;
    int m_colorRotateCount = 0;
    // CE-150 carriage-driven turret state (unused by the CE-1600P path).
    bool m_ce150StartRot = false; // armed once the carriage passes kCE150TurretArmX
    int m_ce150Rot = 0;           // detents at the stop; 3 -> nextColor()
    std::vector<Stroke> m_strokes;
    uint64_t m_revision = 0; // see revision()
    std::deque<std::string> m_events;  // see drainEvents(); a deque so the cap drops the oldest in O(1)
};
