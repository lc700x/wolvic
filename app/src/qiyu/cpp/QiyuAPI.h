#pragma once

// Full Qiyu Native SDK v1.0.0 (SXR) binding for Wolvic's native Qiyu device delegate.
// Reverse engineered from the working reference build (DeviceDelegateQiyu symbols +
// qiyu_* dlsym strings) and validated against third_party/qiyu/Include/QiyuApi.h.
// All symbols are resolved with dlopen/dlsym so the SDK AAR/binaries stay optional.

#include <dlfcn.h>
#include <jni.h>
#include <android/native_window.h>
#include <cstdint>
#include <cstring>

namespace crow {
namespace qiyu {

struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
struct Quaternion { float x, y, z, w; };
struct Matrix4 { float M[4][4]; };

struct ViewFrustum {
  float left;
  float right;
  float top;
  float bottom;
  float near;
  float far;
  Vector3 position;
  Quaternion rotation;
};

struct FoveationParam { Vector2 gainRate; float areaSize; float minResolution; };

struct DeviceInfo {
  ViewFrustum frustumLeftEye;
  ViewFrustum frustumRightEye;
  int32_t iEyeTargetWidth;
  int32_t iEyeTargetHeight;
  FoveationParam foveationLow;
  FoveationParam foveationMedium;
  FoveationParam foveationHigh;
};

enum ButtonType {
  BT_None = 0,
  BT_Trigger = 0x01,
  BT_Grip = 0x02,
  BT_A_X = 0x10,
  BT_B_Y = 0x20,
  BT_Home_Menu = 0x40,
  BT_JoyStick = 0x80
};

enum ControllerMask { CM_Left = 0x01, CM_Right = 0x02, CM_Both = 0x03 };
enum GraphicsApi { GA_OpenGLES = 0 };
enum TrackingOriginMode { TM_Device = 0, TM_Ground = 1 };
enum Eye { EYE_Left = 0, EYE_Right = 1, EYE_COUNT = 2 };
enum TextureType { TT_Texture = 0, TT_TextureArray = 1, TT_Image = 2 };
enum FoveationLevel { FL_None = 0, FL_Low = 1, FL_Medium = 2, FL_High = 3, FL_Custom = 4 };
enum PerfLevel { PL_System = 0, PL_Minimum = 1, PL_Medium = 2, PL_Maximum = 3 };

struct ControllerData {
  int isConnect;
  int button;
  int buttonTouch;
  int batteryLevel;
  float triggerForce;
  float gripForce;
  int isShow;
  Vector2 joyStickPos;
  Vector3 position;
  Quaternion rotation;
  Vector3 velocity;
  Vector3 acceleration;
  Vector3 angVelocity;
  Vector3 angAcceleration;
};

struct HeadPose {
  Quaternion rotation;
  Vector3 position;
};

struct HeadPoseState {
  HeadPose pose;
  int32_t poseStatus;
  uint64_t poseTimeStampNs;
  uint64_t poseFetchTimeNs;
  uint64_t expectedDisplayTimeNs;
};

struct RenderLayerScreenPosUV {
  float LowerLeftPos[4];
  float LowerRightPos[4];
  float UpperLeftPos[4];
  float UpperRightPos[4];
  float LowerUVs[4];
  float UpperUVs[4];
  float TransformMatrix[16];
};

enum RenderLayerEyeMask {
  RL_EyeMask_Left = 0x00000001,
  RL_EyeMask_Right = 0x00000002,
  RL_EyeMask_Both = 0x00000003
};

struct RenderLayer {
  int32_t imageHandle;
  int32_t imageType; // qiyu_TextureType
  RenderLayerScreenPosUV imageCoords;
  int32_t eyeMask; // qiyu_RenderLayer_EyeMask
};

constexpr int kRenderLayerMaxCount = 16;

struct FrameParam {
  int32_t minVsyncs;
  RenderLayer renderLayers[kRenderLayerMaxCount];
  HeadPoseState headPoseState;
};

// Version of the loaded SDK.
struct SdkVersion { int versionProduct, versionMajor, versionMinor; };

// Qiyu Native SDK surface (subset used by Wolvic; all resolved dynamically).
class API {
public:
  // qiyu_Init: matches QiyuApi.h (activity, vm, graphicsApi, trackingOrigin, verbose).
  using InitFn = bool (*)(jobject activity, JavaVM* vm, int graphicsApi, int trackingOrigin, bool logVerbose);

