---
name: qiyu-openxr-compat
description: Make Android VR / OpenXR apps work on iQIYI (Qiyu) headsets — Qiyu X, Qiyu 3, Dream, Dream Pro, MIX (A081) and Lenovo Legion VR700 (A091). Use when an app launches but renders wrong, shows a black screen, only gets 3DoF, places controllers above/behind the head, points the ray the wrong way, or cannot read buttons on a Qiyu device. Covers the native SXR (qiyuapi) path vs the OpenXR runtime, exact pose/6DoF handling, controller and button fallbacks, aim rays, firmware quirks, packaging/R8 rules and on-device verification.
---

# Qiyu (iQIYI) VR compatibility skill

Distilled from bringing Wolvic up on Qiyu hardware against the official
`Qiyu_Native_SDK_v1.0.0` (its `QiyuVR_NativeActivity` sample is the ground truth),
Firefox Reality 12.3 and the Qiyu Unity/Unreal SDKs.

## 0. Golden rules (read first)

1. **Never invent axis flips.** `qiyu_*` poses are right-handed, Y-up, -Z-forward,
   the same convention as OpenGL/OpenXR. A `(x,-y,-z)` "fix" mirrors yaw/pitch
   (point left, beam goes right) and puts controllers above/behind the head.
2. **Camera = SDK view matrices, controllers = raw poses.** The official sample
   renders with `qiyu_GetViewMatrix`; invert both eye matrices for the head/eye
   transforms and keep controllers on their raw `qiyu_GetControllerData` poses.
   Never mix a view-derived head position with the `qiyu_PredictHeadPose`
   rotation (or use the predicted pose as the camera with raw controllers) —
   that makes the world look glued to the head ("0DoF").
3. **Ground vs device mode is a real branch.** `TM_Ground` subtracts
   `trackingOffset` from Y; `TM_Device` must use the pose unchanged. Apply the
   same rule to head and controllers.
4. **6DoF is SXR's `sxrSetTrackingMode`, not `qiyu_SetTrackingOriginMode`.**
   `qiyu_StartVR` already calls `sxrSetTrackingMode(3)`; verify it and log it.
5. **Don't bail on `qiyu_Init() == false`** if `qiyu_IsControllerInit() == true`:
   the SXR stack is already up in-process on Qiyu X.
6. **Never strip `com.qualcomm.**` / `com.qiyi.**`** from ProGuard/R8; SXR
   resolves `com/qualcomm/sxrapi/SxrApi` with JNI `FindClass`.
7. **Create your controller objects/actions before the first update.** Gating
   them on a later "connected" event means every per-frame update is a no-op.
8. **Handle the firmware input gap**: Qiyu X fills touch bits but can leave
   press bits and analog trigger at zero (grip reaches 1.0). Fall back to
   touch-as-click or read the vendor controller service.
9. **Aim = the controller pose's -Z axis.** No separate aim pose exists; a fixed
   pitch offset may be needed, make it tunable.
10. **MIX (A081) must degrade, not crash** (no OpenXR broker, missing
    `libStereoRectifyWrapper.so`). **VR700 (A091) on Monado shows double-warp**
    due to an FOV mismatch; the native SXR path avoids it.

## 1. Device / runtime matrix

| Device | Model hints | OpenXR runtime | Native SXR (qiyuapi) | Notes |
|---|---|---|---|---|
| Qiyu X | `Qiyu X`, iQIYI | Monado `QIYIVR` in-process, `khr/simple_controller` only; grip action inactive, aim pose works | **Recommended**: direct SXR compositor, ~72 fps @1920×1920 | Best path: native SXR; WebXR still works through the engine bridge |
| Qiyu 3 | `Qiyu3` | vendor/OpenXR | works | |
| Dream / Dream Pro | `A091`, `iQIYI-A6*` | Monado `QIYIVR` | works | OXR path can double-warp |
| Lenovo Legion VR700 | `VR700` | Monado `QIYIVR` | works | App FOV 60×63° vs panel 82×92° → SXR warp scale 1.369/1.452, `No valid frame to warp`, moving bands/snow |
| MIX (A081) | `A081` | **none** — no `org.khronos.openxr.runtime_broker`; `xrCreateInstance` → `XR_ERROR_RUNTIME_UNAVAILABLE` | SXR libs unusable (no `libStereoRectifyWrapper.so`) | Must fail gracefully, no crash loop |

## 2. Choose the rendering path

