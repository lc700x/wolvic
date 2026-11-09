#pragma once

// Native Qiyu (SXR) device delegate.
//
// Implements Wolvic's DeviceDelegate on top of the Qiyu Native SDK (libqiyuapi.so),
// presenting straight to the SXR compositor (qiyu_StartVR / qiyu_StartEye /
// qiyu_SubmitFrame). This is the path the working reference client uses, and it does
// not need an OpenXR runtime. WebXR still works because Wolvic's Gecko backend drives
// WebXR through ExternalVR (VRManager IPC), not through OpenXR.

#include "DeviceDelegate.h"
#include "QiyuAPI.h"

#include <android/native_window.h>

namespace crow {

class BrowserEGLContext;
struct JavaContext;

class DeviceDelegateQiyu;
typedef std::shared_ptr<DeviceDelegateQiyu> DeviceDelegateQiyuPtr;

class DeviceDelegateQiyu : public DeviceDelegate {
public:
  static DeviceDelegateQiyuPtr Create(vrb::RenderContextPtr& aContext, JavaContext* aJavaContext);

  device::DeviceType GetDeviceType() override;
  void SetRenderMode(const device::RenderMode aMode) override;
  device::RenderMode GetRenderMode() override;
  void RegisterImmersiveDisplay(ImmersiveDisplayPtr aDisplay) override;
  void SetImmersiveSize(const uint32_t aEyeWidth, const uint32_t aEyeHeight) override;
  GestureDelegateConstPtr GetGestureDelegate() override;
  vrb::CameraPtr GetCamera(const device::Eye aWhich) override;
  const vrb::Matrix& GetHeadTransform() const override;
  const vrb::Matrix& GetReorientTransform() const override;
  void SetReorientTransform(const vrb::Matrix& aMatrix) override;
  void Reorient(const vrb::Matrix& aMatrix, const ReorientMode aMode) override;
  void SetClearColor(const vrb::Color& aColor) override;
  void SetClipPlanes(const float aNear, const float aFar) override;
  void SetControllerDelegate(ControllerDelegatePtr& aController) override;
  void ReleaseControllerDelegate() override;
  int32_t GetControllerModelCount() const override;
  const std::string GetControllerModelName(const int32_t aModelIndex) const override;
  bool IsPositionTrackingSupported() const override;
  void SetCPULevel(const device::CPULevel aLevel) override;
  void ProcessEvents() override;
  void StartFrame(const FramePrediction aPrediction) override;
  void BindEye(const device::Eye aWhich) override;
  void EndFrame(const FrameEndMode aMode) override;
  bool PopulateTrackedKeyboardInfo(TrackedKeyboardInfo& keyboardInfo) override;
  float GetSelectThreshold(int32_t controllerIndex) override;

  // Platform hooks used by native-lib (native SXR presentation).
  void EnterVR(const BrowserEGLContext& aEGLContext) override;
  void LeaveVR() override;
  void SetNativeWindow(ANativeWindow* aWindow) override;
  bool IsInVRMode() const override;
  bool ShouldExitRenderLoop() const override;
  void OnDestroy() override;
  bool ExitApp() override;

protected:
  struct State;
  DeviceDelegateQiyu(State& aState);
private:
  State& m;
  VRB_NO_DEFAULTS(DeviceDelegateQiyu)
};

} // namespace crow
