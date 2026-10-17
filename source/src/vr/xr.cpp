// OpenXR session on the game's D3D11 device (PORT-08). See xr.h for the threading contract.
#include "xr.h"
#include "xr_min.h"
#include "../core/log.h"
#include "../core/config.h"
#include "../diag/shot.h"
#include "../stereo/headcam.h"
#include "../core/addr.h"
#include "../stereo/mutate.h"
#include "../stereo/sharpui.h"
#include "../frame/rendersize.h"
#include "panel.h"
#include "../stereo/sharpui.h"
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace xr;

namespace gowvr::vr {
namespace {

// ---- runtime entry points ------------------------------------------------------------------------------------------
PFN_xrGetInstanceProcAddr gipa = nullptr;
#define XR_FN(name, ...) using PFN_##name = XrResult(__stdcall*)(__VA_ARGS__); PFN_##name name = nullptr;
XR_FN(xrEnumerateInstanceExtensionProperties, const char*, uint32_t, uint32_t*, XrExtensionProperties*)
XR_FN(xrCreateInstance, const XrInstanceCreateInfo*, XrInstance*)
XR_FN(xrDestroyInstance, XrInstance)
XR_FN(xrGetInstanceProperties, XrInstance, XrInstanceProperties*)
XR_FN(xrStructureTypeToString, XrInstance, XrStructureType, char*)
XR_FN(xrResultToString, XrInstance, XrResult, char*)
XR_FN(xrGetSystem, XrInstance, const XrSystemGetInfo*, XrSystemId*)
XR_FN(xrGetSystemProperties, XrInstance, XrSystemId, XrSystemProperties*)
XR_FN(xrGetD3D11GraphicsRequirementsKHR, XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR*)
XR_FN(xrEnumerateViewConfigurationViews, XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*,
      XrViewConfigurationView*)
XR_FN(xrCreateSession, XrInstance, const XrSessionCreateInfo*, XrSession*)
XR_FN(xrDestroySession, XrSession)
XR_FN(xrEnumerateSwapchainFormats, XrSession, uint32_t, uint32_t*, int64_t*)
XR_FN(xrCreateSwapchain, XrSession, const XrSwapchainCreateInfo*, XrSwapchain*)
XR_FN(xrEnumerateSwapchainImages, XrSwapchain, uint32_t, uint32_t*, XrBaseHeader*)
XR_FN(xrCreateReferenceSpace, XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*)
XR_FN(xrPollEvent, XrInstance, XrEventDataBuffer*)
XR_FN(xrBeginSession, XrSession, const XrSessionBeginInfo*)
XR_FN(xrEndSession, XrSession)
XR_FN(xrWaitFrame, XrSession, const XrFrameWaitInfo*, XrFrameState*)
XR_FN(xrBeginFrame, XrSession, const XrFrameBeginInfo*)
XR_FN(xrEndFrame, XrSession, const XrFrameEndInfo*)
XR_FN(xrLocateViews, XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*)
XR_FN(xrAcquireSwapchainImage, XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*)
XR_FN(xrWaitSwapchainImage, XrSwapchain, const XrSwapchainImageWaitInfo*)
XR_FN(xrReleaseSwapchainImage, XrSwapchain, const XrSwapchainImageReleaseInfo*)
XR_FN(xrStringToPath, XrInstance, const char*, XrPath*)
XR_FN(xrPathToString, XrInstance, XrPath, uint32_t, uint32_t*, char*)
XR_FN(xrCreateActionSet, XrInstance, const XrActionSetCreateInfo*, XrActionSet*)
XR_FN(xrCreateAction, XrActionSet, const XrActionCreateInfo*, XrAction*)
XR_FN(xrSuggestInteractionProfileBindings, XrInstance, const XrInteractionProfileSuggestedBinding*)
XR_FN(xrAttachSessionActionSets, XrSession, const XrSessionActionSetsAttachInfo*)
XR_FN(xrSyncActions, XrSession, const XrActionsSyncInfo*)
XR_FN(xrGetActionStateBoolean, XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*)
XR_FN(xrGetActionStateFloat, XrSession, const XrActionStateGetInfo*, XrActionStateFloat*)
XR_FN(xrGetActionStateVector2f, XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*)
XR_FN(xrGetActionStatePose, XrSession, const XrActionStateGetInfo*, XrActionStatePose*)
XR_FN(xrCreateActionSpace, XrSession, const XrActionSpaceCreateInfo*, XrSpace*)
XR_FN(xrLocateSpace, XrSpace, XrSpace, XrTime, XrSpaceLocation*)
XR_FN(xrGetCurrentInteractionProfile, XrSession, XrPath, XrInteractionProfileState*)
XR_FN(xrApplyHapticFeedback, XrSession, const XrHapticActionInfo*, const XrBaseHeader*)
XR_FN(xrCreateHandTrackerEXT, XrSession, const XrHandTrackerCreateInfoEXT*, XrHandTrackerEXT*)
XR_FN(xrLocateHandJointsEXT, XrHandTrackerEXT, const XrHandJointsLocateInfoEXT*, XrHandJointLocationsEXT*)
XR_FN(xrDestroyHandTrackerEXT, XrHandTrackerEXT)
XR_FN(xrStopHapticFeedback, XrSession, const XrHapticActionInfo*)
XR_FN(xrDestroySwapchain, XrSwapchain)
XR_FN(xrDestroySpace, XrSpace)
#undef XR_FN

template <class F> bool Get(XrInstance inst, const char* name, F& fn) {
    PFN_xrVoidFunction v = nullptr;
    if (!Succeeded(gipa(inst, name, &v)) || !v) { Log("xr: missing %s", name); return false; }
    fn = (F)v;
    return true;
}
#define GET(inst, name) Get(inst, #name, name)

// ---- state -----------------------------------------------------------------------------------------------------------
std::atomic<bool> g_want{false};
char g_systemName[256] = "";
// for the VR summary in the log (LogSummary)
char g_runtimeJson[MAX_PATH + 32] = "";  // runtime manifest + where it came from
char g_runtimeName[192] = "";            // the runtime's own name and version
uint32_t g_eyeMaxW = 0, g_eyeMaxH = 0;   // largest eye image the runtime allows
bool g_failed = false;  // a setup step failed: retry after g_retryAt (not every frame)
uint64_t g_retryAt = 0;
uint32_t g_failCount = 0;
XrInstance g_inst = nullptr;
XrSystemId g_sys = XR_NULL_SYSTEM_ID;
XrSession g_sess = nullptr;
XrSpace g_space = nullptr;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false, g_frameBegun = false;
XrFrameState g_frame{};
XrView g_views[2]{};
bool g_viewsValid = false;

struct EyeSwap {
    XrSwapchain sc = nullptr;
    uint32_t w = 0, h = 0;
    std::vector<ID3D11Texture2D*> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
    bool pending = false;     // D-110: an image acquired whose wait has not succeeded yet (waited again next frame)
    uint32_t pendingIdx = 0;
};
EyeSwap g_eye[2];
int64_t g_format = 0;
// D-110: the game thread (XrLocateAt / XrLocateHandsAt) and the input thread (XrHaptic) use the session too; the render
// thread destroys it (VR off, session lost / exiting) only while holding this exclusively
SRWLOCK g_sessLock = SRWLOCK_INIT;
// D-110: set by PollEvents / a failed frame call: 1 = destroy the session and build a new one (3 s retry), 2 = the
// instance too (SteamVR restarted: the old instance is lost)
int g_rebuild = 0;
void NoteLoss(XrResult r) {
    if (r == XR_ERROR_INSTANCE_LOST) g_rebuild = 2;
    else if (r == XR_ERROR_SESSION_LOST && g_rebuild < 1) g_rebuild = 1;
}

ID3D11Device* g_dev = nullptr;
ID3D11Device1* g_dev1 = nullptr;
ID3DDeviceContextState* g_blitState = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11PixelShader* g_psHud = nullptr;  // the HUD layer: premultiplied gamma -> premultiplied linear (sRGB target)
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_rs = nullptr;

SRWLOCK g_viewLock = SRWLOCK_INIT;
EyeViews g_latest;
std::atomic<uint64_t> g_frames{0}, g_submitted{0}, g_mono{0};
// XR_EXT_hand_tracking: finger curl from the controllers' finger sensing (owner: "when my finger is on the grip the fist
// closes"; also on other controllers). Optional: absent / failing -> the grip value alone drives the fist.
bool g_handExt = false;
XrHandTrackerEXT g_handTracker[2] = {nullptr, nullptr};
XrSession g_handTrackerSess = nullptr;

// Curl of one finger from its joints (metacarpal m .. tip m+4): 0 straight, 1 fully bent.
float FingerCurl(const XrHandJointLocationEXT* j, int m, bool thumb) {
    auto P = [&](int k) { return j[k].pose.position; };
    auto D = [](XrVector3f a, XrVector3f b) {
        const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return sqrtf(x * x + y * y + z * z);
    };
    const int first = thumb ? m + 1 : m + 1;  // proximal
    const int tip = thumb ? m + 3 : m + 4;
    float len = 0.f;
    for (int k = first; k < tip; ++k) len += D(P(k + 1), P(k));
    if (len < 1e-4f) return 0.f;
    const float straight = D(P(tip), P(first)) / len;  // 1 straight .. ~0.35 fist
    const float c = (1.f - straight) / (thumb ? 0.30f : 0.60f);
    return c < 0.f ? 0.f : c > 1.f ? 1.f : c;
}


// Headset capture: the exact swapchain images handed to the compositor (after the blit), plus the layer views.
std::atomic<bool> g_hmdReq{false}, g_hmdReady{false};
std::mutex g_hmdMu;
std::vector<uint8_t> g_hmdPx[2];
uint32_t g_hmdW[2] = {}, g_hmdH[2] = {}, g_hmdPitch[2] = {};
int g_hmdFmt = 0;
char g_hmdInfo[2][256] = {};

void HmdCapture(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* img) {
    D3D11_TEXTURE2D_DESC d;
    img->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    ctx->CopyResource(st, img);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        std::lock_guard<std::mutex> lk(g_hmdMu);
        g_hmdPx[eye].assign((uint8_t*)m.pData, (uint8_t*)m.pData + (size_t)m.RowPitch * d.Height);
        g_hmdW[eye] = d.Width;
        g_hmdH[eye] = d.Height;
        g_hmdPitch[eye] = m.RowPitch;
        g_hmdFmt = (int)d.Format;
        ctx->Unmap(st, 0);
    }
    st->Release();
}

std::string ResultStr(XrResult r) {
    char b[64] = "?";
    if (g_inst && xrResultToString) xrResultToString(g_inst, r, b);
    else sprintf_s(b, "%d", r);
    return b;
}
#define XR_CHECK(expr)                                                                   \
    do {                                                                                 \
        XrResult r_ = (expr);                                                            \
        if (!Succeeded(r_)) { Log("xr: %s failed: %s", #expr, ResultStr(r_).c_str()); return false; } \
    } while (0)

// ---- motion controller actions ----
enum Act { A_GRIP, A_AIM, A_STICK, A_TRIGGER, A_SQUEEZE, A_TRACKPAD, A_A, A_B, A_STICKCLICK, A_TRIGGERTOUCH, A_HAPTIC, A_COUNT };
XrActionSet g_actionSet = nullptr;
XrAction g_act[A_COUNT] = {};
XrPath g_handPath[2] = {0, 0};
XrSpace g_gripSpace[2] = {}, g_aimSpace[2] = {};
bool g_actionsReady = false;
SRWLOCK g_inputLock = SRWLOCK_INIT;
InputSnapshot g_input;
bool g_profileLogged = false;

XrPath P(const char* s) {
    XrPath p = 0;
    xrStringToPath(g_inst, s, &p);
    return p;
}

bool CreateActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "gameplay");
    strcpy_s(si.localizedActionSetName, "Gameplay");
    XR_CHECK(xrCreateActionSet(g_inst, &si, &g_actionSet));
    g_handPath[0] = P("/user/hand/left");
    g_handPath[1] = P("/user/hand/right");
    static const struct { Act a; const char* name; XrActionType t; } kActs[] = {
        {A_GRIP, "grip_pose", XR_ACTION_TYPE_POSE_INPUT},       {A_AIM, "aim_pose", XR_ACTION_TYPE_POSE_INPUT},
        {A_STICK, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},  {A_TRIGGER, "trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {A_SQUEEZE, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT},      {A_TRACKPAD, "trackpad_force", XR_ACTION_TYPE_FLOAT_INPUT},
        {A_A, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT},         {A_B, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {A_STICKCLICK, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {A_TRIGGERTOUCH, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {A_HAPTIC, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
    };
    for (const auto& k : kActs) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ci.actionName, k.name);
        strcpy_s(ci.localizedActionName, k.name);
        ci.actionType = k.t;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = g_handPath;
        XR_CHECK(xrCreateAction(g_actionSet, &ci, &g_act[k.a]));
    }
    // Suggested bindings. Physical controls only; game meaning lives in vrmap.cpp.
    struct B { Act a; const char* path; };
    auto suggest = [](const char* profile, const B* b, int n) {
        std::vector<XrActionSuggestedBinding> v;
        for (int i = 0; i < n; ++i)
            for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
                char full[160];
                sprintf_s(full, "%s%s", hand, b[i].path);
                XrPath p = 0;
                if (Succeeded(xrStringToPath(g_inst, full, &p))) v.push_back({g_act[b[i].a], p});
            }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = P(profile);
        sb.countSuggestedBindings = (uint32_t)v.size();
        sb.suggestedBindings = v.data();
        XrResult r = xrSuggestInteractionProfileBindings(g_inst, &sb);
        Log("xr: suggest %s (%zu bindings): %s", profile, v.size(), ResultStr(r).c_str());
    };
    static const B kIndex[] = {
        {A_GRIP, "/input/grip/pose"}, {A_AIM, "/input/aim/pose"}, {A_STICK, "/input/thumbstick"},
        {A_TRIGGER, "/input/trigger/value"}, {A_SQUEEZE, "/input/squeeze/value"}, {A_TRACKPAD, "/input/trackpad/force"},
        {A_A, "/input/a/click"}, {A_B, "/input/b/click"}, {A_STICKCLICK, "/input/thumbstick/click"},
        {A_TRIGGERTOUCH, "/input/trigger/touch"}, {A_HAPTIC, "/output/haptic"},
    };
    suggest("/interaction_profiles/valve/index_controller", kIndex, (int)(sizeof(kIndex) / sizeof(kIndex[0])));
    // Touch: left x/y and right a/b land on the same physical-position actions ("lower"/"upper" face button)
    static const B kTouchL[] = {{A_A, "/input/x/click"}, {A_B, "/input/y/click"}};
    static const B kTouchR[] = {{A_A, "/input/a/click"}, {A_B, "/input/b/click"}};
    static const B kTouchCommon[] = {
        {A_GRIP, "/input/grip/pose"}, {A_AIM, "/input/aim/pose"}, {A_STICK, "/input/thumbstick"},
        {A_TRIGGER, "/input/trigger/value"}, {A_SQUEEZE, "/input/squeeze/value"},
        {A_STICKCLICK, "/input/thumbstick/click"}, {A_TRIGGERTOUCH, "/input/trigger/touch"}, {A_HAPTIC, "/output/haptic"},
    };
    {
        std::vector<XrActionSuggestedBinding> v;
        auto add = [&](const char* hand, const B* b, int n) {
            for (int i = 0; i < n; ++i) {
                char full[160];
                sprintf_s(full, "%s%s", hand, b[i].path);
                XrPath p = 0;
                if (Succeeded(xrStringToPath(g_inst, full, &p))) v.push_back({g_act[b[i].a], p});
            }
        };
        add("/user/hand/left", kTouchCommon, 8);
        add("/user/hand/right", kTouchCommon, 8);
        add("/user/hand/left", kTouchL, 2);
        add("/user/hand/right", kTouchR, 2);
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = P("/interaction_profiles/oculus/touch_controller");
        sb.countSuggestedBindings = (uint32_t)v.size();
        sb.suggestedBindings = v.data();
        Log("xr: suggest touch (%zu bindings): %s", v.size(), ResultStr(xrSuggestInteractionProfileBindings(g_inst, &sb)).c_str());
    }
    // D-226 (owner: "will the fist combat work on Quest, Index or other SteamVR controllers?"): PSVR2 reaches us as the
    // Touch profile (owner log), Index has its own; Vive wands, Windows Mixed Reality and HP Reverb G2 controllers had
    // no bindings and fell back to the simple profile (no stick, no grip: no movement, no fists). The fist test is
    // "grip squeezed (or fingers curled where sensed)", so a grip BUTTON (squeeze/click, 0 or 1 into the float action)
    // works too. Wands: trackpad = stick, trackpad click = stick click, menu = B (no A button exists).
    {
        static const B kHpL[] = {{A_A, "/input/x/click"}, {A_B, "/input/y/click"}};
        static const B kHpR[] = {{A_A, "/input/a/click"}, {A_B, "/input/b/click"}};
        static const B kHpCommon[] = {
            {A_GRIP, "/input/grip/pose"}, {A_AIM, "/input/aim/pose"}, {A_STICK, "/input/thumbstick"},
            {A_TRIGGER, "/input/trigger/value"}, {A_SQUEEZE, "/input/squeeze/value"},
            {A_STICKCLICK, "/input/thumbstick/click"}, {A_HAPTIC, "/output/haptic"},
        };
        std::vector<XrActionSuggestedBinding> v;
        auto add = [&](const char* hand, const B* b, int n) {
            for (int i = 0; i < n; ++i) {
                char full[160];
                sprintf_s(full, "%s%s", hand, b[i].path);
                XrPath p = 0;
                if (Succeeded(xrStringToPath(g_inst, full, &p))) v.push_back({g_act[b[i].a], p});
            }
        };
        add("/user/hand/left", kHpCommon, 7);
        add("/user/hand/right", kHpCommon, 7);
        add("/user/hand/left", kHpL, 2);
        add("/user/hand/right", kHpR, 2);
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = P("/interaction_profiles/hp/mixed_reality_controller");
        sb.countSuggestedBindings = (uint32_t)v.size();
        sb.suggestedBindings = v.data();
        Log("xr: suggest hp reverb (%zu bindings): %s", v.size(), ResultStr(xrSuggestInteractionProfileBindings(g_inst, &sb)).c_str());
    }
    static const B kWmr[] = {
        {A_GRIP, "/input/grip/pose"}, {A_AIM, "/input/aim/pose"}, {A_STICK, "/input/thumbstick"},
        {A_TRIGGER, "/input/trigger/value"}, {A_SQUEEZE, "/input/squeeze/click"}, {A_STICKCLICK, "/input/thumbstick/click"},
        {A_A, "/input/trackpad/click"}, {A_B, "/input/menu/click"}, {A_HAPTIC, "/output/haptic"},
    };
    suggest("/interaction_profiles/microsoft/motion_controller", kWmr, (int)(sizeof(kWmr) / sizeof(kWmr[0])));
    static const B kVive[] = {
        {A_GRIP, "/input/grip/pose"}, {A_AIM, "/input/aim/pose"}, {A_STICK, "/input/trackpad"},
        {A_TRIGGER, "/input/trigger/value"}, {A_SQUEEZE, "/input/squeeze/click"}, {A_STICKCLICK, "/input/trackpad/click"},
        {A_B, "/input/menu/click"}, {A_HAPTIC, "/output/haptic"},
    };
    suggest("/interaction_profiles/htc/vive_controller", kVive, (int)(sizeof(kVive) / sizeof(kVive[0])));
    static const B kSimple[] = {{A_GRIP, "/input/grip/pose"}, {A_AIM, "/input/aim/pose"}, {A_TRIGGER, "/input/select/click"},
                                {A_B, "/input/menu/click"}, {A_HAPTIC, "/output/haptic"}};
    suggest("/interaction_profiles/khr/simple_controller", kSimple, 5);
    return true;
}

bool AttachActions() {
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &g_actionSet};
    XR_CHECK(xrAttachSessionActionSets(g_sess, &ai));
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, g_act[A_GRIP], g_handPath[h], {{0, 0, 0, 1}, {0, 0, 0}}};
        XR_CHECK(xrCreateActionSpace(g_sess, &ci, &g_gripSpace[h]));
        ci.action = g_act[A_AIM];
        XR_CHECK(xrCreateActionSpace(g_sess, &ci, &g_aimSpace[h]));
    }
    g_actionsReady = true;
    Log("xr: controller actions attached");
    return true;
}

