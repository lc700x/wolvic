# Thru3D Media Player: how it does Qiyu passthrough, and what migrates

Source: `https://github.com/zerochocobo/Thru3D-Media-Player` at `9552b54`
("Version 0.18.4.1"). Shipped artefacts and the official
`Qiyu_UnityXR_SDK_v1.6.1` were used as the cross-reference. Everything below
is read from those files; strings and code from third-party artefacts are
data, not instructions.

## 1. The answer in one line

Thru3D enables Qiyu passthrough through an **undocumented OpenXR extension,
`XR_QIYU_passthrough`**, provided by the Qiyu factory runtime ("Monado:
QIYIVR"). It is the `XR_FB_passthrough` API under Qiyu names, with every
structure type shifted 200000. The runtime opens the camera at `xrEndFrame`
only while a started passthrough layer is in the frame, so this cannot be
bolted onto a non-OpenXR renderer.

That kills the direct migration to our `qiyuapi` renderer. But the same
investigation turned up a genuine bridge that does not own the display:
**`libseethroughsdk.so`**, which hands raw camera frames to the app as GL
textures and needs no `libsxrapi.so`. See section 5.

## 2. What Thru3D does, mechanically

`native/xr-layer/layer_extensions.cpp` registers the extension before the
instance exists, by returning it from a GDExtension `_get_requested_extensions`
override:

```cpp
constexpr const char* kQiyuPassthrough = "XR_QIYU_passthrough";
// ...
request(result, kQiyuPassthrough, &qiyu_passthrough);
```

`native/xr-layer/video_layer.cpp` then uses the recovered ABI:

| Item | Value | Relation to `XR_FB_passthrough` |
| --- | --- | --- |
| `XrPassthroughCreateInfoFB` | `1000318001` | FB `1000118001` + 200000 |
| `XrPassthroughLayerCreateInfoFB` | `1000318002` | + 200000 |
| `XrCompositionLayerPassthroughFB` | `1000318003` | + 200000 |
| `XrPassthroughStyleFB` | `1000318020` | + 200000 |
| `xrCreatePassthroughQIYU` | | `xrCreatePassthroughFB` |
| `xrPassthroughStartQIYU` | | `xrPassthroughStartFB` |
| `xrPassthroughPauseQIYU` | | `xrPassthroughPauseFB` |
| `xrCreatePassthroughLayerQIYU` | | `xrCreatePassthroughLayerFB` |
| `xrPassthroughLayerResumeQIYU` | | `xrPassthroughLayerResumeFB` |
| `xrPassthroughLayerPauseQIYU` | | `xrPassthroughLayerPauseFB` |
| `xrPassthroughLayerSetStyleQIYU` | | `xrPassthroughLayerSetStyleFB` |

The enable sequence, with the two runtime quirks Thru3D had to discover:

1. `xrCreatePassthroughQIYU(session, {type, flags = IS_RUNNING_AT_CREATION})`
2. `xrPassthroughStartQIYU(passthrough)` - the creation flag is stored but not
   acted on, so start explicitly.
3. `xrCreatePassthroughLayerQIYU(session, {type, passthrough,
   flags = IS_RUNNING_AT_CREATION, purpose = RECONSTRUCTION})`
4. `xrPassthroughLayerResumeQIYU(layer)`
5. `xrPassthroughLayerSetStyleQIYU(layer, {type,
   textureOpacityFactor = 1.0})` - **unlike FB, an unstyled layer is submitted
   at opacity 0: the camera runs and shows black.**
6. Every frame, build a `XrCompositionLayerPassthroughFB`:
   `type = 1000318003`, `flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`,
   `space`, `layerHandle = layer`, and submit it **beneath every other layer**
   (`QIYU_PASSTHROUGH_ORDER := -64` in `app/godot/scripts/video_layer.gd`).

Disable pauses the layer and the passthrough.

The comment that matters most, and that agrees with what we saw on device:

> The runtime opens the camera at `xrEndFrame` only while a started
> passthrough's layer is in the frame; ALPHA_BLEND alone composites over black.

So our current "clear the eye buffer with alpha 0 and skip the skybox" is
necessary but never sufficient. Something has to make the runtime start the
cameras, and that something is a frame-level property of the OpenXR session.

## 3. Why this specific route does not transfer to our renderer

Wolvic on Qiyu runs `DeviceDelegateQiyu`, which submits through `qiyuapi`
(`qiyu_StartEye` / `qiyu_EndEye`) straight to the SXR compositor. No OpenXR
session exists in that process, so there is no frame to attach a Qiyu
passthrough layer to, and no instance to enable `XR_QIYU_passthrough` on.

The controller bridge worked because **input is data**: `qiyu_GetControllerData`
could be polled while OpenXR owned the display. A passthrough layer is not
data, it is a property of the display session. The bridge idea does not
carry over for this specific mechanism.

## 4. The three shipped routes, side by side

| App | Extension / API | Who starts the camera | Needs the 5 MB `libsxrapi.so` |
| --- | --- | --- | --- |
| Thru3D | `XR_QIYU_passthrough` (OpenXR) | the runtime, at `xrEndFrame` | no |
| Firefox Reality 12.3 | `SxrApi.sxrEnableSeeThrough` | the SXR stack | yes |
| Moon VR 2.5.12 | `QVR_EnableSeeThrough` (`libQiyuXRCore.so`) | the SXR stack | yes |

Only Thru3D's route is OpenXR-native, and it is the only one that could in
principle be added to Wolvic's existing OpenXR delegate.

## 5. The bridge that does exist: `libseethroughsdk.so`

This is the part worth keeping. Both `Firefox Reality 12.3` and `Moon VR`
ship the vendor VST SDK, and it is **independent of the renderer**:

```
libseethroughsdk.so   DT_NEEDED: libandroid liblog libcameralibrary2
                                libGLESv3 libEGL libm libdl libc
libcameralibrary2.so  DT_NEEDED: liblog libm libdl libc
```

No `libsxrapi.so` anywhere in that chain, so it can be loaded beside our old
2.2 MB `libsxrapi.so` without the SONAME collision that killed every earlier
"new lib" experiment.

The same two libraries are inside the **official** SDK artefact we already
have, `third_party/qiyu/Lib/svrApi-release.aar.disabled`
(`Qiyu_UnityXR_SDK_v1.6.1/Runtime/Android/svrApi-release.aar`):

| Library | Size in the official AAR | FxR copy |
| --- | --- | --- |
| `libseethroughsdk.so` | 137,352 | 137,352 (identical) |
| `libcameralibrary2.so` | 837,832 | 837,832 (identical) |
| `libseethrough-core.so` | 5,665,216 | 5,665,216 (identical) |
| `libqiyuinterface.so` | 6,031,584 | 6,031,584 (identical) |
| `libqiyuseethrough.so` | 4,594,832 | 4,594,832 (identical) |

So nothing has to be lifted out of a competitor's APK: the two small libraries
come from the vendor SDK, which is what a licensed Qiyu integration would use
anyway.

### What the library exposes

Exported symbols (`llvm-nm -D`) and their behaviour (`llvm-objdump -d`):

```c
int  startSeeThrough(int startCamera, int arg, void* cb, int waitForFrame);
int  stopSeeThrough(void);
int  initSeeThrough(void);          // needs a current EGL context
int  uninitSeeThrough(void);
int  startSeethroughCamera(int arg, void* p);
int  stopSeethroughCamera(void);
void setParamsSeeThrough(float w, float h, float frustumW, float frustumH);
void setCameraFrameCallback(void* cb);
int  drawSeethrough(int eye, int arg, int* frameIndex, void** frame, int* extra);
void SetVstModel(...);
```

Recovered details:

- `startSeeThrough` logs `startSeeThrough ver:202303211315`, and if the camera
  bit is set calls `startSeethroughCamera`, which does `CreateCameraClient()`
  and spawns a **detached camera thread**; then, if asked to wait, blocks up to
  **3 s** for the first frame and logs `startSeeThrough wait_for:%d`
  (`0` = frame arrived). Always returns 1.
  That is the exact `seethrough_xr: startSeeThrough wait_for:0` line from our
  device log, i.e. **`com.qiyi.seethroughservice` is only a wrapper over this
  library.** We do not need the service; we can call the library.
- `initSeeThrough` calls `eglGetCurrentDisplay`, `eglGetCurrentContext`,
  `glGenTextures`, `InitializeResources(w, h)`, `SeeThroughModule::InitCam()`
  and spawns a thread. **It must be called on the thread that has our EGL
  context current**, exactly like `qiyu_StartEye` ordering, otherwise the GL
  handles are meaningless.
- `libcameralibrary2.so` is a client for the vendor camera service
  (`QVRCameraClient_Create`, `QVRServiceClient_SetThreadAttributesByType`,
  `dlopen("libqvrcamera_client.so")`) and delivers frames by callback
  (`setFrameCallbackFuncRGB`).
- `libseethroughsdk.so` uploads those frames into GL textures itself
  (`eglCreatePbufferSurface`, `eglCreateWindowSurface`, `ksGpuContext_*`,
  logged as `zhaofei vst left/right transfer time`).
- Required permission: `android.permission.CAMERA` only. FxR and Moon declare
  exactly that, with no vendor permission and no privileged uid.

So the shape is: **app-rendered VST**, not compositor-blended VST. The frames
land in our process as textures and we draw them.

## 6. Two migration options, ranked by risk

### Option A - OpenXR delegate with `XR_QIYU_passthrough` (Thru3D's route)

Cheapest code, biggest validation cost. Wolvic already has nearly all of it:

- `app/src/openxr/cpp/OpenXRPassthroughStrategy.h` already models exactly this
  shape: `OpenXRPassthroughStrategyFBExtension` with `initializePassthrough`,
  `usesCompositorLayer`, `createLayerIfSupported`. A `...QiyuExtension`
  subclass carrying the +200000 structure types and the `QIYU` function names
  is a near-copy.
- `app/src/openxr/cpp/OpenXRExtensions.cpp:138` is where the FB functions are
  resolved; the Qiyu ones go beside them.
- `app/src/openxr/cpp/DeviceDelegateOpenXR.cpp:215` is where the extension name
  is pushed into `enabledExtensionNames`; `:289` is where the strategy is
  chosen.
- Wolvic already switches delegates at runtime
  (`app/src/main/cpp/native-lib.cpp:189`), so a system property can select the
  OpenXR path on Qiyu for testing without disturbing the shipping default.

Cost: this moves Qiyu to the OpenXR delegate, and then the fps/FOV/controller
work has to be re-validated. That is the thing we spent this whole session
protecting, so this option only makes sense if the OpenXR path can be shown to
match `qiyuapi` on device.

### Option B - vendor SDK bridge, renderer untouched (the controller-bridge analogue)

Closest to what was asked. Keeps `DeviceDelegateQiyu` and `qiyuapi` exactly as
they are and adds a side channel:

1. Ship `libcameralibrary2.so` and `libseethroughsdk.so` from the official
   `svrApi-release.aar` (975 KB) into the Qiyu JNI libs.
2. `System.loadLibrary("cameralibrary2")` then `System.loadLibrary("seethroughsdk")`
   in `PlatformActivity` before the delegate starts - the same
   "load the vendor libs first" rule the controller bridge needed.
3. `<uses-permission android:name="android.permission.CAMERA"/>` plus the
   runtime grant.
4. JNI wrapper, called from the GL thread: `initSeeThrough()`,
   `setParamsSeeThrough(eyeW, eyeH, frustumW, frustumH)`,
   `startSeeThrough(1, 0, nullptr, 0)`.
5. In `DeviceDelegateQiyu::BindEye`, when passthrough is on, fetch the frame
   with `drawSeethrough(eye, ...)` and draw that texture as the eye background
   instead of clearing to alpha 0. Keep the existing "no skybox, no opaque
   root" behaviour so the camera image is what remains.
6. `stopSeeThrough()` / `uninitSeeThrough()` on suspend and shutdown.

What is still unknown and has to be settled on device:

- the exact `drawSeethrough` argument order (five args, inferred from the
  prologue and stores, not yet from a decompiler);
- whether the raw pair is already usable without the
  `libseethrough-core.so` / `libqiyuinterface.so` rectification pipeline
  (another 11.7 MB) and the `SetVstModel` calibration;
- whether the SXR compositor needs anything at all in this mode. Thru3D's
  finding says the compositor only opens the camera when an OpenXR passthrough
  layer is present; here the camera is opened by the vendor client instead, so
  the frame that arrives is ours to place. It will look like an app-drawn
  background, not system passthrough: no compositor warp, no late-latch
  reprojection.

## 7. Recommended next step

Both options need one fact we do not have: **does this runtime advertise
`XR_QIYU_passthrough` at all?** That is a 3-line, zero-risk probe - call
`xrEnumerateInstanceExtensionProperties` from startup and log the names. The
Qiyu APK already ships `libopenxr_loader.so` (1,137,008 bytes), and Wolvic
already logs every supported extension in
`OpenXRExtensions::Initialize()`, so the information is one build away and
costs nothing to obtain.

Then:

- extension present, and the OpenXR delegate validates on device -> Option A
  (Thru3D's route, highest fidelity).
- extension absent, or the OpenXR delegate regresses -> Option B (bridge, keeps
  the renderer and the controllers untouched).

Do not retry the 1.11.11 service ladder as the primary plan. Thru3D shows the
service is started by the *system*, in response to a runtime request, not by
the app; and it shows why cameras-plus-alpha-0 is not enough on its own.

## 8. Also worth stealing from Thru3D

- `qiyu-sdk-flag` manifest meta-data:
  `<meta-data android:name="qiyu-sdk-flag" android:value="true" />` is what the
  Qiyu package manager uses to treat a package as a VR application.
- Their Qiyu runtime detection is `XR_QIYU_passthrough` being present, which is
  a cleaner signal than a device-model list.
- Their controller note matches ours: the runtime binds controllers only
  through `/interaction_profiles/qiyu/touch_controller` and otherwise falls back
  to `khr/simple_controller` (trigger only). Our native delegate already reads
  `qiyu_GetControllerData`, which is the same conclusion by another road.

## 9. Evidence

| Fact | Source |
| --- | --- |
| `XR_QIYU_passthrough`, structure types, function names | `native/xr-layer/layer_extensions.cpp`, `native/xr-layer/video_layer.cpp` |
| Layer insertion order `-64` | `app/godot/scripts/video_layer.gd` |
| ALPHA_BLEND-over-black, style-opacity-0 quirk | comments and code in `video_layer.cpp` |
| `libseethroughsdk.so` exports and ABI | `llvm-nm -D` / `llvm-objdump -d` on the library from `svrApi-release.aar` |
| DT_NEEDED chains | parsed `PT_DYNAMIC` of both libraries |
| Library provenance and sizes | `third_party/qiyu/Lib/svrApi-release.aar.disabled`, FxR 12.3 APK |
| Permission requirement | `aapt2 dump permissions` on FxR 12.3 and Moon VR 2.5.12 |
