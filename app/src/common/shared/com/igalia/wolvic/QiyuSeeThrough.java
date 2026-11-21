/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package com.igalia.wolvic;

import android.content.ComponentName;
import android.content.ContentValues;
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
 * <p>Enabling therefore walks a ladder of the known mechanisms, cheapest and
 * best documented first, and stops as soon as {@code RgbCameraStatus} reports
 * that the cameras are actually running:
 *
 * <ol>
 *   <li>Start the service with a {@code mode} extra. Observed on device every
 *       time a Qiyu OpenXR application asked for passthrough
 *       ({@code SeethroughService: onStartCommand mode = 1}): {@code 3} =
 *       (re)apply the passthrough layer model, {@code 1} = start the cameras,
 *       {@code 2} = stop.</li>
 *   <li>The {@code com.qiyu.browser.vst.on} broadcast; FxR sends the
 *       {@code .off} counterpart when it disables passthrough.</li>
 *   <li>Writing {@code AppVstStatus} through the VST provider, which is what
 *       the vendor {@code SeeThroughHelper.writeAppStatus()} does.</li>
 *   <li>{@code SxrApi.sxrEnableSeeThrough()}, only present in the newer 5 MB
 *       {@code libsxrapi.so}.</li>
 * </ol>
 *
 * <p>The provider at {@code content://com.iqiyi.vr.provider/vst/1} is
 * {@code RgbCameraStatus}. It is the only trustworthy success signal: the
 * service start returns a {@link ComponentName} even when it does nothing.
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

    /**
     * The service reads the command from the {@code mode} extra. This is the key the on-device
     * {@code am startservice --ei mode N} test used; the list below is only a fallback.
     */
    private static final String EXTRA_MODE = "mode";
    private static final String[] EXTRA_MODE_ALIASES = {"mode", "cmd", "command", "vst_mode"};

    private static final int MODE_START = 1;
    private static final int MODE_STOP = 2;
    private static final int MODE_APPLY_MODEL = 3;

    private static final String ACTION_VST_ON = "com.qiyu.browser.vst.on";
    private static final String ACTION_VST_OFF = "com.qiyu.browser.vst.off";

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
            sendBroadcast(aContext, ACTION_VST_OFF);
            writeAppVstStatus(aContext, 0);
            Log.i(VSTTAG, "VST stop, service=" + stopped + ", " + statusSummary(aContext));
            return stopped;
        }

        // 1. Direct service command, exactly as `am startservice --ei mode 1` builds it.
        sendMode(aContext, MODE_APPLY_MODEL);
        boolean serviceAccepted = sendMode(aContext, MODE_START);
        int rgb = waitForCamera(aContext, 1800);
        Log.i(VSTTAG, "VST attempt 1 (service start, accepted=" + serviceAccepted + ") "
                + statusSummary(aContext));
        if (rgb > 0) {
            logSuccess(aContext, rgb);
            return true;
        }

        // 1b. Same command plus the vendor alias keys, for a firmware that reads another one.
        sendMode(aContext, MODE_APPLY_MODEL, true);
        sendMode(aContext, MODE_START, true);
        rgb = waitForCamera(aContext, 1200);
        Log.i(VSTTAG, "VST attempt 1b (service start, alias extras) "
                + statusSummary(aContext));
        if (rgb > 0) {
            logSuccess(aContext, rgb);
            return true;
        }

        // 2. Broadcast companion of FxR's "com.qiyu.browser.vst.off".
        sendBroadcast(aContext, ACTION_VST_ON);
        rgb = waitForCamera(aContext, 900);
        Log.i(VSTTAG, "VST attempt 2 (broadcast " + ACTION_VST_ON + ") "
                + statusSummary(aContext));
        if (rgb > 0) {
            logSuccess(aContext, rgb);
            return true;
        }

        // 3. Vendor status write (SeeThroughHelper.writeAppStatus).
        boolean wrote = writeAppVstStatus(aContext, 1);
        rgb = waitForCamera(aContext, 900);
        Log.i(VSTTAG, "VST attempt 3 (AppVstStatus=1, wrote=" + wrote + ") "
                + statusSummary(aContext));
        if (rgb > 0) {
            logSuccess(aContext, rgb);
            return true;
        }

        // 4. Vendor SXR entry point, only in the newer libsxrapi. On Qiyu X this is where
        // the runtime hooks in, so the failure reason matters more than the others.
        boolean sxr = sxrEnableSeeThrough(true);
        rgb = waitForCamera(aContext, 900);
        Log.w(VSTTAG, "VST attempt 4 (sxrEnableSeeThrough, called=" + sxr + ") "
                + statusSummary(aContext));
        if (rgb > 0) {
            logSuccess(aContext, rgb);
            return true;
        }

        // Nothing started the cameras. Roll everything back so the scene stays
        // opaque instead of clearing to transparent over a black world.
        Log.w(VSTTAG, "no VST path started the cameras; keeping the opaque scene"
                + " (service=" + serviceAccepted + ", sxr=" + sxr + ") "
                + statusSummary(aContext));
        sendMode(aContext, MODE_STOP);
        sendBroadcast(aContext, ACTION_VST_OFF);
        writeAppVstStatus(aContext, 0);
        sxrEnableSeeThrough(false);
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
        Log.i(VSTTAG, "VST running, rgb=" + aRgb
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
        return sendMode(aContext, aMode, false);
    }

    /**
     * Starts the see-through service with the {@code mode} command.
     *
     * <p>The plain form is byte-for-byte what {@code am startservice -n <pkg>/<cls> --ei mode N}
     * builds, which is the only shape verified to reach the service on device: a 1.11.13
     * build that also added the alias keys below always arrived as {@code mode = 0}, while the
     * adb command produced {@code mode = 3} / {@code mode = 1} and then
     * {@code VstManager: startCamera: started} with {@code RgbCameraStatus = 1}. The aliases
     * are kept as a second attempt only.
     */
    private static boolean sendMode(Context aContext, int aMode, boolean aAliases) {
        try {
            Intent intent = new Intent();
            intent.setComponent(new ComponentName(SERVICE_PACKAGE, SERVICE_CLASS));
            intent.putExtra(EXTRA_MODE, aMode);
            if (aAliases) {
                for (String key : EXTRA_MODE_ALIASES) {
                    if (!EXTRA_MODE.equals(key)) {
                        intent.putExtra(key, aMode);
                    }
                }
            }
            ComponentName started = aContext.startService(intent);
            Log.i(VSTTAG, "seethroughservice mode=" + aMode + (aAliases ? " (aliases)" : "")
                    + " -> " + started);
            return started != null;
        } catch (Throwable t) {
            Log.w(VSTTAG, "seethroughservice mode=" + aMode + " failed: " + t);
            return false;
        }
    }

    private static void sendBroadcast(Context aContext, String aAction) {
        try {
            aContext.sendBroadcast(new Intent(aAction));
            Log.i(LOGTAG, "QIYU: broadcast " + aAction);
        } catch (Throwable t) {
            Log.w(LOGTAG, "QIYU: broadcast " + aAction + " failed: " + t);
        }
    }

    private static boolean writeAppVstStatus(Context aContext, int aValue) {
        try {
            ContentValues values = new ContentValues();
            values.put(STATUS_COLUMN, String.valueOf(aValue));
            int rows = aContext.getContentResolver()
                    .update(URI_APP_VST_STATUS, values, null, null);
            Log.i(VSTTAG, "AppVstStatus=" + aValue + " rows=" + rows);
            return rows > 0;
        } catch (Throwable t) {
            Log.w(VSTTAG, "cannot write AppVstStatus=" + aValue + ": " + t);
            return false;
        }
    }

    private static boolean sxrEnableSeeThrough(boolean aEnable) {
        try {
            Class<?> sxr = Class.forName("com.qualcomm.sxrapi.SxrApi");
            sxr.getMethod("sxrEnableSeeThrough", boolean.class).invoke(null, aEnable);
            Log.i(VSTTAG, "sxrEnableSeeThrough(" + aEnable + ")");
            return true;
        } catch (Throwable t) {
            Log.i(VSTTAG, "sxrEnableSeeThrough unavailable: " + t);
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
