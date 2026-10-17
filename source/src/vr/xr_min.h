#pragma once
// Minimal OpenXR 1.0 declarations (written from the specification; no SDK dependency). Every XrStructureType value used
// here is verified at runtime against xrStructureTypeToString (xr.cpp: VerifyTypes) before any other call is made.
#include <cstdint>

struct ID3D11Device;
struct ID3D11Texture2D;
struct _LUID;

namespace xr {

using XrResult = int32_t;
using XrVersion = uint64_t;
using XrFlags64 = uint64_t;
using XrBool32 = uint32_t;
using XrTime = int64_t;
using XrDuration = int64_t;
using XrSystemId = uint64_t;
using XrInstance = struct XrInstance_T*;
using XrSession = struct XrSession_T*;
using XrSpace = struct XrSpace_T*;
using XrSwapchain = struct XrSwapchain_T*;
using XrActionSet = struct XrActionSet_T*;
using XrAction = struct XrAction_T*;
using XrPath = uint64_t;

constexpr XrVersion MakeVersion(uint64_t major, uint64_t minor, uint64_t patch) {
    return (major << 48) | (minor << 32) | patch;
}
constexpr uint64_t XR_NULL_SYSTEM_ID = 0;
inline bool Succeeded(XrResult r) { return r >= 0; }
// D-110: XR_TIMEOUT_EXPIRED is a SUCCESS code (>= 0): a swapchain wait that timed out must not count as "image ready"
constexpr XrResult XR_SUCCESS = 0;
constexpr XrResult XR_TIMEOUT_EXPIRED = 1;
constexpr XrResult XR_ERROR_INSTANCE_LOST = -13;
constexpr XrResult XR_ERROR_SESSION_LOST = -17;

enum XrStructureType : int32_t {
    XR_TYPE_EXTENSION_PROPERTIES = 2,
    XR_TYPE_INSTANCE_CREATE_INFO = 3,
    XR_TYPE_SYSTEM_GET_INFO = 4,
    XR_TYPE_SYSTEM_PROPERTIES = 5,
    XR_TYPE_VIEW_LOCATE_INFO = 6,
    XR_TYPE_VIEW = 7,
    XR_TYPE_SESSION_CREATE_INFO = 8,
    XR_TYPE_SWAPCHAIN_CREATE_INFO = 9,
    XR_TYPE_SESSION_BEGIN_INFO = 10,
    XR_TYPE_VIEW_STATE = 11,
    XR_TYPE_FRAME_END_INFO = 12,
    XR_TYPE_EVENT_DATA_BUFFER = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18,
    XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING = 40,  // (49 is EVENTS_LOST - runtime check caught it)
    XR_TYPE_INSTANCE_PROPERTIES = 32,
    XR_TYPE_FRAME_WAIT_INFO = 33,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION = 35,
    XR_TYPE_COMPOSITION_LAYER_QUAD = 36,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37,
    XR_TYPE_VIEW_CONFIGURATION_VIEW = 41,
    XR_TYPE_SPACE_LOCATION = 42,
    XR_TYPE_FRAME_STATE = 44,
    XR_TYPE_FRAME_BEGIN_INFO = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW = 48,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO = 56,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    XR_TYPE_GRAPHICS_BINDING_D3D11_KHR = 1000027000,
    XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR = 1000027001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR = 1000027002,
    XR_TYPE_HAPTIC_VIBRATION = 13,
    XR_TYPE_ACTION_STATE_BOOLEAN = 23,
    XR_TYPE_ACTION_STATE_FLOAT = 24,
    XR_TYPE_ACTION_STATE_VECTOR2F = 25,
    XR_TYPE_ACTION_STATE_POSE = 27,
    XR_TYPE_ACTION_SET_CREATE_INFO = 28,
    XR_TYPE_ACTION_CREATE_INFO = 29,
    XR_TYPE_ACTION_SPACE_CREATE_INFO = 38,
    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING = 51,
    XR_TYPE_INTERACTION_PROFILE_STATE = 53,
    XR_TYPE_ACTION_STATE_GET_INFO = 58,
    XR_TYPE_HAPTIC_ACTION_INFO = 59,
    XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO = 60,
    XR_TYPE_ACTIONS_SYNC_INFO = 61,
};

enum XrFormFactor : int32_t { XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1 };
enum XrViewConfigurationType : int32_t { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2 };
enum XrReferenceSpaceType : int32_t {
    XR_REFERENCE_SPACE_TYPE_VIEW = 1,
    XR_REFERENCE_SPACE_TYPE_LOCAL = 2,
    XR_REFERENCE_SPACE_TYPE_STAGE = 3,
};
enum XrEnvironmentBlendMode : int32_t { XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1 };
enum XrSessionState : int32_t {
    XR_SESSION_STATE_UNKNOWN = 0,
    XR_SESSION_STATE_IDLE = 1,
    XR_SESSION_STATE_READY = 2,
    XR_SESSION_STATE_SYNCHRONIZED = 3,
    XR_SESSION_STATE_VISIBLE = 4,
    XR_SESSION_STATE_FOCUSED = 5,
    XR_SESSION_STATE_STOPPING = 6,
    XR_SESSION_STATE_LOSS_PENDING = 7,
    XR_SESSION_STATE_EXITING = 8,
};
constexpr XrFlags64 XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT = 0x1;
constexpr XrFlags64 XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT = 0x10;
constexpr XrFlags64 XR_SWAPCHAIN_USAGE_SAMPLED_BIT = 0x20;
constexpr XrFlags64 XR_VIEW_STATE_ORIENTATION_VALID_BIT = 0x1;
constexpr XrFlags64 XR_VIEW_STATE_POSITION_VALID_BIT = 0x2;
constexpr XrFlags64 XR_SPACE_LOCATION_ORIENTATION_VALID_BIT = 0x1;
constexpr XrFlags64 XR_SPACE_LOCATION_POSITION_VALID_BIT = 0x2;
enum XrActionType : int32_t {
    XR_ACTION_TYPE_BOOLEAN_INPUT = 1,
    XR_ACTION_TYPE_FLOAT_INPUT = 2,
    XR_ACTION_TYPE_VECTOR2F_INPUT = 3,
    XR_ACTION_TYPE_POSE_INPUT = 4,
    XR_ACTION_TYPE_VIBRATION_OUTPUT = 100,
};
constexpr XrDuration XR_INFINITE_DURATION = 0x7fffffffffffffffLL;

struct XrVector3f { float x, y, z; };
struct XrQuaternionf { float x, y, z, w; };
struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
struct XrFovf { float angleLeft, angleRight, angleUp, angleDown; };
struct XrOffset2Di { int32_t x, y; };
struct XrExtent2Di { int32_t width, height; };
struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; };

