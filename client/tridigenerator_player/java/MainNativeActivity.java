/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * Licensed under the Oculus SDK License Agreement (the "License");
 * you may not use the Oculus SDK except in compliance with the License,
 * which is provided at the time of installation or download, or which
 * otherwise accompanies this software in either electronic or hard copy form.
 *
 * You may obtain a copy of the License at
 * https://developer.oculus.com/licenses/oculussdk/
 *
 * Unless required by applicable law or agreed to in writing, the Oculus SDK
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package io.github.samaust.tridigenerator_player;

import android.os.Bundle;
import android.os.Build;
import android.util.Log;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import java.util.ArrayList;
import java.util.List;

public class MainNativeActivity extends android.app.NativeActivity {
    private static final String COLOR_SETTINGS_PREFERENCES = "color_matching_settings";
    private static final String MESH_SETTINGS_PREFERENCES = "mesh_settings";
    private static final String MESH_DETAIL_DIVISOR = "detail_divisor";
    private static final int DEFAULT_MESH_DETAIL_DIVISOR = 2;
    private static final String DISPLAY_SETTINGS_PREFERENCES = "display_settings";
    private static final String REQUESTED_DISPLAY_REFRESH_RATE =
        "requested_display_refresh_rate";
    private static final float DEFAULT_DISPLAY_REFRESH_RATE = 72.0f;
    private static final String SCENE_PERMISSION = "com.oculus.permission.USE_SCENE";
    private static final String CAMERA_PERMISSION = "horizonos.permission.HEADSET_CAMERA";
    private static final String ANDROID_CAMERA_PERMISSION = "android.permission.CAMERA";
    private static final int PERMISSION_REQUEST_CODE = 2;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        Log.d(MainActivity.TAG, "MainNativeActivity.onCreate() called");
        super.onCreate(savedInstanceState);
        requestMissingPermissions();
    }

    private void requestMissingPermissions() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return;
        }
        List<String> missing = new ArrayList<>();
        if (checkSelfPermission(SCENE_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            missing.add(SCENE_PERMISSION);
        }
        boolean hasHeadsetCamera =
            checkSelfPermission(CAMERA_PERMISSION) == PackageManager.PERMISSION_GRANTED;
        boolean hasAndroidCamera =
            checkSelfPermission(ANDROID_CAMERA_PERMISSION) == PackageManager.PERMISSION_GRANTED;
        if (!hasHeadsetCamera && !hasAndroidCamera) {
            missing.add(CAMERA_PERMISSION);
            missing.add(ANDROID_CAMERA_PERMISSION);
        }
        if (!missing.isEmpty()) {
            Log.d(MainActivity.TAG, "Requesting XR scene and headset camera permissions");
            requestPermissions(missing.toArray(new String[0]), PERMISSION_REQUEST_CODE);
        }
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != PERMISSION_REQUEST_CODE) {
            return;
        }
        for (int i = 0; i < permissions.length; ++i) {
            boolean granted =
                i < grantResults.length && grantResults[i] == PackageManager.PERMISSION_GRANTED;
            Log.d(
                MainActivity.TAG,
                permissions[i] + (granted ? " granted" : " denied"));
        }
    }

    // Called from native code to safely quit app.
    public void onNativeFinish() {
        Log.d(MainActivity.TAG, "MainNativeActivity finish called from native app.");
        finishAndRemoveTask();
    }

    public String loadColorMatchingSettings(String datasetId) {
        return getSharedPreferences(COLOR_SETTINGS_PREFERENCES, Context.MODE_PRIVATE)
            .getString(datasetId, null);
    }

    public boolean saveColorMatchingSettings(String datasetId, String json) {
        SharedPreferences preferences =
            getSharedPreferences(COLOR_SETTINGS_PREFERENCES, Context.MODE_PRIVATE);
        return preferences.edit().putString(datasetId, json).commit();
    }

    public boolean deleteColorMatchingSettings(String datasetId) {
        SharedPreferences preferences =
            getSharedPreferences(COLOR_SETTINGS_PREFERENCES, Context.MODE_PRIVATE);
        return preferences.edit().remove(datasetId).commit();
    }

    public int loadMeshDetailDivisor() {
        return getSharedPreferences(MESH_SETTINGS_PREFERENCES, Context.MODE_PRIVATE)
            .getInt(MESH_DETAIL_DIVISOR, DEFAULT_MESH_DETAIL_DIVISOR);
    }

    public boolean saveMeshDetailDivisor(int divisor) {
        return getSharedPreferences(MESH_SETTINGS_PREFERENCES, Context.MODE_PRIVATE)
            .edit()
            .putInt(MESH_DETAIL_DIVISOR, divisor)
            .commit();
    }

    public float loadRequestedDisplayRefreshRate() {
        return getSharedPreferences(DISPLAY_SETTINGS_PREFERENCES, Context.MODE_PRIVATE)
            .getFloat(
                REQUESTED_DISPLAY_REFRESH_RATE,
                DEFAULT_DISPLAY_REFRESH_RATE);
    }

    public boolean saveRequestedDisplayRefreshRate(float refreshRate) {
        if (refreshRate != 72.0f && refreshRate != 90.0f) {
            return false;
        }
        return getSharedPreferences(DISPLAY_SETTINGS_PREFERENCES, Context.MODE_PRIVATE)
            .edit()
            .putFloat(REQUESTED_DISPLAY_REFRESH_RATE, refreshRate)
            .commit();
    }

    @Override
    public void onDestroy() {
        Log.d(MainActivity.TAG, "MainNativeActivity.onDestroy() called");
        super.onDestroy();
    }
}
