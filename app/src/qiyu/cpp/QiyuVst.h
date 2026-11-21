#pragma once

// Bridge to the Qiyu vendor see-through SDK (libseethroughsdk.so +
// libcameralibrary2.so).
//
// DISABLED since 1.11.13. The vendor SDK is not renderer independent after all: the
// camera client it dereferences comes from libsxrapi.so's SvrCameraManager, and the
// 2.2 MB libsxrapi.so this renderer submits frames through is ABI-incompatible with
// the 2023 camera library. Calling startSeeThrough() therefore crashed with SIGSEGV
// in CameraClientImpl::startCameraRGB() every time (see
// docs/qiyu-passthrough-handoff.md section 13). The two vendor libraries are no
// longer packaged in the APK and Load() refuses unless debug.wolvic.qiyu.vst=1 is
// set, so the module is inert and the caller always keeps the opaque scene.
//
// Kept so the recovered ABI and the reasoning stay next to the code that used them:
//   startSeeThrough(int flags, int a1, void* a2, int waitForFrame)
//   startSeethroughCamera(int a0, void* a1)
//   startCameraRGB is vtable slot 0 of the client created by CreateCameraClient()
//
// Call order, if it is ever revived on top of a matching libsxrapi.so:
//   Start() on the thread with the eye EGL context current (once),
//   Update() once per frame, Draw() per eye, Stop() when passthrough is switched off.

#include <cstdint>

namespace crow {
namespace qiyu {
namespace vst {

// True once the vendor libraries are loaded and their entry points resolved.
bool Available();

// Loads the vendor libraries. Safe to call repeatedly; logs only once.
bool Load();

// Allocates the eye textures, initialises the vendor pipeline and starts the cameras.
// Must run on the GL thread. Returns false when the bridge is unusable.
bool Start(uint32_t aWidth, uint32_t aHeight);

// Pulls one camera frame from the vendor SDK into the eye textures.
//
// IMPORTANT: the vendor function binds its own framebuffer and leaves framebuffer 0
// bound, so it must run *before* the caller binds the eye FBO.
bool Update();

// Draws the last frame of aEye as an opaque full screen quad into the currently bound
// framebuffer. Returns false when no frame is available.
bool Draw(int32_t aEye);

// Stops the cameras and releases the GL objects created by Start().
void Stop();

} // namespace vst
} // namespace qiyu
} // namespace crow
