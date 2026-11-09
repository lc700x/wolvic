# Qiyu (iQIYI / SXR) handoff — Wolvic

Status snapshot for whoever picks this up. Everything below is grounded in real
device logs and reverse-engineered reference artifacts, not guesses.

Branch: `main` (repo `lc700x/wolvic`), tip at the time of writing: `e05781c3`.

---

## 1. What works today

| Area | State |
|---|---|
| Qiyu X (native SXR path) | **VR renders**: `qiyu_Init=1`, eye buffers `1920x1920`, `sxrBeginXr` OK, `qiyu_StartVR=1`, steady **71.8 fps**, no Monado, no warp starvation |
| WebXR | Routes through Gecko `ExternalVR` (VRManager IPC), so it does **not** need OpenXR |
| Qiyu X controller | Detected (`controller 1 connected battery=100`), pose+haptics plumbed; **aim/pointer still under test** after the tracking-offset fix in `e05781c3` |
| Qiyu MIX (A081) | App no longer crash-loops; firmware has **no OpenXR runtime broker** and lacks `libStereoRectifyWrapper.so`, so VR is not usable there yet |
| Qiyu Dream/VR700 (A091) | OpenXR path works structurally (Monado `QIYIVR`), but shows runtime-side warp artifacts (see §5) |

## 2. Architecture (why two paths exist)

- **OpenXR path** (`app/src/openxr/cpp/`): Monado (or vendor runtime) drives display; `khr/simple_controller` is the only profile Qiyu advertises, so `QiyuControllerBridge` supplements input via `libqiyuapi`.
- **Native SXR path** (`app/src/qiyu/cpp/`): `DeviceDelegateQiyu` talks to `libqiyuapi.so` directly and submits to the SXR compositor (`qiyu_StartVR` / `StartEye` / `EndEye` / `SubmitFrame`). This is what the working reference client does and what makes Qiyu X smooth.
- Selection happens in `app/src/main/cpp/native-lib.cpp`: known Qiyu model → `DeviceDelegateQiyu`, otherwise `DeviceDelegateOpenXR`.

## 3. Files

New:
```
app/src/qiyu/AndroidManifest.xml
app/src/qiyu/java/com/igalia/wolvic/PlatformActivity.java
app/src/qiyu/cpp/QiyuAPI.h                      # full Qiyu Native SDK v1.0.0 binding (dlopen/dlsym)
app/src/qiyu/cpp/DeviceDelegateQiyu.{h,cpp}     # native SXR device delegate
app/src/qiyu/cpp/QiyuControllerBridge.{h,cpp}   # qiyuapi controller fallback for the OpenXR path
app/src/qiyu/res/layout/webxr_interstitial_controller.xml
```

Modified:
```
app/build.gradle                     # qiyu flavor, SDK AAR bundling, QIYU_CONTROLLER_BRIDGE
app/CMakeLists.txt                   # -DQIYU -DOPENXR, qiyu sources
app/proguard-rules.pro               # keep com.qiyi.** / com.qualcomm.** (required by SXR)
app/src/main/cpp/DeviceUtils.cpp     # Qiyu model detection incl. A091/A081/VR700
app/src/main/cpp/Device.h, DeviceType.java
app/src/main/cpp/DeviceDelegate.h    # EnterVR/LeaveVR/SetNativeWindow/IsInVRMode/... hooks
app/src/main/cpp/native-lib.cpp      # delegate selection, EndVR on pause/term, re-enter on window
app/src/openxr/cpp/*                 # Qiyu mappings, runtime-unavailable handling, refresh/MSAA/perf
AGENTS.md                            # on-device lessons ("do not repeat these")
```

## 4. Build & install

```powershell
# repo root, Windows
.\gradlew.bat assembleQiyuArm64GeckoGenericRelease
Copy-Item -Force app\build\outputs\apk\qiyuArm64GeckoGeneric\release\Wolvic-qiyu-arm64-gecko-generic-release.apk dist\Wolvic-qiyu-universal-release.apk
```

- Release APK is currently **debug-signed** (`userProperties.useDebugSigningOnRelease=true`) — sideload/QA only.
- `versionCode` is static (`userProperties.useStaticVersionCode=true`).
- Qiyu SDK path comes from `local.properties` → `qiyuSdkDir=F:/wolvic/third_party/qiyu` (`Include/` + `Lib/*release*.aar`).
- Pure-OpenXR fallback build: add `-PqiyuControllerBridge=false` (no SDK classes/libs).