```
Is the target Qiyu X / Dream / VR700?
├─ Does your engine/backend have a native SXR compositor path (qiyuapi)? → YES
│   Use it. It avoids Monado, gives correct FOV and frame pacing.
└─ NO → OpenXR
    ├─ Create the instance; if XR_ERROR_RUNTIME_UNAVAILABLE (MIX) → run 2D / exit cleanly
    ├─ Bind khr/simple_controller; treat the aim pose as the controller pose
    └─ Expect the VR700 double-warp; if it appears, switch to the native SXR path
```

Important: **WebXR does not require an OpenXR runtime.** If your app is a Gecko-
or Chromium-based browser, WebXR is driven through the engine's VR bridge
(e.g. Gecko `ExternalVR` / VRManager IPC), so the native SXR compositor path can
serve both the browser UI and WebXR.

## 3. Packaging (Android, native SXR path)

- Bundle the vendor AARs from the Qiyu SDK: `qiyuapi`, `qiyivrsdkcore`, `sxrApi`
  (arm64-v8a).
- Load the vendor libs **before** the native activity / before SXR is initialized,
  e.g. in your `PlatformActivity`:
  ```java
  System.loadLibrary("qiyivrsdkcore");
  System.loadLibrary("sxrapi");
  System.loadLibrary("ashreader");
  System.loadLibrary("qiyuapi");
  ```
  This makes the app classloader own `libsxrapi.so` first (important on the
  Monado/VR700 runtime) and brings up `VRSDKCore` before `qiyu_Init`.
- Keep the vendor Java classes:
  ```
  -keep class com.qualcomm.** { *; }
  -keep class com.qiyi.** { *; }
  ```
  Without this `sxrInitialize()` cannot resolve `com/qualcomm/sxrapi/SxrApi`,
  `qiyu_Init` returns 0, and a pending `ClassNotFoundException` aborts the process.
- Manifest: keep `WAKE_LOCK` with `tools:node="replace"` (dropping it crashes
  `OffscreenDisplay` / `Presentation` on launch).
- `minSdk` 26+, `arm64-v8a`; the vendor libs are 64-bit only.

## 4. Native SXR lifecycle (exact order)

```cpp
qiyu_Init(activity, vm, GA_OpenGLES, TM_Ground, /*verbose=*/false);
const bool ok = ...;
const bool controllerUp = qiyu_IsControllerInit();
if (!ok && !controllerUp) -> no SXR; fall back to OpenXR/2D.
```

Per frame / lifecycle:

```
StartVR(window, PL_System, PL_System)   // APP_CMD_START/RESUME/INIT_WINDOW
  for each eye:
      StartEye(multiview, eye, TT_Texture)   // AFTER binding the eye FBO
      ...render...
      EndEye(multiview, eye, TT_Texture)
  SubmitFrame(frameParam)
Update(dt)                              // before rendering
EndVR()                                 // APP_CMD_PAUSE/TERM_WINDOW
```

- `StartEye` must run **after** the eye target is bound and **before** that eye is
  rendered; the ordering is not optional.
- A stale SXR session (missing `EndVR`) presents as a black screen on the next
  launch; always `EndVR` on pause/window teardown.

## 5. Poses and 6DoF (the part everyone gets wrong)

### 5.1 Raw pose, no conversion

The official `QYModel::DrawHandleLR` does:

```cpp
glm::fquat q(rot.w, rot.x, rot.y, rot.z);          // raw quaternion
glm::mat4 rotM = glm::mat4_cast(q);
float y = pos.y;
if (trackingMode == TM_Ground) y -= trackingOffset; // only in ground mode
glm::mat4 world = glm::translate(vec3(pos.x, y, pos.z)) * rotM;
```

Do exactly that, for the head and the controllers:

```cpp
matrix = Rotation(Quaternion(q.x, q.y, q.z, q.w));
matrix.TranslateInPlace(Vector(p.x, p.y, p.z));
if (mode == TM_Ground) matrix.TranslateInPlace(Vector(0, -trackingOffset, 0));
```

No negation, no swap, no `(x, -y, -z)`, no quaternion component remap.

### 5.2 Sanity check at runtime

At startup the head should be near `(0,0,0)` and a controller held in front
roughly `(±0.15, -0.20…-0.35, -0.20…-0.35)` — i.e. right/left, below and in front
of the head. Values like `(0.13, +0.35, +0.35)` mean Y/Z were negated.

### 5.3 Camera vs controllers: use the SDK view matrices for the camera

`qiyu_GetViewMatrix` is what the official sample renders with. Use it for the
whole camera:

