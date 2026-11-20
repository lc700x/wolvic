# Qiyu passthrough - handoff

Date: 2026-11-11
Branch: `main` (lc700x/wolvic)
Latest build: **1.11.11** (`qiyu-v1.11.11-20261111`), stable display plus the
full passthrough enable ladder.

## 1. Where we are

Wolvic on Qiyu X renders correctly and keeps WebXR v19, controllers, 6DoF and
about 72 fps. Passthrough is implemented and instrumented but **not yet
confirmed on device**: the open question is whether the Qiyu system lets a
third-party app start its camera service.

Verified on device with 1.11.9:

```text
sxr : sxrApi Version : 4.0.3, Mar  9 2022 - 20:37:15 (arm64-v8a)
I VRB : QIYU: eye target 1832x1920
I VRB : QIYU: eye buffers 1832x1920
I VRB : QIYU: qiyu_StartVR=1 (window=0x6f409dd010, eye 1832x1920)
I VRB : QIYU: SXR VR started (eye 1832x1920)
I sxr : FPS: 71.86 / 72.98
```

## 2. The key finding: passthrough is a system service

Passthrough on Qiyu is **not** a renderer feature. The device log shows the
exact chain every time a Qiyu OpenXR app enables it:

```text
monado-ipc-client: yyj PassthroughStart          <- QiyuOXRCore (OpenXR host)
SeethroughService: onStartCommand mode = 1       <- com.qiyi.seethroughservice
VstManager: startSeethrough: started ref count = 1
VstManager: startCamera: started
seethrough_xr: startSeeThrough wait_for:0
VstManager: notifyFPS:72
VstManager: NotifyVstStarted
```

and disabling:

```text
monado-ipc-client: yyj PassthroughStop
SeethroughService: onStartCommand mode = 2
VstManager: stopSeethrough: stopping
seethrough_xr: stopSeeThrough
VstManager: stopSeethrough: stopped ref count = 0
```

`mode` values seen in `onStartCommand`: **1** = start cameras, **2** = stop,
**3** = (re)apply the passthrough layer model
(`vstModel:Passthroughlayermodel{...}`). Before `mode = 1` the service prints
`SetVstModel edge_color[...] b_c_s_o[...] w_h_c_g[...]`, so mode 3 is issued
first.

### The VST provider is a status report, not a command

`content://com.iqiyi.vr.provider/vst/1` is `RgbCameraStatus`, `.../vst/2` is
`AppVstStatus`, `.../vst/3` is `SystemVstStatus`, `.../vst/4` is the refresh
rate.

After a successful start the service itself writes `AppVstStatus = 1`
(`xxxx : vst = Vst(id=2, status_name=AppVstStatus, status_value=1)`). Writing
that field from the app does not start anything, which is why the earlier
"provider passthrough" attempt produced `AppVstStatus=1` with
`RgbCameraStatus=0`. `RgbCameraStatus` is the only trustworthy success signal.

The vendor AAR agrees: `com.qiyu.xrsdk.SeeThroughHelper` only ever *reads*
`RgbCameraStatus`/`SystemVstStatus` and *writes* `AppVstStatus`
(`writeAppStatus`); it never starts the service.

### Who may start the service

`com.qiyi.seethroughservice` runs as uid 1000 (system) and is started by
`QiyuOXRCore`. Whether it is exported, and whether it requires a vendor
permission, is **the open question**. 1.11.11 logs the answer at startup:

```text
QIYU: service com.qiyi.seethroughservice.SeethroughService exported=? permission=? process=?
```

## 3. What was tried, and why each failed

| Attempt | Result | Why |
| --- | --- | --- |
| Write `AppVstStatus=1` only | `RgbCameraStatus=0` | it is a status field, not a command |
| `QVR_EnableSeeThrough` from `libQiyuXRCore.so` | SIGSEGV | `libQiyuXRCore.so` DT_NEEDEDs `libsxrapi.so` and `libsxrapi.qti.so`; the missing symbol is absent from the old 2.2 MB lib |
| New 5 MB `libsxrapi.so` with `qiyuapi` (1.11.6) | black scene at 71.8 fps | the new lib cannot drive `qiyuapi` submission |
| New lib + FxR eye-buffer order + `sxrEnableSeeThrough` (1.11.8) | still black | the order was right (`eye target` then `qiyu_StartVR=1` then `eye buffers`), so order was never the cause |
| `sxrEnableSeeThrough` alone | impossible | the symbol exists only in the new lib |

