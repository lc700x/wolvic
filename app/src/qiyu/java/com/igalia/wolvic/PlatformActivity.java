/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package com.igalia.wolvic;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import com.igalia.wolvic.ui.widgets.WidgetManagerDelegate;
import com.igalia.wolvic.utils.DeviceType;
import com.igalia.wolvic.utils.SystemUtils;

public class PlatformActivity extends NativeActivity {
    static String LOGTAG = SystemUtils.createLogtag(PlatformActivity.class);

    static boolean loadQiyuLibrary(String name) {
        try {
            System.loadLibrary(name);
            Log.i(LOGTAG, "Loaded QIYU/SXR library " + name);
            return true;
        } catch (Throwable t) {
            Log.w(LOGTAG, "Optional QIYU/SXR library " + name + " unavailable: " + t);
            return false;
        }
    }

    public static boolean filterPermission(final String aPermission) {
        return false;
    }

    public static boolean isPositionTrackingSupported() {
        return true;
    }

    protected Intent getStoreIntent() {
        return null;
    }

    protected String getEyeTrackingPermissionString() {
        return null;
    }

    private static boolean sSxrLibrariesLoaded = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Load the QiYu/SXR native libraries BEFORE NativeActivity starts the OpenXR thread.
        // This is the same ordering ALVR uses. The Monado/VR700 runtime also wants libsxrapi.so;
        // if the runtime wins the race, the app-side SxrApi class is marked erroneous
        // ("Shared library ... already opened by ClassLoader ...") and sxrInitialize fails,
        // leaving a black screen. Loading the app copy first lets the runtime reuse/copy it,
        // and lets the app-side SxrApi.<clinit> (System.loadLibrary("sxrapi")) complete later.
        //
        // Load order matters: core/base first, qiyuapi last. System.loadLibrary is required so
        // JNI_OnLoad initializes the QVR globals before qiyu_Init.
        if (BuildConfig.QIYU_CONTROLLER_BRIDGE) {
            sSxrLibrariesLoaded = loadQiyuLibrary("qiyivrsdkcore");
            sSxrLibrariesLoaded &= loadQiyuLibrary("sxrapi");
            sSxrLibrariesLoaded &= loadQiyuLibrary("ashreader");
            sSxrLibrariesLoaded &= loadQiyuLibrary("qiyuapi");
        }

        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setFullScreen();

        if (BuildConfig.QIYU_CONTROLLER_BRIDGE) {
            try {
                nativeSetSxrLibrariesLoaded(sSxrLibrariesLoaded);

                int deviceType = nativeGetQiyuDeviceType();
                boolean qiyuX = deviceType == DeviceType.QiyuX || DeviceType.isQiyuXModel();
                // Enable the controller bridge for all QIYU devices (ALVR v20 OpenXR-path behavior).
                // The QIYU OpenXR runtime (Monado: QIYIVR) only exposes khr/simple_controller
                // (select+menu), so full controller pose/buttons must come from qiyuapi.
                // qiyu_Init itself is deferred and gated on SXR libs being loaded in native code,
                // so enabling here is safe even if one SXR lib failed (Init will skip).
                nativeSetQiyuControllerBridgeEnabled(true);
                // Retain the Activity for the deferred qiyu_Init call. The bridge initializes
                // lazily from OpenXRInputSource once the OpenXR session is running; calling
                // qiyu_Init here would race the runtime's own SXR initialization.
                nativeSetQiyuActivity();

                Log.i(LOGTAG, "QIYU: deviceType=" + deviceType + (qiyuX ? " (Qiyu X)" : "") +
                        " -> QIYU controller bridge enabled for all clients (deferred until OpenXR is ready)" +
                        " sxrLoaded=" + sSxrLibrariesLoaded);
            } catch (Throwable t) {
                Log.w(LOGTAG, "QIYU controller bridge init unavailable: " + t);
            }
        } else {
            Log.i(LOGTAG, "QIYU: OpenXR-only build; Qiyu SDK controller bridge not packaged");
        }
    }

    @Override
    public void onBackPressed() {
        queueRunnable(new Runnable() {
            @Override
            public void run() {
                platformExit();
            }
        });
    }

    @Override
    protected void onResume() {
        setFullScreen();
        super.onResume();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            setFullScreen();
        }
    }

    protected void setFullScreen() {
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        getWindow().getDecorView().setSystemUiVisibility(flags);
    }

    public final PlatformActivityPlugin createPlatformPlugin(WidgetManagerDelegate delegate) {
        return null;
    }

    protected native void queueRunnable(Runnable aRunnable);
    protected native boolean platformExit();
    private static native int nativeGetQiyuDeviceType();
    private static native void nativeSetQiyuControllerBridgeEnabled(boolean enabled);
    private static native void nativeSetSxrLibrariesLoaded(boolean loaded);
    private native void nativeSetQiyuActivity();
}