- invert both eye view matrices,
- head position = midpoint of the two eye world positions,
- head rotation = the left eye's world rotation,
- eye transform = `eyeWorld * headWorld⁻¹` so `head * eye` reproduces the SDK
  eye pose.

Controllers stay on their raw `qiyu_GetControllerData` poses with the
ground-mode correction. This is exactly `matView * matWorld` in the sample.

`qiyu_PredictHeadPose` and the view matrix disagree on Qiyu X (the view head is
a point reflection of the raw pose):

```
raw  = (-0.05, 0.31,  0.64)
view = ( 0.05, 1.28, -0.64)     // = (-x, 1.59 - y, -z)
```

Do not mix them — view position with predicted rotation, or predicted head with
raw controllers. Those mixed frames produced the "everything follows the head /
0DoF" and misplaced-controller symptoms during bring-up.

### 5.4 Ground vs device mode

`qiyu_GetTrackingOriginMode()`:

- `TM_Ground` → subtract `trackingOffset` from the Y of **head and controllers**.
- `TM_Device` → use the pose unchanged.

The offset is obtained from `qiyu_GetViewMatrix(..., outTrackingOffset, ...)`
and, despite the header comment, is used as `pos.y - trackingOffset`.

### 5.5 6DoF (positional) tracking

`qiyu_SetTrackingOriginMode` only stores an SDK-side enum (it decides whether
the view matrix subtracts the offset); it does **not** enable positional
tracking. Positional tracking is SXR:

```cpp
// libsxrapi.so exports the native C API
int  (*sxrGetSupportedTrackingModes)();   // bitmask: 1 = rotational, 2 = positional
int  (*sxrGetTrackingMode)();
int  (*sxrSetTrackingMode)(int);          // 3 = positional with rotational fallback

sxrSetTrackingMode(3);                    // qiyu_StartVR already does this
```

Expected logs on Qiyu X:

```
sxr: QVR Service supports rotational tracking
sxr: QVR Service supports positional tracking
sxr: sxrSetTrackingMode : Using application defined tracking mode (3).
sxr: sxrSetTrackingMode : Setting tracking mode to positional
VRSDKCore: ControllerHandManager Init isGlobal3Dof:0
Fusion: ... Mode[6] 6Dof[1]   (per hand, continuous)
```

If `sxrGetTrackingMode()` returns 1 or capability lacks bit 2, it is the
runtime/hardware, not your pose math.

## 6. Controllers

- `qiyu_GetControllerData(&left, &right)` → index 0 = left, 1 = right.
- Pose is the controller's pointing pose (no separate aim pose). The ray is the
  pose's **-Z** axis.
- Register/create both controllers/actions at startup, even if only one is
  connected yet, and disable them until `isConnect`; otherwise per-frame state
  is silently dropped.
- `isConnect` can report a controller whose pose is stale; treat an unchanging
  pose with zero velocity as "not actually tracked".
- Handedness: index 0 → left, 1 → right; forward/backward mapping must match the
  physical hand. Do not derive handedness from X sign.

### 6.1 Buttons on Qiyu X firmware

`qiyu_ButtonType`: `BT_Trigger 0x01`, `BT_Grip 0x02`, `BT_A_X 0x10`,
`BT_B_Y 0x20`, `BT_Home_Menu 0x40`, `BT_JoyStick 0x80`.

Observed on Qiyu X: `buttonTouch` changes (`0x1`, `0x10`, `0x30`, …) while
`button` stays `0x0` and `triggerForce` stays `0.00`; `gripForce` does reach
`1.00`. The controller service itself does have the data (its log shows
`getButton click:… trigger:N`).

Practical fallback: when no analog trigger value was ever delivered, treat
"touched" as "pressed" for trigger/grip/face buttons, and log once when the
fallback engages. If the app truly needs press bits, read the vendor controller
service (`VRSDKCore` / `ash_reader`, or the SXR controller API) instead of
`qiyu_GetControllerData`.

### 6.2 Aim / laser beam

- Draw the beam from the controller pose along -Z.
- A fixed grip→aim pitch often helps. With the controller held level the reported
  forward can read ~`(0, 0.57, -0.82)` (≈35° up); a −35° pitch offset aligns the
  beam with where the user points. Keep it tunable at runtime, e.g.
  `debug.wolvic.qiyu.aim "pitch,yaw,roll"` (degrees), re-read once a second,
  applied to the beam/target ray only.
- Roll/position of the origin: put the ray origin at the controller pose; the
  vendor Unity toolkit offsets it by ~(0, −0.045, 0.015) m only.

