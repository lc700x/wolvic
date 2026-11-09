#if defined(QIYU)

#include "QiyuControllerBridge.h"
#include "DeviceUtils.h"
#include "SystemUtils.h"
#include "vrb/Logger.h"
#include "vrb/Matrix.h"
#include "vrb/Quaternion.h"

#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace crow {

namespace {
// Qiyu_Native_SDK_v1.0.0-compatible definitions (F:\vr-sdks\qiyu / ALVR).
enum qiyu_ButtonType : int {
  BT_None      = 0,
  BT_Trigger   = 0x01,
  BT_Grip      = 0x02,
  BT_A_X       = 0x10,
  BT_B_Y       = 0x20,
  BT_Home_Menu = 0x40,
  BT_JoyStick  = 0x80
};
enum qiyu_ControllerMask : int { CM_Left = 0x01, CM_Right = 0x02, CM_Both = 0x03 };
enum qiyu_GraphicsApi : int { GA_OpenGLES = 0 };
enum qiyu_TrackingOriginMode : int { TM_Device = 0, TM_Ground = 1 };
constexpr float kClickThreshold = 0.2f;
}

QiyuControllerBridge& QiyuControllerBridge::Instance() {
  static QiyuControllerBridge sInstance;
  return sInstance;
}

void QiyuControllerBridge::SetEnabled(bool enabled) {
  mEnabled.store(enabled);
}

void QiyuControllerBridge::SetSxrLibrariesLoaded(bool loaded) {
  mSxrLibrariesLoaded.store(loaded);
}

void QiyuControllerBridge::SetActivity(JNIEnv* env, jobject activity) {
  if (!env || !activity)
    return;
  if (!mVm) {
    env->GetJavaVM(&mVm);
  }
  if (!mActivityRef) {
    mActivityRef = env->NewGlobalRef(activity);
  }
}

void QiyuControllerBridge::EnsureLoaded() {
  if (!mEnabled.load())
    return;
  if (mAttemptedLoad)
    return;
  mAttemptedLoad = true;

  mLib = dlopen("libqiyuapi.so", RTLD_NOW | RTLD_LOCAL);
  if (!mLib) {
    VRB_WARN("QIYU controller bridge: libqiyuapi.so unavailable: %s", dlerror());
    return;
  }

  mInit = reinterpret_cast<InitFn>(dlsym(mLib, "qiyu_Init"));
  mGetControllerData = reinterpret_cast<GetControllerDataFn>(dlsym(mLib, "qiyu_GetControllerData"));
  mIsControllerInit = reinterpret_cast<IsControllerInitFn>(dlsym(mLib, "qiyu_IsControllerInit"));
  mStartVibration = reinterpret_cast<StartVibrationFn>(dlsym(mLib, "qiyu_StartControllerVibration"));
  mStopVibration = reinterpret_cast<StopVibrationFn>(dlsym(mLib, "qiyu_StopControllerVibration"));
  mUpdate = reinterpret_cast<UpdateFn>(dlsym(mLib, "qiyu_Update"));

  VRB_LOG("QIYU controller bridge: loaded libqiyuapi.so init=%d get=%d isInit=%d update=%d vib=%d",
          mInit != nullptr, mGetControllerData != nullptr, mIsControllerInit != nullptr,
          mUpdate != nullptr, mStartVibration != nullptr);
}

void QiyuControllerBridge::EnsureInit(JNIEnv* env, jobject activity) {
  if (!mEnabled.load())
    return;
  EnsureLoaded();
  if (!mLib || !mInit || !mIsControllerInit)
    return;
  if (mIsControllerInit())
    return;
  if (mInitAttempted)
    return;
  if (!mSxrLibrariesLoaded.load()) {
    VRB_WARN("QIYU controller bridge: SXR libraries not loaded via System.loadLibrary; skipping qiyu_Init");
    return;
  }

  JavaVM* vm = mVm;
  if (!vm && env) {
    env->GetJavaVM(&vm);
  }
  if (!vm || (!activity && !mActivityRef)) {
    VRB_WARN("QIYU controller bridge: missing JavaVM/activity for qiyu_Init");
    return;
  }

  JNIEnv* initEnv = env;
  bool attached = false;
  if (vm->GetEnv(reinterpret_cast<void**>(&initEnv), JNI_VERSION_1_6) == JNI_EDETACHED) {
    JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("wolvic_qiyu"), nullptr};
    if (vm->AttachCurrentThread(&initEnv, &args) != JNI_OK || !initEnv) {
      VRB_WARN("QIYU controller bridge: failed to attach thread for qiyu_Init");
      return;
    }
    attached = true;
  }
  if (!initEnv) return;

  mVm = vm;
  if (!mActivityRef && activity) {
    mActivityRef = initEnv->NewGlobalRef(activity);
  }
  if (!mActivityRef) {
    VRB_WARN("QIYU controller bridge: failed to retain activity ref");
    return;
  }
  mInitAttempted = true;

  // Do NOT call qiyu_StartVR: Wolvic manages the OpenXR session/runtime.
  const bool ok = mInit(mActivityRef, mVm, GA_OpenGLES, TM_Ground, false);
  VRB_LOG("QIYU controller bridge: qiyu_Init=%d controller_init=%d", ok, mIsControllerInit() ? 1 : 0);
  (void)attached; // keep attached; SDK may use thread-local JNI state
}

