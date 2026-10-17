#pragma once
#include <cstdint>

namespace gowvr::fp {

// F1 combat plan, Phase 0 (docs/F1_COMBAT_PLAN.md): read-only move logger. Every frame (game thread) reads the hero's
// active move (creature+0x3a70 move player, instance name hash +0xd8, active flag +0x128 & 4, progress +0x88 -
// LuaInteract::IsPlayingMove / GetActiveMovePercent) and the primary animation name, and for every finished move logs
// its duration and how much Kratos' body moved: root travel / rise / drop, root yaw (net and total), chest twist
// against the root, neck offset against the root (what a first-person camera on the head would feel).
// Names come from gowvr_movenames.txt (strings of r_heroa00.wad, hashed with the game's hash), loaded on first use.
// Output: "move:" log lines and gowvr_logs/moves.csv. Command "movelog on|off", gowvr.ini move_log=1.
void MoveLogTick();
void MoveLogEnable(bool on);
// The hero's active move right now (0 = none) and its name if known (nullptr otherwise).
uint64_t ActiveMoveHash();
// Primary animation name of the active move ("" if none yet): sampled until an attack/evade animation shows up.
const char* ActiveMoveAnim();
// The hero's primary animation right now (updated every other frame, "" if unknown), copied into out.
void HeroPrimaryAnim(char* out, size_t n);
// LuaInteract::IsDoingSyncMove (0x140223470): creature+0x3b81 & 1, or the sync list at creature+0x3b60 not empty.
bool HeroInSyncMove();
const char* MoveName(uint64_t hash);

}  // namespace gowvr::fp