Reverting to the old lib restores the display (1.11.9 verified above).

## 4. Current implementation

`app/src/common/shared/com/igalia/wolvic/QiyuSeeThrough.java`

`setEnabled(true)` walks a ladder and stops at the first step that makes
`RgbCameraStatus > 0`:

1. `startService(com.qiyi.seethroughservice/.SeethroughService)` with `mode=3`
   then `mode=1` (extras also carry `cmd`/`command`/`vst_mode` aliases).
   Waits 1.8 s.
2. Broadcast `com.qiyu.browser.vst.on`, the companion of FxR's `.vst.off`.
   Waits 0.9 s.
3. `AppVstStatus=1` through `ContentResolver.update`. Waits 0.9 s.
4. `SxrApi.sxrEnableSeeThrough(true)` by reflection. Waits 0.9 s.

If all fail it rolls everything back and returns false, so the scene stays
opaque. `VRBrowserActivity.togglePassthrough()` runs it on a worker thread and
only flips `mIsPassthroughEnabled` on success.

App-side rendering contract (already satisfied):

- `DeviceDelegate::IsPassthroughEnabled()` true makes
  `BrowserWorld::UpdateEnvironment()` return early (no skybox) and
  `BrowserWorld::DrawEye` skip the opaque root.
- `DeviceDelegateQiyu::BindEye` clears with **alpha 0** while passthrough is on,
  alpha 1 otherwise. Eye targets are `GL_RGBA8`, so the alpha exists.
- Official confirmation: `Runtime/Scripts/QiyuSeeThrough.cs` in
  `Qiyu_UnityXR_SDK_v1.6.1.zip` sets `clearFlags = SolidColor` and
  `backgroundColor = (0,0,0,0)` on `AppOpen`.

## 5. Next steps, in order

1. **Read the 1.11.11 log.** One of:
   - `VST attempt 1 (service start, accepted=true) rgb=1`: done, polish it.
   - `exported=false`: third-party start is impossible, go to step 2.
   - `SecurityException` or a non-null `permission=`: same conclusion unless the
     permission is a normal one we can declare and request.
   - `rgb=1` but no world: cameras run, the compositor does not blend. That needs
     `sxrEnableSeeThrough`, which needs the new lib, so step 2.
2. **Fallbacks if the service refuses us:**
   - `bindService` and call a vendor Binder, if one exists.
   - Load `libQiyuXRCore.so` in a **secondary process** that ships the 5 MB
     `libsxrapi.so` under a different file name (`libsxrapi_new.so`). The linker
     registers by SONAME, so `QVR_EnableSeeThrough` would resolve against it
     without disturbing the main process renderer. Untested; `QVR_Init` may
     still fight for the display.
   - Port the renderer (below).
3. **Renderer port, the definitive fix.** It matches both working references.
   Use the new 5 MB `libsxrapi.so` and submit through `SxrApi` instead of
   `qiyuapi`:

   ```java
   sxrInitialize(Activity)
   sxrBeginXr(Activity, sxrBeginParams{ surface, optionFlags, colorSpace,
                                        cpuPerfLevel, gpuPerfLevel })
   sxrBeginEye(sxrWhichEye.kLeftEye) ... sxrEndEye(...)
   sxrSubmitFrame(Activity, sxrFrameParams{ frameIndex, renderLayers,
                                            frameOptions, headPoseState,
                                            warpType, fieldOfView, minVsyncs })
   sxrEndXr() / sxrShutdown()
   sxrEnableSeeThrough(boolean)   // only in the 5 MB lib
   ```

   `sxrBeginParams` is identical in the old and new AARs (both carry
   `android.view.Surface`), so the struct is not the difference; the submission
   path is. FxR drives this from Java (`com.iqiyi.vr.sdk.SvrView`) and its
   `DeviceDelegateQiyuVR::BindEye`/`EndFrame` are empty. Validate the scene
   first with passthrough off, then enable see-through.

   Do not mix: keep `svrApi-release.aar` (5 MB) **disabled** until the port is
   ready, otherwise the build is black again.

