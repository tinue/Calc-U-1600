// Headless C++ tests for AlpsPlotterMechanism -- the CE-1600P plotter's
// motor-phase-to-pen-stroke simulation. Same no-framework, assert-and-tally
// style as lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>

#include "../Connector/AlpsPlotterMechanism.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// cmotor.cpp's phaseList forms an 8-position ring over these phases: moving
// from kRing[i] to kRing[i+1] is always a half-step ("_MID") in the X
// motor's own RI_MOVE convention (+1); kRing[0] = 0x9 is where
// AlpsPlotterMechanism::Stepper starts (CurrentPhase = 0 is never a valid
// "current" ring position, so the first write to any motor must be 0x9 to
// anchor it there for free -- phaseList[0][9] has no entry, so that first
// write never moves the pen). Verified by hand against every entry in
// AlpsPlotterMechanism::Stepper::deltaFor().
constexpr uint8_t kRing[8] = {0x9, 0x8, 0xc, 0x4, 0x6, 0x2, 0x3, 0x1};

/// Drives `write` through however many half-steps are needed to change the
/// X motor's own signed convention by exactly `deltaXConvention` (positive
/// = RI_MOVE direction), threading the ring position across calls so a
/// step count that isn't a multiple of 8 doesn't leave the next call
/// assuming the wrong current phase. Returns the new ring position.
int stepRing(void (AlpsPlotterMechanism::*write)(uint8_t), AlpsPlotterMechanism& m,
             int pos, int deltaXConvention) {
    const int dir = deltaXConvention > 0 ? 1 : -1;
    const int n = deltaXConvention > 0 ? deltaXConvention : -deltaXConvention;
    for (int i = 0; i < n; ++i) {
        pos = (pos + dir + 8) % 8;
        (m.*write)(kRing[pos]);
    }
    return pos;
}

void test_stepper_full_and_half_steps() {
    AlpsPlotterMechanism x;
    CHECK(x.penX() == 0);
    x.writeMotorX(0x9); // Stepper already starts at phase 9 (ring position 0); no move
    x.writeMotorX(0xc); // 9 -> c: two ring positions forward = full step, RI_MOVE, +2
    CHECK(x.penX() == 2);
    x.writeMotorX(0x8); // c -> 8: one ring position back = half step, LE_MOVE_MID, -1
    CHECK(x.penX() == 1);
    x.writeMotorX(0x8); // repeat same phase: no-op (phase == currentPhase)
    CHECK(x.penX() == 1);
}

// A brake/hold write (phase 0xF, all coils on) can appear between real
// moves. On real CE-1600P hardware a brake write holds position without
// relocating the tracked motor phase, so deltaFor() has no row for 0xF and
// the step immediately after a brake must land exactly where it would have
// without the brake in between.
void test_brake_write_does_not_eat_the_next_step() {
    AlpsPlotterMechanism x;
    x.writeMotorX(0x9); // anchor at ring position 0
    x.writeMotorX(0xf); // brake/hold -- must not move, must not relocate currentPhase
    CHECK(x.penX() == 0);
    x.writeMotorX(0xc); // 9 -> c: full step, +2 -- same as if the brake write never happened
    CHECK(x.penX() == 2);
}

// Y and Z apply the opposite sign to the same phase table as X (see
// AlpsPlotterMechanism::stepXY()/stepZ()'s comments): a +1 step in X's own
// convention moves Y/Z by -1 instead.
void test_y_applies_inverted_sign_from_x() {
    AlpsPlotterMechanism m;
    m.writeMotorY(0x9); // establish, ring position 0
    stepRing(&AlpsPlotterMechanism::writeMotorY, m, 0, -5); // X-convention -5 -> Y +5
    CHECK(m.penY() == 5);
}

// Z-motor thresholds (ce1600p.cpp::run()): penZ==0 -> pen down, penZ==6 ->
// pen up. Walk Z up from 0 to 50 (never crossing back through a threshold
// on the way, since it only increases), then back down to exactly 0 --
// crossing 6 (already up, no-op) then landing on 0, which must fire the
// false->true pen-down edge and start a stroke; a subsequent X move while
// down must extend it; returning to 6 must end it.
void test_pen_down_traces_a_stroke_and_pen_up_ends_it() {
    AlpsPlotterMechanism m;
    CHECK(!m.penDown());

    m.writeMotorZ(0x9); // establish, ring position 0
    int pos = stepRing(&AlpsPlotterMechanism::writeMotorZ, m, 0, -50); // Z: 0 -> 50
    CHECK(m.penZ() == 50);
    CHECK(!m.penDown());

    pos = stepRing(&AlpsPlotterMechanism::writeMotorZ, m, pos, 50); // Z: 50 -> 0
    CHECK(m.penZ() == 0);
    CHECK(m.penDown());
    CHECK(m.strokes().size() == 1);
    CHECK(m.strokes().back().points.size() == 1); // just the pen-down point so far
    CHECK(m.strokes().back().color == AlpsPlotterMechanism::PenColor::Black);

    m.writeMotorX(0x9);
    m.writeMotorX(0xc); // +2 while pen is down -> extends the open stroke
    CHECK(m.strokes().back().points.size() == 2);
    CHECK(m.strokes().back().points.back().x == 2);

    stepRing(&AlpsPlotterMechanism::writeMotorZ, m, pos, -6); // Z: 0 -> 6, ends the stroke
    CHECK(m.penZ() == 6);
    CHECK(!m.penDown());
    CHECK(m.strokes().size() == 1); // no new stroke opened just by lifting

    // Moving X with the pen up must not extend the (now closed) stroke.
    m.writeMotorX(0x8); // c -> 8, half step
    CHECK(m.strokes().back().points.size() == 2);
}

// The mechanism's resting state (after AlpsPlotterMechanism's constructor,
// standing in for the real CE-1600P's power-on startup spin -- see the
// class comment) is pen UP at penZ==6, not penZ==0. The very first real
// pen-down command the ROM issues walks Z straight down from that rest
// position to 0 with no detour through a higher value first -- unlike
// test_pen_down_traces_a_stroke_and_pen_up_ends_it() above, which first
// walks *up* to 50 and back down. This guards against the rest position
// silently reporting penZ==0 already, which would suppress the down edge
// and, on the real CE-1600P TEST diagnostic, leave the black rectangle
// (drawn first, right after power-on) empty while the others still draw.
void test_first_pen_down_from_rest_fires_immediately() {
    AlpsPlotterMechanism m;
    CHECK(!m.penDown());
    CHECK(m.penZ() == 6);

    m.writeMotorZ(0x9); // establish, ring position 0
    stepRing(&AlpsPlotterMechanism::writeMotorZ, m, 0, 6); // Z: 6 -> 0, straight down
    CHECK(m.penZ() == 0);
    CHECK(m.penDown());
    CHECK(m.strokes().size() == 1);
}

} // namespace

int run_ce1600p_tests() {
    test_stepper_full_and_half_steps();
    test_brake_write_does_not_eat_the_next_step();
    test_y_applies_inverted_sign_from_x();
    test_pen_down_traces_a_stroke_and_pen_up_ends_it();
    test_first_pen_down_from_rest_fires_immediately();

    std::printf("ce1600p_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
