# Wolvic for Qiyu 1.11.13

This build fixes the crash introduced in 1.11.12. Passthrough toggling no longer
takes the app down, and a toggle that cannot start VST no longer leaves the user
on a black screen.

## Crash fix

1.11.12 crashed with `SIGSEGV` in `VRB Render` one to two seconds after enabling
passthrough. Three identical tombstones pinned it down:

```
Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xd650911000531
#00 libcameralibrary2.so (CameraClientImpl::startCameraRGB(void*)+288)
#01 libseethroughsdk.so +0xd954
#02 __pthread_start
```

`+288` is `ldr x8, [x8, #0x8]` where `x8` came from the camera client returned by
`getQvrCameraClientInstance()`. That client is ABI-incompatible with the 2023
camera library, so the vtable pointer read from it is garbage and the detached
camera thread started by `startSeeThrough()` dies immediately. The argument we
were passing (`&OnCameraFrame`) never reached that code path; removing the
callback cannot fix it.

The camera stack only works as a whole. `libsxrapi.so` owns `SvrCameraManager`
and is what supplies the `qvrcamera_device_helper_t` the SDK dereferences, and
the vendor's 2023 `libsxrapi.so` is the one that matches
`libcameralibrary2.so`. That library cannot be shipped here: replacing the
2.2 MB renderer library with it black-screens the `qiyuapi` frame submission
(71.8 fps, empty eye buffer), which is why every build since 1.11.9 keeps the
2.2 MB one.

So the in-app bridge is now inert:

- `startSeeThrough()` is never called, which removes the crash;
- `QiyuVst` refuses to load unless `debug.wolvic.qiyu.vst=1` is set explicitly;
- `libcameralibrary2.so` and `libseethroughsdk.so` are no longer packaged in the
  APK (the build is ~1 MB smaller and the mismatched camera library cannot be
  loaded by accident).

## No more black screen on a failed toggle

`VRBrowserActivity.togglePassthrough` used to forward the toggle to native even
when `QiyuSeeThrough.setEnabled` reported that nothing had started the cameras.
Native then cleared the eye buffer with alpha 0 with no image behind it, which
shows as a black world. Now a failed enable request leaves the switch off and the
scene opaque.

## Diagnostics

The enable/disable round trip now logs under the low-volume `QIYUVST` tag. The
per-frame `QIYU` tag is rate-limited away by chatty on a busy session, which is
why 1.11.12's log had no `QIYU VST:` lines at all. Each attempt prints the three
provider statuses:

```
QIYUVST: VST attempt 1 (service start, accepted=ComponentInfo{...}) rgb=0 app=0 sys=0
QIYUVST: VST attempt 4 (sxrEnableSeeThrough, called=false) rgb=0 app=1 sys=0
QIYUVST: no VST path started the cameras; keeping the opaque scene (service=true, sxr=false) ...
```

`rgb` is `RgbCameraStatus` (`content://com.iqiyi.vr.provider/vst/1`) and is the
only trustworthy success signal; `app` is the value written to
`AppVstStatus` (`/vst/2`); `sys` is `SystemVstStatus` (`/vst/3`). Reading `app=1`
with `rgb=0` means the platform accepted our status write but never asked the
runtime to open the cameras, i.e. the remaining route is the runtime's own
passthrough request.

## Unchanged on purpose

- The native SXR renderer (`qiyuapi`, `qiyu_StartEye` / `qiyu_SubmitFrame`),
  6DoF (`sxrSetTrackingMode(3)`), controllers (0.5 thresholds, 250 ms
  hold-to-click), the head/controller transform rules and ~71.8 fps.
- WebXR: the GeckoView external VR protocol stays at v19.
- The 1.11.11 system-service ladder, minus the rollback change above.

## Log lines to check

```
adb logcat -s QIYUVST:V QIYU:V seethrough_xr:V VstManager:V SeethroughService:V sxr:V
```

## Build

`Wolvic-qiyu-arm64-gecko-generic-release.apk`, versionName `1.11.13`.
