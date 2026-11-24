#include "OpenXRPassthroughStrategy.h"

#include <vrb/Logger.h>
#include "OpenXRExtensions.h"
#include "OpenXRHelpers.h"
#include <assert.h>

namespace crow {

OpenXRLayerPassthroughPtr
OpenXRPassthroughStrategy::createLayerIfSupported() const {
    VRB_ERROR("asking to create a layer for passthrough when it isn't actually supported");
    return nullptr;
};

OpenXRPassthroughStrategyFBExtension::~OpenXRPassthroughStrategyFBExtension() {
    if (passthroughHandle != XR_NULL_HANDLE && OpenXRExtensions::sXrDestroyPassthroughFB) {
        MessageXrResult(OpenXRExtensions::sXrDestroyPassthroughFB(passthroughHandle),
                        "xrDestroyPassthroughFB", FILE_AND_LINE);
        passthroughHandle = XR_NULL_HANDLE;
    }
}

void
OpenXRPassthroughStrategyFBExtension::initializePassthrough(XrSession session) {
    if (session == XR_NULL_HANDLE)
        return;

    if (!OpenXRExtensions::sXrCreatePassthroughFB || passthroughHandle != XR_NULL_HANDLE) {
        VRB_WARN("OpenXR passthrough initialization skipped: invalid FB entry point or handle");
        return;
    }

    XrPassthroughCreateInfoFB passthroughCreateInfo = {
            .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB,
            .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
    };
    CHECK_XRCMD(OpenXRExtensions::sXrCreatePassthroughFB(session, &passthroughCreateInfo, &passthroughHandle));
}

OpenXRPassthroughStrategy::HandleEventResult
OpenXRPassthroughStrategyFBExtension::handleEvent(const XrEventDataBaseHeader& event) {
    XrPassthroughStateChangedFlagsFB passthroughState = reinterpret_cast<const XrEventDataPassthroughStateChangedFB&>(event).flags;
    HandleEventResult result = HandleEventResult::NoError;

    if ((passthroughState & XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB) ||
        (passthroughState & XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB)) {
        result = HandleEventResult::NonRecoverableError;
        mIsInErrorState = true;
        if (passthroughHandle != XR_NULL_HANDLE && OpenXRExtensions::sXrDestroyPassthroughFB) {
            MessageXrResult(OpenXRExtensions::sXrDestroyPassthroughFB(passthroughHandle),
                            "xrDestroyPassthroughFB", FILE_AND_LINE);
            passthroughHandle = XR_NULL_HANDLE;
        }
    }
    if (passthroughState & XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB) {
        result = HandleEventResult::NeedsReinit;
        mIsInErrorState = false;
    } else if ((passthroughState & XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB)) {
        mIsInErrorState = false;
    } else {
        // XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB
        mIsInErrorState = true;
    }
    return result;
}

void
OpenXRPassthroughStrategyFBExtension::setLayerEnabled(XrPassthroughLayerFB layer, bool enabled) const {
    if (layer == XR_NULL_HANDLE)
        return;
    if ((enabled && !OpenXRExtensions::sXrPassthroughLayerResumeFB) ||
        (!enabled && !OpenXRExtensions::sXrPassthroughLayerPauseFB)) {
        VRB_WARN("OpenXR passthrough layer %s function is unavailable",
                 enabled ? "resume" : "pause");
        return;
    }
    const XrResult result = enabled
        ? OpenXRExtensions::sXrPassthroughLayerResumeFB(layer)
        : OpenXRExtensions::sXrPassthroughLayerPauseFB(layer);
    MessageXrResult(result, enabled ? "xrPassthroughLayerResumeFB" : "xrPassthroughLayerPauseFB",
                    FILE_AND_LINE);
}

bool
OpenXRPassthroughStrategyFBExtension::isReady() const {
    return !mIsInErrorState && passthroughHandle != XR_NULL_HANDLE;
}

OpenXRLayerPassthroughPtr
OpenXRPassthroughStrategyFBExtension::createLayerIfSupported() const {
    return OpenXRLayerPassthrough::Create(passthroughHandle);
}

OpenXRPassthroughStrategyQIYUExtension::~OpenXRPassthroughStrategyQIYUExtension() {
    if (passthroughHandle != XR_NULL_HANDLE && OpenXRExtensions::sXrDestroyPassthroughQIYU) {
        MessageXrResult(OpenXRExtensions::sXrDestroyPassthroughQIYU(passthroughHandle),
                        "xrDestroyPassthroughQIYU", FILE_AND_LINE);
        passthroughHandle = XR_NULL_HANDLE;
    }
}

void
OpenXRPassthroughStrategyQIYUExtension::initializePassthrough(XrSession session) {
    if (session == XR_NULL_HANDLE)
        return;

    XrPassthroughCreateInfoFB createInfo = {
        .type = OpenXRExtensions::kXrTypePassthroughCreateInfoQIYU,
        .next = XR_NULL_HANDLE,
        .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
    };
    XrResult result = OpenXRExtensions::sXrCreatePassthroughQIYU(session, &createInfo, &passthroughHandle);
    if (XR_FAILED(result)) {
        VRB_ERROR("Qiyu passthrough create failed: %s", to_string(result));
        mIsInErrorState = true;
        return;
    }

    // Qiyu records the creation flag but does not start the camera until this
    // explicit call has succeeded.
    result = OpenXRExtensions::sXrPassthroughStartQIYU(passthroughHandle);
    if (XR_FAILED(result)) {
        VRB_ERROR("Qiyu passthrough start failed: %s", to_string(result));
        mIsInErrorState = true;
    }
}

void
OpenXRPassthroughStrategyQIYUExtension::setLayerEnabled(XrPassthroughLayerFB layer, bool enabled) const {
    if (layer == XR_NULL_HANDLE)
        return;
    if ((enabled && !OpenXRExtensions::sXrPassthroughLayerResumeQIYU) ||
        (!enabled && !OpenXRExtensions::sXrPassthroughLayerPauseQIYU)) {
        VRB_WARN("Qiyu passthrough layer %s function is unavailable",
                 enabled ? "resume" : "pause");
        return;
    }
    const XrResult result = enabled
        ? OpenXRExtensions::sXrPassthroughLayerResumeQIYU(layer)
        : OpenXRExtensions::sXrPassthroughLayerPauseQIYU(layer);
    MessageXrResult(result, enabled ? "xrPassthroughLayerResumeQIYU" : "xrPassthroughLayerPauseQIYU",
                    FILE_AND_LINE);
}

void
OpenXRPassthroughStrategyQIYUExtension::configureLayer(XrPassthroughLayerFB layer) const {
    XrPassthroughStyleFB style = {
        .type = OpenXRExtensions::kXrTypePassthroughStyleQIYU,
        .next = XR_NULL_HANDLE,
        .textureOpacityFactor = 1.0f,
        .edgeColor = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    if (layer != XR_NULL_HANDLE && OpenXRExtensions::sXrPassthroughLayerSetStyleQIYU) {
        MessageXrResult(OpenXRExtensions::sXrPassthroughLayerSetStyleQIYU(layer, &style),
                        "xrPassthroughLayerSetStyleQIYU", FILE_AND_LINE);
    }
}

bool
OpenXRPassthroughStrategyQIYUExtension::isReady() const {
    return !mIsInErrorState && passthroughHandle != XR_NULL_HANDLE;
}

OpenXRLayerPassthroughPtr
OpenXRPassthroughStrategyQIYUExtension::createLayerIfSupported() const {
    return OpenXRLayerPassthrough::Create(passthroughHandle, true);
}

} // namespace crow
