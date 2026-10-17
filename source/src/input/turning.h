#pragma once
// First-person right-stick turning (owner 2026-10-17, the VR settings panel): smooth - the stick's X scaled by
// turn_speed, the game camera turns and the view with it - or snap (gowvr.ini turn_snap=1): one push turns the view by
// snap_turn_degrees at once. In gameplay first person the view's yaw is the mod's own (mutate.cpp CamSync: detached from
// the game camera, which swings behind Kratos by itself), so a snap turns that yaw - and the drawn body with it - and
// the game gets no stick X. F1 gameplay only (outside F1 the stick is the game's own).

namespace gowvr::input {

// vrmap (pad thread), F1 gameplay: the right stick X the game gets for the stick X `x` (-1..1); in snap mode 0 (a push
// is a step for SnapTakeSteps)
float TurnStickX(float x);
bool SnapMode();
// mutate.cpp CamSync (game thread): the snap steps pushed since the last call (signed: + = a push right)
int SnapTakeSteps();
float SnapStepRad();

}  // namespace gowvr::input