bool QiyuControllerBridge::Init(JNIEnv* env, jobject activity) {
  if (!mEnabled.load())
    return false;
  EnsureInit(env, activity);
  EnsureLoaded();
  return mLib != nullptr && (mGetControllerData != nullptr);
}

bool QiyuControllerBridge::Poll(qiyu_ControllerData& left, qiyu_ControllerData& right) {
  memset(&left, 0, sizeof(left));
  memset(&right, 0, sizeof(right));
  if (!mGetControllerData)
    return false;
  mGetControllerData(&left, &right);
  return true;
}

bool QiyuControllerBridge::ApplyControllerState(int32_t index, bool leftHanded,
                                                device::RenderMode /*renderMode*/,
                                                ControllerDelegate& delegate) {
  if (!mEnabled.load())
    return false;
  // First controller update happens after the OpenXR session is running, which is the
  // safe point to call qiyu_Init (ALVR v20 controller-bridge pattern). Display/tracking
  // stay on OpenXR; qiyu_StartVR is never called here.
  EnsureInit(nullptr, nullptr);
  EnsureLoaded();
  if (!mGetControllerData)
    return false;
  if (mIsControllerInit && !mIsControllerInit())
    return false;

  // NOTE: do NOT call qiyu_Update here. In ALVR v20 only the full SXR render loop drives
  // qiyu_Update (with real frame delta); the OpenXR-path controller bridge polls
  // qiyu_GetControllerData directly. Calling it with 0.0f twice per frame (once per hand)
  // is a stutter suspect and has no effect on controller data freshness.

  qiyu_ControllerData left{}, right{};
  Poll(left, right);
  const qiyu_ControllerData& data = leftHanded ? left : right;
  if (!data.isConnect) {
    // Cheap change-gated diagnostic (avoids per-frame log spam). Left controller on the
    // reported A091 was disconnected (isConnect=0) while right was connected.
    static int sLastLeftConnect = -1, sLastRightConnect = -1;
    const int lc = left.isConnect ? 1 : 0, rc = right.isConnect ? 1 : 0;
    if (lc != sLastLeftConnect || rc != sLastRightConnect) {
      sLastLeftConnect = lc; sLastRightConnect = rc;
      VRB_LOG("QIYU controller bridge: poll leftConnect=%d rightConnect=%d leftShow=%d rightShow=%d",
              lc, rc, left.isShow, right.isShow);
    }
    return false;
  }
  // Provide pose from qiyuapi when available (helps runtimes where OpenXR pose is unreliable).
  // isShow indicates visibility from the SDK; we still enable the delegate so the model
  // can be rendered by Wolvic even when OpenXR aim is inactive.
  (void)0;

  delegate.SetEnabled(index, true);
  delegate.SetLeftHanded(index, leftHanded);
  delegate.SetTargetRayMode(index, device::TargetRayMode::TrackedPointer);
  delegate.SetBatteryLevel(index, data.batteryLevel);
  delegate.SetCapabilityFlags(index, device::Orientation | device::Position | device::PositionEmulated);

  // Provide pose from qiyuapi when available (helps Qiyu X / runtimes where OpenXR pose is unreliable).
  if (data.isShow || std::abs(data.position.x) + std::abs(data.position.y) + std::abs(data.position.z) > 0.0001f ||
      std::abs(data.rotation.x) + std::abs(data.rotation.y) + std::abs(data.rotation.z) + std::abs(data.rotation.w) > 0.0001f) {
    vrb::Quaternion q(data.rotation.x, data.rotation.y, data.rotation.z, data.rotation.w);
    vrb::Matrix transform = vrb::Matrix::Rotation(q);
    transform.PreMultiplyInPlace(vrb::Matrix::Position(vrb::Vector(-data.position.x, -data.position.y, -data.position.z)));
    delegate.SetTransform(index, transform);
  }

  const bool triggerPressed = (data.button & BT_Trigger) != 0 || data.triggerForce >= kClickThreshold;
  const bool triggerTouched = (data.buttonTouch & BT_Trigger) != 0 || data.triggerForce > 0.001f;
  const bool squeezePressed = (data.button & BT_Grip) != 0 || data.gripForce >= kClickThreshold;
  const bool squeezeTouched = (data.buttonTouch & BT_Grip) != 0 || data.gripForce > 0.001f;
  const bool stickPressed = (data.button & BT_JoyStick) != 0;
  const bool stickTouched = (data.buttonTouch & BT_JoyStick) != 0;
  const bool axPressed = (data.button & BT_A_X) != 0;
  const bool axTouched = (data.buttonTouch & BT_A_X) != 0;
  const bool byPressed = (data.button & BT_B_Y) != 0;
  const bool byTouched = (data.buttonTouch & BT_B_Y) != 0;
  const bool menuPressed = (data.button & BT_Home_Menu) != 0;

  delegate.SetButtonCount(index, 7);
  delegate.SetButtonState(index, ControllerDelegate::BUTTON_TRIGGER, device::kImmersiveButtonTrigger,
                          triggerPressed, triggerTouched, data.triggerForce);
  delegate.SetButtonState(index, ControllerDelegate::BUTTON_SQUEEZE, device::kImmersiveButtonSqueeze,
                          squeezePressed, squeezeTouched, data.gripForce);
  delegate.SetButtonState(index, ControllerDelegate::BUTTON_TOUCHPAD, device::kImmersiveButtonThumbstick,
                          stickPressed, stickTouched, stickPressed ? 1.0f : 0.0f);

  if (leftHanded) {
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_X, device::kImmersiveButtonA, axPressed, axTouched, axPressed ? 1.0f : 0.0f);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_Y, device::kImmersiveButtonB, byPressed, byTouched, byPressed ? 1.0f : 0.0f);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_APP, -1, menuPressed, false, menuPressed ? 1.0f : 0.0f);
  } else {
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_A, device::kImmersiveButtonA, axPressed, axTouched, axPressed ? 1.0f : 0.0f);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_B, device::kImmersiveButtonB, byPressed, byTouched, byPressed ? 1.0f : 0.0f);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_APP, -1, menuPressed, false, menuPressed ? 1.0f : 0.0f);
  }

  float axes[4] = {0.0f, 0.0f, data.joyStickPos.x, -data.joyStickPos.y};
  delegate.SetAxes(index, axes, 4);
  delegate.SetScrolledDelta(index, -data.joyStickPos.x, data.joyStickPos.y);
  delegate.SetSelectFactor(index, data.triggerForce);
  return true;
}

