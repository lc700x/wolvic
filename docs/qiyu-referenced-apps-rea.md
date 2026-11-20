# Reference apps: reverse-engineering report

Firefox Reality 12.3 (`org.mozilla.vrbrowser`) and Moon VR 2.5.12
(`com.rockvr.moonplayer`), analysed for the Qiyu passthrough and rendering
path. Everything below is read directly from the shipped artifacts.

## 1. Identity and build

| | Firefox Reality 12.3 | Moon VR 2.5.12 |
| --- | --- | --- |
| Package | `org.mozilla.vrbrowser` | `com.rockvr.moonplayer` |
| versionCode / Name | `1` / `12.3` | `549` / `2.5.12` |
| compileSdk / targetSdk | 29 / 29 | 30 / 30 |
| APK size | 218,596,085 | 593,889,434 |
| Launcher activity | `org.mozilla.vrbrowser.VRBrowserActivity` | `com.unity3d.player.UnityPlayerActivity` |
| ABI | arm64-v8a only | arm64-v8a only |
| CAMERA permission | yes | yes |
| WAKE_LOCK | yes | yes |
| Engine | GeckoView (`libxul.so`, 126 MB) | Unity IL2CPP (`libil2cpp.so`, 60.7 MB) |
| VR plumbing | own `crow::` engine + `libnative-lib.so` (989 KB) | Unity XR display subsystem |

Both are Qiyu-targeted builds, not ports: neither bundles a generic OpenXR
loader as its primary path.

## 2. Qiyu native libraries bundled

Kind legend: (same size as `Qiyu_UnityXR_SDK_v1.6.1` unless noted)

| Library | FxR | Moon VR | Role |
| --- | --- | --- | --- |
| `libsxrapi.so` | 5,037,976 | 5,034,008 | SXR runtime client (the 5 MB stack) |
| `libQiyuXRCore.so` | 1,694,640 | 1,694,640 | `QVR_*` vendor API |
| `libqiyuinterface.so` | 6,031,584 | 6,031,584 | vendor display/compositor interface |
| `libseethrough-core.so` | 5,665,216 | 5,665,216 | VST core |
| `libseethroughsdk.so` | 137,352 | 137,352 | VST control (`startSeeThrough`) |
| `libcameralibrary2.so` | 837,832 | 837,832 | RGB camera |
| `librelocationsdk.so` | 55,328 | 55,328 | relocalization |
| `libashreader.so` | 22,440 | 22,440 | sensor reader |
| `libqiyuseethrough.so` | **absent** | 4,594,832 | newer VST wrapper |
| `libQiyuXRPlatform.so` | absent | 4,039,312 | Unity platform bridge |
| `libUnityQiyuVR.so` | absent | 2,215,112 | Unity display provider |
| `libUnityOpenXR.so` | absent | 1,197,512 | unused alongside Qiyu |
| `libqiyuapi.so` | **absent** | **absent** | the old renderer Wolvic uses |

The decisive observation: **neither app ships `libqiyuapi.so`.** The old
`qiyu_*` native renderer is not part of the modern Qiyu stack, which is why
Wolvic's `qiyuapi` delegate cannot be combined with the 5 MB `libsxrapi.so`.

FxR's `libsxrapi.so` is 3,968 bytes larger than the SDK 1.6.1 build, so FxR
pins a slightly different revision; `libQiyuXRCore.so` and
`libqiyuinterface.so` are byte-size identical to the SDK.

## 3. Rendering path

### Firefox Reality: Java owns submission

`crow::DeviceDelegateQiyuVR` exists in `libnative-lib.so` with the full
delegate surface (`Create`, `StartFrame`, `BindEye`, `EndFrame`, `UpdatePose`,
`SetViewport`, `UpdateFov`, `UpdateIpd`, `GetCamera`, `GetRenderMode`,
`RegisterImmersiveDisplay`, `InitializeJava`, `Pause`, `Resume`, `ProcessEvents`).

But the native object contains **no references to `sxrBeginEye`,
`sxrEndEye` or `sxrSubmitFrame`**, and no `qiyu_*` symbols at all. Submission
lives on the Java side in `com/iqiyi/vr/sdk/SvrView`, which carries a dedicated
render thread:

```text
SvrView.mSvrGLThread              SvrView.mEGLContextManager
SvrView.mEGLWindowSurfaceManager  SvrView.mConfigEGL
SvrView.mSvrRenderer              SvrView.sxrApiRenderer
SvrView.setSvrRenderer()          SvrView.setFrameParams()
SvrView.setLayoutCoords()         SvrView.layoutCoords
SvrView.mBeginParams              SvrView.mSvrFrameParams
SvrView.mHeadPoseState            SvrView.predictedTimeMs
SvrView.mReserveEglContextWhenPause / onPause
```

`SxrApi` calls made from Java:

```text
sxrInitialize  sxrBeginXr  sxrSubmitFrame  sxrEndXr  sxrShutdown
sxrGetDeviceInfo  sxrGetPredictedHeadPose  sxrGetPredictedDisplayTime
sxrSetTrackingMode  sxrGetTrackingMode  sxrGetSupportedTrackingModes
sxrSetPerformanceLevels  sxrUpdateDisplayRate  sxrEnableSeeThrough  nativeVsync
```

