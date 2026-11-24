#pragma once

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#include <string>
#include <unordered_set>

namespace crow {
  class OpenXRExtensions {
  public:
    static void Initialize();
    static bool IsRuntimeAvailable();
    static void LoadExtensions(XrInstance instance);
    static bool IsExtensionSupported(const char*);
    static void LoadApiLayers(XrInstance instance);
    static bool IsApiLayerSupported(const char*);

    static PFN_xrGetOpenGLESGraphicsRequirementsKHR sXrGetOpenGLESGraphicsRequirementsKHR;
    static PFN_xrCreateSwapchainAndroidSurfaceKHR sXrCreateSwapchainAndroidSurfaceKHR;

    // hand tracking extension prototypes
    static PFN_xrCreateHandTrackerEXT sXrCreateHandTrackerEXT;
    static PFN_xrDestroyHandTrackerEXT sXrDestroyHandTrackerEXT;
    static PFN_xrLocateHandJointsEXT sXrLocateHandJointsEXT;

    static PFN_xrPerfSettingsSetPerformanceLevelEXT sXrPerfSettingsSetPerformanceLevelEXT;
    static PFN_xrEnumerateDisplayRefreshRatesFB sXrEnumerateDisplayRefreshRatesFB;
    static PFN_xrRequestDisplayRefreshRateFB sXrRequestDisplayRefreshRateFB;

    static PFN_xrCreatePassthroughFB sXrCreatePassthroughFB;
    static PFN_xrDestroyPassthroughFB sXrDestroyPassthroughFB;
    static PFN_xrCreatePassthroughLayerFB sXrCreatePassthroughLayerFB;
    static PFN_xrDestroyPassthroughLayerFB sXrDestroyPassthroughLayerFB;
    static PFN_xrPassthroughLayerResumeFB sXrPassthroughLayerResumeFB;
    static PFN_xrPassthroughLayerPauseFB sXrPassthroughLayerPauseFB;

    // Qiyu exposes the FB passthrough ABI under XR_QIYU_passthrough. The
    // function signatures are identical; only the entry-point names and
    // structure type values differ.
    static PFN_xrCreatePassthroughFB sXrCreatePassthroughQIYU;
    static PFN_xrDestroyPassthroughFB sXrDestroyPassthroughQIYU;
    static PFN_xrPassthroughStartFB sXrPassthroughStartQIYU;
    static PFN_xrPassthroughPauseFB sXrPassthroughPauseQIYU;
    static PFN_xrCreatePassthroughLayerFB sXrCreatePassthroughLayerQIYU;
    static PFN_xrDestroyPassthroughLayerFB sXrDestroyPassthroughLayerQIYU;
    static PFN_xrPassthroughLayerResumeFB sXrPassthroughLayerResumeQIYU;
    static PFN_xrPassthroughLayerPauseFB sXrPassthroughLayerPauseQIYU;
    static PFN_xrPassthroughLayerSetStyleFB sXrPassthroughLayerSetStyleQIYU;

    // Private Qiyu OpenXR extension values used by the working Supermoon client.
    // These are not defined by the public Qiyu Native SDK headers.
    static constexpr XrStructureType kXrTypePassthroughCreateInfoQIYU = static_cast<XrStructureType>(0x3B9FA431);
    static constexpr XrStructureType kXrTypePassthroughLayerCreateInfoQIYU = static_cast<XrStructureType>(0x3B9FA432);
    static constexpr XrStructureType kXrTypeCompositionLayerPassthroughQIYU = static_cast<XrStructureType>(0x3B9FA433);
    static constexpr XrStructureType kXrTypePassthroughStyleQIYU = static_cast<XrStructureType>(0x3B9FA436);

    static PFN_xrCreateHandMeshSpaceMSFT sXrCreateHandMeshSpaceMSFT;
    static PFN_xrUpdateHandMeshMSFT sXrUpdateHandMeshMSFT;

    static PFN_xrCreateKeyboardSpaceFB xrCreateKeyboardSpaceFB;
    static PFN_xrQuerySystemTrackedKeyboardFB xrQuerySystemTrackedKeyboardFB;

    static PFN_xrEnumerateRenderModelPathsFB sXrEnumerateRenderModelPathsFB;
    static PFN_xrGetRenderModelPropertiesFB sXrGetRenderModelPropertiesFB;
    static PFN_xrLoadRenderModelFB sXrLoadRenderModelFB;
  private:
     static std::unordered_set<std::string> sSupportedExtensions;
     static std::unordered_set<std::string> sSupportedApiLayers;
     static bool sRuntimeAvailable;
  };
} // namespace crow