void QiyuControllerBridge::ApplyHaptic(int32_t /*index*/, bool leftHanded, float durationSeconds, float amplitude) {
  if (!mEnabled.load())
    return;
  EnsureInit(nullptr, nullptr);
  EnsureLoaded();
  const int mask = leftHanded ? static_cast<int>(CM_Left) : static_cast<int>(CM_Right);
  if (amplitude <= 0.0f || durationSeconds <= 0.0f) {
    if (mStopVibration) mStopVibration(mask);
    return;
  }
  if (mStartVibration) mStartVibration(mask, std::max(0.0f, std::min(amplitude, 1.0f)), std::max(0.0f, durationSeconds));
}

} // namespace crow

extern "C" JNIEXPORT void JNICALL
Java_com_igalia_wolvic_PlatformActivity_nativeSetQiyuControllerBridgeEnabled(JNIEnv*, jclass, jboolean enabled) {
  crow::QiyuControllerBridge::Instance().SetEnabled(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_igalia_wolvic_PlatformActivity_nativeGetQiyuDeviceType(JNIEnv*, jclass) {
  return static_cast<jint>(crow::DeviceUtils::GetDeviceTypeFromSystem());
}

extern "C" JNIEXPORT void JNICALL
Java_com_igalia_wolvic_PlatformActivity_nativeSetSxrLibrariesLoaded(JNIEnv*, jclass, jboolean loaded) {
  crow::QiyuControllerBridge::Instance().SetSxrLibrariesLoaded(loaded == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_igalia_wolvic_PlatformActivity_nativeSetQiyuActivity(JNIEnv* env, jobject thiz) {
  crow::QiyuControllerBridge::Instance().SetActivity(env, thiz);
}

#endif // defined(QIYU)