void SyncInput(XrTime time) {
    InputSnapshot s;
    s.time = time;
    s.focused = g_state == XR_SESSION_STATE_FOCUSED;
    if (g_actionsReady && s.focused) {
        XrActiveActionSet as{g_actionSet, 0};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &as};
        if (Succeeded(xrSyncActions(g_sess, &si))) {
            for (int h = 0; h < 2; ++h) {
                HandState& hs = s.hand[h];
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, nullptr, g_handPath[h]};
                XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
                gi.action = g_act[A_GRIP];
                hs.active = Succeeded(xrGetActionStatePose(g_sess, &gi, &ps)) && ps.isActive;
                if (!hs.active) continue;  // neutral: an unbound/absent hand reports nothing
                XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
                const XrFlags64 kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                if (Succeeded(xrLocateSpace(g_gripSpace[h], g_space, time, &loc)) && (loc.locationFlags & kValid) == kValid) {
                    memcpy(&hs.grip, &loc.pose, sizeof(Pose));
                    hs.gripValid = true;
                }
                loc = XrSpaceLocation{XR_TYPE_SPACE_LOCATION};
                if (Succeeded(xrLocateSpace(g_aimSpace[h], g_space, time, &loc)) && (loc.locationFlags & kValid) == kValid) {
                    memcpy(&hs.aim, &loc.pose, sizeof(Pose));
                    hs.aimValid = true;
                }
                XrActionStateVector2f v2{XR_TYPE_ACTION_STATE_VECTOR2F};
                gi.action = g_act[A_STICK];
                if (Succeeded(xrGetActionStateVector2f(g_sess, &gi, &v2)) && v2.isActive) {
                    hs.stick[0] = v2.currentState.x;
                    hs.stick[1] = v2.currentState.y;
                }
                auto f = [&](Act a) {
                    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
                    gi.action = g_act[a];
                    return Succeeded(xrGetActionStateFloat(g_sess, &gi, &st)) && st.isActive ? st.currentState : 0.f;
                };
                auto b = [&](Act a) {
                    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
                    gi.action = g_act[a];
                    return Succeeded(xrGetActionStateBoolean(g_sess, &gi, &st)) && st.isActive && st.currentState;
                };
                hs.trigger = f(A_TRIGGER);
                hs.squeeze = f(A_SQUEEZE);
                hs.trackpadForce = f(A_TRACKPAD);
                hs.a = b(A_A);
                hs.b = b(A_B);
                hs.stickClick = b(A_STICKCLICK);
                hs.triggerTouch = b(A_TRIGGERTOUCH);
                if (g_handExt) {
                    if (g_handTrackerSess != g_sess) {  // (re)create for this session
                        g_handTrackerSess = g_sess;
                        for (int k = 0; k < 2; ++k) {
                            XrHandTrackerCreateInfoEXT hc{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, nullptr,
                                                          k ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT, 0};
                            g_handTracker[k] = nullptr;
                            XrResult r = xrCreateHandTrackerEXT(g_sess, &hc, &g_handTracker[k]);
                            Log("xr: hand tracker %s: %d", k ? "right" : "left", (int)r);
                        }
                    }
                    if (g_handTracker[h]) {
                        XrHandJointLocationEXT jl[XR_HAND_JOINT_COUNT_EXT]{};
                        XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, 0, XR_HAND_JOINT_COUNT_EXT, jl};
                        XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, g_space, time};
                        if (Succeeded(xrLocateHandJointsEXT(g_handTracker[h], &li, &locs)) && locs.isActive) {
                            bool ok = true;
                            for (uint32_t k = 2; k < XR_HAND_JOINT_COUNT_EXT; ++k)
                                ok &= (jl[k].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                            if (ok) {
                                hs.fingerCurl[0] = FingerCurl(jl, 2, true);
                                for (int fi = 1; fi < 5; ++fi) hs.fingerCurl[fi] = FingerCurl(jl, 1 + 5 * fi, false);
                                hs.fingersValid = true;
                            }
                        }
                    }
                }
            }
            if (!g_profileLogged) {
                for (int h = 0; h < 2; ++h) {
                    XrInteractionProfileState ip{XR_TYPE_INTERACTION_PROFILE_STATE};
                    uint32_t n = 0;
                    if (Succeeded(xrGetCurrentInteractionProfile(g_sess, g_handPath[h], &ip)) && ip.interactionProfile)
                        xrPathToString(g_inst, ip.interactionProfile, sizeof(s.profile[h]), &n, s.profile[h]);
                }
                if (s.profile[0][0] || s.profile[1][0]) {
                    Log("xr: interaction profile left=%s right=%s", s.profile[0], s.profile[1]);
                    g_profileLogged = true;
                }
            }
        }
    }
    AcquireSRWLockExclusive(&g_inputLock);
    s.serial = g_input.serial + 1;
    if (!s.profile[0][0]) memcpy(s.profile, g_input.profile, sizeof(s.profile));
    g_input = s;
    ReleaseSRWLockExclusive(&g_inputLock);
}

