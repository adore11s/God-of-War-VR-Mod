#pragma once
#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Texture2D;

namespace gowvr::vr {

struct Pose { float q[4]; float p[3]; };  // OpenXR convention: quaternion (x,y,z,w), position (m), view space -Z forward
struct Fov { float left, right, up, down; };  // radians
struct EyeViews {
    bool valid = false;
    Pose pose[2];
    Fov fov[2];
    // the runtime's own (asymmetric) frustum of each eye, when `fov` is the wider symmetric one the game renders
    // (headcam.cpp): the eye image is cropped to it and the layer reports it (Quest Link: near objects doubled)
    Fov ownFov[2];
    bool hasOwnFov = false;
    int64_t displayTime = 0;
    int64_t period = 0;  // predicted display period (ns)
    uint64_t serial = 0;
};

// PORT-08. The OpenXR runtime (SteamVR) is loaded through the loader-runtime negotiation interface (no loader DLL).
// Everything runs on the game's render thread from the Present hook, because the runtime uses the game's immediate
// context: session creation on the first Present after XrRequest(true), then per Present: poll events, end the frame
// begun at the previous Present with the eye images of the frame being presented, wait/begin the next frame and
// locate its views (published for the game thread through XrLatestViews).
void XrRequest(bool on);
bool XrRunning();
// Render thread, before the original Present. eye0/eye1 = this frame's eye images (nullptr: mono -> backbuffer to
// both eyes); `used` = the views the game rendered them with (nullptr: the latest located views).
void XrOnPresent(IDXGISwapChain* sc, ID3D11Texture2D* eye0, ID3D11Texture2D* eye1, const EyeViews* used);
// Any thread: most recently located views (for the eye cameras of the next frame).
EyeViews XrLatestViews();
// Runtime system name (e.g. "SteamVR/OpenXR : <driver>"), empty until the instance exists.
const char* XrSystemName();
// Any thread: locate both eye views for an arbitrary (future) display time. False if not running.
bool XrLocateAt(int64_t displayTime, EyeViews* out);
void XrStatus();
// D-085: the flat 2D screen is placed in front of the head again at the next flat frame (F8 / runtime recenter)
void XrScreenRecenter();

// Motion controllers (OpenXR actions). Physical state only - game meaning is decided by the consumer (vrmap.cpp).
struct HandState {
    bool active = false;                 // controller connected / bound
    bool gripValid = false, aimValid = false;
    Pose grip{}, aim{};                  // LOCAL space, predicted for the frame's display time
    float stick[2] = {0, 0};
    float trigger = 0, squeeze = 0, trackpadForce = 0;
    bool a = false, b = false, stickClick = false, triggerTouch = false;
    // Finger curl 0 (open) .. 1 (fist) from the runtime's hand skeleton (XR_EXT_hand_tracking driven by the
    // controller's finger sensing: PSVR2 touch sensors, Index capacitive grip, ...): thumb, index, middle, ring, little.
    float fingerCurl[5] = {0, 0, 0, 0, 0};
    bool fingersValid = false;
};
struct InputSnapshot {
    uint64_t serial = 0;
    bool focused = false;                // session FOCUSED: otherwise every value is neutral
    HandState hand[2];                   // 0 = left, 1 = right
    char profile[2][96] = {};
    int64_t time = 0;
};
InputSnapshot XrInput();
// D-110: any thread - both controllers' grip poses (LOCAL space) for an arbitrary (future) display time; valid[h] false
// for an untracked hand. False: not running / neither hand located.
bool XrLocateHandsAt(int64_t displayTime, Pose grip[2], bool valid[2]);
// Haptics: amplitude 0..1, seconds, frequency (0 = runtime default).
void XrHaptic(int hand, float amplitude, float seconds, float frequency = 0.f);
// D-111: the game's pad rumble (large / small motor 0..1, 0 = off): kept on the controllers until changed (any thread).
void XrRumble(float heavy, float light);
// Command thread: save the swapchain images of the next submitted frame (what the headset receives) + layer views.
void XrHmdShot(const char* name, int div);

}  // namespace gowvr::vr
