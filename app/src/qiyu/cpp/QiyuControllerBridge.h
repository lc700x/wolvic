#pragma once

#if defined(QIYU)

#include "ControllerDelegate.h"
#include "Device.h"
#include <jni.h>
#include <atomic>
#include <cstdint>

namespace crow {

// Qiyu Native SDK v1.0.0-compatible PODs (minimal ABI subset; see third_party/qiyu/Include/QiyuApi.h).
struct qiyu_Vector2 { float x, y; };
struct qiyu_Vector3 { float x, y, z; };
struct qiyu_Quaternion { float x, y, z, w; };
struct qiyu_ControllerData {
  int isConnect; int button; int buttonTouch; int batteryLevel;
  float triggerForce; float gripForce; int isShow;
  qiyu_Vector2 joyStickPos; qiyu_Vector3 position; qiyu_Quaternion rotation;
  qiyu_Vector3 velocity; qiyu_Vector3 acceleration; qiyu_Vector3 angVelocity; qiyu_Vector3 angAcceleration;
};

// QIYU controller bridge using libqiyuapi.so (QIYU Native SDK, v1.0.0 ABI).
// Used as a fallback/augmentation for QIYU headsets (QIYU 3, Dream/Dream Pro,
// Lenovo Legion VR700, MIX, Qiyu X) when OpenXR controller input is incomplete.
// Follows ALVR/alvr-openxr-v20 safety rules:
// - SXR libraries (qiyivrsdkcore/sxrapi/ashreader/qiyuapi) are loaded via
//   System.loadLibrary in PlatformActivity.onCreate BEFORE NativeActivity starts OpenXR,
//   so JNI_OnLoad initializes QVR globals and the app classloader owns libsxrapi.so first.
// - qiyu_Init is deferred until the OpenXR input path is live (first controller update);
//   qiyu_StartVR is NOT called here to avoid conflicts with Wolvic's OpenXR session.
class QiyuControllerBridge {
public:
  static QiyuControllerBridge& Instance();

  // The Qiyu SDK bridge is only safe when the active OpenXR runtime does not
  // already own the SXR libraries (e.g. Qiyu X). On Monado/VR700 runtimes the
  // runtime loads its own SXR stack and app-side SXR loads must never happen.
  void SetEnabled(bool enabled);

  void SetSxrLibrariesLoaded(bool loaded);
  // Retains the Activity and JavaVM for the deferred qiyu_Init call. Does NOT
  // call qiyu_Init: the bridge initializes lazily from ApplyControllerState,
  // which runs after the OpenXR session is up.
  void SetActivity(JNIEnv* env, jobject activity);
  // Safe to call from Java/UI thread; attaches to JVM if needed. Returns true
  // when init was attempted or controller module is ready.
  bool Init(JNIEnv* env, jobject activity);

  // Returns true if qiyuapi reported a connected controller and state was applied.
  bool ApplyControllerState(int32_t index, bool leftHanded, device::RenderMode renderMode,
                            ControllerDelegate& delegate);
  void ApplyHaptic(int32_t index, bool leftHanded, float durationSeconds, float amplitude);

private:
  QiyuControllerBridge() = default;
  void EnsureLoaded();
  void EnsureInit(JNIEnv* env, jobject activity);
  bool Poll(qiyu_ControllerData& left, qiyu_ControllerData& right);

  void* mLib = nullptr;
  bool mAttemptedLoad = false;
  std::atomic_bool mEnabled{false};
  std::atomic_bool mSxrLibrariesLoaded{false};
  bool mInitAttempted = false;
  JavaVM* mVm = nullptr;
  jobject mActivityRef = nullptr;

  using GetControllerDataFn = void (*)(qiyu_ControllerData* left, qiyu_ControllerData* right);
  using GetControllerDataSingleFn = bool (*)(int index, qiyu_ControllerData* data);
  using IsControllerInitFn = bool (*)();
  // qiyu_Init: matches Qiyu Native SDK v1.0.0 (QiyuApi.h). graphicsApi/trackingOrigin passed as int for ABI stability.
  using InitFn = bool (*)(jobject activity, JavaVM* vm, int graphicsApi, int trackingOrigin, bool logVerbose);
  // Vibration APIs return void in Qiyu Native SDK v1.0.0.
  using StartVibrationFn = void (*)(int mask, float amplitude, float durationSeconds);
  using StopVibrationFn = void (*)(int mask);
  // Optional: some runtimes/SDK integrations expect periodic updates for controller state.
  using UpdateFn = void (*)(float deltaTimeSeconds);

  InitFn mInit = nullptr;
  GetControllerDataFn mGetControllerData = nullptr;
  IsControllerInitFn mIsControllerInit = nullptr;
  StartVibrationFn mStartVibration = nullptr;
  StopVibrationFn mStopVibration = nullptr;
  UpdateFn mUpdate = nullptr;
};

} // namespace crow

#endif // defined(QIYU)