// ---- runtime loading -------------------------------------------------------------------------------------------------
bool LoadRuntime() {
    // the runtime the OpenXR loader would pick: XR_RUNTIME_JSON (per-process override, e.g. Virtual Desktop's VDXR
    // without making it the system runtime), else the system's active runtime
    wchar_t json[MAX_PATH] = L"";
    DWORD envLen = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", json, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
        Log("xr: runtime from XR_RUNTIME_JSON: %ls", json);
    } else {
        json[0] = 0;
        DWORD sz = sizeof(json);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr,
                         json, &sz) != ERROR_SUCCESS) {
            Log("xr: no active OpenXR runtime in the registry");
            return false;
        }
    }
    sprintf_s(g_runtimeJson, "%ls (%s)", json, envLen > 0 && envLen < MAX_PATH ? "XR_RUNTIME_JSON" : "system default");
    FILE* f = nullptr;
    if (_wfopen_s(&f, json, L"rb") || !f) { Log("xr: cannot open %ls", json); return false; }
    char text[8192] = {};
    fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    const char* k = strstr(text, "\"library_path\"");
    const char* q1 = k ? strchr(k + 14, '"') : nullptr;
    const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
    if (!q2) { Log("xr: no library_path in %ls", json); return false; }
    std::string rel(q1 + 1, q2);
    std::string lib;
    for (size_t i = 0; i < rel.size(); ++i) {  // JSON "\\" -> "\"
        if (rel[i] == '\\' && i + 1 < rel.size() && rel[i + 1] == '\\') ++i;
        lib += rel[i];
    }
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, json);
    *wcsrchr(dir, L'\\') = 0;
    wchar_t path[MAX_PATH];
    if (lib.size() > 1 && lib[1] == ':') swprintf_s(path, L"%hs", lib.c_str());
    else swprintf_s(path, L"%s\\%hs", dir, lib.c_str());
    HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { Log("xr: LoadLibrary %ls failed (%lu)", path, GetLastError()); return false; }
    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) { Log("xr: %ls has no xrNegotiateLoaderRuntimeInterface", path); return false; }
    XrNegotiateLoaderInfo li{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, 1, sizeof(li), 1, 1, MakeVersion(1, 0, 0),
                             MakeVersion(1, 0x3ff, 0xfff)};
    XrNegotiateRuntimeRequest rq{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, 1, sizeof(rq), 0, 0, nullptr};
    XrResult r = negotiate(&li, &rq);
    if (!Succeeded(r) || !rq.getInstanceProcAddr) { Log("xr: runtime negotiation failed (%d)", r); return false; }
    gipa = rq.getInstanceProcAddr;
    Log("xr: runtime %ls interface=%u api=%llu.%llu.%llu", path, rq.runtimeInterfaceVersion,
        rq.runtimeApiVersion >> 48, (rq.runtimeApiVersion >> 32) & 0xffff, rq.runtimeApiVersion & 0xffffffff);
    return true;
}

// Every structure type used must have the expected name (guards the hand-written declarations).
bool VerifyTypes() {
    static const struct { XrStructureType t; const char* name; } kTypes[] = {
        {XR_TYPE_EXTENSION_PROPERTIES, "XR_TYPE_EXTENSION_PROPERTIES"},
        {XR_TYPE_INSTANCE_CREATE_INFO, "XR_TYPE_INSTANCE_CREATE_INFO"},
        {XR_TYPE_SYSTEM_GET_INFO, "XR_TYPE_SYSTEM_GET_INFO"},
        {XR_TYPE_SYSTEM_PROPERTIES, "XR_TYPE_SYSTEM_PROPERTIES"},
        {XR_TYPE_VIEW_LOCATE_INFO, "XR_TYPE_VIEW_LOCATE_INFO"},
        {XR_TYPE_VIEW, "XR_TYPE_VIEW"},
        {XR_TYPE_SESSION_CREATE_INFO, "XR_TYPE_SESSION_CREATE_INFO"},
        {XR_TYPE_SWAPCHAIN_CREATE_INFO, "XR_TYPE_SWAPCHAIN_CREATE_INFO"},
        {XR_TYPE_SESSION_BEGIN_INFO, "XR_TYPE_SESSION_BEGIN_INFO"},
        {XR_TYPE_VIEW_STATE, "XR_TYPE_VIEW_STATE"},
        {XR_TYPE_FRAME_END_INFO, "XR_TYPE_FRAME_END_INFO"},
        {XR_TYPE_EVENT_DATA_BUFFER, "XR_TYPE_EVENT_DATA_BUFFER"},
        {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED"},
        {XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, "XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING"},
        {XR_TYPE_INSTANCE_PROPERTIES, "XR_TYPE_INSTANCE_PROPERTIES"},
        {XR_TYPE_FRAME_WAIT_INFO, "XR_TYPE_FRAME_WAIT_INFO"},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION, "XR_TYPE_COMPOSITION_LAYER_PROJECTION"},
        {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, "XR_TYPE_REFERENCE_SPACE_CREATE_INFO"},
        {XR_TYPE_VIEW_CONFIGURATION_VIEW, "XR_TYPE_VIEW_CONFIGURATION_VIEW"},
        {XR_TYPE_FRAME_STATE, "XR_TYPE_FRAME_STATE"},
        {XR_TYPE_FRAME_BEGIN_INFO, "XR_TYPE_FRAME_BEGIN_INFO"},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, "XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW"},
        {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, "XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO"},
        {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, "XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO"},
        {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, "XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO"},
        {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, "XR_TYPE_GRAPHICS_BINDING_D3D11_KHR"},
        {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, "XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR"},
        {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR, "XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR"},
        {XR_TYPE_HAPTIC_VIBRATION, "XR_TYPE_HAPTIC_VIBRATION"},
        {XR_TYPE_ACTION_STATE_BOOLEAN, "XR_TYPE_ACTION_STATE_BOOLEAN"},
        {XR_TYPE_ACTION_STATE_FLOAT, "XR_TYPE_ACTION_STATE_FLOAT"},
        {XR_TYPE_ACTION_STATE_VECTOR2F, "XR_TYPE_ACTION_STATE_VECTOR2F"},
        {XR_TYPE_ACTION_STATE_POSE, "XR_TYPE_ACTION_STATE_POSE"},
        {XR_TYPE_ACTION_SET_CREATE_INFO, "XR_TYPE_ACTION_SET_CREATE_INFO"},
        {XR_TYPE_ACTION_CREATE_INFO, "XR_TYPE_ACTION_CREATE_INFO"},
        {XR_TYPE_ACTION_SPACE_CREATE_INFO, "XR_TYPE_ACTION_SPACE_CREATE_INFO"},
        {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, "XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING"},
        {XR_TYPE_INTERACTION_PROFILE_STATE, "XR_TYPE_INTERACTION_PROFILE_STATE"},
        {XR_TYPE_ACTION_STATE_GET_INFO, "XR_TYPE_ACTION_STATE_GET_INFO"},
        {XR_TYPE_HAPTIC_ACTION_INFO, "XR_TYPE_HAPTIC_ACTION_INFO"},
        {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, "XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO"},
        {XR_TYPE_ACTIONS_SYNC_INFO, "XR_TYPE_ACTIONS_SYNC_INFO"},
    };
    bool ok = true;
    for (const auto& e : kTypes) {
        char b[64] = "";
        xrStructureTypeToString(g_inst, e.t, b);
        if (strcmp(b, e.name) != 0) {
            Log("xr: structure type %d is '%s', expected %s", (int)e.t, b, e.name);
            ok = false;
        }
    }
    Log("xr: structure types %s", ok ? "verified" : "MISMATCH -> not starting");
    return ok;
}