struct XrBaseHeader { XrStructureType type; const void* next; };

struct XrExtensionProperties {
    XrStructureType type; void* next; char extensionName[128]; uint32_t extensionVersion;
};
struct XrApplicationInfo {
    char applicationName[128]; uint32_t applicationVersion; char engineName[128]; uint32_t engineVersion;
    XrVersion apiVersion;
};
struct XrInstanceCreateInfo {
    XrStructureType type; const void* next; XrFlags64 createFlags; XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount; const char* const* enabledExtensionNames;
};
struct XrInstanceProperties {
    XrStructureType type; void* next; XrVersion runtimeVersion; char runtimeName[128];
};
struct XrSystemGetInfo { XrStructureType type; const void* next; XrFormFactor formFactor; };
struct XrSystemGraphicsProperties { uint32_t maxSwapchainImageHeight, maxSwapchainImageWidth, maxLayerCount; };
struct XrSystemTrackingProperties { XrBool32 orientationTracking, positionTracking; };
struct XrSystemProperties {
    XrStructureType type; void* next; XrSystemId systemId; uint32_t vendorId; char systemName[256];
    XrSystemGraphicsProperties graphicsProperties; XrSystemTrackingProperties trackingProperties;
};
struct XrGraphicsRequirementsD3D11KHR {
    XrStructureType type; void* next; uint32_t adapterLuidLow; int32_t adapterLuidHigh; int32_t minFeatureLevel;
};
struct XrGraphicsBindingD3D11KHR { XrStructureType type; const void* next; ID3D11Device* device; };
struct XrSessionCreateInfo { XrStructureType type; const void* next; XrFlags64 createFlags; XrSystemId systemId; };
struct XrViewConfigurationView {
    XrStructureType type; void* next; uint32_t recommendedImageRectWidth, maxImageRectWidth,
        recommendedImageRectHeight, maxImageRectHeight, recommendedSwapchainSampleCount, maxSwapchainSampleCount;
};
struct XrSwapchainCreateInfo {
    XrStructureType type; const void* next; XrFlags64 createFlags; XrFlags64 usageFlags; int64_t format;
    uint32_t sampleCount, width, height, faceCount, arraySize, mipCount;
};
struct XrSwapchainImageD3D11KHR { XrStructureType type; void* next; ID3D11Texture2D* texture; };
struct XrReferenceSpaceCreateInfo {
    XrStructureType type; const void* next; XrReferenceSpaceType referenceSpaceType; XrPosef poseInReferenceSpace;
};
struct XrSessionBeginInfo { XrStructureType type; const void* next; XrViewConfigurationType primaryViewConfigurationType; };
struct XrFrameWaitInfo { XrStructureType type; const void* next; };
struct XrFrameState {
    XrStructureType type; void* next; XrTime predictedDisplayTime; XrDuration predictedDisplayPeriod; XrBool32 shouldRender;
};
struct XrFrameBeginInfo { XrStructureType type; const void* next; };
struct XrViewLocateInfo {
    XrStructureType type; const void* next; XrViewConfigurationType viewConfigurationType; XrTime displayTime; XrSpace space;
};
struct XrViewState { XrStructureType type; void* next; XrFlags64 viewStateFlags; };
struct XrView { XrStructureType type; void* next; XrPosef pose; XrFovf fov; };
struct XrSwapchainImageAcquireInfo { XrStructureType type; const void* next; };
struct XrSwapchainImageWaitInfo { XrStructureType type; const void* next; XrDuration timeout; };
struct XrSwapchainImageReleaseInfo { XrStructureType type; const void* next; };
struct XrSwapchainSubImage { XrSwapchain swapchain; XrRect2Di imageRect; uint32_t imageArrayIndex; };
struct XrCompositionLayerProjectionView {
    XrStructureType type; const void* next; XrPosef pose; XrFovf fov; XrSwapchainSubImage subImage;
};
struct XrCompositionLayerProjection {
    XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; uint32_t viewCount;
    const XrCompositionLayerProjectionView* views;
};
struct XrFrameEndInfo {
    XrStructureType type; const void* next; XrTime displayTime; XrEnvironmentBlendMode environmentBlendMode;
    uint32_t layerCount; const XrBaseHeader* const* layers;
};
struct XrEventDataBuffer { XrStructureType type; const void* next; uint8_t varying[4000]; };
struct XrVector2f { float x, y; };
struct XrActionSetCreateInfo {
    XrStructureType type; const void* next; char actionSetName[64]; char localizedActionSetName[128]; uint32_t priority;
};
struct XrActionCreateInfo {
    XrStructureType type; const void* next; char actionName[64]; XrActionType actionType;
    uint32_t countSubactionPaths; const XrPath* subactionPaths; char localizedActionName[128];
};
struct XrActionSuggestedBinding { XrAction action; XrPath binding; };
struct XrInteractionProfileSuggestedBinding {
    XrStructureType type; const void* next; XrPath interactionProfile; uint32_t countSuggestedBindings;
    const XrActionSuggestedBinding* suggestedBindings;
};
struct XrSessionActionSetsAttachInfo {
    XrStructureType type; const void* next; uint32_t countActionSets; const XrActionSet* actionSets;
};
struct XrActiveActionSet { XrActionSet actionSet; XrPath subactionPath; };
struct XrActionsSyncInfo {
    XrStructureType type; const void* next; uint32_t countActiveActionSets; const XrActiveActionSet* activeActionSets;
};
struct XrActionStateGetInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; };
struct XrActionStateBoolean {
    XrStructureType type; void* next; XrBool32 currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime;
    XrBool32 isActive;
};
struct XrActionStateFloat {
    XrStructureType type; void* next; float currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime;
    XrBool32 isActive;
};
struct XrActionStateVector2f {
    XrStructureType type; void* next; XrVector2f currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime;
    XrBool32 isActive;
};
struct XrActionStatePose { XrStructureType type; void* next; XrBool32 isActive; };
struct XrActionSpaceCreateInfo {
    XrStructureType type; const void* next; XrAction action; XrPath subactionPath; XrPosef poseInActionSpace;
};
struct XrSpaceLocation { XrStructureType type; void* next; XrFlags64 locationFlags; XrPosef pose; };
struct XrInteractionProfileState { XrStructureType type; void* next; XrPath interactionProfile; };
struct XrHapticActionInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; };
struct XrHapticVibration {
    XrStructureType type; const void* next; XrDuration duration; float frequency; float amplitude;
};

