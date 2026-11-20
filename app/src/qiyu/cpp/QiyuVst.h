#pragma once

// Bridge to the Qiyu vendor see-through SDK (libseethroughsdk.so +
// libcameralibrary2.so).
//
// The system see-through service (com.qiyi.seethroughservice) is only started by the
// Qiyu OpenXR runtime for an OpenXR client, so an app that renders through qiyuapi
// cannot make the SXR compositor open the cameras by itself. The vendor SDK, however,
// is renderer independent: it opens a camera client and hands the frames to the app as
// GL textures. That is the same shape as the controller bridge - a second vendor
// subsystem that does not own the display.
//
// Call order (see docs/qiyu-thru3d-passthrough-rea.md):
//   Start() on the thread with the eye EGL context current (once),
//   Update() once per frame, Draw() per eye, Stop() when passthrough is switched off.
//
// All of it is best effort: when the libraries are missing or no camera frame ever
// arrives the caller keeps the opaque scene.

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