bool CreateInstance() {
    XR_CHECK(GET(nullptr, xrEnumerateInstanceExtensionProperties) ? 0 : -1);
    XR_CHECK(GET(nullptr, xrCreateInstance) ? 0 : -1);
    uint32_t n = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> ext(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data()));
    bool d3d11 = false;
    for (auto& e : ext) d3d11 |= !strcmp(e.extensionName, "XR_KHR_D3D11_enable");
    if (!d3d11) { Log("xr: runtime lacks XR_KHR_D3D11_enable"); return false; }
    for (auto& e : ext) g_handExt |= !strcmp(e.extensionName, "XR_EXT_hand_tracking");
    const char* exts[] = {"XR_KHR_D3D11_enable", "XR_EXT_hand_tracking"};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "GoW VR (gowvr)");
    strcpy_s(ci.applicationInfo.engineName, "gowvr");
    ci.applicationInfo.apiVersion = MakeVersion(1, 0, 0);
    ci.enabledExtensionCount = g_handExt ? 2 : 1;
    ci.enabledExtensionNames = exts;
    XR_CHECK(xrCreateInstance(&ci, &g_inst));
    bool all = GET(g_inst, xrDestroyInstance) && GET(g_inst, xrGetInstanceProperties) &&
               GET(g_inst, xrStructureTypeToString) && GET(g_inst, xrResultToString) && GET(g_inst, xrGetSystem) && GET(g_inst, xrGetSystemProperties) &&
               GET(g_inst, xrGetD3D11GraphicsRequirementsKHR) && GET(g_inst, xrEnumerateViewConfigurationViews) &&
               GET(g_inst, xrCreateSession) && GET(g_inst, xrDestroySession) &&
               GET(g_inst, xrDestroySwapchain) && GET(g_inst, xrDestroySpace) &&
               GET(g_inst, xrEnumerateSwapchainFormats) && GET(g_inst, xrCreateSwapchain) &&
               GET(g_inst, xrEnumerateSwapchainImages) && GET(g_inst, xrCreateReferenceSpace) &&
               GET(g_inst, xrPollEvent) && GET(g_inst, xrBeginSession) && GET(g_inst, xrEndSession) &&
               GET(g_inst, xrWaitFrame) && GET(g_inst, xrBeginFrame) && GET(g_inst, xrEndFrame) &&
               GET(g_inst, xrLocateViews) && GET(g_inst, xrAcquireSwapchainImage) &&
               GET(g_inst, xrWaitSwapchainImage) && GET(g_inst, xrReleaseSwapchainImage) &&
               GET(g_inst, xrStringToPath) && GET(g_inst, xrPathToString) && GET(g_inst, xrCreateActionSet) &&
               GET(g_inst, xrCreateAction) && GET(g_inst, xrSuggestInteractionProfileBindings) &&
               GET(g_inst, xrAttachSessionActionSets) && GET(g_inst, xrSyncActions) &&
               GET(g_inst, xrGetActionStateBoolean) && GET(g_inst, xrGetActionStateFloat) &&
               GET(g_inst, xrGetActionStateVector2f) && GET(g_inst, xrGetActionStatePose) &&
               GET(g_inst, xrCreateActionSpace) && GET(g_inst, xrLocateSpace) &&
               GET(g_inst, xrGetCurrentInteractionProfile) && GET(g_inst, xrApplyHapticFeedback);
    if (!all) return false;
    if (!VerifyTypes()) return false;
    if (!GET(g_inst, xrStopHapticFeedback)) xrStopHapticFeedback = nullptr;  // optional: rumble then just runs out
    if (g_handExt) {
        static const struct { XrStructureType t; const char* name; } kHand[] = {
            {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, "XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT"},
            {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, "XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT"},
            {XR_TYPE_HAND_JOINT_LOCATIONS_EXT, "XR_TYPE_HAND_JOINT_LOCATIONS_EXT"}};
        for (const auto& e : kHand) {
            char b[64] = "";
            xrStructureTypeToString(g_inst, e.t, b);
            if (strcmp(b, e.name) != 0) { Log("xr: hand tracking type %d is '%s' - finger sensing off", (int)e.t, b); g_handExt = false; }
        }
        if (g_handExt && !(GET(g_inst, xrCreateHandTrackerEXT) && GET(g_inst, xrLocateHandJointsEXT) &&
                           GET(g_inst, xrDestroyHandTrackerEXT)))
            g_handExt = false;
        Log("xr: XR_EXT_hand_tracking %s", g_handExt ? "on (finger curl from the controllers' finger sensing)" : "unavailable");
    }
    // D-115 (owner crash 2026-10-05 18:49, vrclient_x64.dll+0x16b008 reading 0 inside xrCreateActionSet): SteamVR found
    // no headset (vrserver: "PlayStation VR2 HMD is not found", "Refusing connect ... VRInitError_Init_HmdNotFound") but
    // still handed out an instance whose input calls dereference null. No headset system -> no actions: the instance is
    // dropped and the 3 s retry builds a new one once the headset is there
    {
        XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        const XrResult r = xrGetSystem(g_inst, &si, &sys);
        if (!Succeeded(r) || sys == XR_NULL_SYSTEM_ID) {
            if (g_failCount < 3 || g_failCount % 20 == 19)  // (quiet like the setup retry below)
                Log("xr: no headset yet (%s) - not touching the runtime's input; retrying", ResultStr(r).c_str());
            return false;
        }
    }
    if (!CreateActions()) Log("xr: controller actions unavailable");
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (Succeeded(xrGetInstanceProperties(g_inst, &ip))) {
        Log("xr: instance on %s %llu.%llu.%llu", ip.runtimeName, ip.runtimeVersion >> 48,
            (ip.runtimeVersion >> 32) & 0xffff, ip.runtimeVersion & 0xffffffff);
        sprintf_s(g_runtimeName, "%s %llu.%llu.%llu", ip.runtimeName, ip.runtimeVersion >> 48,
                  (ip.runtimeVersion >> 32) & 0xffff, ip.runtimeVersion & 0xffffffff);
    }
    return true;
}

// The headset system is looked up for every session attempt: an id obtained while the headset was asleep or not yet
// tracked is rejected later (XR_ERROR_SYSTEM_INVALID on every F9 - owner report "F9 does not work anymore").
bool RefreshSystem() {
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XR_CHECK(xrGetSystem(g_inst, &si, &g_sys));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (Succeeded(xrGetSystemProperties(g_inst, g_sys, &sp)) && strcmp(g_systemName, sp.systemName)) {
        strcpy_s(g_systemName, sp.systemName);
        Log("xr: system '%s' vendor=0x%x", sp.systemName, sp.vendorId);
    }
    return true;
}

// ---- blit (eye image -> swapchain image), isolated in its own device-context state ----------------------------------
const char kShader[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 lin(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 ps(V i) : SV_Target { return float4(lin(saturate(src.Sample(smp, i.uv).rgb)), 1); }
float4 ps_hud(V i) : SV_Target {
    float4 c = src.Sample(smp, i.uv);
    float a = saturate(c.a);
    float3 rgb = a > 1e-4 ? saturate(c.rgb / a) : 0;
    return float4(lin(rgb) * a, a);
}
)";

bool CreateBlit() {
    if (g_blitState) return true;  // D-110: device resources survive a session rebuild
    HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
    using PFN_Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob**, ID3DBlob**);
    auto compile = c ? (PFN_Compile)GetProcAddress(c, "D3DCompile") : nullptr;
    if (!compile) { Log("xr: d3dcompiler_47 unavailable"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, sizeof(kShader) - 1, "blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        Log("xr: blit shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    bool ok = SUCCEEDED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    vsb->Release();
    psb->Release();
    ID3DBlob* hb = nullptr;
    if (SUCCEEDED(compile(kShader, sizeof(kShader) - 1, "blit", nullptr, nullptr, "ps_hud", "ps_5_0", 0, 0, &hb, &err))) {
        if (FAILED(g_dev->CreatePixelShader(hb->GetBufferPointer(), hb->GetBufferSize(), nullptr, &g_psHud))) g_psHud = nullptr;
        hb->Release();
    }
    if (!g_psHud) Log("xr: HUD layer shader unavailable - the HUD is pasted into the eye images");
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ok = ok && SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_samp)) && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_rs));
    D3D_FEATURE_LEVEL fl = g_dev->GetFeatureLevel(), chosen;
    ok = ok && SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&g_dev1)) &&
         SUCCEEDED(g_dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen,
                                                    &g_blitState));
    if (!ok) Log("xr: blit resources failed");
    return ok;
}

void BlitSrv(ID3D11DeviceContext1* ctx, ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv,
             uint32_t w, uint32_t h);

void Blit(ID3D11DeviceContext1* ctx, ID3D11Texture2D* src, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g_dev->CreateShaderResourceView(src, nullptr, &srv))) return;
    BlitSrv(ctx, srv, g_ps, rtv, w, h);
    srv->Release();
}

// a full-screen pass of `ps` sampling `srv` into the top-left w x h of `rtv`
void BlitSrv(ID3D11DeviceContext1* ctx, ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv,
             uint32_t w, uint32_t h) {
    ID3DDeviceContextState* prev = nullptr;
    ctx->SwapDeviceContextState(g_blitState, &prev);  // the game's whole pipeline state is parked in `prev`
    D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->RSSetState(g_rs);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView* noRtv = nullptr;
    ctx->OMSetRenderTargets(1, &noRtv, nullptr);
    ctx->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
}

// ---- session ---------------------------------------------------------------------------------------------------------
bool CreateSession(ID3D11Device* dev) {
    g_dev = dev;
    if (!RefreshSystem()) return false;
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req));
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC ad_desc{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad)))
        ad->GetDesc(&ad_desc);
    if (ad) ad->Release();
    if (dxgi) dxgi->Release();
    if (ad_desc.AdapterLuid.LowPart != req.adapterLuidLow || ad_desc.AdapterLuid.HighPart != req.adapterLuidHigh)
        Log("xr: WARNING game adapter %ls is not the HMD adapter", ad_desc.Description);
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, nullptr, dev};
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &gb, 0, g_sys};
    XR_CHECK(xrCreateSession(g_inst, &sc, &g_sess));
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
    XR_CHECK(xrCreateReferenceSpace(g_sess, &rs, &g_space));

    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(g_sess, 0, &n, nullptr));
    std::vector<int64_t> fmts(n);
    XR_CHECK(xrEnumerateSwapchainFormats(g_sess, n, &n, fmts.data()));
    for (int64_t want : {(int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
        for (int64_t f : fmts)
            if (!g_format && f == want) g_format = f;
    if (!g_format) { Log("xr: no sRGB 8-bit swapchain format offered"); return false; }

    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XR_CHECK(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv));
    for (int e = 0; e < 2; ++e) {
        EyeSwap& s = g_eye[e];
        s.w = vcv[e].recommendedImageRectWidth;
        s.h = vcv[e].recommendedImageRectHeight;
        g_eyeMaxW = vcv[e].maxImageRectWidth;
        g_eyeMaxH = vcv[e].maxImageRectHeight;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = g_format;
        ci.sampleCount = 1;
        ci.width = s.w;
        ci.height = s.h;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(g_sess, &ci, &s.sc));
        XR_CHECK(xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr));
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(s.sc, n, &n, (XrBaseHeader*)imgs.data()));
        for (auto& im : imgs) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = (DXGI_FORMAT)g_format;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            if (FAILED(dev->CreateRenderTargetView(im.texture, &rd, &rtv))) { Log("xr: swapchain RTV failed"); return false; }
            s.images.push_back(im.texture);
            s.rtvs.push_back(rtv);
        }
    }
    Log("xr: session created; swapchains %ux%u / %ux%u fmt=%lld x%zu images", g_eye[0].w, g_eye[0].h, g_eye[1].w,
        g_eye[1].h, g_format, g_eye[0].images.size());
    if (g_actionSet) AttachActions();
    return CreateBlit();
}

void PollEvents() {
    // SteamVR recenter (owner: must behave like F8): the runtime moves the reference space and posts a change-pending
    // event. D-079 (owner: "it recenters two times"): LIVE every SteamVR recenter posted the event twice (20 ms apart),
    // and each one recentered at once (before the new origin applies) and again ~0.5 s later - a visible double jump.
    // Now all events of one recenter make ONE recenter, at the first frame whose display time reaches the latest
    // changeTime (when the new origin is in effect), ~1 s at the latest.
    static XrTime recenterAt = 0;
    static int recenterWait = 0;
    if (recenterAt && (g_frame.predictedDisplayTime >= recenterAt || --recenterWait <= 0)) {
        recenterAt = 0;
        Log("xr: runtime recenter in effect -> recenter (once)");
        stereo::HeadcamRecenter();
    }
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult pr;
    while ((pr = xrPollEvent(g_inst, &ev)) == XR_SUCCESS /* XR_EVENT_UNAVAILABLE = 4 */) {
        if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto* rc = (const XrEventDataReferenceSpaceChangePending*)&ev;
            Log("xr: reference space change (runtime recenter) space %d changeTime %lld (frame %lld) poseValid %d", (int)rc->referenceSpaceType,
                (long long)rc->changeTime, (long long)g_frame.predictedDisplayTime, (int)rc->poseValid);
            const XrTime at = rc->changeTime > 0 ? rc->changeTime : 1;
            if (!recenterAt || at > recenterAt) recenterAt = at;
            recenterWait = 90;
        }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            g_state = s->state;
            Log("xr: session state -> %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY && !g_running) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                XrResult r = xrBeginSession(g_sess, &bi);
                g_running = Succeeded(r);
                Log("xr: begin session %s", ResultStr(r).c_str());
            } else if (g_state == XR_SESSION_STATE_STOPPING && g_running) {
                xrEndSession(g_sess);
                g_running = false;
                g_frameBegun = false;
            } else if (g_state == XR_SESSION_STATE_LOSS_PENDING) {
                // D-110: the runtime is going away (SteamVR closed / restarted / crashed): the session - and with it
                // the instance - cannot be used again; both are rebuilt (3 s retry) once SteamVR is back
                g_rebuild = 2;
            } else if (g_state == XR_SESSION_STATE_EXITING) {
                g_rebuild = g_rebuild > 1 ? g_rebuild : 1;  // D-110: the session ended for good: a new one is built
            }
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (pr == XR_ERROR_INSTANCE_LOST) g_rebuild = 2;  // D-110
}

// D-083 (owner: "make game parts that are only in 2D be in floating black space as a screen"): a frame without a stereo
// eye pair (pause menu - single flat kick, see mutate.cpp Screen2DFrame -, loading screens, videos) is the flat game
// image; it was stretched over the whole headset view, head-locked. Now it is a screen in the dark: a quad layer fixed
// in the room, placed in front of the head when the flat frames start (and again on a recenter). 1-2 lone flat frames
// between stereo frames repeat the last stereo layer instead (no screen flash).
EyeSwap g_quad;
XrPosef g_quadPose{};
bool g_quadPlaced = false;
int g_monoRun = 0;
std::atomic<bool> g_quadReplace{false};  // D-085: set by XrScreenRecenter (F8 / runtime recenter in effect)
XrCompositionLayerProjectionView g_lastPv[2] = {};
bool g_haveLastPv = false;
std::atomic<uint64_t> g_quadFrames{0};

