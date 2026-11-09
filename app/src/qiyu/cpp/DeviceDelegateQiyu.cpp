#include "DeviceDelegateQiyu.h"

#include "BrowserEGLContext.h"
#include "BrowserWorld.h"
#include "DeviceUtils.h"
#include "JNIUtil.h"
#include "VRBrowser.h"
#include "vrb/CameraEye.h"
#include "vrb/Color.h"
#include "vrb/ConcreteClass.h"
#include "vrb/CreationContext.h"
#include "vrb/FBO.h"
#include "vrb/GLError.h"
#include "vrb/Logger.h"
#include "vrb/Matrix.h"
#include "vrb/Quaternion.h"
#include "vrb/RenderContext.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace crow {

namespace {
constexpr float kClickThreshold = 0.2f;
constexpr int kMaxControllers = 2;

// Qiyu SDK is right-handed like SXR/OpenVR; Wolvic world is Y-up, -Z forward.
// Negate position (both axes) and keep the rotation, matching the reference delegate
// and the in-tree QiyuControllerBridge fallback.
vrb::Matrix ToWolvicMatrix(const qiyu::Vector3& p, const qiyu::Quaternion& r) {
  vrb::Quaternion q(r.x, r.y, r.z, r.w);
  vrb::Matrix matrix = vrb::Matrix::Rotation(q);
  matrix.PreMultiplyInPlace(vrb::Matrix::Position(vrb::Vector(-p.x, -p.y, -p.z)));
  return matrix;
}

float ToDegrees(float radians) {
  return radians * 180.0f / static_cast<float>(M_PI);
}

double NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

void FillLayerIdentityUV(qiyu::RenderLayerScreenPosUV& layout) {
  const float ll[4] = {-1.0f, -1.0f, 0.0f, 1.0f};
  const float lr[4] = {1.0f, -1.0f, 0.0f, 1.0f};
  const float ul[4] = {-1.0f, 1.0f, 0.0f, 1.0f};
  const float ur[4] = {1.0f, 1.0f, 0.0f, 1.0f};
  const float luv[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  const float uuv[4] = {0.0f, 1.0f, 1.0f, 1.0f};
  memcpy(layout.LowerLeftPos, ll, sizeof(ll));
  memcpy(layout.LowerRightPos, lr, sizeof(lr));
  memcpy(layout.UpperLeftPos, ul, sizeof(ul));
  memcpy(layout.UpperRightPos, ur, sizeof(ur));
  memcpy(layout.LowerUVs, luv, sizeof(luv));
  memcpy(layout.UpperUVs, uuv, sizeof(uuv));
}
} // namespace

struct DeviceDelegateQiyu::State {
  vrb::RenderContextWeak context;
  JavaContext* javaContext { nullptr };

  qiyu::API api;
  qiyu::DeviceInfo deviceInfo {};

  ImmersiveDisplayPtr immersiveDisplay;
  ControllerDelegatePtr controller;
  vrb::CameraEyePtr cameras[2];
  vrb::FBOPtr eyeFBO[2];
  GLuint eyeTextures[2] { 0, 0 };

  uint32_t eyeWidth { 0 };
  uint32_t eyeHeight { 0 };
  ANativeWindow* window { nullptr };

  bool apiInitialized { false };
  bool vrReady { false };
  bool startEyesActive { false };
  device::RenderMode renderMode { device::RenderMode::StandAlone };

  vrb::Matrix reorientMatrix { vrb::Matrix::Identity() };
  vrb::Matrix headTransform { vrb::Matrix::Identity() };
  vrb::Matrix eyeTransforms[2] { vrb::Matrix::Identity(), vrb::Matrix::Identity() };
  vrb::Color clearColor { vrb::Color(0.0f, 0.0f, 0.0f, 0.0f) };
  float near { 0.05f };
  float far { 1000.0f };

  int32_t boundEye { -1 };
  double lastUpdateMs { 0.0 };
  float trackingOffset { 0.0f };
  int lastConnected[2] { -1, -1 };
  int lastBattery[2] { -1, -1 };

