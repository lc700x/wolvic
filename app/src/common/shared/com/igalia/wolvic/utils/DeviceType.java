package com.igalia.wolvic.utils;

import android.content.Context;
import android.os.Build;
import android.util.Log;

import androidx.annotation.IntDef;

import com.igalia.wolvic.R;
import com.igalia.wolvic.BuildConfig;

public class DeviceType {
    // These values need to match those in Device.h
    @IntDef(value = {Unknown, OculusGo, OculusQuest, ViveFocus, ViveFocusPlus, PicoNeo2, PicoG2, PicoNeo3, OculusQuest2, HVR3DoF, HVR6DoF, Pico4x, MetaQuestPro, LynxR1, LenovoA3, LenovoVRX, MagicLeap2, MetaQuest3, VisionGlass, Pico4U, PfdmYVR1, PfdmYVR2, PfdmMR, Qiyu3, QiyuDream, QiyuDreamPro, QiyuMix, QiyuX})
    public @interface Type {}
    public static final int Unknown = 0;
    public static final int OculusGo = 1;
    public static final int OculusQuest = 2;
    public static final int ViveFocus = 3;
    public static final int ViveFocusPlus = 4;
    public static final int PicoNeo2 = 6;
    public static final int PicoG2 = 7;
    public static final int PicoNeo3 = 8;
    public static final int OculusQuest2 = 9;
    public static final int HVR3DoF = 10;
    public static final int HVR6DoF = 11;
    public static final int Pico4x = 12;
    public static final int MetaQuestPro = 13;
    public static final int LynxR1 = 14;
    public static final int LenovoA3 = 15;
    public static final int LenovoVRX = 16;
    public static final int MagicLeap2 = 17;
    public static final int MetaQuest3 = 18;
    public static final int VisionGlass = 19;
    public static final int Pico4U = 20;
    public static final int PfdmYVR1 = 21;
    public static final int PfdmYVR2 = 22;
    public static final int PfdmMR = 23;
    public static final int Qiyu3 = 24;
    public static final int QiyuDream = 25;
    public static final int QiyuDreamPro = 26;
    public static final int QiyuMix = 27;
    public static final int QiyuX = 28;

    private static @Type int mType = Unknown;
    private static String mDeviceName = "Unknown Device";

    public static void setType(@Type int aType) {
        switch (aType) {
            case OculusGo:
                mDeviceName = "Oculus Go";
                break;
            case OculusQuest:
                mDeviceName = "Oculus Quest";
                break;
            case OculusQuest2:
                mDeviceName = "Oculus Quest 2";
                break;
            case MetaQuestPro:
                mDeviceName = "Meta Quest Pro";
                break;
            case ViveFocus:
                mDeviceName = "Vive Focus";
                break;
            case ViveFocusPlus:
                mDeviceName = "Vive Focus Plus";
                break;
            case PicoNeo2:
                mDeviceName = "Pico Neo 2";
                break;
            case PicoNeo3:
                mDeviceName = "Pico Neo 3";
                break;
            case PicoG2:
                mDeviceName = "Pico G2";
                break;
            case Pico4x:
                mDeviceName = "Pico 4/4E";
                break;
            case Pico4U:
                mDeviceName = "Pico 4U";
                break;
            case LynxR1:
                mDeviceName = "Lynx-R1";
                break;
            case LenovoA3:
                mDeviceName = "Lenovo A3";
                break;
            case LenovoVRX:
                mDeviceName = "Lenovo VRX";
                break;
            case MagicLeap2:
                mDeviceName = "Magic Leap 2";
                break;
            case MetaQuest3:
                mDeviceName = "Meta Quest 3";
                break;
            case PfdmYVR1:
                mDeviceName = "PFDM YVR1";
                break;
            case PfdmYVR2:
                mDeviceName = "PFDM YVR2";
                break;
            case PfdmMR:
                mDeviceName = "PFDM MR";
                break;
            case Qiyu3:
                mDeviceName = "QIYU 3";
                break;
            case QiyuDream:
                mDeviceName = "QIYU Dream";
                break;
            case QiyuDreamPro:
                mDeviceName = "QIYU Dream Pro";
                break;
            case QiyuMix:
                mDeviceName = "QIYU MIX";
                break;
            case QiyuX:
                mDeviceName = "QIYU X";
                break;
            default:
                mDeviceName = "Unknown Device";
                break;
        }
        Log.d("VRB", "Setting device type to: " + mDeviceName);
        mType = aType;
    }
    public static @Type int getType() {
        return mType;
    }

