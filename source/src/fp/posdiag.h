#pragma once

namespace gowvr::fp {

// D-062 G1 research: find the game's authoritative copy of Kratos' position (see posdiag.cpp)
void PosScan();                 // "posscan"
void PosScanAll();              // "posscanall" (all private RW memory; stalls the game)
void PosTrack();                // "postrack"
void PosFind(int i);            // "posfind <i>": pointer paths from the hero object / creature to candidate i
void PosHold(int ms, int sel);  // "poshold <ms> [i|all|live]" (sel -1 all, -2 live)
void PosDiagTick();             // game thread, every frame (BodyPostAnim)

}  // namespace gowvr::fp