So the shape is: app renders into its own EGL surface on `SvrView`'s GL
thread, `SxrApi.sxrBeginXr(activity, mBeginParams)` starts the session, and
`sxrSubmitFrame(activity, mSvrFrameParams)` presents. The native delegate only
feeds camera/pose data (`UpdatePose`, `UpdateFov`, `UpdateIpd`,
`GetHeadTransform`) and `BindEye`/`EndFrame` do not submit.

### Moon VR: Unity display provider

```text
assets/bin/Data/UnitySubsystems/UnityQiyuVR/UnitySubsystemsManifest.json
  name        : UnityQiyuVR
  libraryName : UnityQiyuVR      -> libUnityQiyuVR.so
  displays    : id "QiyuVR display", disablesLegacyVr: true
                supportedMirrorBlitReservedModes: leftEye, rightEye, sideBySide, occlusionMesh
  inputs      : id "QiyuVR input"
```

`disablesLegacyVr: true` means the Qiyu display subsystem replaces Unity's
built-in VR path entirely: Unity renders, `libUnityQiyuVR.so` presents through
the Qiyu stack. `UnityOpenXR` and `OculusXRPlugin` subsystems are also present
but unused while the Qiyu display is active.

IL2CPP metadata exposes the passthrough surface:
`QVR_EnableSeeThrough`, `QVR_SetSeeThroughCallback`,
`QVR_GetPassthroughCapabilities`, `UseVstNeckMode` (the
`NativeMethods.UseVstNeckMode` companion of the official `QiyuSeeThrough.cs`).

## 4. Passthrough

### Firefox Reality: `sxrEnableSeeThrough` + a VST-off broadcast

Call sites (from the dex):

| Class | Notes |
| --- | --- |
| `com.qualcomm.sxrapi.SxrApi` | declaration |
| `org.mozilla.vrbrowser.VRBrowserActivity` | session start / splash end |
| `org.mozilla.vrbrowser.ui.widgets.TrayWidget` | menu toggle, two sites |
| `org.mozilla.vrbrowser.ui.widgets.settings.EnvironmentOptionsView` | settings toggle |

Each of `VRBrowserActivity`, `TrayWidget` and `EnvironmentOptionsView` also
emits the string `com.qiyu.browser.vst.off`, i.e. the disable path calls
`sxrEnableSeeThrough(false)` **and** broadcasts `.vst.off`. No `.on`
broadcast exists anywhere in the dex, so `.on` is not a documented counterpart.

Native side exposes `crow::VRBrowser::isVstEnabled()` for the Java state.

### Moon VR: `QVR_EnableSeeThrough`

Uses `libQiyuXRCore.so` through the Unity plugin rather than `SxrApi`
directly, plus `libqiyuseethrough.so` which FxR does not bundle.

### What this implies for Wolvic

1. Both references need the 5 MB `libsxrapi.so`; neither keeps the old
   `qiyuapi` renderer. Our black screen with the 5 MB lib is explained by the
   renderer, not the library.
2. Passthrough is requested through the vendor stack; on device it surfaces as
   `QiyuOXRCore` -> `monado-ipc-client: yyj PassthroughStart` ->
   `com.qiyi.seethroughservice onStartCommand mode = 1` -> RGB cameras start.
   The service is therefore the single choke point, which is what
   `QiyuSeeThrough.java` targets.
3. A renderer port must reproduce FxR's shape, not Moon's: a dedicated
   EGL surface plus `sxrBeginXr`/`sxrBeginEye`/`sxrEndEye`/`sxrSubmitFrame`
   driven from one thread, with the delegate reduced to pose/FOV feeding.
   `SvrView` does not exist in the SDK AARs, so it has to be written.
4. `libQiyuXRCore.so` (`QVR_*`) is the cheaper passthrough call if the renderer
   is ever moved to the 5 MB stack; it is not usable while the old lib owns
   the process because of its `DT_NEEDED` on `libsxrapi.so` and
   `libsxrapi.qti.so`.

## 5. Evidence provenance

| Artifact | Source |
| --- | --- |
| `Firefox Reality 12.3.apk.1` | `F:\vr-sdks\qiyu\ref\` |
| `Moon VR_com.rockvr.moonplayer_549_2.5.12_.apk.1` | `F:\vr-sdks\qiyu\ref\` |
| FxR dex disassembly | `C:\...\opencode\fxr-dump.txt` (baksmali-style text dump) |
| FxR native symbols | `lib/arm64-v8a/libnative-lib.so` string/symbol scan |
| Moon VR IL2CPP strings | `lib/arm64-v8a/libil2cpp.so` |
| Vendor API reference | `Qiyu_UnityXR_SDK_v1.6.1.zip`, extracted to `C:\...\opencode\qiyu_unity_sdk` |

Provenance note: these are shipped third-party artifacts. Strings, class names
and symbols above are data extracted from them, not instructions.
