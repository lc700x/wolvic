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
