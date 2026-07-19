package com.studioonair.creatorstudio;

import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.media.projection.MediaProjection;
import android.media.Image;
import android.media.ImageReader;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.graphics.PixelFormat;
import android.os.Bundle;

import org.qtproject.qt.android.bindings.QtActivity;

/** Owns the one user-visible MediaProjection consent request for this activity. */
public class CreatorStudioActivity extends QtActivity {
    private static final int REQUEST_MEDIA_PROJECTION = 0x4353;
    private static CreatorStudioActivity activeActivity;

    private long pendingGeneration;
    private long approvedGeneration;
    private int projectionResultCode;
    private Intent projectionData;
    private MediaProjection projection;
    private ImageReader projectionReader;
    private VirtualDisplay projectionDisplay;

    public static native void nativeProjectionResult(long generation, boolean granted);
    public static native boolean nativeProjectionFrame(long generation, java.nio.ByteBuffer bytes,
        int width, int height, int rowStride, int pixelStride, long timestampNs);
    public static native void nativeProjectionRevoked(long generation);

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
        if (resultCode == RESULT_OK && data != null) {
            approvedGeneration = generation;
            projectionResultCode = resultCode;
            projectionData = data;
        }
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

    public static void startProjection(long generation, int width, int height) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity != null) activity.runOnUiThread(
            () -> activity.startProjectionOnUiThread(generation, width, height));
    }

    private void startProjectionOnUiThread(long generation, int width, int height) {
        if (generation != approvedGeneration || projectionData == null || projection != null) return;
        CreatorStudioProjectionService.start(this);
        MediaProjectionManager manager = (MediaProjectionManager)
            getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        projection = manager.getMediaProjection(projectionResultCode, projectionData);
        projection.registerCallback(new MediaProjection.Callback() {
            @Override public void onStop() { nativeProjectionRevoked(generation); releaseProjection(); }
        }, null);
        projectionReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
        projectionReader.setOnImageAvailableListener(reader -> {
            Image image = reader.acquireLatestImage();
            if (image == null) return;
            try {
                Image.Plane plane = image.getPlanes()[0];
                nativeProjectionFrame(generation, plane.getBuffer(), image.getWidth(), image.getHeight(),
                    plane.getRowStride(), plane.getPixelStride(), image.getTimestamp());
            } finally { image.close(); }
        }, null);
        projectionDisplay = projection.createVirtualDisplay("CreatorStudioCapture", width, height,
            getResources().getDisplayMetrics().densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, projectionReader.getSurface(), null, null);
    }

    private void releaseProjection() {
        if (projectionDisplay != null) { projectionDisplay.release(); projectionDisplay = null; }
        if (projectionReader != null) { projectionReader.close(); projectionReader = null; }
        if (projection != null) { projection.stop(); projection = null; }
        CreatorStudioProjectionService.stop(this);
    }
}
