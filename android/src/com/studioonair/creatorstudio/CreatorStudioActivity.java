package com.studioonair.creatorstudio;

import android.content.Context;
import android.content.Intent;
import android.Manifest;
import android.content.pm.PackageManager;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.media.projection.MediaProjectionManager;
import android.media.projection.MediaProjection;
import android.media.Image;
import android.media.ImageReader;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

import org.qtproject.qt.android.bindings.QtActivity;

/** Owns the one user-visible MediaProjection consent request for this activity. */
public class CreatorStudioActivity extends QtActivity {
    private static final int REQUEST_MEDIA_PROJECTION = 0x4353;
    private static final int REQUEST_CAMERA = 0x4354;
    private static final int REQUEST_MICROPHONE = 0x4355;
    private static final int CAMERA_KIND = 0;
    private static final int MICROPHONE_KIND = 1;
    private static CreatorStudioActivity activeActivity;

    private long pendingGeneration;
    private long approvedGeneration;
    private int projectionResultCode;
    private Intent projectionData;
    private MediaProjection projection;
    private ImageReader projectionReader;
    private VirtualDisplay projectionDisplay;
    private boolean releasingProjection;
    private long revokedGeneration;
    private long pendingCameraPermissionGeneration;
    private long pendingMicrophonePermissionGeneration;
    private HandlerThread cameraThread;
    private Handler cameraHandler;
    private CameraDevice cameraDevice;
    private CameraCaptureSession cameraSession;
    private ImageReader cameraReader;
    private long cameraGeneration;
    private AudioRecord microphoneRecord;
    private Thread microphoneThread;
    private volatile boolean microphoneRunning;
    private long microphoneGeneration;

    public static native void nativeProjectionResult(long generation, boolean granted);
    public static native boolean nativeProjectionFrame(long generation, java.nio.ByteBuffer bytes,
        int width, int height, int rowStride, int pixelStride, long timestampNs);
    public static native void nativeProjectionRevoked(long generation);
    public static native void nativeMediaPermissionResult(int kind, long generation, boolean granted);
    public static native void nativeCameraFrame(long generation, ByteBuffer bytes,
        int width, int height, long timestampNs);
    public static native void nativeMicrophonePcm16(long generation, ByteBuffer bytes,
        int sampleCount, int sampleRate, int channels, long timestampNs);
    public static native void nativeCameraFailed(long generation);
    public static native void nativeMicrophoneFailed(long generation);