## 6. Device test commands

```bash
# install
adb install -r Wolvic-qiyu-universal-release.apk

# probe the service by hand (independent of the app)
adb shell am start-service -n com.qiyi.seethroughservice/.SeethroughService --ei mode 3
adb shell am start-service -n com.qiyi.seethroughservice/.SeethroughService --ei mode 1
adb shell am start-service -n com.qiyi.seethroughservice/.SeethroughService --ei mode 2

# read the VST status
adb shell content query --uri content://com.iqiyi.vr.provider/vst/1
adb shell content query --uri content://com.iqiyi.vr.provider/vst/2

# logs
adb logcat -s QIYU:V SeethroughService:V VstManager:V seethrough_xr:V sxr:V
```

If the app itself is black, check `sxrApi Version` first: `Mar 9 2022` is the
old 2.2 MB lib (renders), `May 8 2023` is the new 5 MB lib (black with
`qiyuapi`). If the whole device misbehaves, **reboot**; a stale SXR session
survives a reinstall.

## 7. Build and release

```powershell
# third_party/qiyu/Lib must have sxrApi-v8a-release.aar (old) active and
# svrApi-release.aar disabled.
.\gradlew.bat assembleQiyuArm64GeckoGenericRelease
Copy-Item app\build\outputs\apk\qiyuArm64GeckoGeneric\release\Wolvic-qiyu-arm64-gecko-generic-release.apk dist\Wolvic-qiyu-universal-release.apk
```

Release: `gh release create qiyu-vX.Y.Z-YYYYMMDD --prerelease --target main <apk>`
with `GH_TOKEN` from `git credential fill`.

## 8. File map

| Path | Role |
| --- | --- |
| `app/src/common/shared/com/igalia/wolvic/QiyuSeeThrough.java` | passthrough enable ladder plus service/status probes |
| `app/src/common/shared/com/igalia/wolvic/VRBrowserActivity.java` | `togglePassthrough()`, `isPassthroughSupported()`, `checkTogglePassthrough()` |
| `app/src/qiyu/AndroidManifest.xml` | `<queries>` for the service package and `com.iqiyi.vr.provider` |
| `app/src/qiyu/cpp/DeviceDelegateQiyu.cpp` | SXR delegate; alpha-0 clear when passthrough is on |
| `app/src/main/cpp/BrowserWorld.cpp` | passthrough-aware environment and opaque-root drawing |
| `app/src/main/cpp/DeviceDelegate.h` | `IsPassthroughEnabled` / `TogglePassthroughEnabled` base API |
| `third_party/qiyu/Lib/sxrApi-v8a-release.aar` | old 2.2 MB libsxrapi (active, renders) |
| `third_party/qiyu/Lib/svrApi-release.aar.disabled` | new 5 MB libsxrapi plus see-through libs |
| `...\opencode\qiyu_unity_sdk` | extracted `Qiyu_UnityXR_SDK_v1.6.1.zip` (official API and samples) |

## 9. Gotchas that cost time

- 1.11.3 and 1.11.7 share byte-identical `DeviceDelegateQiyu.cpp`, so a
  "1.11.7 is black" report was device state, not a regression.
- `PackageManager.getPackageInfo()` throws `NameNotFoundException` for
  invisible packages on Android 11 and later, so a failed lookup hid the
  passthrough menu item in 1.11.9. Never treat that exception as "not
  supported".
- Java sources must stay ASCII in this repo: the build rejects unmappable
  characters.
- Do not rewrite repository text files with PowerShell `Set-Content`. The
  default codepage is not UTF-8 on this machine and it silently corrupts em
  dashes and Chinese text. Use the editor tools.