  void Initialize() {
    vrb::RenderContextPtr render = context.lock();
    if (!render) {
      return;
    }
    vrb::CreationContextPtr create = render->GetRenderThreadCreationContext();
    for (int i = 0; i < 2; ++i) {
      cameras[i] = vrb::CameraEye::Create(create);
    }
  }

  void LoadAPI() {
    if (!api.Load()) {
      VRB_ERROR("QIYU: libqiyuapi.so unavailable; native SXR delegate disabled");
      return;
    }
    if (api.getSdkVersion) {
      auto v = api.getSdkVersion();
      VRB_LOG("QIYU: native SDK %d.%d.%d", v.versionProduct, v.versionMajor, v.versionMinor);
    }
  }

  bool InitSDK() {
    if (apiInitialized) {
      return true;
    }
    if (!api.Valid() || !javaContext || !javaContext->vm || !javaContext->activity) {
      return false;
    }
    const bool ok = api.init(javaContext->activity, javaContext->vm,
                             qiyu::GA_OpenGLES, qiyu::TM_Ground, false);
    const bool controllerUp = api.isControllerInit && api.isControllerInit();
    // On Qiyu X the SXR stack is already brought up in-process by the SXR libraries that
    // PlatformActivity loads first (VRSDKCore JNI_OnLoad), so qiyu_Init can report false
    // while qiyu_IsControllerInit() is already true. Treat that as a usable SDK instead of
    // bailing out (that was the black screen: init=0 -> EnterVR returned early).
    VRB_LOG("QIYU: qiyu_Init=%d controller_init=%d", ok, controllerUp ? 1 : 0);
    apiInitialized = ok || controllerUp;
    if (apiInitialized) {
      deviceInfo = api.getDeviceInfo();
      VRB_LOG("QIYU: eye target %dx%d", deviceInfo.iEyeTargetWidth, deviceInfo.iEyeTargetHeight);
    }
    return apiInitialized;
  }

  void UpdateFovAndProjection() {
    const qiyu::ViewFrustum& left = deviceInfo.frustumLeftEye;
    const qiyu::ViewFrustum& right = deviceInfo.frustumRightEye;
    auto buildPerspective = [this](const qiyu::ViewFrustum& f) {
      const float n = f.near > 0.0001f ? f.near : 0.05f;
      return vrb::Matrix::PerspectiveMatrix(std::atan(std::fabs(f.left) / n),
                                            std::atan(std::fabs(f.right) / n),
                                            std::atan(std::fabs(f.top) / n),
                                            std::atan(std::fabs(f.bottom) / n),
                                            near, far);
    };
    cameras[0]->SetPerspective(buildPerspective(left));
    cameras[1]->SetPerspective(buildPerspective(right));

    if (immersiveDisplay) {
      auto setFov = [this](device::Eye eye, const qiyu::ViewFrustum& f) {
        const float n = f.near > 0.0001f ? f.near : 0.05f;
        immersiveDisplay->SetFieldOfView(eye,
            ToDegrees(std::atan(std::fabs(f.left) / n)),
            ToDegrees(std::atan(std::fabs(f.right) / n)),
            ToDegrees(std::atan(std::fabs(f.top) / n)),
            ToDegrees(std::atan(std::fabs(f.bottom) / n)));
      };
      setFov(device::Eye::Left, left);
      setFov(device::Eye::Right, right);

      // Eye separation from the frustum positions (SXR reports metres).
      const float ipd = std::fabs(right.position.x - left.position.x);
      if (ipd > 0.001f) {
        immersiveDisplay->SetEyeOffset(device::Eye::Left, -ipd * 0.5f, 0.0f, 0.0f);
        immersiveDisplay->SetEyeOffset(device::Eye::Right, ipd * 0.5f, 0.0f, 0.0f);
      }
      immersiveDisplay->SetEyeResolution(static_cast<int32_t>(eyeWidth),
                                         static_cast<int32_t>(eyeHeight));
      immersiveDisplay->SetCapabilityFlags(device::Orientation | device::Position |
                                           device::Present | device::InlineSession |
                                           device::ImmersiveVRSession | device::StageParameters);
    }
  }

  void DestroyEyeBuffers() {
    for (int i = 0; i < 2; ++i) {
      eyeFBO[i].reset();
      if (eyeTextures[i]) {
        VRB_GL_CHECK(glDeleteTextures(1, &eyeTextures[i]));
        eyeTextures[i] = 0;
      }
    }
  }