    public static boolean isOculusBuild() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("oculusvr");
    }

    public static boolean isOculus6DOFBuild() {
        return BuildConfig.FLAVOR_platform.equalsIgnoreCase("oculusvr") || BuildConfig.FLAVOR_platform.equalsIgnoreCase("oculusvrStore");
    }

    public static boolean isHVRBuild() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("hvr");
    }

    public static boolean isPicoXR() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("picoxr");
    }

    public static boolean isLynx() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("lynx");
    }

    public static boolean isSnapdragonSpaces() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("spaces");
    }
    
    public static boolean isPfdmXR() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("pfdmxr");
    }

    public static boolean isQiyu() {
        return BuildConfig.FLAVOR_platform.toLowerCase().contains("qiyu");
    }

    // Best-effort Qiyu X detection for the optional qiyuapi controller bridge.
    // Qiyu X is the only known Qiyu model whose OpenXR runtime does not provide
    // the complete controller input surface. Dream/VR700/Monado runtimes own the
    // SXR stack themselves, so loading the app-side SDK there breaks presentation.
    public static boolean isQiyuXModel() {
        String model = safeUpper(Build.MODEL);
        String device = safeUpper(Build.DEVICE);
        String product = safeUpper(Build.PRODUCT);
        String manufacturer = safeUpper(Build.MANUFACTURER);
        if (model.contains("MIX") || device.contains("MIX") || product.contains("MIX")) {
            return false;
        }
        boolean qiyu = model.contains("QIYU") || model.contains("IQIYI") ||
                device.contains("QIYU") || device.contains("IQIYI") ||
                product.contains("QIYU") || product.contains("IQIYI") ||
                manufacturer.contains("IQIYI");
        boolean x = model.endsWith("X") || model.contains(" X") || model.contains("QIYUX") || model.contains("X1") ||
                device.endsWith("X") || device.contains(" X") || device.contains("QIYUX") ||
                product.endsWith("X") || product.contains(" X") || product.contains("QIYUX");
        return qiyu && x;
    }

    private static String safeUpper(String value) {
        return value == null ? "" : value.toUpperCase(java.util.Locale.ROOT);
    }

    public static String getDeviceTypeId() {
        String type = BuildConfig.FLAVOR_platform;
        if (DeviceType.isOculusBuild()) {
            type = "oculusvr";
        } else if (DeviceType.isPicoXR()) {
            type = "picoxr";
        } else if (DeviceType.isLynx()) {
            type = "lynx";
        } else if (DeviceType.isSnapdragonSpaces()) {
            type = "spaces";
        } else if (DeviceType.isPfdmXR()) {
            type = "pfdmxr";
        } else if (DeviceType.isQiyu()) {
            type = "qiyu";
        }

        return type;
    }

    // Identifiers for store-specific builds.
    public enum StoreType {NONE, META_STORE, MAINLAND_CHINA}

    public static StoreType getStoreType() {
        if (BuildConfig.FLAVOR_store.toLowerCase().contains("metastore"))
            return StoreType.META_STORE;
        else if (BuildConfig.FLAVOR_store.toLowerCase().contains("mainlandchina"))
            return StoreType.MAINLAND_CHINA;
        else
            return StoreType.NONE;
    }

    public static String getDeviceName(Context aContext) {
        String appName = aContext.getString(R.string.app_name);
        String deviceName = mDeviceName;
        if (mType == DeviceType.Unknown) {
            deviceName = Build.MANUFACTURER + " " + Build.MODEL;
        }
        return aContext.getString(R.string.device_name, appName, deviceName);
    }

    public static boolean isTetheredDevice() {
        return mType == HVR3DoF || mType == HVR6DoF || mType == VisionGlass || mType == LenovoA3;
    }
}