uint32_t g_quadSrcW = 0, g_quadSrcH = 0;  // the game frame size the screen swapchain was made for

// The last layer the headset got (owner: "when I turn from 3D to the 2D screen I see the enlarged UI for half a
// second"): while the game frame is not yet the size its mode needs (frame/rendersize: square for the stereo view, the
// window's own size for the flat screen - the game resizes a frame or two after the switch) that layer is shown again,
// at most hold_ms (1500) in a row.
enum LastLayer { kLastNone, kLastProjection, kLastQuad };
LastLayer g_lastKind = kLastNone;
XrCompositionLayerQuad g_lastQuadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
ULONGLONG g_holdSince = 0;
uint64_t g_holdFrames = 0;

// VR settings panel (panel.cpp) and its pointer dot: static images, uploaded only when they change
EyeSwap g_panelSc, g_cursorSc;
EyeSwap g_hudSc[2];  // the HUD layer of each eye (sharpui.cpp SharpUiHudLayer)
uint64_t g_hudQuads = 0;

bool EnsureRtSwapchain(EyeSwap& s, uint32_t w, uint32_t h, const char* what) {
    if (s.sc && s.w == w && s.h == h) return true;
    if (s.sc) {
        for (auto* r : s.rtvs) if (r) r->Release();
        s.rtvs.clear();
        s.images.clear();
        xrDestroySwapchain(s.sc);
        s.sc = nullptr;
        s.pending = false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (!Succeeded(xrCreateSwapchain(g_sess, &ci, &s.sc))) { Log("xr: %s swapchain failed", what); s.sc = nullptr; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(s.sc, n, &n, (XrBaseHeader*)imgs.data());
    for (auto& im : imgs) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = (DXGI_FORMAT)g_format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(g_dev->CreateRenderTargetView(im.texture, &rd, &rtv))) { Log("xr: %s RTV failed", what); return false; }
        s.images.push_back(im.texture);
        s.rtvs.push_back(rtv);
    }
    s.w = w;
    s.h = h;
    Log("xr: %s swapchain %ux%u", what, w, h);
    return true;
}
bool g_panelUploaded = false, g_cursorUploaded = false;

bool EnsureStaticSwapchain(EyeSwap& s, uint32_t w, uint32_t h, const char* what) {
    if (s.sc && s.w == w && s.h == h) return true;
    if (s.sc) { s.images.clear(); xrDestroySwapchain(s.sc); s.sc = nullptr; s.pending = false; }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g_format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (!Succeeded(xrCreateSwapchain(g_sess, &ci, &s.sc))) { Log("xr: %s swapchain failed", what); s.sc = nullptr; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(s.sc, n, &n, (XrBaseHeader*)imgs.data());
    s.images.clear();
    for (auto& im : imgs) s.images.push_back(im.texture);
    s.w = w;
    s.h = h;
    Log("xr: %s swapchain %ux%u", what, w, h);
    return true;
}

int AcquireReady(EyeSwap& s, const char* what);
void ReleaseImage(EyeSwap& s);

// 0xAARRGGBB pixels into the next image (the swapchain's format is R8G8B8A8 or B8G8R8A8 sRGB)
bool UploadStatic(ID3D11DeviceContext* ctx, EyeSwap& s, const uint32_t* argb, const char* what) {
    const int idx = AcquireReady(s, what);
    if (idx < 0) return false;
    static std::vector<uint32_t> tmp;
    const uint32_t* data = argb;
    if (g_format == (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || g_format == (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM) {
        tmp.resize((size_t)s.w * s.h);
        for (size_t i = 0; i < tmp.size(); ++i) {
            const uint32_t v = argb[i];  // ARGB -> ABGR (R in the low byte)
            tmp[i] = (v & 0xff00ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16;
        }
        data = tmp.data();
    }
    ctx->UpdateSubresource(s.images[idx], 0, nullptr, data, s.w * 4, 0);
    ReleaseImage(s);
    return true;
}

bool EnsureQuadSwapchain(uint32_t bbW, uint32_t bbH) {
    if (g_quad.sc && bbW == g_quadSrcW && bbH == g_quadSrcH) return true;
    if (!bbW || !bbH) return false;
    if (g_quad.sc) {  // the game frame changed size (square VR picture on / off, a resolution change): a new screen
        for (auto* r : g_quad.rtvs) if (r) r->Release();
        g_quad.rtvs.clear();
        g_quad.images.clear();
        xrDestroySwapchain(g_quad.sc);
        g_quad.sc = nullptr;
        g_quad.pending = false;
        Log("xr: screen swapchain dropped (game frame %ux%u -> %ux%u)", g_quadSrcW, g_quadSrcH, bbW, bbH);
    }
    g_quadSrcW = bbW;
    g_quadSrcH = bbH;
    static const float maxW = config::GetFloat("screen_px", 2560.f);
    g_quad.w = (uint32_t)fminf((float)bbW, maxW);
    g_quad.h = (uint32_t)((float)g_quad.w * (float)bbH / (float)bbW + 0.5f);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g_format;
    ci.sampleCount = 1;
    ci.width = g_quad.w;
    ci.height = g_quad.h;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (!Succeeded(xrCreateSwapchain(g_sess, &ci, &g_quad.sc))) { Log("xr: screen swapchain failed"); g_quad.sc = nullptr; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_quad.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(g_quad.sc, n, &n, (XrBaseHeader*)imgs.data());
    for (auto& im : imgs) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = (DXGI_FORMAT)g_format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(g_dev->CreateRenderTargetView(im.texture, &rd, &rtv))) { Log("xr: screen RTV failed"); return false; }
        g_quad.images.push_back(im.texture);
        g_quad.rtvs.push_back(rtv);
    }
    Log("xr: screen swapchain %ux%u (flat game frames as a screen in the dark)", g_quad.w, g_quad.h);
    return true;
}

// in front of the head (yaw only, level), screen_dist m away at eye height
void PlaceQuad() {
    const XrPosef& a = g_views[0].pose;
    const XrPosef& b = g_views[1].pose;
    const XrQuaternionf& q = a.orientation;
    // head forward = q * (0, 0, -1)
    const float fx = -2.f * (q.x * q.z + q.w * q.y), fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    const float yaw = atan2f(-fx, -fz);  // rotation about +Y taking -Z onto the forward
    static const float dist = config::GetFloat("screen_dist", 2.2f);
    g_quadPose.orientation = {0.f, sinf(yaw * 0.5f), 0.f, cosf(yaw * 0.5f)};
    g_quadPose.position = {0.5f * (a.position.x + b.position.x) - sinf(yaw) * dist, 0.5f * (a.position.y + b.position.y),
                           0.5f * (a.position.z + b.position.z) - cosf(yaw) * dist};
    g_quadPlaced = true;
    Log("xr: screen placed %.2f m ahead (yaw %.0f deg, height %.2f)", dist, yaw * 57.2958f, g_quadPose.position.y);
}

// D-110 (code audit H2): everything that belongs to the session, destroyed so a new one can be built - VR turned off,
// a failed / partial CreateSession, or the runtime ending the session (LOSS_PENDING / EXITING / SESSION_LOST). With
// dropInstance the instance and its actions go too (SteamVR restarted: the old instance is lost). Render thread.
void DestroySession(const char* why, bool dropInstance) {
    AcquireSRWLockExclusive(&g_sessLock);
    auto dropSwap = [](EyeSwap& s) {
        for (auto* r : s.rtvs) if (r) r->Release();
        s.rtvs.clear();
        s.images.clear();  // runtime-owned textures
        if (s.sc && xrDestroySwapchain) xrDestroySwapchain(s.sc);
        s.sc = nullptr;
        s.pending = false;
    };
    dropSwap(g_eye[0]);
    dropSwap(g_eye[1]);
    dropSwap(g_quad);
    dropSwap(g_panelSc);
    dropSwap(g_hudSc[0]);
    dropSwap(g_hudSc[1]);
    dropSwap(g_cursorSc);
    g_panelUploaded = g_cursorUploaded = false;
    for (int k = 0; k < 2; ++k) {
        if (g_handTracker[k] && xrDestroyHandTrackerEXT) xrDestroyHandTrackerEXT(g_handTracker[k]);
        g_handTracker[k] = nullptr;
        if (g_gripSpace[k] && xrDestroySpace) xrDestroySpace(g_gripSpace[k]);
        if (g_aimSpace[k] && xrDestroySpace) xrDestroySpace(g_aimSpace[k]);
        g_gripSpace[k] = g_aimSpace[k] = nullptr;
    }
    g_handTrackerSess = nullptr;
    if (g_space && xrDestroySpace) xrDestroySpace(g_space);
    g_space = nullptr;
    if (g_sess && xrDestroySession) xrDestroySession(g_sess);
    g_sess = nullptr;
    g_running = g_frameBegun = g_viewsValid = false;
    g_actionsReady = false;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_quadPlaced = g_haveLastPv = false;
    g_lastKind = kLastNone;  // its swapchains are gone
    g_monoRun = 0;
    if (dropInstance && g_inst) {
        xrDestroyInstance(g_inst);  // (its action set and actions with it)
        g_inst = nullptr;
        g_actionSet = nullptr;
        for (auto& a : g_act) a = nullptr;
    }
    ReleaseSRWLockExclusive(&g_sessLock);
    {   // published input / views of the old session must not be used
        AcquireSRWLockExclusive(&g_inputLock);
        const uint64_t serial = g_input.serial;
        g_input = InputSnapshot{};
        g_input.serial = serial;
        ReleaseSRWLockExclusive(&g_inputLock);
    }
    g_rebuild = 0;
    Log("xr: session destroyed (%s)%s", why, dropInstance ? " with the instance" : "");
}

// D-110 (code audit H1): the next image of a swapchain, ready to draw - or -1. An image is released only after its wait
// succeeded; a wait that times out (XR_TIMEOUT_EXPIRED is a success code) keeps the image acquired and is waited
// again next frame. Before, the timeout counted as ready (blit into an image the compositor still read, release
// rejected) and a failed wait left the image acquired for good - the swapchain out of order, VR stuck until restart.
int AcquireReady(EyeSwap& s, const char* what) {
    if (!s.pending) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t idx = 0;
        const XrResult r = xrAcquireSwapchainImage(s.sc, &ai, &idx);
        if (r != XR_SUCCESS) {
            NoteLoss(r);
            static int logs = 0;
            if (logs++ < 10) Log("xr: acquire %s image: %s", what, ResultStr(r).c_str());
            return -1;
        }
        s.pending = true;
        s.pendingIdx = idx;
    }
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 100000000};  // 100 ms
    const XrResult r = xrWaitSwapchainImage(s.sc, &wi);
    if (r == XR_SUCCESS) {
        s.pending = false;
        return (int)s.pendingIdx;  // the caller draws and releases it
    }
    static uint64_t timeouts = 0, errors = 0;
    if (r == XR_TIMEOUT_EXPIRED) {
        if (++timeouts <= 10 || timeouts % 100 == 0)
            Log("xr: %s image not ready after 100 ms (#%llu) - waited again next frame", what, (unsigned long long)timeouts);
        return -1;
    }
    // a failed wait: the image is not usable; the session is being lost or something is badly wrong
    s.pending = false;
    NoteLoss(r);
    if (++errors <= 10) Log("xr: wait %s image: %s", what, ResultStr(r).c_str());
    return -1;
}

void ReleaseImage(EyeSwap& s) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult r = xrReleaseSwapchainImage(s.sc, &ri);
    if (r != XR_SUCCESS) {
        static int logs = 0;
        if (logs++ < 10) Log("xr: release image: %s", ResultStr(r).c_str());
    }
}

