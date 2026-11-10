/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef VRBROWSER_MOZ_EXTERNAL_VR_SELECTOR_H
#define VRBROWSER_MOZ_EXTERNAL_VR_SELECTOR_H

// The external VR shared-memory protocol must match the GeckoView we link.
//
// Upstream Wolvic links a GeckoView built from Igalia/wolvic-gecko-patches,
// which carries protocol v21 (SHMEM_VERSION "0.0.13"). That is the default.
//
// Qiyu builds use the prebuilt Maven GeckoView (stock Firefox 153), whose
// protocol is v19 (SHMEM_VERSION "0.0.12"). Gecko silently rejects a shmem
// whose version/size differ, which makes navigator.xr.isSessionSupported()
// return false for immersive sessions while inline WebXR still works.
#if defined(WOLVIC_GECKOVIEW_EXTERNAL_VR_19)
#include "moz_external_vr_v19.h"
#else
#include "moz_external_vr_v21.h"
#endif

#endif  // VRBROWSER_MOZ_EXTERNAL_VR_SELECTOR_H