## 7. OpenXR path notes (when you cannot use native SXR)

- Qiyu runtimes expose only `khr/simple_controller`; the **grip action may report
  inactive while `xrLocateSpace` tracks**. Trust the location flags instead of
  the action `isActive` (ALVR does exactly this for Qiyu).
- The aim pose works and follows the hand; use it as the pointer.
- `XR_ERROR_RUNTIME_UNAVAILABLE` on MIX (A081): catch it and degrade to 2D or a
  clear error. Do not attempt `xrCreateInstance` in a loop and abort.
- Dream/VR700: Monado's Qiyu backend gives a 60×63° app FOV against an 82×92°
  panel, so SXR warp expands the image
  (`Frustum Display Width/Height Scale 1.369/1.452`) and starves
  (`No valid frame to warp`) → moving vertical bands/snow. Runtime-side
  double-warp; a native SXR compositor path is the fix, not a shader change.
- `ANativeWindow_setFrameRate` is not exported on VR700 firmware; the refresh
  request is compositor-side only (check `compositor_request_display_refresh_rate`).

## 8. Verification checklist

1. `qiyu_Init=1` (or `init=0 && controller_init=1`), eye target size logged.
2. `qiyu_StartVR=1`, eyes `StartEye/EndEye` balanced, steady FPS (~72 on Qiyu X).
3. `sxrSetTrackingMode` logs "positional"; `Fusion … 6Dof[1]`.
4. Head raw pose changes when you walk/lean; scene has parallax (the UI does not
   follow your head).
5. Controller at `(±0.15, -0.25, -0.30)`-ish, below/in front, on the correct side.
6. Beam points where the controller points; tune pitch if it sits high.
7. Trigger/squeeze/face buttons click; check the touch-as-click fallback log.
8. Pause/resume re-enters VR; no black screen on the next launch (EndVR ran).
9. MIX: app exits/degrade cleanly. VR700: no bands/snow on the native path.

Useful logcat filter:

```
adb logcat -c && adb logcat | findstr /C:"QIYU:" /C:"native QIYU" /C:"sxr " ^
  /C:"VRSDKCore" /C:"ash_reader" /C:"QvrSDK:ConCoreS" /C:"No valid frame to warp" ^
  /C:"Fatal signal" /C:"Abort message"
```

## 9. Anti-patterns (these all caused real regressions)

| Anti-pattern | Symptom |
|---|---|
| `(x,-y,-z)` / quaternion component swaps | Ray points opposite; controllers above/behind the head |
| Mixing `GetViewMatrix` head with raw controller poses | World glued to the head ("0DoF") |
| Always subtracting `trackingOffset` | Wrong offset in `TM_Device`; head/controllers displaced |
| Treating `qiyu_SetTrackingOriginMode` as 6DoF switch | Head stays 3DoF |
| Bailing on `qiyu_Init()==0` | Black screen then exit on Qiyu X |
| Stripping `com.qualcomm.**` / `com.qiyi.**` | `SxrApi` `ClassNotFoundException` → SIGABRT |
| Creating controllers only after `isConnect` | No model/beam, no input, "controller doesn't work" |
| Missing `EndVR` | Black screen on the next launch |
| Assuming analog trigger exists | Nothing is clickable |

## 10. Reference material

- Official native SDK + sample (authoritative):
  `Qiyu_Native_SDK_v1.0.0/Samples/QiyuVR_NativeActivity/app/jni/QYModel.cpp`
  (`DrawHandleLR`), `QiyuVR_NativeActivity.cpp` (`QYApp_PreRender`), headers in
  `QiyuNativeSDK/Include/QiyuApi.h`.
- Unity SDK: `Qiyu_UnityXR_SDK_v1.5.0.zip` (`QiyuVisualBase.cs` ray attach;
  poses come through Unity XR input).
- Unreal SDK: `Qiyu_Unreal_SDK_v1.5.0.zip` (`QIYUVRInput.cpp`,
  `UnityToUnreal` remap for Unreal only).
- Working reference clients: Firefox Reality 12.3, Wolvic Qiyu builds.
- Comparison implementation: ALVR `qiyu_backend.cpp` / `interaction.rs`
  (trusts `xrLocateSpace` flags, raw poses, identity grip offset).
- Wolvic implementation: `app/src/qiyu/cpp/DeviceDelegateQiyu.cpp`,
  `QiyuControllerBridge.cpp`, `QiyuAPI.h`; `docs/qiyu-handoff.md`.