struct XrEventDataSessionStateChanged {
    XrStructureType type; const void* next; XrSession session; XrSessionState state; XrTime time;
};
struct XrEventDataReferenceSpaceChangePending {  // D-079
    XrStructureType type; const void* next; XrSession session; XrReferenceSpaceType referenceSpaceType; XrTime changeTime;
    uint32_t poseValid; XrPosef poseInPreviousSpace;
};
// D-083: the flat game screen floating in the dark (2D-only game screens)
enum XrEyeVisibility : int32_t { XR_EYE_VISIBILITY_BOTH = 0 };
struct XrExtent2Df { float width, height; };
struct XrCompositionLayerQuad {
    XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; XrEyeVisibility eyeVisibility;
    XrSwapchainSubImage subImage; XrPosef pose; XrExtent2Df size;
};

// Loader <-> runtime negotiation (the loader-runtime interface; lets us load the active runtime without the loader).
enum XrLoaderInterfaceStructs : int32_t {
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
};
using PFN_xrVoidFunction = void(__stdcall*)();
using PFN_xrGetInstanceProcAddr = XrResult(__stdcall*)(XrInstance, const char*, PFN_xrVoidFunction*);
struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType; uint32_t structVersion; size_t structSize;
    uint32_t minInterfaceVersion, maxInterfaceVersion; XrVersion minApiVersion, maxApiVersion;
};
struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType; uint32_t structVersion; size_t structSize;
    uint32_t runtimeInterfaceVersion; XrVersion runtimeApiVersion; PFN_xrGetInstanceProcAddr getInstanceProcAddr;
};
using PFN_xrNegotiateLoaderRuntimeInterface = XrResult(__stdcall*)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*);

// XR_EXT_hand_tracking (optional; verified with xrStructureTypeToString before use)
using XrHandTrackerEXT = struct XrHandTrackerEXT_T*;
constexpr XrStructureType XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT = (XrStructureType)1000051001;
constexpr XrStructureType XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT = (XrStructureType)1000051002;
constexpr XrStructureType XR_TYPE_HAND_JOINT_LOCATIONS_EXT = (XrStructureType)1000051003;
struct XrHandTrackerCreateInfoEXT { XrStructureType type; const void* next; int32_t hand; int32_t handJointSet; };
struct XrHandJointsLocateInfoEXT { XrStructureType type; const void* next; XrSpace baseSpace; XrTime time; };
struct XrHandJointLocationEXT { XrFlags64 locationFlags; XrPosef pose; float radius; };
struct XrHandJointLocationsEXT { XrStructureType type; void* next; XrBool32 isActive; uint32_t jointCount; XrHandJointLocationEXT* jointLocations; };
constexpr int32_t XR_HAND_LEFT_EXT = 1, XR_HAND_RIGHT_EXT = 2;
constexpr uint32_t XR_HAND_JOINT_COUNT_EXT = 26;

}  // namespace xr