    public static int mediaPermissionStatus(int kind) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) return 0;
        final String permission = kind == CAMERA_KIND ? Manifest.permission.CAMERA
                                                       : Manifest.permission.RECORD_AUDIO;
        return activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED ? 1 : 0;
    }

    public static void requestMediaPermission(int kind, long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) { nativeMediaPermissionResult(kind, generation, false); return; }
        activity.runOnUiThread(() -> activity.requestMediaPermissionOnUiThread(kind, generation));
    }

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
        stopCameraOnUiThread(cameraGeneration);
        stopMicrophoneOnUiThread(microphoneGeneration);
        if (activeActivity == this) {
            activeActivity = null;
        }
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        final boolean granted = grantResults.length == 1 &&
            grantResults[0] == PackageManager.PERMISSION_GRANTED;
        if (requestCode == REQUEST_CAMERA && pendingCameraPermissionGeneration != 0) {
            final long generation = pendingCameraPermissionGeneration;
            pendingCameraPermissionGeneration = 0;
            nativeMediaPermissionResult(CAMERA_KIND, generation, granted);
        } else if (requestCode == REQUEST_MICROPHONE && pendingMicrophonePermissionGeneration != 0) {
            final long generation = pendingMicrophonePermissionGeneration;
            pendingMicrophonePermissionGeneration = 0;
            nativeMediaPermissionResult(MICROPHONE_KIND, generation, granted);
        }
    }

    private void requestMediaPermissionOnUiThread(int kind, long generation) {
        final String permission = kind == CAMERA_KIND ? Manifest.permission.CAMERA
                                                       : Manifest.permission.RECORD_AUDIO;
        if (checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) {
            nativeMediaPermissionResult(kind, generation, true);
            return;
        }
        if (kind == CAMERA_KIND) {
            if (pendingCameraPermissionGeneration != 0) {
                nativeMediaPermissionResult(kind, generation, false); return;
            }
            pendingCameraPermissionGeneration = generation;
            requestPermissions(new String[] { permission }, REQUEST_CAMERA);
        } else {
            if (pendingMicrophonePermissionGeneration != 0) {
                nativeMediaPermissionResult(kind, generation, false); return;
            }
            pendingMicrophonePermissionGeneration = generation;
            requestPermissions(new String[] { permission }, REQUEST_MICROPHONE);
        }
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
        if (resultCode == RESULT_OK && data != null) {
            approvedGeneration = generation;
            projectionResultCode = resultCode;
            projectionData = data;
        } else {
            approvedGeneration = 0;
            projectionResultCode = 0;
            projectionData = null;
        }
        // The native side may start its source synchronously when this callback
        // returns, so publish the consent token before notifying it.
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

    public static void startProjection(long generation, int width, int height) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity != null) activity.runOnUiThread(
            () -> activity.startProjectionOnUiThread(generation, width, height));
    }

    public static void stopProjection(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity != null) activity.runOnUiThread(() -> {
            if (generation == activity.approvedGeneration) activity.releaseProjection();
        });
    }

    private void startProjectionOnUiThread(long generation, int width, int height) {
        if (generation != approvedGeneration || projectionData == null || projection != null ||
            releasingProjection) return;
        CreatorStudioProjectionService.start(this);
        MediaProjectionManager manager = (MediaProjectionManager)
            getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        projection = manager.getMediaProjection(projectionResultCode, projectionData);
        projection.registerCallback(new MediaProjection.Callback() {
            @Override public void onStop() {
                notifyProjectionRevoked(generation);
                releaseProjection();
            }
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
        if (releasingProjection) return;
        releasingProjection = true;
        if (projectionDisplay != null) { projectionDisplay.release(); projectionDisplay = null; }
        if (projectionReader != null) { projectionReader.close(); projectionReader = null; }
        final MediaProjection projectionToStop = projection;
        projection = null;
        approvedGeneration = 0;
        projectionResultCode = 0;
        projectionData = null;
        if (projectionToStop != null) projectionToStop.stop();
        CreatorStudioProjectionService.stop(this);
        releasingProjection = false;
    }

    private void notifyProjectionRevoked(long generation) {
        if (generation == 0 || revokedGeneration == generation) return;
        revokedGeneration = generation;
        nativeProjectionRevoked(generation);
    }

    public static void startCamera(long generation, int requestedWidth, int requestedHeight, int requestedFps) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) { nativeCameraFailed(generation); return; }
        activity.runOnUiThread(() -> activity.startCameraOnUiThread(
            generation, requestedWidth, requestedHeight, requestedFps));
    }

    public static void stopCamera(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity != null) activity.runOnUiThread(() -> activity.stopCameraOnUiThread(generation));
    }

    private void startCameraOnUiThread(long generation, int requestedWidth, int requestedHeight,
                                       int requestedFps) {
        if (cameraDevice != null || cameraGeneration != 0 ||
            checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            nativeCameraFailed(generation); return;
        }
        try {
            final CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
            if (manager == null) { nativeCameraFailed(generation); return; }
            final String[] ids = manager.getCameraIdList();
            if (ids.length == 0) { nativeCameraFailed(generation); return; }
            final String cameraId = ids[0];
            final StreamConfigurationMap map = manager.getCameraCharacteristics(cameraId)
                .get(android.hardware.camera2.CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            final Size chosen = chooseCameraSize(map == null ? null :
                map.getOutputSizes(ImageReader.class), requestedWidth, requestedHeight);
            if (chosen == null) { nativeCameraFailed(generation); return; }
            cameraGeneration = generation;
            cameraThread = new HandlerThread("CreatorStudioCamera");
            cameraThread.start();
            cameraHandler = new Handler(cameraThread.getLooper());
            cameraReader = ImageReader.newInstance(chosen.getWidth(), chosen.getHeight(),
                android.graphics.ImageFormat.YUV_420_888, 3);
            cameraReader.setOnImageAvailableListener(reader -> {
                Image image = reader.acquireLatestImage();
                if (image == null) return;
                try {
                    ByteBuffer rgba = ByteBuffer.allocateDirect(image.getWidth() * image.getHeight() * 4);
                    rgba.order(ByteOrder.nativeOrder());
                    yuv420ToBgra(image, rgba);
                    rgba.rewind();
                    nativeCameraFrame(cameraGeneration, rgba, image.getWidth(), image.getHeight(),
                        image.getTimestamp());
                } finally { image.close(); }
            }, cameraHandler);
            manager.openCamera(cameraId, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice camera) {
                    if (cameraGeneration != generation) { camera.close(); return; }
                    cameraDevice = camera;
                    try {
                        final CaptureRequest.Builder request = camera.createCaptureRequest(
                            CameraDevice.TEMPLATE_RECORD);
                        request.addTarget(cameraReader.getSurface());
                        request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                            new android.util.Range<>(Math.max(15, requestedFps), Math.max(15, requestedFps)));
                        camera.createCaptureSession(Arrays.asList(cameraReader.getSurface()),
                            new CameraCaptureSession.StateCallback() {
                                @Override public void onConfigured(CameraCaptureSession session) {
                                    if (cameraGeneration != generation) { session.close(); return; }
                                    cameraSession = session;
                                    try { session.setRepeatingRequest(request.build(), null, cameraHandler); }
                                    catch (CameraAccessException error) { nativeCameraFailed(generation); }
                                }
                                @Override public void onConfigureFailed(CameraCaptureSession session) {
                                    nativeCameraFailed(generation);
                                }
                            }, cameraHandler);
                    } catch (CameraAccessException error) { nativeCameraFailed(generation); }
                }
                @Override public void onDisconnected(CameraDevice camera) {
                    camera.close(); nativeCameraFailed(generation);
                }
                @Override public void onError(CameraDevice camera, int error) {
                    camera.close(); nativeCameraFailed(generation);
                }
            }, cameraHandler);
        } catch (SecurityException | CameraAccessException error) { nativeCameraFailed(generation); }
    }

    private void stopCameraOnUiThread(long generation) {
        if (generation != 0 && generation != cameraGeneration) return;
        cameraGeneration = 0;
        if (cameraSession != null) { cameraSession.close(); cameraSession = null; }
        if (cameraDevice != null) { cameraDevice.close(); cameraDevice = null; }
        if (cameraReader != null) { cameraReader.close(); cameraReader = null; }
        if (cameraThread != null) { cameraThread.quitSafely(); cameraThread = null; cameraHandler = null; }
    }

    private static Size chooseCameraSize(Size[] sizes, int requestedWidth, int requestedHeight) {
        if (sizes == null || sizes.length == 0) return null;
        Size best = sizes[0];
        long bestDistance = Long.MAX_VALUE;
        for (Size size : sizes) {
            final long distance = Math.abs((long) size.getWidth() - requestedWidth) +
                Math.abs((long) size.getHeight() - requestedHeight);
            if (distance < bestDistance) { best = size; bestDistance = distance; }
        }
        return best;
    }

    private static void yuv420ToBgra(Image image, ByteBuffer output) {
        final Image.Plane[] planes = image.getPlanes();
        final ByteBuffer y = planes[0].getBuffer();
        final ByteBuffer u = planes[1].getBuffer();
        final ByteBuffer v = planes[2].getBuffer();
        final int width = image.getWidth(), height = image.getHeight();
        final int yStride = planes[0].getRowStride();
        final int uvStride = planes[1].getRowStride();
        final int uvPixelStride = planes[1].getPixelStride();
        for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column) {
            int yy = y.get(row * yStride + column) & 0xff;
            int uv = (row / 2) * uvStride + (column / 2) * uvPixelStride;
            int uu = (u.get(uv) & 0xff) - 128, vv = (v.get(uv) & 0xff) - 128;
            int r = clamp((int) (yy + 1.402f * vv));
            int g = clamp((int) (yy - 0.344f * uu - 0.714f * vv));
            int b = clamp((int) (yy + 1.772f * uu));
            output.put((byte) b); output.put((byte) g); output.put((byte) r); output.put((byte) 0xff);
        }
    }

    private static int clamp(int value) { return Math.max(0, Math.min(255, value)); }

    public static void startMicrophone(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) { nativeMicrophoneFailed(generation); return; }
        activity.runOnUiThread(() -> activity.startMicrophoneOnUiThread(generation));
    }

    public static void stopMicrophone(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity != null) activity.runOnUiThread(() -> activity.stopMicrophoneOnUiThread(generation));
    }

    private void startMicrophoneOnUiThread(long generation) {
        if (microphoneRunning || checkSelfPermission(Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED) { nativeMicrophoneFailed(generation); return; }
        final int sampleRate = 48000;
        final int channelMask = AudioFormat.CHANNEL_IN_MONO;
        final int minimum = AudioRecord.getMinBufferSize(sampleRate, channelMask,
            AudioFormat.ENCODING_PCM_16BIT);
        if (minimum <= 0) { nativeMicrophoneFailed(generation); return; }
        try {
            microphoneRecord = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, 4096));
            if (microphoneRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                microphoneRecord.release(); microphoneRecord = null; nativeMicrophoneFailed(generation); return;
            }
            microphoneGeneration = generation;
            microphoneRunning = true;
            microphoneRecord.startRecording();
            final AudioRecord recorder = microphoneRecord;
            microphoneThread = new Thread(() -> {
                final ByteBuffer pcm = ByteBuffer.allocateDirect(1920 * 2).order(ByteOrder.nativeOrder());
                while (microphoneRunning && microphoneGeneration == generation) {
                    pcm.clear();
                    final int read = recorder.read(pcm, 1920 * 2, AudioRecord.READ_BLOCKING);
                    if (read <= 0) { if (microphoneRunning) nativeMicrophoneFailed(generation); break; }
                    pcm.rewind();
                    nativeMicrophonePcm16(generation, pcm, read / 2, sampleRate, 1, System.nanoTime());
                }
            }, "CreatorStudioMicrophone");
            microphoneThread.start();
        } catch (IllegalStateException | SecurityException error) { nativeMicrophoneFailed(generation); }
    }

    private void stopMicrophoneOnUiThread(long generation) {
        if (generation != 0 && generation != microphoneGeneration) return;
        microphoneRunning = false;
        microphoneGeneration = 0;
        if (microphoneRecord != null) {
            try { microphoneRecord.stop(); } catch (IllegalStateException ignored) { }
            microphoneRecord.release(); microphoneRecord = null;
        }
        microphoneThread = null;
    }
}