  bool CreateEyeBuffers(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
      return false;
    }
    if (eyeWidth == width && eyeHeight == height && eyeTextures[0] != 0) {
      return true;
    }
    DestroyEyeBuffers();

    vrb::RenderContextPtr render = context.lock();
    if (!render) {
      return false;
    }
    eyeWidth = width;
    eyeHeight = height;

    for (int i = 0; i < 2; ++i) {
      VRB_GL_CHECK(glGenTextures(1, &eyeTextures[i]));
      VRB_GL_CHECK(glBindTexture(GL_TEXTURE_2D, eyeTextures[i]));
      VRB_GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
      VRB_GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
      VRB_GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
      VRB_GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
      // RGBA8 immutable storage, as the reference SXR render target.
      VRB_GL_CHECK(glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8,
                                  static_cast<GLsizei>(width), static_cast<GLsizei>(height)));
      VRB_GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));

      vrb::FBOPtr fbo = vrb::FBO::Create(render);
      vrb::FBO::Attributes attributes;
      attributes.depth = true;
      attributes.samples = 0;
      fbo->SetTextureHandle(eyeTextures[i], static_cast<int32_t>(width),
                            static_cast<int32_t>(height), attributes);
      eyeFBO[i] = fbo;
    }

    if (api.postSetEyeBufferSize) {
      api.postSetEyeBufferSize(static_cast<int>(width), static_cast<int>(height));
    }
    VRB_LOG("QIYU: eye buffers %ux%u", width, height);
    return true;
  }

  void UpdateTracking() {
    if (!api.predictHeadPose) {
      return;
    }
    const float displayTimeMs = api.predictDisplayTime ? api.predictDisplayTime() : 0.0f;
    qiyu::HeadPoseState head = api.predictHeadPose(displayTimeMs);

    vrb::Matrix headMatrix = ToWolvicMatrix(head.pose.position, head.pose.rotation);

    // Ground-mode offset from the SXR view matrices (also gives us the eye transforms).
    if (api.getViewMatrix) {
      qiyu::Matrix4 leftMatrix{}, rightMatrix{};
      float offset = trackingOffset;
      if (api.getViewMatrix(leftMatrix, rightMatrix, offset, head,
                            deviceInfo.frustumLeftEye.rotation,
                            deviceInfo.frustumRightEye.rotation)) {
        trackingOffset = offset;
      }
    }
    headMatrix.TranslateInPlace(vrb::Vector(0.0f, -trackingOffset, 0.0f));

    headTransform = reorientMatrix.PostMultiply(headMatrix);
    cameras[0]->SetHeadTransform(headTransform);
    cameras[1]->SetHeadTransform(headTransform);

    const qiyu::Vector3& leftPos = deviceInfo.frustumLeftEye.position;
    const qiyu::Vector3& rightPos = deviceInfo.frustumRightEye.position;
    eyeTransforms[0] = vrb::Matrix::Translation(vrb::Vector(leftPos.x, leftPos.y, leftPos.z));
    eyeTransforms[1] = vrb::Matrix::Translation(vrb::Vector(rightPos.x, rightPos.y, rightPos.z));
    cameras[0]->SetEyeTransform(eyeTransforms[0]);
    cameras[1]->SetEyeTransform(eyeTransforms[1]);
    if (immersiveDisplay) {
      immersiveDisplay->SetEyeTransform(device::Eye::Left, eyeTransforms[0]);
      immersiveDisplay->SetEyeTransform(device::Eye::Right, eyeTransforms[1]);
    }
  }

  void UpdateController(int hand, const qiyu::ControllerData& data, ControllerDelegate& delegate) {
    const int32_t index = hand;
    const bool leftHanded = hand == 0;

    if (!data.isConnect) {
      if (lastConnected[hand] != 0) {
        lastConnected[hand] = 0;
        delegate.SetEnabled(index, false);
      }
      return;
    }
    if (lastConnected[hand] != 1) {
      lastConnected[hand] = 1;
      VRB_LOG("QIYU: controller %d connected battery=%d pos=(%.3f,%.3f,%.3f) rot=(%.2f,%.2f,%.2f,%.2f)",
              hand, data.batteryLevel, data.position.x, data.position.y, data.position.z,
              data.rotation.x, data.rotation.y, data.rotation.z, data.rotation.w);
    }

    // Input diagnostics: log raw button/axis transitions so we can tell whether SXR input
    // reaches the delegate at all (throttled: only on change).
    static int sLastButton[2] = {-1, -1};
    static int sLastTouch[2] = {-1, -1};
    if (sLastButton[hand] != data.button || sLastTouch[hand] != data.buttonTouch) {
      sLastButton[hand] = data.button;
      sLastTouch[hand] = data.buttonTouch;
      VRB_LOG("QIYU: controller %d button=0x%x touch=0x%x trigger=%.2f grip=%.2f stick=(%.2f,%.2f)",
              hand, data.button, data.buttonTouch, data.triggerForce, data.gripForce,
              data.joyStickPos.x, data.joyStickPos.y);
    }

    delegate.SetEnabled(index, true);
    delegate.SetLeftHanded(index, leftHanded);
    delegate.SetTargetRayMode(index, device::TargetRayMode::TrackedPointer);
    delegate.SetCapabilityFlags(index, device::Orientation | device::Position | device::PositionEmulated);
    delegate.SetTransform(index, ToWolvicMatrix(data.position, data.rotation));
    if (data.batteryLevel != lastBattery[hand]) {
      lastBattery[hand] = data.batteryLevel;
      delegate.SetBatteryLevel(index, data.batteryLevel);
    }

    const bool triggerPressed = (data.button & qiyu::BT_Trigger) != 0 || data.triggerForce >= kClickThreshold;
    const bool triggerTouched = (data.buttonTouch & qiyu::BT_Trigger) != 0 || data.triggerForce > 0.001f;
    const bool squeezePressed = (data.button & qiyu::BT_Grip) != 0 || data.gripForce >= kClickThreshold;
    const bool squeezeTouched = (data.buttonTouch & qiyu::BT_Grip) != 0 || data.gripForce > 0.001f;
    const bool stickPressed = (data.button & qiyu::BT_JoyStick) != 0;
    const bool stickTouched = (data.buttonTouch & qiyu::BT_JoyStick) != 0;
    const bool axPressed = (data.button & qiyu::BT_A_X) != 0;
    const bool axTouched = (data.buttonTouch & qiyu::BT_A_X) != 0;
    const bool byPressed = (data.button & qiyu::BT_B_Y) != 0;
    const bool byTouched = (data.buttonTouch & qiyu::BT_B_Y) != 0;
    const bool menuPressed = (data.button & qiyu::BT_Home_Menu) != 0;

    delegate.SetButtonCount(index, 7);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_TRIGGER,
                            device::kImmersiveButtonTrigger, triggerPressed, triggerTouched, data.triggerForce);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_SQUEEZE,
                            device::kImmersiveButtonSqueeze, squeezePressed, squeezeTouched, data.gripForce);
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_TOUCHPAD,
                            device::kImmersiveButtonThumbstick, stickPressed, stickTouched,
                            stickPressed ? 1.0f : 0.0f);
    if (leftHanded) {
      delegate.SetButtonState(index, ControllerDelegate::BUTTON_X, device::kImmersiveButtonA,
                              axPressed, axTouched, axPressed ? 1.0f : 0.0f);
      delegate.SetButtonState(index, ControllerDelegate::BUTTON_Y, device::kImmersiveButtonB,
                              byPressed, byTouched, byPressed ? 1.0f : 0.0f);
    } else {
      delegate.SetButtonState(index, ControllerDelegate::BUTTON_A, device::kImmersiveButtonA,
                              axPressed, axTouched, axPressed ? 1.0f : 0.0f);
      delegate.SetButtonState(index, ControllerDelegate::BUTTON_B, device::kImmersiveButtonB,
                              byPressed, byTouched, byPressed ? 1.0f : 0.0f);
    }
    delegate.SetButtonState(index, ControllerDelegate::BUTTON_APP, -1, menuPressed, false,
                            menuPressed ? 1.0f : 0.0f);

    float axes[4] = {0.0f, 0.0f, data.joyStickPos.x, -data.joyStickPos.y};
    delegate.SetAxes(index, axes, 4);
    delegate.SetScrolledDelta(index, -data.joyStickPos.x, data.joyStickPos.y);
    delegate.SetSelectFactor(index, data.triggerForce);
  }

  void UpdateControllers() {
    if (!controller || !api.isControllerInit || !api.getControllerData) {
      return;
    }
    if (!api.isControllerInit()) {
      return;
    }
    qiyu::ControllerData data[2]{};
    api.getControllerData(&data[0], &data[1]);
    UpdateController(0, data[0], *controller);
    UpdateController(1, data[1], *controller);
  }

  void Shutdown() {
    DestroyEyeBuffers();
    if (vrReady && api.endVR) {
      api.endVR();
    }
    vrReady = false;
    if (apiInitialized && api.release) {
      api.release();
    }
    apiInitialized = false;
    api.Unload();
  }
};

