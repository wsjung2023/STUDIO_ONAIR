package com.studioonair.creatorstudio;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.Manifest;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.MediaRecorder;
import android.media.projection.MediaProjectionManager;
import android.media.projection.MediaProjection;
import android.media.Image;
import android.media.ImageReader;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
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
    private HandlerThread projectionThread;
    private Handler projectionHandler;
    private boolean releasingProjection;
    private long revokedGeneration;
    private long releasedGeneration;
    private volatile long requestedStopGeneration;
    private long pendingCameraPermissionGeneration;
    private long pendingMicrophonePermissionGeneration;
    private HandlerThread cameraThread;
    private Handler cameraHandler;
    private CameraDevice cameraDevice;
    private CameraCaptureSession cameraSession;
    private ImageReader cameraReader;
    private volatile long cameraGeneration;
    private volatile boolean cameraStopping;
    private volatile long stoppingCameraGeneration;
    private volatile long stoppedCameraGeneration;
    private AudioRecord microphoneRecord;
    private Thread microphoneThread;
    private volatile boolean microphoneRunning;
    private volatile long microphoneGeneration;
    private volatile boolean microphoneStopping;
    private volatile long stoppingMicrophoneGeneration;
    private volatile long stoppedMicrophoneGeneration;
    private AudioRecord playbackRecord;
    private Thread playbackThread;
    private volatile boolean playbackRunning;
    private volatile long playbackGeneration;
    private volatile boolean playbackStopping;
    private volatile long stoppingPlaybackGeneration;
    private volatile long stoppedPlaybackGeneration;

    public static native void nativeProjectionResult(long generation, boolean granted);
    public static native boolean nativeProjectionFrame(long generation, java.nio.ByteBuffer bytes,
        int width, int height, int rowStride, int pixelStride, long timestampNs);
    public static native void nativeProjectionRevoked(long generation);
    public static native void nativeProjectionReleased(long generation, boolean revoked);
    public static native void nativeMediaPermissionResult(int kind, long generation, boolean granted);
    public static native void nativeCameraFrame(long generation, ByteBuffer bytes,
        int width, int height, long timestampNs);
    public static native void nativeMicrophonePcm16(long generation, ByteBuffer bytes,
        int sampleCount, int sampleRate, int channels, long timestampNs);
    public static native void nativeCameraFailed(long generation);
    public static native void nativeMicrophoneFailed(long generation);
    public static native void nativeCameraStopped(long generation);
    public static native void nativeMicrophoneStopped(long generation);
    public static native void nativeSystemAudioPcm16(long generation, ByteBuffer bytes,
        int sampleCount, int sampleRate, int channels, long timestampNs);
    public static native void nativeSystemAudioFailed(long generation);
    public static native void nativeSystemAudioStopped(long generation);

    public static int deviceMemoryClassMiB() {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) return 0;
        final ActivityManager manager =
            (ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
        return manager == null ? 0 : manager.getMemoryClass();
    }

    public static boolean isLowRamDevice() {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) return true;
        final ActivityManager manager =
            (ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
        return manager == null || manager.isLowRamDevice();
    }

    public static boolean isPowerSaveMode() {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) return true;
        final PowerManager manager =
            (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
        return manager == null || manager.isPowerSaveMode();
    }

    /** Returns 0 nominal, 1 serious, or 2 critical for the shared policy. */
    public static int thermalState() {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return 0;
        final PowerManager manager =
            (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
        if (manager == null) return 0;
        final int status = manager.getCurrentThermalStatus();
        if (status >= PowerManager.THERMAL_STATUS_CRITICAL) return 2;
        if (status >= PowerManager.THERMAL_STATUS_SEVERE) return 1;
        return 0;
    }

    /** Copies a completed private-cache render into a user-selected SAF URI. */
    public static String publishExport(String stagedPath, String destinationUri,
                                       boolean replaceExisting) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) return "Android activity is unavailable";
        final Uri uri;
        try {
            uri = Uri.parse(destinationUri);
        } catch (RuntimeException error) {
            return "Android export destination is invalid";
        }
        if (!"content".equals(uri.getScheme())) {
            return "Android export destination is not a content URI";
        }
        final String mode = replaceExisting ? "wt" : "w";
        try (InputStream input = new FileInputStream(stagedPath);
             OutputStream output = activity.getContentResolver().openOutputStream(uri, mode)) {
            if (output == null) return "Android could not open the export destination";
            final byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read > 0) output.write(buffer, 0, read);
            }
            output.flush();
            return "";
        } catch (Exception error) {
            final String message = error.getMessage();
            return message == null || message.isEmpty()
                ? "Android scoped-storage export failed" : message;
        }
    }

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
        if (pendingGeneration != 0) {
            nativeProjectionResult(pendingGeneration, false);
            pendingGeneration = 0;
        }
        if (pendingCameraPermissionGeneration != 0) {
            nativeMediaPermissionResult(CAMERA_KIND, pendingCameraPermissionGeneration, false);
            pendingCameraPermissionGeneration = 0;
        }
        if (pendingMicrophonePermissionGeneration != 0) {
            nativeMediaPermissionResult(MICROPHONE_KIND, pendingMicrophonePermissionGeneration, false);
            pendingMicrophonePermissionGeneration = 0;
        }
        if (approvedGeneration != 0) {
            releaseProjection(approvedGeneration, true, true);
        }
        stopCameraOnUiThread(cameraGeneration);
        stopMicrophoneOnUiThread(microphoneGeneration);
        stopPlaybackAudioOnUiThread(playbackGeneration);
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
        if (activity == null) {
            nativeProjectionRevoked(generation);
            nativeProjectionReleased(generation, true);
            return;
        }
        activity.runOnUiThread(
            () -> activity.startProjectionOnUiThread(generation, width, height));
    }

    public static void stopProjection(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeProjectionReleased(generation, false);
            return;
        }
        activity.runOnUiThread(() -> {
            if (generation == activity.approvedGeneration) {
                activity.releaseProjection(generation, false, true);
            } else {
                nativeProjectionReleased(generation, false);
            }
        });
    }

    private void startProjectionOnUiThread(long generation, int width, int height) {
        if (generation != approvedGeneration || projectionData == null || projection != null ||
            releasingProjection || width <= 0 || height <= 0) {
            failProjectionStart(generation);
            return;
        }
        try {
            CreatorStudioProjectionService.start(this);
            final MediaProjectionManager manager = (MediaProjectionManager)
                getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            if (manager == null) {
                failProjectionStart(generation);
                return;
            }
            projection = manager.getMediaProjection(projectionResultCode, projectionData);
            if (projection == null) {
                failProjectionStart(generation);
                return;
            }
            projectionThread = new HandlerThread("CreatorStudioProjection");
            projectionThread.start();
            projectionHandler = new Handler(projectionThread.getLooper());
            projection.registerCallback(new MediaProjection.Callback() {
                @Override public void onStop() {
                    if (requestedStopGeneration == generation) return;
                    runOnUiThread(() -> releaseProjection(generation, true, false));
                }
            }, projectionHandler);
            projectionReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
            projectionReader.setOnImageAvailableListener(reader -> {
                final Image image = reader.acquireLatestImage();
                if (image == null) return;
                try {
                    final Image.Plane plane = image.getPlanes()[0];
                    nativeProjectionFrame(generation, plane.getBuffer(), image.getWidth(),
                        image.getHeight(), plane.getRowStride(), plane.getPixelStride(),
                        image.getTimestamp());
                } finally {
                    image.close();
                }
            }, projectionHandler);
            projectionDisplay = projection.createVirtualDisplay("CreatorStudioCapture", width, height,
                getResources().getDisplayMetrics().densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, projectionReader.getSurface(),
                null, projectionHandler);
        } catch (IllegalArgumentException | IllegalStateException | SecurityException error) {
            failProjectionStart(generation);
        }
    }

    private void failProjectionStart(long generation) {
        releaseProjection(generation, true, projection != null);
    }

    private void releaseProjection(long generation, boolean revoked, boolean stopProjection) {
        if (generation == 0 || releasedGeneration == generation || releasingProjection) return;
        if (generation != approvedGeneration) {
            nativeProjectionReleased(generation, revoked);
            return;
        }
        releasingProjection = true;
        if (!revoked) requestedStopGeneration = generation;
        if (projectionDisplay != null) { projectionDisplay.release(); projectionDisplay = null; }
        if (projectionReader != null) {
            projectionReader.setOnImageAvailableListener(null, null);
            projectionReader.close();
            projectionReader = null;
        }
        final MediaProjection projectionToStop = projection;
        stopPlaybackAudioOnUiThread(playbackGeneration);
        projection = null;
        approvedGeneration = 0;
        projectionResultCode = 0;
        projectionData = null;
        if (stopProjection && projectionToStop != null) {
            try { projectionToStop.stop(); } catch (IllegalStateException ignored) { }
        }
        if (projectionThread != null) {
            projectionThread.quitSafely();
            projectionThread = null;
            projectionHandler = null;
        }
        CreatorStudioProjectionService.stop(this);
        releasingProjection = false;
        if (revoked) notifyProjectionRevoked(generation);
        releasedGeneration = generation;
        nativeProjectionReleased(generation, revoked);
    }

    private void notifyProjectionRevoked(long generation) {
        if (generation == 0 || revokedGeneration == generation) return;
        revokedGeneration = generation;
        nativeProjectionRevoked(generation);
    }

    public static void startCamera(long generation, int requestedWidth, int requestedHeight, int requestedFps) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeCameraFailed(generation);
            nativeCameraStopped(generation);
            return;
        }
        activity.runOnUiThread(() -> activity.startCameraOnUiThread(
            generation, requestedWidth, requestedHeight, requestedFps));
    }

    public static void stopCamera(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeCameraStopped(generation);
            return;
        }
        activity.runOnUiThread(() -> activity.stopCameraOnUiThread(generation));
    }

    private void startCameraOnUiThread(long generation, int requestedWidth, int requestedHeight,
                                       int requestedFps) {
        if (cameraDevice != null || cameraGeneration != 0 || cameraStopping ||
            checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            failCamera(generation); return;
        }
        try {
            final CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
            if (manager == null) { failCamera(generation); return; }
            final String[] ids = manager.getCameraIdList();
            if (ids.length == 0) { failCamera(generation); return; }
            final String cameraId = ids[0];
            final StreamConfigurationMap map = manager.getCameraCharacteristics(cameraId)
                .get(android.hardware.camera2.CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            final Size chosen = chooseCameraSize(map == null ? null :
                map.getOutputSizes(ImageReader.class), requestedWidth, requestedHeight);
            if (chosen == null) { failCamera(generation); return; }
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
                    nativeCameraFrame(generation, rgba, image.getWidth(), image.getHeight(),
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
                                    catch (CameraAccessException error) { failCamera(generation); }
                                }
                                @Override public void onConfigureFailed(CameraCaptureSession session) {
                                    failCamera(generation);
                                }
                            }, cameraHandler);
                    } catch (CameraAccessException error) { failCamera(generation); }
                }
                @Override public void onDisconnected(CameraDevice camera) {
                    camera.close(); failCamera(generation);
                }
                @Override public void onError(CameraDevice camera, int error) {
                    camera.close(); failCamera(generation);
                }
            }, cameraHandler);
        } catch (SecurityException | CameraAccessException error) { failCamera(generation); }
    }

    private void failCamera(long generation) {
        nativeCameraFailed(generation);
        runOnUiThread(() -> stopCameraOnUiThread(generation));
    }

    private void stopCameraOnUiThread(long generation) {
        if (generation == 0) generation = cameraGeneration;
        if (generation == 0 || stoppedCameraGeneration == generation) return;
        if (cameraStopping && stoppingCameraGeneration == generation) return;
        if (generation != cameraGeneration) {
            stoppedCameraGeneration = generation;
            nativeCameraStopped(generation);
            return;
        }
        cameraStopping = true;
        stoppingCameraGeneration = generation;
        cameraGeneration = 0;
        if (cameraSession != null) { cameraSession.close(); cameraSession = null; }
        if (cameraDevice != null) { cameraDevice.close(); cameraDevice = null; }
        if (cameraReader != null) {
            cameraReader.setOnImageAvailableListener(null, null);
            cameraReader.close();
            cameraReader = null;
        }
        final HandlerThread thread = cameraThread;
        cameraThread = null;
        cameraHandler = null;
        if (thread != null) thread.quitSafely();
        final long stoppedGeneration = generation;
        new Thread(() -> {
            if (thread != null) {
                try { thread.join(); } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                }
            }
            cameraStopping = false;
            stoppingCameraGeneration = 0;
            stoppedCameraGeneration = stoppedGeneration;
            nativeCameraStopped(stoppedGeneration);
        }, "CreatorStudioCameraStop").start();
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
        if (activity == null) {
            nativeMicrophoneFailed(generation);
            nativeMicrophoneStopped(generation);
            return;
        }
        activity.runOnUiThread(() -> activity.startMicrophoneOnUiThread(generation));
    }

    public static void stopMicrophone(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeMicrophoneStopped(generation);
            return;
        }
        activity.runOnUiThread(() -> activity.stopMicrophoneOnUiThread(generation));
    }

    private void startMicrophoneOnUiThread(long generation) {
        if (microphoneRunning || microphoneStopping ||
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED) { failMicrophone(generation); return; }
        final int sampleRate = 48000;
        final int channelMask = AudioFormat.CHANNEL_IN_MONO;
        final int minimum = AudioRecord.getMinBufferSize(sampleRate, channelMask,
            AudioFormat.ENCODING_PCM_16BIT);
        if (minimum <= 0) { failMicrophone(generation); return; }
        try {
            microphoneGeneration = generation;
            microphoneRecord = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, 4096));
            if (microphoneRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                failMicrophone(generation); return;
            }
            microphoneRunning = true;
            microphoneRecord.startRecording();
            final AudioRecord recorder = microphoneRecord;
            microphoneThread = new Thread(() -> {
                final ByteBuffer pcm = ByteBuffer.allocateDirect(1920 * 2).order(ByteOrder.nativeOrder());
                while (microphoneRunning && microphoneGeneration == generation) {
                    pcm.clear();
                    final int read = recorder.read(pcm, 1920 * 2, AudioRecord.READ_BLOCKING);
                    if (read <= 0) { if (microphoneRunning) failMicrophone(generation); break; }
                    pcm.rewind();
                    nativeMicrophonePcm16(generation, pcm, read / 2, sampleRate, 1, System.nanoTime());
                }
            }, "CreatorStudioMicrophone");
            microphoneThread.start();
        } catch (IllegalStateException | SecurityException error) { failMicrophone(generation); }
    }

    private void failMicrophone(long generation) {
        nativeMicrophoneFailed(generation);
        runOnUiThread(() -> stopMicrophoneOnUiThread(generation));
    }

    private void stopMicrophoneOnUiThread(long generation) {
        if (generation == 0) generation = microphoneGeneration;
        if (generation == 0 || stoppedMicrophoneGeneration == generation) return;
        if (microphoneStopping && stoppingMicrophoneGeneration == generation) return;
        if (generation != microphoneGeneration) {
            stoppedMicrophoneGeneration = generation;
            nativeMicrophoneStopped(generation);
            return;
        }
        microphoneStopping = true;
        stoppingMicrophoneGeneration = generation;
        microphoneRunning = false;
        microphoneGeneration = 0;
        final AudioRecord recorder = microphoneRecord;
        final Thread worker = microphoneThread;
        microphoneRecord = null;
        microphoneThread = null;
        if (recorder != null) {
            try { recorder.stop(); } catch (IllegalStateException ignored) { }
        }
        final long stoppedGeneration = generation;
        new Thread(() -> {
            if (worker != null && worker != Thread.currentThread()) {
                try { worker.join(); } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                }
            }
            if (recorder != null) recorder.release();
            microphoneStopping = false;
            stoppingMicrophoneGeneration = 0;
            stoppedMicrophoneGeneration = stoppedGeneration;
            nativeMicrophoneStopped(stoppedGeneration);
        }, "CreatorStudioMicrophoneStop").start();
    }

    public static void startPlaybackAudio(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeSystemAudioFailed(generation);
            nativeSystemAudioStopped(generation);
            return;
        }
        activity.runOnUiThread(() -> activity.startPlaybackAudioOnUiThread(generation));
    }

    public static void stopPlaybackAudio(long generation) {
        final CreatorStudioActivity activity = activeActivity;
        if (activity == null) {
            nativeSystemAudioStopped(generation);
            return;
        }
        activity.runOnUiThread(() -> activity.stopPlaybackAudioOnUiThread(generation));
    }

    private void startPlaybackAudioOnUiThread(long generation) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || playbackRunning ||
            playbackStopping || projection == null) {
            failPlaybackAudio(generation); return;
        }
        final int sampleRate = 48000;
        final int channelMask = AudioFormat.CHANNEL_IN_STEREO;
        final int minimum = AudioRecord.getMinBufferSize(sampleRate, channelMask,
            AudioFormat.ENCODING_PCM_16BIT);
        if (minimum <= 0) { failPlaybackAudio(generation); return; }
        try {
            playbackGeneration = generation;
            final AudioPlaybackCaptureConfiguration config =
                new AudioPlaybackCaptureConfiguration.Builder(projection)
                    .addMatchingUsage(android.media.AudioAttributes.USAGE_MEDIA)
                    .addMatchingUsage(android.media.AudioAttributes.USAGE_GAME)
                    .build();
            playbackRecord = new AudioRecord.Builder()
                .setAudioFormat(new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sampleRate).setChannelMask(channelMask).build())
                .setBufferSizeInBytes(Math.max(minimum, 7680))
                .setAudioPlaybackCaptureConfig(config)
                .build();
            if (playbackRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                failPlaybackAudio(generation); return;
            }
            playbackRunning = true;
            playbackRecord.startRecording();
            final AudioRecord recorder = playbackRecord;
            playbackThread = new Thread(() -> {
                final ByteBuffer pcm = ByteBuffer.allocateDirect(1920 * 4).order(ByteOrder.nativeOrder());
                while (playbackRunning && playbackGeneration == generation) {
                    pcm.clear();
                    final int read = recorder.read(pcm, 1920 * 4, AudioRecord.READ_BLOCKING);
                    if (read <= 0) { if (playbackRunning) failPlaybackAudio(generation); break; }
                    pcm.rewind();
                    nativeSystemAudioPcm16(generation, pcm, read / 2, sampleRate, 2, System.nanoTime());
                }
            }, "CreatorStudioPlaybackAudio");
            playbackThread.start();
        } catch (IllegalStateException | SecurityException error) { failPlaybackAudio(generation); }
    }

    private void failPlaybackAudio(long generation) {
        nativeSystemAudioFailed(generation);
        runOnUiThread(() -> stopPlaybackAudioOnUiThread(generation));
    }

    private void stopPlaybackAudioOnUiThread(long generation) {
        if (generation == 0) generation = playbackGeneration;
        if (generation == 0 || stoppedPlaybackGeneration == generation) return;
        if (playbackStopping && stoppingPlaybackGeneration == generation) return;
        if (generation != playbackGeneration) {
            stoppedPlaybackGeneration = generation;
            nativeSystemAudioStopped(generation);
            return;
        }
        playbackStopping = true;
        stoppingPlaybackGeneration = generation;
        playbackRunning = false;
        playbackGeneration = 0;
        final AudioRecord recorder = playbackRecord;
        final Thread worker = playbackThread;
        playbackRecord = null;
        playbackThread = null;
        if (recorder != null) {
            try { recorder.stop(); } catch (IllegalStateException ignored) { }
        }
        final long stoppedGeneration = generation;
        new Thread(() -> {
            if (worker != null && worker != Thread.currentThread()) {
                try { worker.join(); } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                }
            }
            if (recorder != null) recorder.release();
            playbackStopping = false;
            stoppingPlaybackGeneration = 0;
            stoppedPlaybackGeneration = stoppedGeneration;
            nativeSystemAudioStopped(stoppedGeneration);
        }, "CreatorStudioPlaybackAudioStop").start();
    }
}