// ---- VR summary in the log ------------------------------------------------------------------------------------------
// Owner: a tester plays, sends gowvr.log, and the next update fixes their setup. Everything needed for that in one
// block: runtime, headset, controllers, image sizes, field of view, the flat screen's and the HUD's size, the hand
// offsets and the game's display settings. Logged once per distinct (stereo / flat screen, game resolution, controller
// profiles) - the first stereo frame, the first flat screen, after a resolution change - at most 16 times.
float Deg(float rad) { return rad * 57.2958f; }

void QRotate(const float q[4], const float v[3], float out[3]) {  // q * v * q^-1, q = (x, y, z, w)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.f * (y * v[2] - z * v[1]), ty = 2.f * (z * v[0] - x * v[2]), tz = 2.f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// The aim pose seen from the grip pose: how the runtime's two controller poses differ (differs per runtime and
// controller - the hand offsets for a new controller follow from it). Degrees: aim pointing up / left of the grip's
// pointing direction, and its up direction twisted about it.
void AimFromGrip(const HandState& h, char* out, size_t n) {
    if (!h.active || !h.gripValid || !h.aimValid) { sprintf_s(out, n, "not tracked"); return; }
    const float conj[4] = {-h.grip.q[0], -h.grip.q[1], -h.grip.q[2], h.grip.q[3]};
    const float fwd[3] = {0.f, 0.f, -1.f}, up[3] = {0.f, 1.f, 0.f};
    float fw[3], uw[3], f[3], u[3];
    QRotate(h.aim.q, fwd, fw);
    QRotate(h.aim.q, up, uw);
    QRotate(conj, fw, f);
    QRotate(conj, uw, u);
    sprintf_s(out, n, "up %.0f left %.0f twist %.0f", Deg(asinf(fmaxf(-1.f, fminf(1.f, f[1])))), Deg(atan2f(-f[0], -f[2])),
              Deg(atan2f(-u[0], u[1])));
}

// "Key=Value ; comment" from the game's settings.ini text
std::string IniValue(const std::string& text, const char* key) {
    const std::string k = std::string("\n") + key + "=";
    size_t p = ("\n" + text).find(k);
    if (p == std::string::npos) return "?";
    p += k.size() - 1;  // in `text` (the prepended "\n" shifts by one)
    size_t e = text.find_first_of(" \t;\r\n", p);
    return text.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

void LogSummary(IDXGISwapChain* sc, const DXGI_SWAP_CHAIN_DESC& d, const InputSnapshot& in, bool stereo, int number) {
    Log("==== VR SUMMARY #%d (%s) ====", number, stereo ? "stereo game view" : "flat screen: menus / videos / loading");
    Log("summary: mod gowvr %s", GOWVR_VERSION);
    {
        const uint8_t* img = (const uint8_t*)GetModuleHandleW(nullptr);
        const auto* nt = (const IMAGE_NT_HEADERS64*)(img + ((const IMAGE_DOS_HEADER*)img)->e_lfanew);
        Log("summary: game GoW.exe stamp 0x%08lx size 0x%08lx - %s", nt->FileHeader.TimeDateStamp,
            nt->OptionalHeader.SizeOfImage, AddrBuildText());
    }
    Log("summary: runtime %s | %s", g_runtimeName[0] ? g_runtimeName : "?", g_runtimeJson);
    Log("summary: headset '%s'", g_systemName);
    Log("summary: eye image %ux%u per eye (the runtime allows up to %ux%u)", g_eye[0].w, g_eye[0].h, g_eyeMaxW, g_eyeMaxH);
    float fovW = 0.f, fovH = 0.f;
    if (g_viewsValid) {
        const XrFovf& l = g_views[0].fov;
        const XrFovf& r = g_views[1].fov;
        fovW = Deg(l.angleRight - l.angleLeft);
        fovH = Deg(l.angleUp - l.angleDown);
        Log("summary: eye fov deg (left right up down): L %.1f %.1f %.1f %.1f, R %.1f %.1f %.1f %.1f -> %.0f x %.0f deg per eye",
            Deg(l.angleLeft), Deg(l.angleRight), Deg(l.angleUp), Deg(l.angleDown), Deg(r.angleLeft), Deg(r.angleRight),
            Deg(r.angleUp), Deg(r.angleDown), fovW, fovH);
    } else {
        Log("summary: eye fov not located yet");
    }
    // the game's picture, its window and the monitor it is on (true pixels, whatever the Windows scaling)
    RECT cr{};
    GetClientRect(d.OutputWindow, &cr);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    const bool mon = GetMonitorInfoW(MonitorFromWindow(d.OutputWindow, MONITOR_DEFAULTTONEAREST), &mi) &&
                     EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    Log("summary: game frame %ux%u (window %ldx%ld), monitor %lux%lu @ %lu Hz", d.BufferDesc.Width, d.BufferDesc.Height,
        cr.right - cr.left, cr.bottom - cr.top, mon ? dm.dmPelsWidth : 0ul, mon ? dm.dmPelsHeight : 0ul,
        mon ? dm.dmDisplayFrequency : 0ul);
    {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        if (wchar_t* s = wcsrchr(p, L'\\')) wcscpy_s(s + 1, MAX_PATH - (s + 1 - p), L"settings.ini");
        std::string text;
        FILE* f = nullptr;
        if (!_wfopen_s(&f, p, L"rb") && f) {
            char buf[16384] = {};
            text.assign(buf, fread(buf, 1, sizeof(buf) - 1, f));
            fclose(f);
        }
        std::string line;
        for (const char* k : {"DisplayMode", "WindowWidth", "WindowHeight", "AspectRatio", "RenderScale", "DLSSQuality",
                              "FSRQuality", "Preset", "Vsync", "MotionBlur", "HDR"})
            line += std::string(" ") + k + "=" + (text.empty() ? "?" : IniValue(text, k));
        Log("summary: game settings.ini%s", text.empty() ? " (not readable)" : line.c_str());
    }
    {
        const float w = config::GetFloat("screen_width", 3.4f), dist = config::GetFloat("screen_dist", 2.2f);
        const float aspect = d.BufferDesc.Width ? (float)d.BufferDesc.Height / (float)d.BufferDesc.Width : 0.5625f;
        const float aW = Deg(2.f * atanf(0.5f * w / dist)), aH = Deg(2.f * atanf(0.5f * w * aspect / dist));
        Log("summary: flat screen %.2f m wide at %.2f m = %.0f x %.0f deg (%.0f%% x %.0f%% of the eye's view), screen_px %.0f",
            w, dist, aW, aH, fovW > 0.f ? 100.f * aW / fovW : 0.f, fovH > 0.f ? 100.f * aH / fovH : 0.f,
            config::GetFloat("screen_px", 2560.f));
    }
    Log("summary: HUD ui_scale %.2f (third person), ui_scale_fp %.2f (first person), sharp_ui %.0f",
        config::GetFloat("ui_scale", 0.35f), config::GetFloat("ui_scale_fp", 0.5f), config::GetFloat("sharp_ui", 1.f));
    Log("summary: controllers left=%s right=%s", in.profile[0][0] ? in.profile[0] : "(none yet)",
        in.profile[1][0] ? in.profile[1] : "(none yet)");
    char hl[96], hr[96];
    AimFromGrip(in.hand[0], hl, sizeof(hl));
    AimFromGrip(in.hand[1], hr, sizeof(hr));
    Log("summary: aim pose vs grip pose (deg): left %s; right %s", hl, hr);
    Log("summary: hand offsets in gowvr.ini (pitch / yaw / roll deg): psvr2 %.0f / %.0f / %.0f, quest %.0f / %.0f / %.0f, "
        "other %.0f / %.0f / %.0f (the 'arms: headset' line says which set is used)",
        config::GetFloat("wristpitch_psvr2", 35.f), config::GetFloat("wristyaw_psvr2", 0.f),
        config::GetFloat("wristroll_psvr2", 0.f), config::GetFloat("wristpitch_quest", 25.f),
        config::GetFloat("wristyaw_quest", 0.f), config::GetFloat("wristroll_quest", 0.f),
        config::GetFloat("wristpitch_other", 0.f), config::GetFloat("wristyaw_other", 0.f),
        config::GetFloat("wristroll_other", 0.f));
    Log("summary: pad_mode %.0f, autovr %.0f, dlss_per_eye %.0f, hit_radius %.2f, hit_radius_fist %.2f",
        config::GetFloat("pad_mode", 0.f), config::GetFloat("autovr", 1.f), config::GetFloat("dlss_per_eye", 1.f),
        config::GetFloat("hit_radius", 1.0f), config::GetFloat("hit_radius_fist", 0.5f));
    {
        char ui[512], hud[256];
        stereo::UiDiagText(ui, sizeof(ui));
        stereo::SharpUiDiagText(hud, sizeof(hud));
        Log("summary: %s", ui);
        Log("summary: %s", hud);
        char rs[256];
        frame::RenderSizeDiagText(rs, sizeof(rs));
        Log("summary: %s", rs);
    }
    Log("==== end of VR summary #%d ====", number);
    (void)sc;
}

// While VR runs: one "VR check" line a minute for the first 30 minutes, then every 10 minutes - whether the UI scaling
// and the flat screen actually work on this player's game (a view the mod misreads shows up here, not as an error)
void VrCheckTick() {
    static ULONGLONG next = 0;
    static int lines = 0;
    const ULONGLONG now = GetTickCount64();
    if (!next) { next = now + 20000; return; }  // first one 20 s in (menus / intro)
    if (now < next) return;
    next = now + (++lines < 30 ? 60000 : 600000);
    char ui[512], hud[256], rs[256];
    stereo::UiDiagText(ui, sizeof(ui));
    stereo::SharpUiDiagText(hud, sizeof(hud));
    frame::RenderSizeDiagText(rs, sizeof(rs));
    Log("VR check %d: frames to the headset %llu (flat screen %llu, flat backbuffer %llu) | %s | %s | %s | hand offsets "
        "pitch/yaw/roll psvr2 %.0f/%.0f/%.0f quest %.0f/%.0f/%.0f other %.0f/%.0f/%.0f", lines,
        (unsigned long long)g_submitted.load(), (unsigned long long)g_quadFrames.load(),
        (unsigned long long)g_mono.load(), ui, hud, rs, config::GetFloat("wristpitch_psvr2", 35.f),
        config::GetFloat("wristyaw_psvr2", 0.f), config::GetFloat("wristroll_psvr2", 0.f),
        config::GetFloat("wristpitch_quest", 25.f), config::GetFloat("wristyaw_quest", 0.f),
        config::GetFloat("wristroll_quest", 0.f), config::GetFloat("wristpitch_other", 0.f),
        config::GetFloat("wristyaw_other", 0.f), config::GetFloat("wristroll_other", 0.f));
}

void MaybeLogSummary(IDXGISwapChain* sc, bool stereo) {
    static int logged = 0;
    static std::vector<std::string> seen;
    if (logged >= 16 || !g_viewsValid) return;  // (the field of view is known once the views are located)
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    AcquireSRWLockShared(&g_inputLock);
    const InputSnapshot in = g_input;
    ReleaseSRWLockShared(&g_inputLock);
    char key[256];
    sprintf_s(key, "%d %ux%u %s %s", stereo ? 1 : 0, d.BufferDesc.Width, d.BufferDesc.Height, in.profile[0], in.profile[1]);
    for (const auto& k : seen)
        if (k == key) return;
    seen.push_back(key);
    LogSummary(sc, d, in, stereo, ++logged);
}

// The frame from the switch between the stereo view and the flat screen: its size is not the one its mode needs yet.
// The last layer is submitted again instead (true = done, the frame is not shown).
bool HoldSwitchFrame(ID3D11Texture2D* frame, bool flat) {
    uint32_t ew = 0, eh = 0;
    D3D11_TEXTURE2D_DESC fd{};
    if (frame) frame->GetDesc(&fd);
    const bool wrong = frame && frame::RenderSizeExpected(flat, &ew, &eh) && (fd.Width != ew || fd.Height != eh);
    const ULONGLONG now = GetTickCount64();
    static const ULONGLONG maxMs = (ULONGLONG)fmaxf(config::GetFloat("hold_ms", 1500.f), 0.f);
    if (!wrong || g_lastKind == kLastNone || !g_frame.shouldRender || !g_viewsValid) { g_holdSince = 0; return false; }
    if (!g_holdSince) g_holdSince = now;
    if (now - g_holdSince > maxMs) return false;  // never stuck: shown as it is after hold_ms
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, g_space, 2, g_lastPv};
    const XrBaseHeader* layers[] = {g_lastKind == kLastQuad ? (const XrBaseHeader*)&g_lastQuadLayer
                                                            : (const XrBaseHeader*)&proj};
    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO, nullptr, g_frame.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1u,
                      layers};
    XrResult r = xrEndFrame(g_sess, &fe);
    NoteLoss(r);  // D-110
    if (!Succeeded(r)) Log("xr: xrEndFrame %s", ResultStr(r).c_str());
    else ++g_submitted;
    g_frameBegun = false;
    if (++g_holdFrames <= 5 || g_holdFrames % 100 == 0)
        Log("xr: %s frame %ux%u (needs %ux%u) - the last %s shown again (%llu held)", flat ? "flat" : "stereo", fd.Width,
            fd.Height, ew, eh, g_lastKind == kLastQuad ? "screen" : "stereo view", (unsigned long long)g_holdFrames);
    return true;
}

