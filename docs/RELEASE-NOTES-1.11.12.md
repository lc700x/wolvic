# Wolvic for Qiyu 1.11.12

## Passthrough

This is the first build that renders see-through from inside the app instead of
asking the system to do it.

- New vendor SDK bridge (`app/src/qiyu/cpp/QiyuVst.cpp`). The two renderer
  independent libraries from the official `svrApi-release.aar` -
  `libcameralibrary2.so` and `libseethroughsdk.so` - are packaged in the APK and
  loaded at runtime. They have no `libsxrapi.so` dependency, so they coexist with
  the 2.2 MB renderer library the app submits frames with.
- `DeviceDelegateQiyu::BindEye` pulls one camera frame per frame and draws it as
  an opaque full screen background before the scene. The eye buffer is only
  cleared to alpha 0 when no camera frame is available, which keeps the old
  system-service path working.
- `android.permission.CAMERA` is declared and requested on the first toggle.
- The 1.11.11 system-service ladder still runs in parallel and is logged with
  the same `QIYU: VST attempt N` lines, so both routes can be compared on device.

### Fail-safes

The bridge can never black out the scene by itself:

- three failed starts disable it for the session;
- after 30 camera frames an 8x8 pixel read-back checks the camera texture, and a
  still-empty texture disables the bridge;
- `adb shell setprop debug.wolvic.qiyu.vst 0` disables it before it loads;
- `adb shell setprop debug.wolvic.qiyu.vst.flip 1` flips the camera image
  vertically for the next launch.

While the bridge is warming up, and if it never delivers, the scene stays opaque
and the build behaves like 1.11.11.

## Unchanged on purpose

- The native SXR renderer (`qiyuapi`, `qiyu_StartEye` / `qiyu_SubmitFrame`).
- Controllers: `qiyu_GetControllerData`, the 0.5 trigger/grip thresholds and the
  250 ms hold-to-click fallback.
- 6DoF (`sxrSetTrackingMode(3)`) and the head/controller transform rules.
- WebXR: the GeckoView external VR protocol stays at v19.
- No OpenXR delegate on Qiyu, so the FOV/frame-pacing work from 1.11.9 is
  untouched.

## Log lines to check

```
adb logcat -s QIYU:V seethrough_xr:V VstManager:V SeethroughService:V sxr:V
```

Expected on a working bridge:

```
QIYU VST: vendor see-through SDK loaded (flipV=0)
QIYU VST: startSeeThrough init=1 start=1 (1832x1920)
QIYU VST: camera frames confirmed (30 frames, N callbacks)
```

If `camera texture is empty after 30 frames` shows up, the vendor SDK opened its
camera client but delivered nothing; the build then falls back to the 1.11.11
behaviour and the system path.

## Build

`Wolvic-qiyu-arm64-gecko-generic-release.apk`, versionName `1.11.12`.
