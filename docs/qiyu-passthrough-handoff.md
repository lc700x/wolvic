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

## 13. 1.11.13: the bridge crashed, and why it cannot be repaired here

1.11.12 crashed one to two seconds after enabling passthrough, every time. This
section is the post-mortem; the code it describes is gone from the build.

### 13.1 The tombstone

Three identical crashes (`14:10:39`, `14:11:09`, `14:13:10`):

```
Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xd650911000531
tid: VRB Render, pid: com.igalia.wolvic
#00 libcameralibrary2.so (CameraClientImpl::startCameraRGB(void*)+288)  BuildId cba05146...
#01 libseethroughsdk.so +0xd954                                        BuildId d52eb595...
#02 __pthread_start
```

`#01` is the detached thread that `startSeethroughCamera()` spawns from inside
`startSeeThrough()`. `BuildId` matches the shipped `svrApi-release.aar` copies, so
this is our code path and not the system service.

### 13.2 What actually faults

`libcameralibrary2.so` exports its symbols, so the crash site resolves exactly:
`startCameraRGB` is at `0x45988` and `+288 = 0x45aa8`.

```
; x24 = the camera client context
45aa0: ldr x8, [x24, #0x8]   ; x8 = instance returned by getQvrCameraClientInstance()
45aa4: str x8, [x22]
45aa8: ldr x8, [x8, #0x8]    ; <-- SEGV, fault addr = x8 + 8
45aac: ldr x8, [x8, #0x18]   ; vtable slot 3
45ab4: ldr x0, [x24, #0x10]  ; the qvrcamera_device_helper_t
```

The register dump matches: `x8 = 0xb90d650911000529`, fault `0xb90d650911000531`.
The object returned by `getQvrCameraClientInstance()` has a garbage vtable
pointer, i.e. the client was produced by an ABI-incompatible provider.

The third argument of `startSeeThrough` (`&OnCameraFrame` in 1.11.12) is *not*
involved. The real ABI, recovered from the disassembly, is:

```
int  startSeeThrough(int flags, int a1, void* a2, int waitForFrame);
       flags & 1  -> startSeethroughCamera(a1, a2);
       waitForFrame & 1 -> wait on a condvar, 3 s timeout, returns whether it timed out
void startSeethroughCamera(int a0, void* a1);
       spawns a thread that calls
       client->vtable[0](client, (a0 != 1), a1, 0, *(0x21000+0xf28));
```

`a1` is captured into the thread argument block and becomes the `void*` argument
of `startCameraRGB`, so passing 0 or a function pointer changes nothing: the crash
is inside the client, before the argument is used.

`setCameraFrameCallback(void*)` is a separate 16-byte export and is the correct
home for a callback; it has nothing to do with this fault.

### 13.3 Why the camera client is incompatible

`libsxrapi.so` owns `SvrCameraManager` and is the component that creates the
`qvrcamera_device_helper_t` the SDK dereferences. That matches the strings in
`libcameralibrary2.so` (`getQvrCameraClientInstance`,
`getQvrCameraDeviceInstance`, `libqvrcamera_client.old.qti.so`,
`libqvrcamera_client.so`).

Firefox Reality 12.3 on the same hardware ships the whole consistent 2023 stack:

```
libsxrapi.so          5 037 976   (2023-05-08)
libseethroughsdk.so     137 352   (identical to ours)
libcameralibrary2.so    837 832   (identical to ours)
libqiyuinterface.so   6 031 584
libseethrough-core.so 5 665 216
librelocationsdk.so      55 328
libQiyuXRCore.so      1 694 640
```

and drives VST through `SxrApi.sxrEnableSeeThrough(boolean)`. In that library
`sxrEnableSeeThrough` only stores a flag (`gEnableSeeThrough`); `SvrStartSeeThrough()`
then calls `setParamsSeeThrough(...)` and `startSeeThrough(...)` from libsxrapi's
own render loop, with `SvrCameraManager` supplying the camera device. The app is
never expected to call `startSeeThrough` itself.

Our APK cannot ship that library. `libsxrapi.so` is the renderer: `qiyuapi`
submits frames through it, and swapping the 2.2 MB 2022 build
(`sxrApi Version : 4.0.3, Mar  9 2022`) for the 5 MB 2023 one produces a black
screen at 71.8 fps - the session runs, the eye buffer is never composited. Only
one `libsxrapi.so` can exist in a process (same SONAME, and `libqiyuapi.so` has it
in `DT_NEEDED`), so there is no way to have both: either the renderer works or the
camera stack does.

That is the real conclusion of 1.11.12: the vendor VST SDK is not a
renderer-independent add-on. It is the other half of the same SXR build, and we
have to keep the half that draws.

### 13.4 What 1.11.13 does instead

- `startSeeThrough()` is never called; the watchdog threads that crashed are gone.
- `QiyuVst` is inert unless `debug.wolvic.qiyu.vst=1` is set explicitly.
- `libcameralibrary2.so` / `libseethroughsdk.so` are no longer packaged.
- A failed enable request no longer forwards the toggle to native, so the scene
  cannot be cleared to alpha 0 with nothing behind it (the black-screen failure
  mode of 1.11.11/1.11.12 after a refused toggle).
- The ladder logs under `QIYUVST` (low volume, survives chatty) with all three
  provider statuses.

### 13.5 The only remaining route