## 5. Known issues / open work

1. **Qiyu X pointer aim** (highest priority). `qiyu_GetControllerData` gives grip pose, not the aim ray. `e05781c3` applies the same `trackingOffset` correction the head uses. Next step if still off-centre: derive the pointer from an arm/aim model the way the reference does (`arm_model::Controller` in the itk build; Wolvic already has `ElbowModel`, used by `noapi`/HVR3DoF).
2. **Button delivery not yet proven on device.** `e05781c3` logs `button/buttonTouch/trigger/grip/stick` on change. If those never change, read controller state from the `VRSDKCore`/`ash_reader` channel (`ControllerHandManager`, shmem pose/button/state zones) instead of `qiyu_GetControllerData`.
3. **MIX (A081)**: no OpenXR runtime broker in firmware; OpenXR must degrade (it does now). SXR also unusable without `libStereoRectifyWrapper.so`.
4. **Dream/VR700 OpenXR artifacts**: Monado's VR700 backend gives the app a 60×63° FOV while the panel is 82×92°, so SXR warp expands the image (`Frustum Display Width/Height Scale 1.369/1.452`) and starves (`No valid frame to warp`) — moving vertical bands / snowflake. Runtime side, not a Wolvic shader. Native SXR delegate avoids it but is only wired for Qiyu models that pass detection.
5. **`ANativeWindow_setFrameRate not found`** on VR700 firmware: our refresh request is compositor-side only; check `compositor_request_display_refresh_rate` in logcat before blaming frame pacing.

## 6. Debugging recipe

```powershell
adb logcat -c
# start app, exercise controller, then:
adb logcat -d | Select-String -Pattern "QIYU:|native QIYU|sxr |VRSDKCore|ash_reader|QvrSDK:ConCoreS|No valid frame to warp|Fatal signal|Abort message"
```

Milestones to look for on Qiyu X:
```
QIYU: native SDK 1.0.0
QIYU: qiyu_Init=1 controller_init=1
QIYU: eye target 1920x1920
QIYU: eye buffers 1920x1920
QIYU: qiyu_StartVR=1 (window=0x..., eye 1920x1920)
QIYU: controller 1 connected battery=...
sxr : FPS: 71.8x
```

## 7. Hard rules learned on device

- **Never** strip `com.qualcomm.**` / `com.qiyi.**` from R8: `sxrInitialize()` resolves `com/qualcomm/sxrapi/SxrApi` via `FindClass`; stripping it makes `qiyu_Init` fail and aborts the process.
- `qiyu_Init == 0` with `qiyu_IsControllerInit() == 1` is **usable** on Qiyu X (SXR stack already up from `VRSDKCore` JNI_OnLoad). Bailing gives a black screen.
- Follow `third_party/qiyu/Include/QiyuApi.h` ordering: `StartEye` after the eye FBO bind, `EndEye` after the eye, `StartVR` on START/RESUME/INIT_WINDOW, `EndVR` on PAUSE/TERM_WINDOW, `Update(dt)` before render.
- A stale SXR session (no `EndVR`) presents as a black screen on the next launch.

## 8. Reference material used

- Official SDK: `third_party/qiyu/Include/QiyuApi.h`, `Qiyu_UnityXR_SDK_v1.5.0.zip`, `Qiyu_Unreal_SDK_v1.5.0.zip` (under `F:\vr-sdks\qiyu`).
- Working reference clients: `F:\vr-sdks\qiyu\ref\Wolvic-qiyu-arm64-gecko-generic-debug-itk2026 9 29.apk.1`, `F:\vr-sdks\qiyu\ref\Firefox Reality 12.3.apk.1`.
- Comparison implementation: `F:\alvr-openxr-v20` (`alvr/client_openxr/src/qiyu_backend.cpp`).
- Logs analysed: `F:\wolvic\.log\log (3..8).log.txt` and `%TEMP%\BNZ.*\log.txt` captures.

## 9. Commit trail

```
e05781c3 Qiyu: apply tracking offset to controller poses and log raw input
ad24e384 docs: record Qiyu/SXR on-device lessons in AGENTS.md
62ed3102 Qiyu: native SXR device delegate (qiyuapi) with WebXR support
5aca9109 OpenXR: Qiyu input mappings and graceful runtime-unavailable handling
3f8039bc Qiyu: add platform flavor, device detection and controller bridge
```