DeviceDelegateQiyuPtr
DeviceDelegateQiyu::Create(vrb::RenderContextPtr& aContext, JavaContext* aJavaContext) {
  DeviceDelegateQiyuPtr result = std::make_shared<vrb::ConcreteClass<DeviceDelegateQiyu, DeviceDelegateQiyu::State>>();
  result->m.context = aContext;
  result->m.javaContext = aJavaContext;
  result->m.Initialize();
  result->m.LoadAPI();
  return result;
}

DeviceDelegateQiyu::DeviceDelegateQiyu(State& aState) : m(aState) {}

device::DeviceType
DeviceDelegateQiyu::GetDeviceType() {
  return DeviceUtils::GetDeviceTypeFromSystem();
}

void
DeviceDelegateQiyu::SetRenderMode(const device::RenderMode aMode) {
  m.renderMode = aMode;
}

device::RenderMode
DeviceDelegateQiyu::GetRenderMode() {
  return m.renderMode;
}

void
DeviceDelegateQiyu::RegisterImmersiveDisplay(ImmersiveDisplayPtr aDisplay) {
  m.immersiveDisplay = std::move(aDisplay);
  if (!m.immersiveDisplay) {
    return;
  }
  m.immersiveDisplay->SetDeviceName("QIYU");
  m.immersiveDisplay->SetCapabilityFlags(device::Orientation | device::Position |
                                         device::Present | device::InlineSession |
                                         device::ImmersiveVRSession | device::StageParameters);
  m.immersiveDisplay->SetBlendModes({ device::BlendMode::Opaque });
  m.immersiveDisplay->CompleteEnumeration();
}

