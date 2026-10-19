#pragma once
#include "../core/mat.h"

namespace gowvr::fp {

// D-160 walls in F1 (owner: "in F1 you can't go through the walls with your head and your hands can't go through the
// walls; rumble once you touch walls"). The game's own ray cast (sub_14053e780, ADDR-021) against the bodies that block
// Kratos: the view and the hand targets stop short of a wall between Kratos' eye point and them, the controllers buzz
// on contact. Game thread only (the view update); gowvr.ini walls_f1 0 = off.
// center: the headset pose relative to base (view = center x base) - its translation is pulled back on a wall.
void WallsHead(Mat4& center, const Mat4& base);
// hand targets (world; [0] left, [1] right) - pulled back on a wall, from an approximate shoulder on base
void WallsHands(Mat4 hands[2], const bool valid[2], const Mat4& base);
void WallsIdle();  // not in F1 gameplay this frame: contact state reset
// D-161: the head's wall shift (base frame) added to a rendered eye's pose relative to base (eye = eyeDelta x base) -
// the headset's two eye views are built from their own poses (mutate.cpp hk_RVBuild), not from center
void WallsEyeShift(Mat4& eyeDelta);
// D-230: a step of an F1 attack's held spot (x / z; from[1] = the feet height) cast against the world: out = where it may
// go (stopped at / slid along a wall, not over a ledge); true = the whole step was free. Game thread.
bool WallsStep(const float from[3], const float to[3], float out[3]);
// D-230: true when a straight move at Kratos' heights from -> to (feet height from[1]) meets no wall
bool WallsClearPath(const float from[3], const float to[3]);
// D-231: the attack-walking wall cast is on (walls_f1 and attack_walk_walls)
bool WallsAttackActive();

}  // namespace gowvr::fp