  void* library = nullptr;

  InitFn init = nullptr;
  SdkVersion (*getSdkVersion)() = nullptr;
  bool (*release)() = nullptr;
  bool (*startVR)(ANativeWindow*, int cpuPerf, int gpuPerf) = nullptr;
  bool (*endVR)() = nullptr;
  bool (*startEye)(bool multiview, int eye, int textureType) = nullptr;
  bool (*endEye)(bool multiview, int eye, int textureType) = nullptr;
  bool (*submitFrame)(const FrameParam&) = nullptr;
  void (*update)(float deltaTime) = nullptr;
  DeviceInfo (*getDeviceInfo)() = nullptr;
  float (*predictDisplayTime)() = nullptr;
  HeadPoseState (*predictHeadPose)(float predictedDisplayTimeMs) = nullptr;
  bool (*getViewMatrix)(Matrix4& left, Matrix4& right, float& trackingOffset,
                        const HeadPoseState&, const Quaternion& eyeRotL, const Quaternion& eyeRotR) = nullptr;
  void (*setTrackingOriginMode)(int mode) = nullptr;
  int (*getTrackingOriginMode)() = nullptr;
  bool (*isControllerInit)() = nullptr;
  void (*getControllerData)(ControllerData* left, ControllerData* right) = nullptr;
  void (*startVibration)(int mask, float amplitude, float durationSeconds) = nullptr;
  void (*stopVibration)(int mask) = nullptr;
  bool (*setFoveation)(int level, const FoveationParam* custom) = nullptr;
  void (*postSetEyeBufferSize)(int width, int height) = nullptr;
  Vector3 (*getBoundaryDimensions)() = nullptr;

  template <typename T>
  static T load(void* lib, const char* name) {
    return reinterpret_cast<T>(dlsym(lib, name));
  }

  bool Load() {
    if (library) {
      return true;
    }
    library = dlopen("libqiyuapi.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      return false;
    }
    init = load<InitFn>(library, "qiyu_Init");
    getSdkVersion = load<SdkVersion (*)()>(library, "qiyu_GetSdkVersion");
    release = load<bool (*)()>(library, "qiyu_Release");
    startVR = load<bool (*)(ANativeWindow*, int, int)>(library, "qiyu_StartVR");
    endVR = load<bool (*)()>(library, "qiyu_EndVR");
    startEye = load<bool (*)(bool, int, int)>(library, "qiyu_StartEye");
    endEye = load<bool (*)(bool, int, int)>(library, "qiyu_EndEye");
    submitFrame = load<bool (*)(const FrameParam&)>(library, "qiyu_SubmitFrame");
    update = load<void (*)(float)>(library, "qiyu_Update");
    getDeviceInfo = load<DeviceInfo (*)()>(library, "qiyu_GetDeviceInfo");
    predictDisplayTime = load<float (*)()>(library, "qiyu_PredictDisplayTime");
    predictHeadPose = load<HeadPoseState (*)(float)>(library, "qiyu_PredictHeadPose");
    getViewMatrix = load<bool (*)(Matrix4&, Matrix4&, float&, const HeadPoseState&,
                                  const Quaternion&, const Quaternion&)>(library, "qiyu_GetViewMatrix");
    setTrackingOriginMode = load<void (*)(int)>(library, "qiyu_SetTrackingOriginMode");
    getTrackingOriginMode = load<int (*)()>(library, "qiyu_GetTrackingOriginMode");
    isControllerInit = load<bool (*)()>(library, "qiyu_IsControllerInit");
    getControllerData = load<void (*)(ControllerData*, ControllerData*)>(library, "qiyu_GetControllerData");
    startVibration = load<void (*)(int, float, float)>(library, "qiyu_StartControllerVibration");
    stopVibration = load<void (*)(int)>(library, "qiyu_StopControllerVibration");
    setFoveation = load<bool (*)(int, const FoveationParam*)>(library, "qiyu_SetFoveation");
    postSetEyeBufferSize = load<void (*)(int, int)>(library, "qiyu_PostSetEyeBufferSize");
    getBoundaryDimensions = load<Vector3 (*)()>(library, "qiyu_GetBoundaryDimensions");

    return init && startVR && submitFrame && getDeviceInfo && predictHeadPose;
  }

  bool Valid() const { return library != nullptr && init != nullptr; }

  void Unload() {
    if (library) {
      dlclose(library);
      library = nullptr;
    }
  }
};

} // namespace qiyu
} // namespace crow