System VST is started by the runtime, not by the app: an SXR/OpenXR client asks
for passthrough and `QiyuOXRCore` sends `PassthroughStart` to
`com.qiyi.seethroughservice`. `content://com.iqiyi.vr.provider/vst/2`
(`AppVstStatus`) is a *status report* - the vendor `SeeThroughHelper.writeAppStatus`
writes exactly the value we write, and it does not start anything. So a build
that keeps the 2.2 MB `libsxrapi.so` can report the wish but cannot make the
request.

A future attempt therefore has to change the renderer too: port
`DeviceDelegateQiyu` to `sxrApi`/`SvrView` (or to the OpenXR delegate that
supports `XR_QIYU_passthrough`, see `docs/qiyu-thru3d-passthrough-rea.md`) so the
2023 `libsxrapi.so` can be shipped. That is a renderer change, not a passthrough
patch, and it must be validated on device before it replaces the current build.

## 14. 1.11.14: the service does work, and the intent was the difference

Two device experiments settled this.

### 14.1 A manual `am startservice` starts passthrough

```
SeethroughService: onStartCommand mode = 3
SeethroughService: vstModel:Passthroughlayermodel{edge_color=[0,0,0,0], ..., w_h_c_g=[1800,1800,0,1]}
SeethroughService: onStartCommand mode = 1
VstManager: startSeethrough: starting ver:202303211315
VstManager: startCamera: starting -> started
seethrough_xr: startSeeThrough ver:202303211315
seethrough_xr: startSeeThrough wait_for:0
VstManager: startSeethrough: started ref count = 1
VstManager: onChange ... DATA_RGB = 1 DATA_APP = 1 DATA_SYS = 2
```

`mode = 2` stops it (`stopSeethrough: stopped ref count = 0`, and the camera is
released 5 s later by `stopCameraDelayed: 5.0`). The service is a normal exported
`startService` target: it runs `initSeeThrough` in standalone mode, creates its
own 1440x1440 Monado session, calls `startSeethroughCamera` and renders the
passthrough layer itself. The app only has to clear its eye buffer with alpha 0.

So the service route never needed the renderer to change. What failed in 1.11.13
was the intent: our two calls (same `mode 3` then `mode 1`, 7 ms apart, same order
as the working run) arrived as `mode = 0`. The only difference was the alias
extras `cmd` / `command` / `vst_mode` that 1.11.11-1.11.13 added "for safety".

1.11.14 sends a single `mode` int extra - the exact intent `am startservice -n
<pkg>/<cls> --ei mode N` builds - and only falls back to the aliases if that does
not start the cameras.

### 14.2 Thru3D does not use the service at all

Its instrumentation log has no `SeethroughService` lines. It is an OpenXR client
(`Vr700ClassLoader: qiyu bootstrap entered`) and drives the runtime extension:

```
{"names":"... XR_QIYU_passthrough","qiyu_passthrough_enabled":true}
{"enabled":true,"layer":true,"result":0},"event":"qiyu_passthrough"
"actual_blend_mode":2, "viewport_transparent_bg":true,
"render_target_size":[1800,1800], "render_scale":1.25
```

The runtime opens the cameras while a `XrCompositionLayerPassthroughFB` is in the
frame, which requires an OpenXR session and is unavailable to the native SXR
delegate. It also costs frame rate: 48-53 fps with passthrough at 72 Hz, versus the
service route which renders the layer out of process and leaves the app's frame
budget alone.

That makes `mode 1` the only route that fits this renderer, and 1.11.14 is the
build that should finally reach it.

## 15. 1.11.15: the service is addressed by action, not by component

1.11.14 still produced `mode = 0` with a single `mode` extra, which ruled out the
extras. `adb shell dumpsys package com.qiyi.seethroughservice` gave the answer:

```
Service Resolver Table:
  Non-Data Actions:
      org.freedesktop.monado.vst:
        com.qiyi.seethroughservice/.SeethroughService
          Action: "org.freedesktop.monado.vst"
```

The service is declared with an intent-filter action and is meant to be resolved
implicitly. An intent built with `setComponent(...)` and no action carries
`action = null`, and the service treats that as "no command" - it logs
`onStartCommand mode = 0` whatever extras it gets. That is why 1.11.11 through
1.11.14 were all ignored while the real caller (the Monado VST client, which uses
the action) reached `mode = 3` / `mode = 1`.

A second confirmation from the device: `am startservice -n <pkg>/<cls> --ei mode 1`
(explicit component, from shell, no action) also left `RgbCameraStatus` at 0.
Inside an app and from `am` the component form fails identically, so this was never
a caller-permission problem.

1.11.15 sends `Intent(org.freedesktop.monado.vst).setPackage(...)` with the
`mode` int, and keeps the component form as a last resort for other firmware:

| Variant | Intent |
| --- | --- |
| `VARIANT_ACTION` (first) | action `org.freedesktop.monado.vst` + `setPackage`, `mode` as int |
| `VARIANT_ACTION_STRING` | same, `mode` as String |
| `VARIANT_COMPONENT` (last) | explicit component, `mode` as int |

Verify from a shell with the implicit form:

```
adb shell am startservice -a org.freedesktop.monado.vst --ei mode 1
adb shell content query --uri content://com.iqiyi.vr.provider/vst/1
```

`status_value=1` plus `VstManager: startCamera: started` in logcat means the
contract is confirmed. Stop with `--ei mode 2`.

Note on logcat: the `QIYUVST` lines are emitted at warning level because chatty's
per-uid quota drops this app's info lines (`uid=... expire N lines`), which is why
1.11.12's `QIYU VST:` lines never showed up at all.



