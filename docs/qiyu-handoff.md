# Qiyu (iQIYI / SXR) handoff — Wolvic

Status snapshot for whoever picks this up. Everything below is grounded in real
device logs and reverse-engineered reference artifacts, not guesses.

Branch: `main` (repo `lc700x/wolvic`), tip at the time of writing: `33d26d7b`.

---

## 1. What works today

| Area | State |
|---|---|
| Qiyu X (native SXR path) | **VR renders**: `qiyu_Init=1`, eye buffers `1920x1920`, `sxrBeginXr` OK, `qiyu_StartVR=1`, steady **71.8 fps**, no Monado, no warp starvation |
| WebXR | Routes through Gecko `ExternalVR` (VRManager IPC), so it does **not** need OpenXR. The Qiyu flavor speaks the stock GeckoView protocol v19 (`moz_external_vr_v19.h`), so immersive sessions are advertised without a patched GeckoView build |
| Qiyu X controller | Detected (`controller 1 connected battery=100`), pose+haptics plumbed, controller entries created with the generic trigger model, official input mapping (`triggerForce`/`gripForce` > 0.5, `button` press bits), touch-as-click only after ~2 s without any analog/press value, beam transforms wired |
| Passthrough (VST) | **Blocked / toggle hidden.** Provider-only sets `AppVstStatus` but leaves the camera off (black); `QiyuXRCore` needs `sxrGetSensorHeadOffset`, missing from the old `libsxrapi.so`, and crashes; the newer 5 MB `libsxrapi.so` black-screens the native renderer. Needs a full migration to the newer `QVR_*` stack |
| Qiyu MIX (A081) | App no longer crash-loops; firmware has **no OpenXR runtime broker** and lacks `libStereoRectifyWrapper.so`, so VR is not usable there yet |
| Qiyu Dream/VR700 (A091) | OpenXR path works structurally (Monado `QIYIVR`), but shows runtime-side warp artifacts (see §5) |

## 2. Architecture (why two paths exist)

- **OpenXR path** (`app/src/openxr/cpp/`): Monado (or vendor runtime) drives display; `khr/simple_controller` is the only profile Qiyu advertises, so `QiyuControllerBridge` supplements input via `libqiyuapi`.
- **Native SXR path** (`app/src/qiyu/cpp/`): `DeviceDelegateQiyu` talks to `libqiyuapi.so` directly and submits to the SXR compositor (`qiyu_StartVR` / `StartEye` / `EndEye` / `SubmitFrame`). This is what the working reference client does and what makes Qiyu X smooth.
- Selection happens in `app/src/main/cpp/native-lib.cpp`: known Qiyu model → `DeviceDelegateQiyu`, otherwise `DeviceDelegateOpenXR`.
- **WebXR protocol:** Qiyu links the stock Maven GeckoView 153 (`kVRExternalVersion = 19`, `SHMEM_VERSION "0.0.12"`). `app/CMakeLists.txt` defines `WOLVIC_GECKOVIEW_EXTERNAL_VR_19` for the Qiyu flavor, `moz_external_vr.h` selects `moz_external_vr_v19.h`, and `ExternalVR.cpp` maps Wolvic's richer API onto that layout. Other flavors keep `moz_external_vr_v21.h` and a GeckoView built from `Igalia/wolvic-gecko-patches`. Gecko silently discards a shmem whose version/size differ, which shows up as `isSessionSupported('immersive-vr') === false` while inline WebXR still works.

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
- Keep `sxrApi-v8a-release.aar`; do not replace it with the newer `svrApi-release.aar` from `Qiyu_UnityXR_SDK_v1.6.1.zip`. That newer `libsxrapi.so` starts SXR at ~72 fps but the native `qiyuapi` path renders a black scene on Qiyu X.
- Passthrough is disabled for now. `QiyuXRCore.aar` must not be used with the old SXR lib: `QVR_EnableSeeThrough` initializes `SxrPlugin`, fails to resolve `sxrGetSensorHeadOffset`, and SIGSEGVs. Provider status alone leaves `RgbCameraStatus=0` and black-screens the scene.
- Pure-OpenXR fallback build: add `-PqiyuControllerBridge=false` (no SDK classes/libs).

## 5. Known issues / open work

1. **Qiyu X pointer aim / 6DoF pose** (highest priority). Poses must be used exactly as the official native sample does (`QiyuVR_NativeActivity/app/jni/QYModel.cpp`): raw `translate(pos) * quat(w,x,y,z)`, with `y -= trackingOffset` only in `TM_Ground` mode (`qiyu_GetTrackingOriginMode()`), and no axis remapping of any kind (the SDK space is right-handed Y-up / -Z-forward, same as Wolvic). The `(x,-y,-z)` conversion used earlier mirrored yaw/pitch ("point left, beam goes right") and moved controllers above/behind the head. `CreateController` is still required or every `ControllerDelegate` call is a no-op (no model, no beam). `debug.wolvic.qiyu.aim "pitch,yaw,roll"` remains available for a genuine grip-to-aim tilt (identity by default).
2. **Button delivery is partial on some Qiyu X firmware.** `e05781c3` logs `button/buttonTouch/trigger/grip/stick` on change; device logs show `touch` bits changing while `button` and `triggerForce` stay 0 (only `gripForce` reaches 1.00). The official mapping is `triggerForce`/`gripForce > 0.5` plus `button` press bits; touch-as-click now engages only after ~2 s with no analog/press value and is disabled once real input appears. If a button is still unusable, read controller state from the `VRSDKCore`/`ash_reader` channel (`ControllerHandManager`, shmem pose/button/state zones) instead of `qiyu_GetControllerData`.
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
- Controller thresholds follow the official sample: `triggerForce > 0.5f`, `gripForce > 0.5f`; anything lower is touch-sensitive.
- Passthrough is vendor VST, not OpenXR, and is currently blocked: provider-only leaves the camera off; `QiyuXRCore` crashes on the missing `sxrGetSensorHeadOffset`; the newer `libsxrapi.so` black-screens the renderer. The toggle is hidden until the renderer is migrated to the newer `QVR_*` stack.

## 8. Reference material used

- Official SDK: `third_party/qiyu/Include/QiyuApi.h`, `Qiyu_UnityXR_SDK_v1.5.0.zip`, `Qiyu_Unreal_SDK_v1.5.0.zip` (under `F:\vr-sdks\qiyu`).
- Working reference clients: `F:\vr-sdks\qiyu\ref\Wolvic-qiyu-arm64-gecko-generic-debug-itk2026 9 29.apk.1`, `F:\vr-sdks\qiyu\ref\Firefox Reality 12.3.apk.1`.
- Comparison implementation: `F:\alvr-openxr-v20` (`alvr/client_openxr/src/qiyu_backend.cpp`).
- Logs analysed: `F:\wolvic\.log\log (3..8).log.txt` and `%TEMP%\BNZ.*\log.txt` captures.

## 9. Commit trail

```
33d26d7b Qiyu: create controller entries, set beam transforms
19fb6034 Qiyu: fix controller pose frame, click fallback, and render mode
e05781c3 Qiyu: apply tracking offset to controller poses and log raw input
ad24e384 docs: record Qiyu/SXR on-device lessons in AGENTS.md
62ed3102 Qiyu: native SXR device delegate (qiyuapi) with WebXR support
5aca9109 OpenXR: Qiyu input mappings and graceful runtime-unavailable handling
3f8039bc Qiyu: add platform flavor, device detection and controller bridge
```
