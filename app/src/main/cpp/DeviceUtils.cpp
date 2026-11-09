/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DeviceUtils.h"
#include "vrb/Matrix.h"
#include "vrb/Quaternion.h"
#include "vrb/Transform.h"
#include "vrb/Vector.h"
#include "vrb/VertexArray.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

namespace crow {

std::unordered_map<std::string, device::DeviceType> DeviceUtils::deviceNamesMap;

vrb::Matrix
DeviceUtils::CalculateReorientationMatrix(const vrb::Matrix& aHeadTransform, const vrb::Vector& aHeightPosition) {
  return CalculateReorientationMatrixWithThreshold(aHeadTransform, aHeightPosition, 0.2f, 0.5f, 0.35f);
}

vrb::Matrix
DeviceUtils::CalculateReorientationMatrixOnHeadLock(const vrb::Matrix& aHeadTransform, const vrb::Vector& aHeightPosition) {
  return CalculateReorientationMatrixWithThreshold(aHeadTransform, aHeightPosition, 0.0f, 0.0f, 0.0f);
}

// CAUTION: using Euler angles is dangerous because they could cause gimbal lock issues.
vrb::Matrix
DeviceUtils::CalculateReorientationMatrixWithThreshold(const vrb::Matrix& aHeadTransform, const vrb::Vector& aHeightPosition, const float kPitchUpThreshold, const float kPitchDownThreshold, const float kRollThreshold) {
  float rx, ry, rz;
  vrb::Quaternion quat(aHeadTransform);
  quat.ToEulerAngles(rx, ry, rz);

  // Use some thresholds to use default roll and yaw values when the rotation is not enough.
  // This makes easier setting a default orientation easier for the user.
  if (rx > 0 && rx < kPitchDownThreshold) {
    rx = 0.0f;
  } else if (rx < 0 && fabsf(rx) < kPitchUpThreshold) {
    rx = 0.0f;
  } else {
    rx -= 0.05f; // It feels better with some extra margin
  }

  if (fabsf(rz) < kRollThreshold) {
    rz = 0.0f;
  }

  if (rx == 0.0f && rz == 0.0f) {
    return vrb::Matrix::Identity();
  }

  quat.SetFromEulerAngles(rx, ry, rz);
  vrb::Matrix result = vrb::Matrix::Rotation(quat.Inverse());

  // Rotate UI reorientation matrix from origin so user height translation doesn't affect the sphere.
  result.PreMultiplyInPlace(vrb::Matrix::Position(aHeightPosition));
  result.PostMultiplyInPlace(vrb::Matrix::Position(-aHeightPosition));

  return result;

}

vrb::Matrix
DeviceUtils::CalculateReorientationMatrixWithoutRoll(const vrb::Matrix& transform, const vrb::Vector& aHeightPosition) {
  auto forward = -transform.MultiplyDirection(vrb::Vector(0.0f, 0.0f, -1.0f));
  // Project forward on the XZ plane to compute a roll-free right vector. Then use the cross product
  // to compute the actual up vector. Rebuild the rotation matrix with up, right, forward vectors.
  auto forwardProjected = vrb::Vector(forward.x(), 0, forward.z()).Normalize();
  auto right = vrb::Vector(0,1,0).Cross(forwardProjected).Normalize();
  auto up = forward.Cross(right);

  auto result = vrb::Matrix(right.x(), right.y(), right.z(), 0.0f,
                            up.x(), up.y(), up.z(), 0.0f,
                            forward.x(), forward.y(), forward.z(), 0.0f,
                            0.0f, 0.0f, 0.0f, 1.0f);

  // Rotate UI reorientation matrix from origin so user height translation doesn't affect the sphere.
  result.PreMultiplyInPlace(vrb::Matrix::Position(aHeightPosition));
  result.PostMultiplyInPlace(vrb::Matrix::Position(-aHeightPosition));

  return result;
}

void DeviceUtils::GetTargetImmersiveSize(const uint32_t aRequestedWidth,
                                         const uint32_t aRequestedHeight,
                                         const uint32_t aRecommendedWidth,
                                         const uint32_t aRecommendedHeight,
                                         const uint32_t aMaxWidth, const uint32_t aMaxHeight,
                                         uint32_t& aTargetWidth, uint32_t& aTargetHeight) {
  const uint32_t minWidth = aRecommendedWidth / 2;
  const uint32_t minHeight = aRecommendedHeight / 2;
  aTargetWidth = (uint32_t) fmaxf(fminf(aRequestedWidth, aMaxWidth), minWidth);
  aTargetHeight = (uint32_t) fmaxf(fminf(aRequestedHeight, aMaxHeight), minHeight);
}

vrb::GeometryPtr DeviceUtils::GetSphereGeometry(vrb::CreationContextPtr& context, uint32_t resolution, float radius)
{
    vrb::VertexArrayPtr array = vrb::VertexArray::Create(context);
    vrb::GeometryPtr geometry = vrb::Geometry::Create(context);
    vrb::TransformPtr transform = vrb::Transform::Create(context);
    std::vector<int> indices;

    int rings = (int) resolution;
    int sectors = (int) resolution;
    float const R = 1.0f / ((float) rings - 1.0f);
    float const S = 1.0f / ((float) sectors - 1.0f);

    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < sectors; s++) {
            float const y = sinf(- (float) M_PI_2 + (float) M_PI * (float) r * R);
            float const x = cosf(2.0f * (float) M_PI * (float) s * S) * sinf(M_PI * (float) r * R);
            float const z = sinf(2.0f * (float) M_PI * (float) s * S) * sinf(M_PI * (float) r * R);
            array->AppendVertex(vrb::Vector(x * radius, y * radius, z * radius));
            array->AppendNormal(vrb::Vector(x, y, z).Normalize());
            array->AppendUV(vrb::Vector((float)s * S, (float)r * R, 0.0));
        }
    }

    geometry->SetVertexArray(array);

    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < sectors; s++) {
            if (r != 0) {
                indices.push_back(r * sectors + s);
                indices.push_back((r + 1) * sectors + s);
                indices.push_back(r * sectors + (s + 1));
                geometry->AddFace(indices, indices, indices);
                indices.clear();
            }

            if (r != rings - 1) {
                indices.push_back(r * sectors + (s + 1));
                indices.push_back((r + 1) * sectors + s);
                indices.push_back((r + 1) * sectors + (s + 1));
                geometry->AddFace(indices, indices, indices);
                indices.clear();
            }
        }
    }

    return std::move(geometry);
}