void
DeviceDelegateQiyu::SetImmersiveSize(const uint32_t aEyeWidth, const uint32_t aEyeHeight) {
  // WebXR may request a different immersive size; SXR composites from our eye textures,
  // so keep our buffers but let the browser scale the blit.
  if (aEyeWidth == 0 || aEyeHeight == 0) {
    return;
  }
}

GestureDelegateConstPtr
DeviceDelegateQiyu::GetGestureDelegate() {
  return nullptr;
}

vrb::CameraPtr
DeviceDelegateQiyu::GetCamera(const device::Eye aWhich) {
  const int32_t index = device::EyeIndex(aWhich);
  return index < 0 ? nullptr : m.cameras[index];
}

const vrb::Matrix&
DeviceDelegateQiyu::GetHeadTransform() const {
  return m.headTransform;
}

const vrb::Matrix&
DeviceDelegateQiyu::GetReorientTransform() const {
  return m.reorientMatrix;
}

void
DeviceDelegateQiyu::SetReorientTransform(const vrb::Matrix& aMatrix) {
  m.reorientMatrix = aMatrix;
}

void
DeviceDelegateQiyu::Reorient(const vrb::Matrix& aMatrix, const ReorientMode aMode) {
  switch (aMode) {
    case ReorientMode::SIX_DOF:
      m.reorientMatrix = DeviceUtils::CalculateReorientationMatrixOnHeadLock(
          aMatrix, m.headTransform.GetTranslation());
      break;
    case ReorientMode::NO_ROLL:
      m.reorientMatrix = DeviceUtils::CalculateReorientationMatrixWithoutRoll(
          aMatrix, m.headTransform.GetTranslation());
      break;
    default:
      VRB_ERROR("QIYU: unsupported reorient mode %d", static_cast<int>(aMode));
      break;
  }
  if (mReorientClient) {
    mReorientClient->OnReorient();
  }
}