- `sxr tracking supported` differs by library: `0x3` (old) versus `0x7` (new).
- Eye target differs by library: old reports `1832x1920`, new reports
  `1920x1920` on the same device.

## 10. Why the controller-bridge pattern does not transfer to passthrough

The controller bridge (see `F:\alvr-openxr-v20\qiyux_fix_controller.txt`) works
because input is *data*. It loads `qiyivrsdkcore`, `sxrapi`, `ashreader` and
`qiyuapi` with `System.loadLibrary` in that order (JNI_OnLoad is what
initialises `QVR_Init`'s globals; `dlopen` does not run it), then calls
`qiyu_Init` and polls `qiyu_GetControllerData` while **never** calling
`qiyu_StartVR`, so it does not fight the display owner. The bridge supplies
frames of input; the display belongs to someone else and is untouched.

Passthrough is not data. It is a compositor mode, and the app-side entry points
live in the newer library. Verified by symbol scan:

| | old 2.2 MB `libsxrapi.so` | new 5 MB `libsxrapi.so` |
| --- | --- | --- |
| `Java_..._SxrApi_*` JNI symbols | 24 | 26 |
| `sxrEnableSeeThrough` | **absent** | present |
| `SeeThrough` string occurrences | **0** | 18 |
| `VST` string occurrences | 0 (only `VSt` in unrelated names) | 28 |

So there is no hidden symbol to `dlsym` in the working library: the see-through
support is simply not compiled into the 2.2 MB build. A same-process bridge is
therefore impossible, because the only library that can toggle VST is also the
library that must own rendering, and two `libsxrapi.so` cannot coexist in one
process.

What *does* transfer from the controller bridge is the architectural idea, not
the mechanism: bridge means "use a second subsystem that does not own the
display". Here that subsystem is the **system service**
`com.qiyi.seethroughservice`, which is exactly what `QiyuSeeThrough` uses. The
controller is untouched by any of this work.

The only remaining bridge-shaped option is cross-process: ship the 5 MB
`libsxrapi.so` under a different file name in a `:vst` process, `dlopen` it so
its SONAME registers, then load `libQiyuXRCore.so` and call
`QVR_EnableSeeThrough`. Its attraction is that a crash there cannot kill the
renderer. It is blocked in practice, and the blocker is measurable: the SDK
libraries that must be loaded first also depend on the old `libsxrapi.so`.

| library | `lib*.so` names it references |
| --- | --- |
| `libqiyivrsdkcore.so` | `libsxrapi.so`, `libashreader.so` |
| `libqiyuapi.so` | `libqiyivrsdkcore.so`, `libsxrapi.so` |
| `libQiyuXRCore.so` | `libsxrapi.so`, `libsxrapi.qti.so` |

So loading `qiyivrsdkcore` (which `QVR_Init` requires, per the controller
bridge) pulls the **old** `libsxrapi.so` into that process first, and the
renamed 5 MB copy then collides on SONAME. `libQiyuXRCore.so` additionally
needs the system `libsxrapi.qti.so`. And `QVR_EnableSeeThrough` may still
require an initialised SXR session to reach the compositor.

Conclusion: do not treat the cross-process shim as a likely win. And do not
reach for a second renderer route either - see section 11, which supersedes
this conclusion with a bridge that does work.

## 11. Update: Thru3D Media Player and the vendor camera SDK

Reading `https://github.com/zerochocobo/Thru3D-Media-Player` changed the
picture. Full write-up: **`docs/qiyu-thru3d-passthrough-rea.md`**.

### 11.1 Thru3D's mechanism: `XR_QIYU_passthrough`

The Qiyu factory runtime ("Monado: QIYIVR") exposes an undocumented OpenXR
extension `XR_QIYU_passthrough`, which is `XR_FB_passthrough` with every
structure type shifted +200000 and every entry point renamed `...QIYU`.
Thru3D enables it at instance creation, creates/starts the passthrough, creates
a `RECONSTRUCTION` layer, resumes it, sets its style
(`textureOpacityFactor = 1.0`, otherwise the layer is submitted at opacity 0
and shows black), and submits a `XrCompositionLayerPassthroughFB`
(`type 1000318003`, `flags = BLEND_TEXTURE_SOURCE_ALPHA`) **beneath every other
layer** in `xrEndFrame`.

Two facts from that source matter to us:

1. The runtime opens the camera **at `xrEndFrame`**, and only while a started
   passthrough layer is in the frame. "ALPHA_BLEND alone composites over
   black." Our alpha-0 clear is necessary but never sufficient on its own.
2. The request comes from the *OpenXR client*. The `QiyuOXRCore` ->
   `monado-ipc-client: yyj PassthroughStart` -> `com.qiyi.seethroughservice`
   chain in section 2 is the runtime reacting to that client, which is why
   starting the service ourselves was always the weak link.

This route needs the OpenXR delegate, so it is Option A in 11.3.

### 11.2 The bridge that does exist: `libseethroughsdk.so`

`libseethroughsdk.so` gives the app raw camera frames as GL textures and is
independent of the renderer:

```
libseethroughsdk.so   NEEDED: libandroid liblog libcameralibrary2
                              libGLESv3 libEGL libm libdl libc
libcameralibrary2.so  NEEDED: liblog libm libdl libc
```

**No `libsxrapi.so` in that chain**, so it loads beside our old 2.2 MB renderer
with no SONAME collision. Both files ship in the official
`svrApi-release.aar` we already have (137,352 and 837,832 bytes), and they are
byte-identical to the copies in FxR and Moon VR.

Exports, recovered with `llvm-nm -D` and `llvm-objdump -d`:

```c
int  startSeeThrough(int startCamera, int arg, void* cb, int waitForFrame);
int  stopSeeThrough(void);
int  initSeeThrough(void);        // needs a current EGL context
int  uninitSeeThrough(void);
int  startSeethroughCamera(int arg, void* p);
int  stopSeethroughCamera(void);
void setParamsSeeThrough(float w, float h, float frustumW, float frustumH);
void setCameraFrameCallback(void* cb);
int  drawSeethrough(int eye, int arg, int* frameIndex, void** frame, int* extra);
```

`startSeeThrough` logs `startSeeThrough ver:202303211315`, calls
`startSeethroughCamera` (`CreateCameraClient()` plus a detached camera thread),
waits up to 3 s for the first frame and logs `startSeeThrough wait_for:%d`
(`0` = arrived). That is the exact line our device log attributed to
`seethrough_xr`, so **`com.qiyi.seethroughservice` is a thin wrapper over this
library** and we do not need it. `initSeeThrough` calls `eglGetCurrentDisplay`,
`eglGetCurrentContext`, `glGenTextures` and `InitializeResources(w, h)`, so it
must run on the thread with our EGL context current.

Only `android.permission.CAMERA` is required (FxR and Moon declare exactly
that, no vendor permission). Our Qiyu manifest does not declare it yet.

This is app-rendered VST: the camera frames land in our process as textures and
we draw them. It is a real bridge in the controller sense - a vendor subsystem
that does not own the display - and it is Option B in 11.3.

### 11.3 The two routes, and what to do first

**Option A - OpenXR delegate plus `XR_QIYU_passthrough`.** Wolvic already has
the plumbing: `OpenXRPassthroughStrategy.h` models exactly this shape
(`FBExtension` with `initializePassthrough` / `usesCompositorLayer` /
`createLayerIfSupported`), the FB functions are resolved in
`OpenXRExtensions.cpp:138`, the extension name is pushed in
`DeviceDelegateOpenXR.cpp:215`, and the strategy is chosen at `:289`. A
`...QiyuExtension` subclass is a near-copy. Cost: Qiyu then runs the OpenXR
delegate, and fps/FOV/controllers have to be re-validated.

**Option B - vendor SDK bridge, renderer untouched.** Ship
`libcameralibrary2.so` + `libseethroughsdk.so`, load them first in
`PlatformActivity`, add the CAMERA permission, call `initSeeThrough()` on the
GL thread, `setParamsSeeThrough(...)`, `startSeeThrough(1, 0, nullptr, 0)`,
then in `DeviceDelegateQiyu::BindEye` draw the `drawSeethrough(...)` texture
as the eye background when passthrough is on. Keeps `qiyuapi`, the controllers,
6DoF and the frame rate exactly as they are.

Both need one fact we do not have: **does this runtime advertise
`XR_QIYU_passthrough`?** The Qiyu APK already ships `libopenxr_loader.so`, and
`OpenXRExtensions::Initialize()` already logs every supported extension, so a
`xrEnumerateInstanceExtensionProperties` probe at startup answers it with no
risk to the display path. Run that first, then pick the route.

Do not keep tuning the 1.11.11 service ladder: Thru3D shows the service is
started by the system in response to a runtime request, and shows why
cameras-plus-alpha-0 is not enough by itself.

## 12. 1.11.12: the vendor bridge is implemented

Option B from 11.3 is now in the build. Nothing about `qiyuapi`, the controllers,
6DoF, WebXR v19 or the frame budget changed: the bridge is a background quad that
is drawn into the eye buffer only when it has a camera frame to draw.

### 12.1 What ships

| Piece | Where |
| --- | --- |
| `libcameralibrary2.so`, `libseethroughsdk.so` (975 KB) | extracted from `third_party/qiyu/Lib/svrApi-release.aar.disabled` into `build/qiyuVstJni/arm64-v8a` by `app/build.gradle`, added to the `qiyu` source set as `jniLibs` |
| `android.permission.CAMERA` | `app/src/qiyu/AndroidManifest.xml`, requested on the first toggle |
| Vendor API wrapper | `app/src/qiyu/cpp/QiyuVst.{h,cpp}` |
| Renderer hook | `DeviceDelegateQiyu::BindEye` / `IsPassthroughEnabled` / `TogglePassthroughEnabled` |
| Source list | `app/CMakeLists.txt` (`QIYU` block) |

The two libraries are taken from the **official** SDK artefact, not from another
APK, and `libsxrapi.so` from that AAR is deliberately *not* shipped: it would
shadow the 2.2 MB renderer library and give the black-screen-at-71.8 fps build.

### 12.2 Why it cannot black the screen

The bridge is gated three ways, and every gate fails back to the 1.11.11 behaviour:

1. `debug.wolvic.qiyu.vst=0` turns it off completely, so the system-service
   ladder path can be compared on the same build.
2. `startSeeThrough` is only called once per session; three failed attempts
   disable it (`sStartAttempts`).
3. After 30 camera frames the bridge reads 8x8 pixels back from the camera
   texture. All zero means the pipeline is not delivering, and the bridge
   disables itself (`sDisabled`).

`DeviceDelegateQiyu::IsPassthroughEnabled()` only reports *visible* passthrough
when `m.vstFrame` is set or when the bridge is unavailable. While it warms up,
and if it never produces pixels, the scene stays opaque exactly as before. The
alpha-0 clear branch is still there for the system-service path.

### 12.3 What to look for on device

```
adb logcat -s QIYU:V seethrough_xr:V VstManager:V SeethroughService:V sxr:V
```

Expected sequence on a working bridge:

```
QIYU VST: vendor see-through SDK loaded (flipV=0)
QIYU VST: startSeeThrough init=1 start=1 (1832x1920)
QIYU VST: camera frames confirmed (30 frames, N callbacks)
```

If instead you see `camera texture is empty after 30 frames`, the vendor SDK
opened its camera client but never produced a frame. Then:

- try the other orientation: `adb shell setprop debug.wolvic.qiyu.vst.flip 1`
  and toggle passthrough again (the property is read at load time);
- try `adb shell setprop debug.wolvic.qiyu.vst 0` to go back to the
  service-ladder path and confirm the old behaviour still works.

Open questions that only a device can answer: whether the vendor camera client
and the system see-through service can both hold the cameras, whether the raw
frames are already usable without the 11.7 MB `libseethrough-core.so`
rectification pipeline, and the exact `drawSeethrough` contract
(`int drawSeethrough(int leftTexture, int rightTexture, int* outIndex,
int64_t* outTime, int* outExtra)`, recovered from the disassembly, returns 1
when it wrote a new frame).
