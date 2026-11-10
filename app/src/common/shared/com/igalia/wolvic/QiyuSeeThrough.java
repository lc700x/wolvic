/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package com.igalia.wolvic;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

/**
 * Controls the Qiyu system see-through (VST) pipeline.
 *
 * <p>On Qiyu devices passthrough is not owned by the rendering app. The system
 * camera service {@code com.qiyi.seethroughservice} starts the RGB cameras and
 * the SXR compositor blends them behind the application eye buffer, so the app
 * only has to clear its eye buffer with an alpha of 0 where the real world has
 * to show through.
 *
 * <p>The service is driven with an explicit start whose {@code mode} extra
 * selects the operation. This sequence was observed on a Qiyu X device log
 * ({@code SeethroughService: onStartCommand mode = N}) every time a Qiyu
 * OpenXR application asked for passthrough:
 *
 * <ul>
 *   <li>3 &mdash; (re)apply the passthrough layer model</li>
 *   <li>1 &mdash; start the RGB cameras</li>
 *   <li>2 &mdash; stop the RGB cameras</li>
 * </ul>
 *
 * <p>The provider at {@code content://com.iqiyi.vr.provider/vst/1} reports
 * {@code RgbCameraStatus} and is only <em>read</em> here to confirm that the
 * cameras really started. Writing {@code AppVstStatus}
 * ({@code .../vst/2}) does not start anything: it is a status report, not a
 * command, which is why writing it in earlier builds left the camera off.
 */
public final class QiyuSeeThrough {
    private static final String LOGTAG = "QIYU";

    static final String SERVICE_PACKAGE = "com.qiyi.seethroughservice";
    static final String SERVICE_CLASS = "com.qiyi.seethroughservice.SeethroughService";
    private static final String EXTRA_MODE = "mode";

    private static final int MODE_START = 1;
    private static final int MODE_STOP = 2;
    private static final int MODE_APPLY_MODEL = 3;

    private static final Uri URI_RGB_CAMERA_STATUS =
            Uri.parse("content://com.iqiyi.vr.provider/vst/1");
    private static final Uri URI_APP_VST_STATUS =
            Uri.parse("content://com.iqiyi.vr.provider/vst/2");

    private static final String STATUS_COLUMN = "status_value";
    private static final long CAMERA_START_TIMEOUT_MS = 2500;
    private static final long CAMERA_POLL_INTERVAL_MS = 100;

    private QiyuSeeThrough() {}

    /**
     * Returns whether this device looks like it can do system see-through. A
     * failure other than "package not found" is treated as available so a
     * package-visibility restriction never hides the toggle.
     */
    public static boolean isAvailable(Context aContext) {
        try {
            aContext.getPackageManager().getPackageInfo(SERVICE_PACKAGE, 0);
            return true;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            return false;
        } catch (Throwable t) {
            return true;
        }
    }

    /**
     * Turns system see-through on or off. Blocking: it waits for the camera
     * status, so never call it from the UI thread.
     *
     * @return true when the request reached the system and, for {@code
     *     aEnable}, the cameras reported a running state.
     */
    public static boolean setEnabled(Context aContext, boolean aEnable) {
        if (!aEnable) {
            boolean stopped = sendMode(aContext, MODE_STOP);
            Log.i(LOGTAG, "QIYU: VST stop requested, service=" + stopped
                    + ", rgb=" + readStatus(aContext, URI_RGB_CAMERA_STATUS)
                    + ", app=" + readStatus(aContext, URI_APP_VST_STATUS));
            return stopped;
        }

        sendMode(aContext, MODE_APPLY_MODEL);
        if (!sendMode(aContext, MODE_START)) {
            return false;
        }

        int rgb = waitForCamera(aContext, CAMERA_START_TIMEOUT_MS);
        Log.i(LOGTAG, "QIYU: VST start requested, rgb=" + rgb
                + ", app=" + readStatus(aContext, URI_APP_VST_STATUS));

        if (rgb <= 0) {
            // The cameras never came up. Leaving passthrough "enabled" would
            // clear the eye buffer to transparent and show a black scene, so
            // stop the service again and keep the opaque scene.
            Log.w(LOGTAG, "QIYU: RGB cameras did not start; keeping the opaque scene");
            sendMode(aContext, MODE_STOP);
            return false;
        }
        return true;
    }

    private static int waitForCamera(Context aContext, long aTimeoutMs) {
        long deadline = System.currentTimeMillis() + aTimeoutMs;
        int status = readStatus(aContext, URI_RGB_CAMERA_STATUS);
        while (status <= 0 && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(CAMERA_POLL_INTERVAL_MS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
            status = readStatus(aContext, URI_RGB_CAMERA_STATUS);
        }
        return status;
    }

    private static boolean sendMode(Context aContext, int aMode) {
        try {
            Intent intent = new Intent();
            intent.setComponent(new ComponentName(SERVICE_PACKAGE, SERVICE_CLASS));
            intent.putExtra(EXTRA_MODE, aMode);
            ComponentName started = aContext.startService(intent);
            Log.i(LOGTAG, "QIYU: seethroughservice mode=" + aMode + " -> " + started);
            return started != null;
        } catch (Throwable t) {
            Log.w(LOGTAG, "QIYU: seethroughservice mode=" + aMode + " failed: " + t);
            return false;
        }
    }

    private static int readStatus(Context aContext, Uri aUri) {
        try (Cursor cursor = aContext.getContentResolver()
                .query(aUri, new String[] {STATUS_COLUMN}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(STATUS_COLUMN);
                if (index >= 0) {
                    String value = cursor.getString(index);
                    if (value != null) {
                        return Integer.parseInt(value.trim());
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(LOGTAG, "QIYU: cannot read " + aUri + ": " + t);
        }
        return 0;
    }
}