void
DeviceDelegateQiyu::SetClearColor(const vrb::Color& aColor) {
  m.clearColor = aColor;
}

void
DeviceDelegateQiyu::SetClipPlanes(const float aNear, const float aFar) {
  m.near = aNear;
  m.far = aFar;
  m.UpdateFovAndProjection();
}

void
DeviceDelegateQiyu::SetControllerDelegate(ControllerDelegatePtr& aController) {
  m.controller = aController;
  for (int i = 0; i < kMaxControllers; ++i) {
    m.lastConnected[i] = -1;
    m.lastBattery[i] = -1;
  }
}

void
DeviceDelegateQiyu::ReleaseControllerDelegate() {
  m.controller = nullptr;
}

int32_t
DeviceDelegateQiyu::GetControllerModelCount() const {
  return 2;
}

const std::string
DeviceDelegateQiyu::GetControllerModelName(const int32_t aModelIndex) const {
  return "generic-trigger.obj";
}

bool
DeviceDelegateQiyu::IsPositionTrackingSupported() const {
  return true;
}

void
DeviceDelegateQiyu::SetCPULevel(const device::CPULevel aLevel) {
}

void
DeviceDelegateQiyu::ProcessEvents() {
}

void
DeviceDelegateQiyu::SetNativeWindow(ANativeWindow* aWindow) {
  m.window = aWindow;
}

void
DeviceDelegateQiyu::EnterVR(const BrowserEGLContext& aEGLContext) {
  if (m.vrReady) {
    return;
  }
  if (!m.InitSDK()) {
    VRB_ERROR("QIYU: cannot enter VR, qiyu_Init failed");
    return;
  }
  if (m.eyeWidth == 0 || m.eyeHeight == 0) {
    const uint32_t w = static_cast<uint32_t>(std::max(m.deviceInfo.iEyeTargetWidth, 1));
    const uint32_t h = static_cast<uint32_t>(std::max(m.deviceInfo.iEyeTargetHeight, 1));
    if (!m.CreateEyeBuffers(w, h)) {
      VRB_ERROR("QIYU: cannot create eye buffers");
      return;
    }
  }
  if (!m.window) {
    VRB_ERROR("QIYU: no ANativeWindow for qiyu_StartVR");
    return;
  }
  m.UpdateFovAndProjection();

  const bool started = m.api.startVR(m.window, qiyu::PL_System, qiyu::PL_System);
  VRB_LOG("QIYU: qiyu_StartVR=%d (window=%p, eye %ux%u)", started, m.window,
          m.eyeWidth, m.eyeHeight);
  if (!started) {
    VRB_ERROR("QIYU: qiyu_StartVR failed");
    return;
  }
  if (m.api.setTrackingOriginMode) {
    m.api.setTrackingOriginMode(qiyu::TM_Ground);
  }
  if (m.api.setFoveation) {
    m.api.setFoveation(qiyu::FL_Low, nullptr);
  }
  if (m.api.postSetEyeBufferSize) {
    m.api.postSetEyeBufferSize(static_cast<int>(m.eyeWidth), static_cast<int>(m.eyeHeight));
  }
  m.vrReady = true;
  m.lastUpdateMs = NowMs();
  VRB_LOG("QIYU: SXR VR started (eye %ux%u)", m.eyeWidth, m.eyeHeight);
}

void
DeviceDelegateQiyu::LeaveVR() {
  if (!m.vrReady) {
    return;
  }
  m.vrReady = false;
  m.startEyesActive = false;
  if (m.api.endVR) {
    m.api.endVR();
  }
  VRB_LOG("QIYU: SXR VR stopped");
}