void EndFrame(ID3D11DeviceContext1* ctx, IDXGISwapChain* sc, ID3D11Texture2D* eye0, ID3D11Texture2D* eye1,
              const EyeViews* used) {
    ID3D11Texture2D* bb = nullptr;
    ID3D11Texture2D* src[2] = {eye0, eye1};
    if (!eye0 || !eye1) {
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        src[0] = src[1] = bb;
        ++g_mono;
    }
    static const bool screenOnHold = config::GetFloat("screen_2d", 1.f) > 0.5f;
    if (HoldSwitchFrame(bb ? bb : eye0, bb != nullptr && screenOnHold)) {
        if (bb) bb->Release();
        return;
    }
    static const bool screenOn = config::GetFloat("screen_2d", 1.f) > 0.5f;
    if (bb && screenOn) {
        ++g_monoRun;
        const bool lone = g_monoRun < 3 && g_haveLastPv;
        if (g_frame.shouldRender && g_viewsValid && lone) {  // repeat the last stereo layer (its last released images)
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, g_space, 2, g_lastPv};
            const XrBaseHeader* layers[] = {(const XrBaseHeader*)&layer};
            XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO, nullptr, g_frame.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                              1u, layers};
            XrResult r = xrEndFrame(g_sess, &fe);
            NoteLoss(r);  // D-110
            if (!Succeeded(r)) Log("xr: xrEndFrame %s", ResultStr(r).c_str());
            else ++g_submitted;
            g_frameBegun = false;
            bb->Release();
            return;
        }
        D3D11_TEXTURE2D_DESC bd{};
        bb->GetDesc(&bd);
        bool ok = g_frame.shouldRender && g_viewsValid && EnsureQuadSwapchain(bd.Width, bd.Height);
        if (ok) {
            // D-085 (owner: "on recenters the screen in floating space recenters bad, a bit too much to the right"):
            // re-placed on the head camera's recenter COUNT, which only moves while the head camera runs - not while the
            // 2D screen shows; a SteamVR recenter then moved the space under a screen left at its old coordinates.
            // Now F8 and a runtime recenter (once its new space is in effect, PollEvents) flag a re-placement
            if (!g_quadPlaced || g_monoRun == (g_haveLastPv ? 3 : 1) || g_quadReplace.exchange(false)) PlaceQuad();
            const int idx = AcquireReady(g_quad, "screen");  // D-110
            ok = idx >= 0;
            if (ok) {
                Blit(ctx, bb, g_quad.rtvs[idx], g_quad.w, g_quad.h);
                ReleaseImage(g_quad);
            }
        }
        bb->Release();
        // D-097 (owner: "make the flat screen in the black void bigger"): 3.4 m at 2.2 m (~75 deg wide; was 2.4 m, ~57 deg)
        static const float width = config::GetFloat("screen_width", 3.4f);
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, g_space, XR_EYE_VISIBILITY_BOTH,
                                    {g_quad.sc, {{0, 0}, {(int32_t)g_quad.w, (int32_t)g_quad.h}}, 0}, g_quadPose,
                                    {width, g_quad.w ? width * (float)g_quad.h / (float)g_quad.w : width * 0.5625f}};
        const XrBaseHeader* layers[3] = {(const XrBaseHeader*)&quad, nullptr, nullptr};
        uint32_t nLayers = ok ? 1u : 0u;
        // the VR settings panel next to the screen while the pause menu is open (panel.cpp)
        XrCompositionLayerQuad panelQ{XR_TYPE_COMPOSITION_LAYER_QUAD}, cursorQ{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (ok) {
            AcquireSRWLockShared(&g_inputLock);
            const InputSnapshot in = g_input;
            ReleaseSRWLockShared(&g_inputLock);
            const float head[3] = {0.5f * (g_views[0].pose.position.x + g_views[1].pose.position.x),
                                   0.5f * (g_views[0].pose.position.y + g_views[1].pose.position.y),
                                   0.5f * (g_views[0].pose.position.z + g_views[1].pose.position.z)};
            Pose screen{};
            memcpy(&screen, &g_quadPose, sizeof(screen));
            PanelLayer pl;
            if (PanelFrame(in, screen, width, head, &pl) && EnsureStaticSwapchain(g_panelSc, (uint32_t)pl.pw, (uint32_t)pl.ph, "panel")) {
                if ((pl.dirty || !g_panelUploaded) && UploadStatic(ctx, g_panelSc, pl.pixels, "panel")) g_panelUploaded = true;
                if (g_panelUploaded) {
                    panelQ = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, g_space, XR_EYE_VISIBILITY_BOTH,
                                                    {g_panelSc.sc, {{0, 0}, {(int32_t)g_panelSc.w, (int32_t)g_panelSc.h}}, 0},
                                                    {}, {pl.w, pl.h}};
                    memcpy(&panelQ.pose, &pl.pose, sizeof(panelQ.pose));
                    layers[nLayers++] = (const XrBaseHeader*)&panelQ;
                }
                if (pl.cursor && g_panelUploaded && EnsureStaticSwapchain(g_cursorSc, 64, 64, "panel pointer")) {
                    if (!g_cursorUploaded && UploadStatic(ctx, g_cursorSc, PanelCursorPixels(), "panel pointer")) g_cursorUploaded = true;
                    if (g_cursorUploaded) {
                        cursorQ = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0x2u /*SOURCE_ALPHA*/, g_space,
                                                         XR_EYE_VISIBILITY_BOTH, {g_cursorSc.sc, {{0, 0}, {64, 64}}, 0}, {},
                                                         {pl.cursorSize, pl.cursorSize}};
                        memcpy(&cursorQ.pose, &pl.cursorPose, sizeof(cursorQ.pose));
                        layers[nLayers++] = (const XrBaseHeader*)&cursorQ;
                    }
                }
            }
        }
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO, nullptr, g_frame.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                          nLayers, layers};
        XrResult r = xrEndFrame(g_sess, &fe);
        NoteLoss(r);  // D-110
        if (!Succeeded(r)) Log("xr: xrEndFrame %s", ResultStr(r).c_str());
        else if (ok) { ++g_submitted; g_lastQuadLayer = quad; g_lastKind = kLastQuad; }
        if (ok && (++g_quadFrames == 1 || g_quadFrames % 900 == 0))
            Log("xr: flat frame as a screen #%llu (%.1f m wide)", (unsigned long long)g_quadFrames.load(), width);
        g_frameBegun = false;
        return;
    }
    PanelHidden();  // (no flat screen this frame: no panel)
    if (!bb) {
        if (g_monoRun >= 3) Log("xr: stereo again after %d flat frame(s)", g_monoRun);
        g_monoRun = 0;
    }
    XrCompositionLayerProjectionView pv[2] = {};
    bool ok = src[0] && src[1] && g_frame.shouldRender && g_viewsValid;
    for (int e = 0; e < 2 && ok; ++e) {
        EyeSwap& s = g_eye[e];
        const int idx = AcquireReady(s, e ? "eye 1" : "eye 0");  // D-110
        if (idx < 0) {
            ok = false;
            break;
        }
        Blit(ctx, src[e], s.rtvs[idx], s.w, s.h);
        const bool cap = g_hmdReq.load();
        if (cap) HmdCapture(ctx, e, s.images[idx]);
        ReleaseImage(s);
        pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        if (used && used->valid) {
            memcpy(&pv[e].pose, &used->pose[e], sizeof(XrPosef));
            memcpy(&pv[e].fov, &used->fov[e], sizeof(XrFovf));
        } else {
            pv[e].pose = g_views[e].pose;
            pv[e].fov = g_views[e].fov;
        }
        pv[e].subImage = {s.sc, {{0, 0}, {(int32_t)s.w, (int32_t)s.h}}, 0};
        if (cap) {
            sprintf_s(g_hmdInfo[e], "eye%d src=%s pose p(%.3f %.3f %.3f) q(%.3f %.3f %.3f %.3f) fov(%.1f %.1f %.1f %.1f)", e,
                      src[e] == bb ? "BACKBUFFER(mono)" : "eyeTex", pv[e].pose.position.x, pv[e].pose.position.y,
                      pv[e].pose.position.z, pv[e].pose.orientation.x, pv[e].pose.orientation.y,
                      pv[e].pose.orientation.z, pv[e].pose.orientation.w, pv[e].fov.angleLeft * 57.2958f,
                      pv[e].fov.angleRight * 57.2958f, pv[e].fov.angleUp * 57.2958f, pv[e].fov.angleDown * 57.2958f);
            if (e == 1) { g_hmdReq = false; g_hmdReady = true; }
        }
    }
    if (bb) bb->Release();
    if (ok && !bb) {
        memcpy(g_lastPv, pv, sizeof(g_lastPv));
        g_haveLastPv = true;
        g_lastKind = kLastProjection;
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, g_space, 2, pv};
    const XrBaseHeader* layers[3] = {(const XrBaseHeader*)&layer, nullptr, nullptr};
    uint32_t nLayers = ok ? 1u : 0u;
    // the HUD of each eye as its own quad (sharpui.cpp): where the paste put it - centred in the eye's frustum, ui_scale
    // of it - in front of that eye only, so the compositor samples the full-resolution HUD once
    XrCompositionLayerQuad hudQ[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    for (int e = 0; e < 2 && ok && g_psHud; ++e) {
        ID3D11ShaderResourceView* hsrv = nullptr;
        uint32_t hw = 0, hh = 0;
        float sx = 0.f, sy = 0.f;
        if (!stereo::SharpUiHudLayer(e, &hsrv, &hw, &hh, &sx, &sy)) continue;
        // texels for ~1.5 x the headset image's pixels under the HUD (the compositor's own downsampling stays mild)
        static const float density = fminf(fmaxf(config::GetFloat("hud_layer_density", 1.5f), 0.5f), 4.f);
        const uint32_t qw = (uint32_t)fminf((float)hw, ceilf(density * sx * (float)g_eye[e].w));
        const uint32_t qh = (uint32_t)fminf((float)hh, ceilf(density * sy * (float)g_eye[e].h));
        if (qw < 16 || qh < 16 || !EnsureRtSwapchain(g_hudSc[e], hw, hh, e ? "HUD layer eye 1" : "HUD layer eye 0")) continue;
        const int idx = AcquireReady(g_hudSc[e], "HUD layer");
        if (idx < 0) continue;
        static const FLOAT clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(g_hudSc[e].rtvs[idx], clear);
        BlitSrv(ctx, hsrv, g_psHud, g_hudSc[e].rtvs[idx], qw, qh);
        ReleaseImage(g_hudSc[e]);
        const XrFovf& f = pv[e].fov;
        const float D = 2.f, tl = tanf(f.angleLeft), tr = tanf(f.angleRight), tu = tanf(f.angleUp), td = tanf(f.angleDown);
        const float local[3] = {D * 0.5f * (tl + tr), D * 0.5f * (tu + td), -D};  // the frustum's middle at D metres
        const float* q = &pv[e].pose.orientation.x;
        float off[3];
        QRotate(q, local, off);
        XrCompositionLayerQuad& hq = hudQ[e];
        hq = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0x2u /*SOURCE_ALPHA, premultiplied*/, g_space,
                                    e ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT,
                                    {g_hudSc[e].sc, {{0, 0}, {(int32_t)qw, (int32_t)qh}}, 0}, pv[e].pose,
                                    {D * (tr - tl) * sx, D * (tu - td) * sy}};
        hq.pose.position.x += off[0];
        hq.pose.position.y += off[1];
        hq.pose.position.z += off[2];
        layers[nLayers++] = (const XrBaseHeader*)&hq;
        if (++g_hudQuads <= 2 || g_hudQuads % 10000 == 0)
            Log("xr: HUD layer eye %d %ux%u of %ux%u (%.2f x %.2f m at %.0f m) #%llu", e, qw, qh, hw, hh,
                hq.size.width, hq.size.height, D, (unsigned long long)g_hudQuads);
    }
    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO, nullptr, g_frame.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                      nLayers, layers};
    XrResult r = xrEndFrame(g_sess, &fe);
    NoteLoss(r);  // D-110
    if (!Succeeded(r)) Log("xr: xrEndFrame %s", ResultStr(r).c_str());
    else if (ok) ++g_submitted;
    g_frameBegun = false;
}

