// Pins the Apple II square-gate joystick model in JoystickInput.
//
// A modern analog stick is bounded by a ROUND gate: a full diagonal only
// reaches (~0.707, ~0.707), so both paddles top out near 217/255 at once and
// the four extreme corners (full X *and* full Y together) are physically
// unreachable. The original Apple II stick rode a SQUARE gate, so the corners
// (255/255) were reachable — which some games (Wings of Fury's take-off)
// require. JoystickInput::applySquareGate() expands the inscribed circle back
// out to the full square.
//
// This test exercises the two pure static helpers (applySquareGate +
// axisToPaddle01), which together encode the corner-reachability behaviour.

#include "JoystickInput.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

bool near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

// Convenience: run the gate and return the mapped paddle pair, mirroring the
// order paddleValue() applies them (gate → axisToPaddle01).
void gateToPaddle(float x, float y, uint8_t& px, uint8_t& py)
{
    JoystickInput::applySquareGate(x, y);
    px = JoystickInput::axisToPaddle01(x);
    py = JoystickInput::axisToPaddle01(y);
}

void testAxisDirectionsUntouched()
{
    // A stick pushed straight along an axis is already on the square edge:
    // the gate must be a no-op (scale factor 1), or it would distort the
    // cardinal directions.
    for (auto p : {std::pair<float,float>{ 1.0f,  0.0f},
                   {-1.0f,  0.0f},
                   { 0.0f,  1.0f},
                   { 0.0f, -1.0f}}) {
        float x = p.first, y = p.second;
        JoystickInput::applySquareGate(x, y);
        assert(near(x, p.first));
        assert(near(y, p.second));
    }
    std::printf("  ok: pure-axis directions pass through unchanged\n");
}

void testFullDiagonalsReachCorners()
{
    // The whole point: a full diagonal on a round gate (~0.707, ~0.707)
    // must expand to the true corner (1, 1) → paddle 255/255.
    const float d = 0.70710678f;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            float x = sx * d, y = sy * d;
            JoystickInput::applySquareGate(x, y);
            assert(near(x, static_cast<float>(sx)));
            assert(near(y, static_cast<float>(sy)));

            uint8_t px, py;
            gateToPaddle(sx * d, sy * d, px, py);
            assert(px == (sx > 0 ? 255 : 0));
            assert(py == (sy > 0 ? 255 : 0));
        }
    }
    std::printf("  ok: full diagonals reach the corners (255/255 etc.)\n");
}

void testRoundGateWouldFallShort()
{
    // Sanity: WITHOUT the gate, that same full diagonal only reaches ~217,
    // never 255 — this is the modern-pad limitation we are correcting.
    const float d = 0.70710678f;
    uint8_t p = JoystickInput::axisToPaddle01(d);
    assert(p > 210 && p < 224);   // ~217
    assert(p != 255);
    std::printf("  ok: ungated diagonal falls short at ~%u (not 255)\n", p);
}

void testPartialTiltScalesProportionally()
{
    // Half-magnitude diagonal (mag 0.5) → the largest component becomes the
    // magnitude (0.5), preserving the 45° direction while expanding travel.
    float x = 0.35355339f, y = 0.35355339f;   // mag = 0.5
    JoystickInput::applySquareGate(x, y);
    assert(near(x, 0.5f));
    assert(near(y, 0.5f));
    std::printf("  ok: partial diagonal scales proportionally (0.5/0.5)\n");
}

void testCenterIsStable()
{
    float x = 0.0f, y = 0.0f;
    JoystickInput::applySquareGate(x, y);   // must not divide by zero
    assert(near(x, 0.0f) && near(y, 0.0f));
    assert(JoystickInput::axisToPaddle01(0.0f) == 128);
    std::printf("  ok: center is a stable no-op (128/128, no div-by-zero)\n");
}

void testMapEndpoints()
{
    assert(JoystickInput::axisToPaddle01(-1.0f) == 0);
    assert(JoystickInput::axisToPaddle01( 0.0f) == 128);
    assert(JoystickInput::axisToPaddle01( 1.0f) == 255);
    // Out-of-range (a stick briefly reporting > 1) clamps, never wraps.
    assert(JoystickInput::axisToPaddle01( 2.0f) == 255);
    assert(JoystickInput::axisToPaddle01(-2.0f) == 0);
    std::printf("  ok: axis→paddle endpoints and clamping\n");
}

} // namespace

int main()
{
    std::printf("joystick_square_gate:\n");
    testAxisDirectionsUntouched();
    testFullDiagonalsReachCorners();
    testRoundGateWouldFallShort();
    testPartialTiltScalesProportionally();
    testCenterIsStable();
    testMapEndpoints();
    std::printf("joystick_square_gate: all assertions passed\n");
    return 0;
}
