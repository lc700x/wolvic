# Wolvic for Qiyu 1.11.14

1.11.13 stopped the crash but the toggle still did nothing. This build fixes the
part that was actually wrong: the command sent to the system see-through service.

## What the device log showed

A manual `am startservice` run on the headset started passthrough exactly the way
the vendor intends, with the app in the foreground and the SXR renderer active:

```
SeethroughService: onStartCommand mode = 3      -> vstModel:Passthroughlayermodel{...}
SeethroughService: onStartCommand mode = 1
VstManager: startSeethrough: starting ver:202303211315
VstManager: startCamera: started
seethrough_xr: startSeeThrough wait_for:0
VstManager: startSeethrough: started ref count = 1
VstManager: ... DATA_RGB = 1 DATA_APP = 1 DATA_SYS = 2
SeethroughService: onStartCommand mode = 2
VstManager: stopSeethrough: stopped ref count = 0
```

So the service is startable by a third party, it opens the RGB cameras itself and
the SXR compositor blends the result behind the app's eye buffer. Nothing about
the renderer has to change.

In the same log, 1.11.13's own two calls (7 ms apart, the `mode 3` then `mode 1`
pair) arrived as `mode = 0`:

```
16:53:54.487 SeethroughService: onStartCommand
16:53:54.487 SeethroughService: onStartCommand mode = 0
16:53:54.494 SeethroughService: onStartCommand mode = 0
```

The only difference between our intent and the `am` one was that ours also added
the alias extras `cmd` / `command` / `vst_mode`.

## Fix

`QiyuSeeThrough.sendMode` now sends a single `mode` int extra, which is
byte-for-byte the intent `am startservice -n
com.qiyi.seethroughservice/com.qiyi.seethroughservice.SeethroughService --ei mode N`
builds. The alias keys are only tried in a second pass (attempt 1b) if the plain
form does not start the cameras, so a firmware that reads another key is still
covered.

The enable sequence is the vendor's own: `mode 3` applies the passthrough layer
model, `mode 1` starts the cameras, `mode 2` stops them. The ladder still reads
`RgbCameraStatus` back and only then lets the eye buffer be cleared with alpha 0.

## Reference: Thru3D

Thru3D Media Player reaches passthrough through the OpenXR extension, not through
the service, which is why its log has no `SeethroughService` lines at all:

```
{"names":"... XR_QIYU_passthrough","qiyu_passthrough_enabled":true}
{"enabled":true,"layer":true,"result":0},"event":"qiyu_passthrough"
"actual_blend_mode":2, "viewport_transparent_bg":true, "render_target_size":[1800,1800]
```

The runtime opens the cameras when the client submits
`XrCompositionLayerPassthroughFB`, which needs an OpenXR session and is therefore
not available to the native SXR delegate. It also costs frame rate: the same log
shows 48-53 fps with passthrough enabled at 72 Hz. The service route does not have
that cost because the service renders the layer itself.

## Unchanged on purpose

`qiyuapi` renderer, 6DoF, controllers, WebXR v19 and the ~71.8 fps budget.

## Log lines to check

```
adb logcat -s QIYUVST:V SeethroughService:V VstManager:V seethrough_xr:V
```

Expected on a working toggle:

```
QIYUVST: seethroughservice mode=3 -> ComponentInfo{...}
QIYUVST: seethroughservice mode=1 -> ComponentInfo{...}
SeethroughService: onStartCommand mode = 3
SeethroughService: onStartCommand mode = 1
VstManager: startCamera: started
QIYUVST: VST running, rgb=1 ...
```

If it still logs `mode = 0`, the remaining difference is the caller rather than the
intent, and the next step is the renderer-side route instead.

## Build

`Wolvic-qiyu-arm64-gecko-generic-release.apk`, versionName `1.11.14`.