device::DeviceType DeviceUtils::GetDeviceTypeFromSystem() {
    char model[128];
    int length = PopulateDeviceModelString(model);

    static std::once_flag deviceNamesInitFlag;
    std::call_once(deviceNamesInitFlag, [] {
        deviceNamesMap.emplace("Quest", device::OculusQuest);
        deviceNamesMap.emplace("Quest 2", device::OculusQuest2);
        deviceNamesMap.emplace("Quest 3", device::MetaQuest3);
        // So far no need to differentiate between Quest 3 and Quest 3S
        deviceNamesMap.emplace("Quest 3S", device::MetaQuest3);
        // So far no need to differentiate between Pico4 and Pico4E
        deviceNamesMap.emplace("A8110", device::Pico4x);
        deviceNamesMap.emplace("A9210", device::Pico4U);
        deviceNamesMap.emplace("Lynx-R1", device::LynxR1);
        deviceNamesMap.emplace("motorola edge 30 pro", device::LenovoA3);
        deviceNamesMap.emplace("Quest Pro", device::MetaQuestPro);
        deviceNamesMap.emplace("VRX", device::LenovoVRX);
        deviceNamesMap.emplace("Magic Leap 2", device::MagicLeap2);
        deviceNamesMap.emplace("Pico Neo 3", device::PicoNeo3);
        deviceNamesMap.emplace("YVR 1", device::PfdmYVR1);
        deviceNamesMap.emplace("YVR 2", device::PfdmYVR2);
        deviceNamesMap.emplace("PFDM MR", device::PfdmMR);
        // QIYU / iQIYI all-in-one headsets (model strings vary by firmware/market)
        deviceNamesMap.emplace("QIYU 3", device::Qiyu3);
        deviceNamesMap.emplace("Qiyu 3", device::Qiyu3);
        deviceNamesMap.emplace("QIYU3", device::Qiyu3);
        deviceNamesMap.emplace("QIYU Dream", device::QiyuDream);
        deviceNamesMap.emplace("Qiyu Dream", device::QiyuDream);
        deviceNamesMap.emplace("QIYU Dream Pro", device::QiyuDreamPro);
        deviceNamesMap.emplace("Qiyu Dream Pro", device::QiyuDreamPro);
        deviceNamesMap.emplace("QIYU MIX", device::QiyuMix);
        deviceNamesMap.emplace("Qiyu MIX", device::QiyuMix);
        deviceNamesMap.emplace("QIYU Mix", device::QiyuMix);
        // Qiyu MIX firmware model (log 4: "Device A081 is not supported"); same family as Dream A091.
        deviceNamesMap.emplace("A081", device::QiyuMix);
        deviceNamesMap.emplace("A081 Mix", device::QiyuMix);
        deviceNamesMap.emplace("iQIYI-A081", device::QiyuMix);
        deviceNamesMap.emplace("iQIYI-A081-P", device::QiyuMix);
        deviceNamesMap.emplace("QIYU X", device::QiyuX);
        deviceNamesMap.emplace("Qiyu X", device::QiyuX);
        deviceNamesMap.emplace("QIYUX", device::QiyuX);
        // Firmware model numbers reported by some Qiyu Dream / Legion VR700 builds.
        deviceNamesMap.emplace("A091", device::QiyuDream);
        deviceNamesMap.emplace("iQIYI-A6-P", device::QiyuDream);
        deviceNamesMap.emplace("iQIYI-A6", device::QiyuDream);
        deviceNamesMap.emplace("Lenovo Legion VR700", device::QiyuDream);
        deviceNamesMap.emplace("Legion VR700", device::QiyuDream);
        deviceNamesMap.emplace("VR700", device::QiyuDream);
    });

    auto device = deviceNamesMap.find(model);
    if (device != deviceNamesMap.end()) {
        return device->second;
    }

    // Fallback heuristics for QIYU/iQIYI/Lenovo Legion VR700 naming variations.
    std::string modelName(model);
    auto containsIgnoreCase = [](const std::string& haystack, const char* needle) {
        auto it = std::search(haystack.begin(), haystack.end(), needle, needle + strlen(needle),
                              [](char a, char b) { return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b)); });
        return it != haystack.end();
    };
    if (containsIgnoreCase(modelName, "vr700") || containsIgnoreCase(modelName, "legion vr") ||
        (containsIgnoreCase(modelName, "lenovo") && containsIgnoreCase(modelName, "legion") && containsIgnoreCase(modelName, "vr")) ||
        containsIgnoreCase(modelName, "a091") || containsIgnoreCase(modelName, "iqiyi-a6") ||
        containsIgnoreCase(modelName, "iqiyi a6")) {
        return device::QiyuDream;
    } else if (containsIgnoreCase(modelName, "dream") && containsIgnoreCase(modelName, "pro")) {
        return device::QiyuDreamPro;
    } else if (containsIgnoreCase(modelName, "dream")) {
        return device::QiyuDream;
    } else if (containsIgnoreCase(modelName, "mix") || containsIgnoreCase(modelName, "a081") ||
               containsIgnoreCase(modelName, "iqiyi-a081") || containsIgnoreCase(modelName, "iqiyi a081")) {
        return device::QiyuMix;
    } else if ((containsIgnoreCase(modelName, "qiyu") || containsIgnoreCase(modelName, "iqiyi")) &&
               (containsIgnoreCase(modelName, " x") || containsIgnoreCase(modelName, "x1") ||
                modelName.back() == 'X' || modelName.back() == 'x')) {
        return device::QiyuX;
    } else if (containsIgnoreCase(modelName, "qiyu 3") || containsIgnoreCase(modelName, "qiyu3")) {
        return device::Qiyu3;
    } else if (containsIgnoreCase(modelName, "qiyu") || containsIgnoreCase(modelName, "iqiyi")) {
        return device::Qiyu3;
    }

    VRB_WARN("Device %s is not supported", model);
    return device::UnknownType;
}

}
