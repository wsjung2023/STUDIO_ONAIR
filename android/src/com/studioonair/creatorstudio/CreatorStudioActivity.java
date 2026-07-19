package com.studioonair.creatorstudio;

import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;

import org.qtproject.qt.android.bindings.QtActivity;

/** Owns the one user-visible MediaProjection consent request for this activity. */
public class CreatorStudioActivity extends QtActivity {
    private static final int REQUEST_MEDIA_PROJECTION = 0x4353;
    private static CreatorStudioActivity activeActivity;

    private long pendingGeneration;

    public static native void nativeProjectionResult(long generation, boolean granted);

    public static void requestProjection(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeProjectionResult(generation, false);
            return;
        }
        activity.runOnUiThread(() -> activity.requestProjectionOnUiThread(generation));
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        activeActivity = this;
    }

    @Override
    protected void onDestroy() {
        if (activeActivity == this) {
            activeActivity = null;
        }
        super.onDestroy();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_MEDIA_PROJECTION) {
            return;
        }

        final long generation = pendingGeneration;
        pendingGeneration = 0;
        if (generation == 0) {
            return;
        }
        nativeProjectionResult(generation, resultCode == RESULT_OK && data != null);
    }

    private void requestProjectionOnUiThread(long generation) {
        if (generation == 0 || pendingGeneration != 0) {
            nativeProjectionResult(generation, false);
            return;
        }
        final MediaProjectionManager manager =
            (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (manager == null) {
            nativeProjectionResult(generation, false);
            return;
        }
        pendingGeneration = generation;
        startActivityForResult(manager.createScreenCaptureIntent(), REQUEST_MEDIA_PROJECTION);
    }
}
