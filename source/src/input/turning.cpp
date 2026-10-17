// First-person right-stick turning (turning.h).
// Snap history (owner 2026-10-17): first the game camera was turned by full stick while the view waited ("a 2 second
// delay"), then the view jumped ahead of the camera - but the F1 view yaw only follows the camera while the PHYSICAL
// stick is held (mutate.cpp CamSync), so after a short flick the view, the walking direction and the drawn body stayed
// behind ("the body doesn't move with the snap"). Now a push turns the view's own yaw directly (CamSync), the body with
// it, and the game camera is left alone.
#include "turning.h"
#include "../core/config.h"
#include <atomic>
#include <cmath>

namespace gowvr::input {
namespace {

std::atomic<int> g_steps{0};  // pushes not yet taken by CamSync (signed)
bool g_armed = true;          // the stick came back to the middle since the last push (pad thread)

}  // namespace

bool SnapMode() { return config::GetFloat("turn_snap", 0.f) > 0.5f; }

float SnapStepRad() {
    const float d = config::GetFloat("snap_turn_degrees", 45.f);
    return fminf(fmaxf(d, 5.f), 180.f) * 0.017453293f;
}

float TurnStickX(float x) {
    if (!SnapMode()) {
        g_steps = 0;
        const float s = config::GetFloat("turn_speed", 1.f);
        return x * fminf(fmaxf(s, 0.05f), 1.f);
    }
    if (fabsf(x) < 0.35f) g_armed = true;
    if (g_armed && fabsf(x) > 0.7f) {
        g_armed = false;
        g_steps += x > 0.f ? 1 : -1;
    }
    return 0.f;  // snap: the game camera does not turn
}

int SnapTakeSteps() { return g_steps.exchange(0); }

}  // namespace gowvr::input