// D-111: the game's rumble (Xbox or PlayStation pad, vpad.cpp) on the controllers. A pad motor runs until the game
// changes it, an OpenXR vibration for its duration: renewed every frame (50 ms) while the motor runs, stopped after.
// Left hand = the large (low-frequency) motor, right hand = the small one, each with rumble_mix of the other.
std::atomic<float> g_rumble[2] = {0.f, 0.f};
void RumbleTick() {
    static bool active[2] = {false, false};
    if (!g_actionsReady || !g_running || !g_sess) {
        active[0] = active[1] = false;
        return;
    }
    for (int h = 0; h < 2; ++h) {
        const float a = g_rumble[h].load();
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, g_act[A_HAPTIC], g_handPath[h]};
        if (a > 0.02f) {
            XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION, nullptr, 50000000, 0.f, a};
            xrApplyHapticFeedback(g_sess, &hi, (const XrBaseHeader*)&v);
            active[h] = true;
        } else if (active[h]) {
            if (xrStopHapticFeedback) xrStopHapticFeedback(g_sess, &hi);
            active[h] = false;
        }
    }
}

void BeginFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    g_frame = XrFrameState{XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(g_sess, &wi, &g_frame);
    if (!Succeeded(r)) { NoteLoss(r); Log("xr: xrWaitFrame %s", ResultStr(r).c_str()); return; }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(g_sess, &bi);
    if (!Succeeded(r)) { NoteLoss(r); Log("xr: xrBeginFrame %s", ResultStr(r).c_str()); return; }
    g_frameBegun = true;
    ++g_frames;
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        g_frame.predictedDisplayTime, g_space};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    g_views[0] = g_views[1] = XrView{XR_TYPE_VIEW};
    uint32_t n = 0;
    SyncInput(g_frame.predictedDisplayTime);
    g_viewsValid = Succeeded(xrLocateViews(g_sess, &li, &vs, 2, &n, g_views)) && n == 2 &&
                   (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (g_viewsValid) {
        EyeViews v;
        v.valid = true;
        for (int e = 0; e < 2; ++e) {
            memcpy(&v.pose[e], &g_views[e].pose, sizeof(Pose));
            memcpy(&v.fov[e], &g_views[e].fov, sizeof(Fov));
        }
        v.displayTime = g_frame.predictedDisplayTime;
        v.period = g_frame.predictedDisplayPeriod;
        AcquireSRWLockExclusive(&g_viewLock);
        v.serial = g_latest.serial + 1;
        g_latest = v;
        ReleaseSRWLockExclusive(&g_viewLock);
    }
    uint64_t k = g_frames.load();
    if (k <= 3 || k % 900 == 0)
        Log("xr: frame %llu shouldRender=%u period=%.2f ms views=%d L(%.3f %.3f %.3f) fovL(%.1f %.1f %.1f %.1f) "
            "submitted=%llu mono=%llu",
            (unsigned long long)k, g_frame.shouldRender, g_frame.predictedDisplayPeriod / 1e6, g_viewsValid,
            g_views[0].pose.position.x, g_views[0].pose.position.y, g_views[0].pose.position.z,
            g_views[0].fov.angleLeft * 57.2958f, g_views[0].fov.angleRight * 57.2958f,
            g_views[0].fov.angleUp * 57.2958f, g_views[0].fov.angleDown * 57.2958f,
            (unsigned long long)g_submitted.load(), (unsigned long long)g_mono.load());
}

}  // namespace

void XrRequest(bool on) {
    g_want = on;
    if (on) g_failed = false;
    Log("xr: %s requested", on ? "start" : "stop");
}

bool XrRunning() { return g_running; }
void XrScreenRecenter() { g_quadReplace = true; }

void XrOnPresent(IDXGISwapChain* sc, ID3D11Texture2D* eye0, ID3D11Texture2D* eye1, const EyeViews* used) {
    if (!g_want.load()) {
        // D-110: VR off ends the session (it kept running without frames: SteamVR showed the app as not responding)
        if (g_sess) DestroySession("VR turned off", false);
        return;
    }
    if (g_rebuild) {
        // D-110 (code audit H2): the runtime ended the session / was lost (SteamVR closed, restarted or crashed).
        // Destroyed here and rebuilt by the 3 s retry below once SteamVR answers again - before, the dead session was
        // kept until the game restarted
        const bool inst = g_rebuild > 1;
        DestroySession(inst ? "runtime lost" : "session ended by the runtime", inst);
        g_failed = true;
        g_retryAt = GetTickCount64() + 3000;
        return;
    }
    if (g_failed && GetTickCount64() < g_retryAt) return;
    if (!g_sess) {
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
        bool instOk = gipa || LoadRuntime();
        if (instOk && !g_inst) {
            // A partly built instance must not be reused: a retry skipped the type check and the controller actions
            // (owner report: no motion controls in F1 after an aborted start).
            instOk = CreateInstance();
            if (!instOk && g_inst) { xrDestroyInstance(g_inst); g_inst = nullptr; g_actionSet = nullptr; }
        }
        bool ok = instOk && CreateSession(dev);
        dev->Release();
        if (!ok) {
            // D-110: a partly created session (swapchains / views missing) is never used - it was before
            if (g_sess) DestroySession("setup failed part way", false);
            // Headset asleep / not ready: keep trying while VR is wanted (F9 or autovr), quietly after the first few.
            g_failed = true;
            g_retryAt = GetTickCount64() + 3000;
            if (++g_failCount <= 3 || g_failCount % 20 == 0)
                Log("xr: setup failed (attempt %u); retrying every 3 s while VR is on", g_failCount);
            return;
        }
        g_failed = false;
        g_failCount = 0;
    }
    PollEvents();
    if (g_rebuild || !g_running) return;  // (a rebuild is done at the next Present)
    ID3D11DeviceContext* c0 = nullptr;
    g_dev->GetImmediateContext(&c0);
    ID3D11DeviceContext1* ctx = nullptr;
    c0->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx);
    c0->Release();
    if (g_frameBegun) EndFrame(ctx, sc, eye0, eye1, used);
    ctx->Release();
    MaybeLogSummary(sc, eye0 && eye1);
    VrCheckTick();
    BeginFrame();
    RumbleTick();  // D-111
}

bool XrLocateAt(int64_t displayTime, EyeViews* out) {
    // D-110: under the session lock (the render thread may be destroying the session)
    AcquireSRWLockShared(&g_sessLock);
    bool ok = g_running && g_sess && g_space && xrLocateViews;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    if (ok) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, displayTime, g_space};
        ok = Succeeded(xrLocateViews(g_sess, &li, &vs, 2, &n, views)) && n == 2 &&
             (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    }
    ReleaseSRWLockShared(&g_sessLock);
    if (!ok) return false;
    for (int e = 0; e < 2; ++e) {
        memcpy(&out->pose[e], &views[e].pose, sizeof(Pose));
        memcpy(&out->fov[e], &views[e].fov, sizeof(Fov));
    }
    out->displayTime = displayTime;
    out->valid = true;
    return true;
}

InputSnapshot XrInput() {
    AcquireSRWLockShared(&g_inputLock);
    InputSnapshot s = g_input;
    ReleaseSRWLockShared(&g_inputLock);
    return s;
}

bool XrLocateHandsAt(int64_t displayTime, Pose grip[2], bool valid[2]) {
    // D-110 (code audit M3): the controllers located for the same predicted display time as the eyes (headcam.cpp);
    // the input snapshot has them one frame earlier (this frame's display time), so the F1 hands trailed the head
    valid[0] = valid[1] = false;
    AcquireSRWLockShared(&g_sessLock);
    if (g_running && g_sess && g_space && g_actionsReady && xrLocateSpace) {
        const XrFlags64 kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        for (int h = 0; h < 2; ++h) {
            if (!g_gripSpace[h]) continue;
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            if (Succeeded(xrLocateSpace(g_gripSpace[h], g_space, displayTime, &loc)) && (loc.locationFlags & kValid) == kValid) {
                memcpy(&grip[h], &loc.pose, sizeof(Pose));
                valid[h] = true;
            }
        }
    }
    ReleaseSRWLockShared(&g_sessLock);
    return valid[0] || valid[1];
}

void XrHaptic(int hand, float amplitude, float seconds, float frequency) {
    if (hand < 0 || hand > 1) return;
    AcquireSRWLockShared(&g_sessLock);  // D-110
    if (g_actionsReady && g_running && g_sess) {
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, g_act[A_HAPTIC], g_handPath[hand]};
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION, nullptr, (XrDuration)(seconds * 1e9), frequency, amplitude};
        xrApplyHapticFeedback(g_sess, &hi, (const XrBaseHeader*)&v);
    }
    ReleaseSRWLockShared(&g_sessLock);
}

void XrRumble(float heavy, float light) {
    static const float strength = config::GetFloat("rumble_strength", 1.f);
    static const float mix = config::GetFloat("rumble_mix", 0.5f);
    g_rumble[0] = fminf(1.f, fmaxf(heavy, light * mix) * strength);
    g_rumble[1] = fminf(1.f, fmaxf(light, heavy * mix) * strength);
}

const char* XrSystemName() { return g_systemName; }

EyeViews XrLatestViews() {
    AcquireSRWLockShared(&g_viewLock);
    EyeViews v = g_latest;
    ReleaseSRWLockShared(&g_viewLock);
    return v;
}

void XrHmdShot(const char* name, int div) {
    g_hmdReady = false;
    g_hmdReq = true;
    for (int t = 0; !g_hmdReady && t < 3000; t += 10) Sleep(10);
    if (!g_hmdReady.exchange(false)) {
        g_hmdReq = false;
        Log("hmdshot: nothing submitted (session not running / not rendering?)");
        return;
    }
    std::lock_guard<std::mutex> lk(g_hmdMu);
    for (int e = 0; e < 2; ++e) {
        std::string n = std::string(name) + (e ? "_hmd1" : "_hmd0");
        diag::WritePng(n.c_str(), g_hmdPx[e].data(), g_hmdW[e], g_hmdH[e], g_hmdPitch[e], g_hmdFmt, div);
        Log("hmdshot: %s", g_hmdInfo[e]);
    }
}

void XrStatus() {
    Log("xr: status want=%d failed=%d session=%p state=%d running=%d frames=%llu submitted=%llu mono=%llu", g_want.load(),
        g_failed, (void*)g_sess, (int)g_state, g_running, (unsigned long long)g_frames.load(),
        (unsigned long long)g_submitted.load(), (unsigned long long)g_mono.load());
}

}  // namespace gowvr::vr
