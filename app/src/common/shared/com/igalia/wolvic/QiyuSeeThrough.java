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
 * <p>The service exposes a command intent. The action is important: starting the service by
 * explicit component alone leaves the service in its default mode and ignores the requested
 * operation. Mode 1 is the app-VST path, which keeps the Wolvic eye buffer visible over the
 * camera layer; mode 2 stops it.
 *
 * <ol>
 *   <li>Start {@code com.qiyi.seethroughservice/.SeethroughService} with action
 *       {@code org.freedesktop.monado.vst}.</li>
 *   <li>Send extra {@code org.freedesktop.monado.vst=1} to start, or {@code =2} to stop.</li>
 *   <li>Wait for {@code RgbCameraStatus} before reporting success to the renderer.</li>
 * </ol>
 *
 * <p>The provider at {@code content://com.iqiyi.vr.provider/vst/1} is
 * {@code RgbCameraStatus}. It is the only trustworthy success signal.
 */
public final class QiyuSeeThrough {
    private static final String LOGTAG = "QIYU";
    /**
     * Low-volume tag for the enable/disable round trip. "QIYU" also carries the per-frame
     * native logs, which chatty rate-limits away; a tag that only produces a handful of
     * lines always survives, so a failed attempt stays diagnosable from a user log.
     */
    private static final String VSTTAG = "QIYUVST";

    static final String SERVICE_PACKAGE = "com.qiyi.seethroughservice";
    static final String SERVICE_CLASS = "com.qiyi.seethroughservice.SeethroughService";
    private static final long STATUS_TIMEOUT_MS = 1500L;

    /**
     * The service's resolver entry and its mode extra use the same action string. Keep both
     * values explicit: a component-only start is accepted by Android but is ignored by the
     * service because its action is null.
     *
     * <pre>
     * am startservice -a org.freedesktop.monado.vst
     *     --ei org.freedesktop.monado.vst 1
     * </pre>
     */
    private static final String ACTION_VST = "org.freedesktop.monado.vst";

    /** Intent shapes used for the action path and old-firmware stop fallback. */
    private static final int VARIANT_ACTION = 0;
    private static final int VARIANT_COMPONENT = 1;

    /** Extra key read by {@code SeethroughService.onStartCommand}. */
    private static final String EXTRA_MODE = ACTION_VST;

    private static final int MODE_START = 1;
    private static final int MODE_STOP = 2;

    private static final Uri URI_RGB_CAMERA_STATUS =
            Uri.parse("content://com.iqiyi.vr.provider/vst/1");
    private static final Uri URI_APP_VST_STATUS =
            Uri.parse("content://com.iqiyi.vr.provider/vst/2");
    private static final Uri URI_SYSTEM_VST_STATUS =
            Uri.parse("content://com.iqiyi.vr.provider/vst/3");
    private static final String STATUS_COLUMN = "status_value";

    private QiyuSeeThrough() {}

    /**
     * Returns whether this device can do system see-through.
     *
     * <p>Since Android 11 a package lookup throws {@link
     * android.content.pm.PackageManager.NameNotFoundException} both when the
     * package is missing and when it is merely not visible to us, so a failure
     * cannot be read as "not supported". Doing that hid the passthrough
     * toggle. Assume the service exists; a real failure surfaces as a
     * SecurityException when the request is issued.
     */
    public static boolean isAvailable(Context aContext) {
        try {
            aContext.getPackageManager().getPackageInfo(SERVICE_PACKAGE, 0);
            Log.i(LOGTAG, "QIYU: see-through service present");
        } catch (Throwable t) {
            Log.i(LOGTAG, "QIYU: cannot verify " + SERVICE_PACKAGE + " (" + t
                    + "), assuming it is available");
        }
        logServiceDeclaration(aContext);
        return true;
    }

    /**
     * Logs how the system declares the see-through service: whether it is
     * exported and which actions it advertises. That turns "did the service
     * refuse us?" into something readable straight from logcat.
     */
    private static void logServiceDeclaration(Context aContext) {
        try {
            android.content.pm.PackageInfo info = aContext.getPackageManager().getPackageInfo(
                    SERVICE_PACKAGE, android.content.pm.PackageManager.GET_SERVICES);
            if (info == null || info.services == null || info.services.length == 0) {
                Log.i(LOGTAG, "QIYU: " + SERVICE_PACKAGE + " declares no visible services");
                return;
            }
            for (android.content.pm.ServiceInfo service : info.services) {
                if (service.name == null || !service.name.contains("Seethrough")) {
                    continue;
                }
                Log.i(LOGTAG, "QIYU: service " + service.name
                        + " exported=" + service.exported
                        + " permission=" + service.permission
                        + " process=" + service.processName);
            }
        } catch (Throwable t) {
            Log.i(LOGTAG, "QIYU: cannot inspect " + SERVICE_PACKAGE + " services: " + t);
        }
    }

    /**
     * Turns system see-through on or off. Blocking (it waits for the camera
     * status), so never call it from the UI thread.
     *
     * @return true when the cameras reported a running state (enable) or the
     *     stop request was accepted (disable).
     */
    public static boolean setEnabled(Context aContext, boolean aEnable) {
        if (!aEnable) {
            boolean stopped = sendMode(aContext, MODE_STOP);
            if (!stopped) {
                // Older firmware may expose only the component form for stopping. Never use
                // that form for enabling: action=null makes the service ignore the mode.
                stopped = sendMode(aContext, MODE_STOP, VARIANT_COMPONENT);
            }
            Log.w(VSTTAG, "VST stop, accepted=" + stopped + ", " + statusSummary(aContext));
            return stopped;
        }

        boolean invoked = sendMode(aContext, MODE_START);
        int rgb = invoked ? waitForCamera(aContext, STATUS_TIMEOUT_MS) : 0;
        Log.w(VSTTAG, "VST App start invoked=" + invoked + ", " + statusSummary(aContext));
        if (rgb > 0) {
            logSuccess(aContext, rgb);
            return true;
        }

        // Do not clear the eye buffer unless the service reported running cameras. Otherwise a
        // failed command would reproduce the black/empty scene seen on unsupported firmware.
        if (invoked) {
            sendMode(aContext, MODE_STOP);
        }
        Log.w(VSTTAG, "no App VST command started the cameras; keeping the opaque scene"
                + " " + statusSummary(aContext));
        return false;
    }

    /**
     * One line with the three provider statuses. {@code rgb} is the only signal that the
     * cameras are actually running; {@code app} is the value we write and {@code sys} is
     * what the platform reports back, which distinguishes "our write was ignored" from
     * "the platform accepted it but never opened the cameras".
     */
    private static String statusSummary(Context aContext) {
        return "rgb=" + readStatus(aContext, URI_RGB_CAMERA_STATUS)
                + " app=" + readStatus(aContext, URI_APP_VST_STATUS)
                + " sys=" + readStatus(aContext, URI_SYSTEM_VST_STATUS);
    }

    private static void logSuccess(Context aContext, int aRgb) {
        Log.w(VSTTAG, "VST running, rgb=" + aRgb
                + ", the eye buffer will now be cleared with alpha 0, "
                + statusSummary(aContext));
    }

    private static int waitForCamera(Context aContext, long aTimeoutMs) {
        long deadline = System.currentTimeMillis() + aTimeoutMs;
        int status = readStatus(aContext, URI_RGB_CAMERA_STATUS);
        while (status <= 0 && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
            status = readStatus(aContext, URI_RGB_CAMERA_STATUS);
        }
        return status;
    }

    private static boolean sendMode(Context aContext, int aMode) {
        return sendMode(aContext, aMode, VARIANT_ACTION);
    }

    /**
     * Sends a command to the exported service. The action variant is required by current Qiyu
     * firmware; the explicit-component variant is retained only as a stop fallback.
     */
    private static boolean sendMode(Context aContext, int aMode, int aVariant) {
        try {
            Intent intent = new Intent();
            if (aVariant == VARIANT_ACTION) {
                // Same shape as: am startservice -a org.freedesktop.monado.vst \
                //     --ei org.freedesktop.monado.vst <mode>
                intent.setAction(ACTION_VST);
                intent.setPackage(SERVICE_PACKAGE);
            } else {
                intent.setComponent(new ComponentName(SERVICE_PACKAGE, SERVICE_CLASS));
            }
            intent.putExtra(EXTRA_MODE, aMode);
            ComponentName started = aContext.startService(intent);
            // Warning level on purpose: chatty rate-limits this app's info lines away.
            Log.w(VSTTAG, "seethroughservice mode=" + aMode + " variant=" + aVariant
                    + " -> " + started);
            return started != null;
        } catch (Throwable t) {
            Log.w(VSTTAG, "seethroughservice mode=" + aMode + " variant=" + aVariant
                    + " failed: " + t);
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