void
DeviceDelegateQiyu::StartFrame(const FramePrediction aPrediction) {
  mShouldRender = false;
  if (!m.vrReady) {
    return;
  }
  const double now = NowMs();
  const float delta = m.lastUpdateMs > 0.0
                          ? static_cast<float>((now - m.lastUpdateMs) / 1000.0)
                          : 0.0f;
  m.lastUpdateMs = now;
  if (m.api.update) {
    m.api.update(std::clamp(delta, 0.0f, 0.1f));
  }

  m.UpdateTracking();
  m.UpdateControllers();

  // QiyuApi.h: qiyu_StartEye must be called AFTER the eye buffer is bound and BEFORE
  // rendering that eye, so it is driven from BindEye() per eye (see there).
  mShouldRender = true;
}

void
DeviceDelegateQiyu::BindEye(const device::Eye aWhich) {
  const int32_t index = device::EyeIndex(aWhich);
  if (index < 0 || !m.eyeFBO[index]) {
    return;
  }
  // Close the previously rendered eye before opening the next one (QiyuApi.h contract:
  // StartEye after the eye buffer is bound, EndEye after that eye has been rendered).
  if (m.startEyesActive && m.boundEye >= 0 && m.boundEye != index) {
    if (m.api.endEye) {
      m.api.endEye(false, m.boundEye == 0 ? qiyu::EYE_Left : qiyu::EYE_Right, qiyu::TT_Texture);
    }
    m.startEyesActive = false;
  }
  m.boundEye = index;
  m.eyeFBO[index]->Bind();
  if (!m.startEyesActive) {
    if (m.api.startEye) {
      m.api.startEye(false, index == 0 ? qiyu::EYE_Left : qiyu::EYE_Right, qiyu::TT_Texture);
      m.startEyesActive = true;
    }
  }
  VRB_GL_CHECK(glViewport(0, 0, static_cast<GLsizei>(m.eyeWidth), static_cast<GLsizei>(m.eyeHeight)));
  VRB_GL_CHECK(glClearColor(m.clearColor.Red(), m.clearColor.Green(),
                            m.clearColor.Blue(), m.clearColor.Alpha()));
  VRB_GL_CHECK(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
}

void
DeviceDelegateQiyu::EndFrame(const FrameEndMode aMode) {
  mShouldRender = false;
  if (!m.vrReady) {
    return;
  }
  if (m.boundEye >= 0) {
    m.eyeFBO[m.boundEye]->Unbind();
  }
  if (m.startEyesActive && m.boundEye >= 0 && m.api.endEye) {
    m.api.endEye(false, m.boundEye == 0 ? qiyu::EYE_Left : qiyu::EYE_Right, qiyu::TT_Texture);
  }
  m.startEyesActive = false;
  m.boundEye = -1;

  if (aMode == FrameEndMode::DISCARD) {
    return;
  }
  if (!m.api.submitFrame) {
    return;
  }

  qiyu::FrameParam frame{};
  frame.minVsyncs = 1;
  for (int eye = 0; eye < 2; ++eye) {
    qiyu::RenderLayer& layer = frame.renderLayers[eye];
    layer.imageHandle = static_cast<int32_t>(m.eyeTextures[eye]);
    layer.imageType = qiyu::TT_Texture;
    FillLayerIdentityUV(layer.imageCoords);
    layer.eyeMask = eye == 0 ? qiyu::RL_EyeMask_Left : qiyu::RL_EyeMask_Right;
  }
  if (m.api.predictHeadPose) {
    frame.headPoseState = m.api.predictHeadPose(m.api.predictDisplayTime ? m.api.predictDisplayTime() : 0.0f);
  }
  if (!m.api.submitFrame(frame)) {
    VRB_WARN("QIYU: qiyu_SubmitFrame failed");
  }
}

bool
DeviceDelegateQiyu::PopulateTrackedKeyboardInfo(TrackedKeyboardInfo& keyboardInfo) {
  return false;
}

float
DeviceDelegateQiyu::GetSelectThreshold(int32_t aControllerIndex) {
  return kClickThreshold;
}

bool
DeviceDelegateQiyu::IsInVRMode() const {
  return m.vrReady;
}

bool
DeviceDelegateQiyu::ShouldExitRenderLoop() const {
  return false;
}

void
DeviceDelegateQiyu::OnDestroy() {
  m.Shutdown();
}

bool
DeviceDelegateQiyu::ExitApp() {
  LeaveVR();
  return true;
}

} // namespace crow
