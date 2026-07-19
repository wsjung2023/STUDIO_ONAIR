package com.studioonair.creatorstudio;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.Manifest;
import android.content.pm.PackageManager;
import android.content.pm.ApplicationInfo;
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
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.media.MediaMuxer;
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
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.io.FileInputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

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
    private static final AtomicLong nextMediaEncoderHandle = new AtomicLong(1);
    private static final ConcurrentHashMap<Long, MediaSegmentEncoder> mediaEncoders =
        new ConcurrentHashMap<>();

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

    /** Reports whether both hardware-abstracted Android release codecs exist. */
    public static String mediaEncoderStatus() {
        try {
            final MediaCodecList codecs =
                new MediaCodecList(MediaCodecList.ALL_CODECS);
            final MediaFormat video = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC, 1280, 720);
            video.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            video.setInteger(MediaFormat.KEY_BIT_RATE, 4_000_000);
            video.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
            video.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            if (codecs.findEncoderForFormat(video) == null) {
                return "Android device has no compatible H.264 encoder";
            }
            final MediaFormat audio = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AAC, 48_000, 2);
            audio.setInteger(MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC);
            audio.setInteger(MediaFormat.KEY_BIT_RATE, 192_000);
            if (codecs.findEncoderForFormat(audio) == null) {
                return "Android device has no compatible AAC encoder";
            }
            return "";
        } catch (Throwable error) {
            return errorMessage(error, "Android MediaCodec probe failed");
        }
    }

    public static long createVideoEncoder(String path, int width, int height,
                                          int bitRate, int frameRate) {
        if (path == null || path.isEmpty() || width <= 0 || height <= 0 ||
            bitRate <= 0 || frameRate <= 0) return 0;
        try {
            final MediaFormat format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC, width, height);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitRate);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, frameRate);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            return registerMediaEncoder(new MediaSegmentEncoder(
                path, format, true, width, height, 0, 0));
        } catch (Throwable error) {
            return 0;
        }
    }

    public static long createAudioEncoder(String path, int sampleRate,
                                          int channels, int bitRate) {
        if (path == null || path.isEmpty() || sampleRate <= 0 ||
            channels <= 0 || bitRate <= 0) return 0;
        try {
            final MediaFormat format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_AAC, sampleRate, channels);
            format.setInteger(MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC);
            format.setInteger(MediaFormat.KEY_BIT_RATE, bitRate);
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 64 * 1024);
            return registerMediaEncoder(new MediaSegmentEncoder(
                path, format, false, 0, 0, sampleRate, channels));
        } catch (Throwable error) {
            return 0;
        }
    }

    public static String writeVideoEncoderFrame(long handle, ByteBuffer bgra,
                                                long presentationTimeUs) {
        final MediaSegmentEncoder encoder = mediaEncoders.get(handle);
        if (encoder == null) return "Android video encoder handle is stale";
        try {
            encoder.writeVideo(bgra, presentationTimeUs);
            return "";
        } catch (Throwable error) {
            return errorMessage(error, "Android video encoding failed");
        }
    }

    public static String writeAudioEncoderSamples(long handle, ByteBuffer pcm16,
                                                  int sampleCount,
                                                  long presentationTimeUs) {
        final MediaSegmentEncoder encoder = mediaEncoders.get(handle);
        if (encoder == null) return "Android audio encoder handle is stale";
        try {
            encoder.writeAudio(pcm16, sampleCount, presentationTimeUs);
            return "";
        } catch (Throwable error) {
            return errorMessage(error, "Android audio encoding failed");
        }
    }

    public static String finishMediaEncoder(long handle) {
        final MediaSegmentEncoder encoder = mediaEncoders.remove(handle);
        if (encoder == null) return "Android media encoder handle is stale";
        try {
            encoder.finish();
            return "";
        } catch (Throwable error) {
            encoder.abort();
            return errorMessage(error, "Android media encoder finalization failed");
        }
    }

    public static void abortMediaEncoder(long handle) {
        final MediaSegmentEncoder encoder = mediaEncoders.remove(handle);
        if (encoder != null) encoder.abort();
    }

    private static long registerMediaEncoder(MediaSegmentEncoder encoder) {
        long handle;
        do {
            handle = nextMediaEncoderHandle.getAndIncrement();
            if (handle <= 0) {
                nextMediaEncoderHandle.compareAndSet(handle + 1, 1);
                handle = nextMediaEncoderHandle.getAndIncrement();
            }
        } while (mediaEncoders.putIfAbsent(handle, encoder) != null);
        return handle;
    }

    private static String errorMessage(Throwable error, String fallback) {
        final String message = error == null ? null : error.getMessage();
        return message == null || message.isEmpty() ? fallback : message;
    }

    private static final class MediaSegmentEncoder {
        private static final long CODEC_TIMEOUT_US = 10_000;
        private final String path;
        private final boolean video;
        private final int width;
        private final int height;
        private final int sampleRate;
        private final int channels;
        private final MediaCodec.BufferInfo bufferInfo =
            new MediaCodec.BufferInfo();
        private MediaCodec codec;
        private MediaMuxer muxer;
        private int muxerTrack = -1;
        private boolean muxerStarted;
        private boolean closed;
        private long lastPresentationTimeUs = -1;

        MediaSegmentEncoder(String path, MediaFormat format, boolean video,
                            int width, int height, int sampleRate, int channels)
                throws Exception {
            this.path = path;
            this.video = video;
            this.width = width;
            this.height = height;
            this.sampleRate = sampleRate;
            this.channels = channels;
            try {
                codec = MediaCodec.createEncoderByType(format.getString(
                    MediaFormat.KEY_MIME));
                codec.configure(format, null, null,
                    MediaCodec.CONFIGURE_FLAG_ENCODE);
                muxer = new MediaMuxer(path,
                    MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
                codec.start();
            } catch (Throwable error) {
                release(false);
                throw error;
            }
        }

        void writeVideo(ByteBuffer bgra, long presentationTimeUs)
                throws Exception {
            ensureOpen(true);
            if (bgra == null || !bgra.isDirect() ||
                bgra.capacity() < width * height * 4L) {
                throw new IllegalArgumentException("BGRA frame size is invalid");
            }
            final int inputIndex = awaitInputBuffer();
            final Image inputImage = codec.getInputImage(inputIndex);
            if (inputImage == null) {
                throw new IllegalStateException(
                    "H.264 encoder does not expose flexible YUV input");
            }
            try {
                fillYuv420(inputImage, bgra, width, height);
            } finally {
                inputImage.close();
            }
            final long pts = monotonicPresentationTime(presentationTimeUs);
            codec.queueInputBuffer(inputIndex, 0, 0, pts, 0);
            drain(false);
        }

        void writeAudio(ByteBuffer pcm16, int sampleCount,
                        long presentationTimeUs) throws Exception {
            ensureOpen(false);
            if (pcm16 == null || !pcm16.isDirect() || sampleCount <= 0 ||
                pcm16.capacity() < sampleCount * 2L) {
                throw new IllegalArgumentException("PCM16 block size is invalid");
            }
            final ByteBuffer source = pcm16.duplicate();
            source.position(0);
            source.limit(sampleCount * 2);
            final int bytesPerFrame = channels * 2;
            long framesWritten = 0;
            while (source.hasRemaining()) {
                final int inputIndex = awaitInputBuffer();
                final ByteBuffer input = codec.getInputBuffer(inputIndex);
                if (input == null) {
                    throw new IllegalStateException(
                        "AAC encoder input buffer is unavailable");
                }
                input.clear();
                int bytes = Math.min(input.remaining(), source.remaining());
                bytes -= bytes % bytesPerFrame;
                if (bytes <= 0) {
                    throw new IllegalStateException(
                        "AAC encoder input buffer cannot hold one frame");
                }
                final int oldLimit = source.limit();
                source.limit(source.position() + bytes);
                input.put(source);
                source.limit(oldLimit);
                final long pts = monotonicPresentationTime(
                    presentationTimeUs + framesWritten * 1_000_000L / sampleRate);
                codec.queueInputBuffer(inputIndex, 0, bytes, pts, 0);
                framesWritten += bytes / bytesPerFrame;
                drain(false);
            }
        }

        void finish() throws Exception {
            ensureOpen(video);
            final int inputIndex = awaitInputBuffer();
            codec.queueInputBuffer(inputIndex, 0, 0,
                Math.max(0, lastPresentationTimeUs + 1),
                MediaCodec.BUFFER_FLAG_END_OF_STREAM);
            drain(true);
            release(true);
        }

        void abort() {
            release(false);
            new File(path).delete();
        }

        private void ensureOpen(boolean expectedVideo) {
            if (closed || codec == null || muxer == null || video != expectedVideo) {
                throw new IllegalStateException("Media encoder is not active");
            }
        }

        private int awaitInputBuffer() throws Exception {
            for (int attempt = 0; attempt < 200; ++attempt) {
                final int index = codec.dequeueInputBuffer(CODEC_TIMEOUT_US);
                if (index >= 0) return index;
                drain(false);
            }
            throw new IllegalStateException("MediaCodec input timed out");
        }

        private long monotonicPresentationTime(long requested) {
            lastPresentationTimeUs = Math.max(requested,
                lastPresentationTimeUs + 1);
            return lastPresentationTimeUs;
        }

        private void drain(boolean endOfStream) throws Exception {
            int idleCount = 0;
            while (true) {
                final int outputIndex = codec.dequeueOutputBuffer(
                    bufferInfo, endOfStream ? CODEC_TIMEOUT_US : 0);
                if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                    if (!endOfStream) return;
                    if (++idleCount >= 500) {
                        throw new IllegalStateException(
                            "MediaCodec end-of-stream timed out");
                    }
                    continue;
                }
                if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    if (muxerStarted) {
                        throw new IllegalStateException(
                            "MediaCodec output format changed twice");
                    }
                    muxerTrack = muxer.addTrack(codec.getOutputFormat());
                    muxer.start();
                    muxerStarted = true;
                    continue;
                }
                if (outputIndex < 0) continue;
                idleCount = 0;
                final ByteBuffer output = codec.getOutputBuffer(outputIndex);
                if ((bufferInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    bufferInfo.size = 0;
                }
                if (bufferInfo.size > 0) {
                    if (!muxerStarted || output == null) {
                        codec.releaseOutputBuffer(outputIndex, false);
                        throw new IllegalStateException(
                            "MediaMuxer was not ready for codec output");
                    }
                    output.position(bufferInfo.offset);
                    output.limit(bufferInfo.offset + bufferInfo.size);
                    muxer.writeSampleData(muxerTrack, output, bufferInfo);
                }
                final boolean eos =
                    (bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                codec.releaseOutputBuffer(outputIndex, false);
                if (eos) return;
                if (!endOfStream) continue;
            }
        }

        private void release(boolean completed) {
            if (closed) return;
            closed = true;
            if (codec != null) {
                try { codec.stop(); } catch (Throwable ignored) {}
                try { codec.release(); } catch (Throwable ignored) {}
                codec = null;
            }
            if (muxer != null) {
                if (muxerStarted) {
                    try { muxer.stop(); } catch (Throwable ignored) {
                        if (completed) new File(path).delete();
                    }
                }
                try { muxer.release(); } catch (Throwable ignored) {}
                muxer = null;
            }
        }

        private static void fillYuv420(Image image, ByteBuffer bgra,
                                       int width, int height) {
            final Image.Plane[] planes = image.getPlanes();
            if (planes.length < 3) {
                throw new IllegalStateException("Flexible YUV image has no planes");
            }
            final ByteBuffer yPlane = planes[0].getBuffer();
            final ByteBuffer uPlane = planes[1].getBuffer();
            final ByteBuffer vPlane = planes[2].getBuffer();
            final int yRowStride = planes[0].getRowStride();
            final int yPixelStride = planes[0].getPixelStride();
            final int uRowStride = planes[1].getRowStride();
            final int uPixelStride = planes[1].getPixelStride();
            final int vRowStride = planes[2].getRowStride();
            final int vPixelStride = planes[2].getPixelStride();
            final int yBase = yPlane.position();
            final int uBase = uPlane.position();
            final int vBase = vPlane.position();
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    final int pixel = (y * width + x) * 4;
                    final int blue = bgra.get(pixel) & 0xff;
                    final int green = bgra.get(pixel + 1) & 0xff;
                    final int red = bgra.get(pixel + 2) & 0xff;
                    final int luma = clamp8(
                        ((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
                    yPlane.put(yBase + y * yRowStride + x * yPixelStride,
                        (byte) luma);
                    if ((x & 1) == 0 && (y & 1) == 0) {
                        final int chromaX = x / 2;
                        final int chromaY = y / 2;
                        final int u = clamp8(
                            ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
                        final int v = clamp8(
                            ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
                        uPlane.put(uBase + chromaY * uRowStride +
                            chromaX * uPixelStride, (byte) u);
                        vPlane.put(vBase + chromaY * vRowStride +
                            chromaX * vPixelStride, (byte) v);
                    }
                }
            }
        }

        private static int clamp8(int value) {
            return Math.max(0, Math.min(255, value));
        }
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
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0 &&
            getIntent().getBooleanExtra("creatorstudio.codecSelfTest", false)) {
            new Thread(this::runMediaEncoderSelfTest,
                "CreatorStudioCodecSelfTest").start();
        }
    }

    private void runMediaEncoderSelfTest() {
        long videoHandle = 0;
        long audioHandle = 0;
        final File videoFile = new File(getCacheDir(), "codec-self-test.mp4");
        final File audioFile = new File(getCacheDir(), "codec-self-test.m4a");
        videoFile.delete();
        audioFile.delete();
        try {
            final String status = mediaEncoderStatus();
            if (!status.isEmpty()) throw new IllegalStateException(status);
            videoHandle = createVideoEncoder(videoFile.getAbsolutePath(),
                64, 64, 256_000, 30);
            if (videoHandle == 0) {
                throw new IllegalStateException("video encoder creation failed");
            }
            final ByteBuffer bgra = ByteBuffer.allocateDirect(64 * 64 * 4);
            for (int pixel = 0; pixel < 64 * 64; ++pixel) {
                bgra.put((byte) 0x20);
                bgra.put((byte) 0x80);
                bgra.put((byte) 0xe0);
                bgra.put((byte) 0xff);
            }
            for (int frame = 0; frame < 3; ++frame) {
                final String error = writeVideoEncoderFrame(
                    videoHandle, bgra, frame * 33_333L);
                if (!error.isEmpty()) throw new IllegalStateException(error);
            }
            final String videoFinish = finishMediaEncoder(videoHandle);
            videoHandle = 0;
            if (!videoFinish.isEmpty()) {
                throw new IllegalStateException(videoFinish);
            }

            audioHandle = createAudioEncoder(audioFile.getAbsolutePath(),
                48_000, 2, 96_000);
            if (audioHandle == 0) {
                throw new IllegalStateException("audio encoder creation failed");
            }
            final int sampleCount = 4_800 * 2;
            final ByteBuffer pcm = ByteBuffer.allocateDirect(sampleCount * 2)
                .order(ByteOrder.nativeOrder());
            for (int sample = 0; sample < sampleCount; ++sample) {
                pcm.putShort((short) 0);
            }
            final String audioWrite = writeAudioEncoderSamples(
                audioHandle, pcm, sampleCount, 0);
            if (!audioWrite.isEmpty()) {
                throw new IllegalStateException(audioWrite);
            }
            final String audioFinish = finishMediaEncoder(audioHandle);
            audioHandle = 0;
            if (!audioFinish.isEmpty()) {
                throw new IllegalStateException(audioFinish);
            }
            if (videoFile.length() <= 0 || audioFile.length() <= 0) {
                throw new IllegalStateException("MediaMuxer output is empty");
            }
            Log.i("CreatorStudioCodec", "PASS video=" + videoFile.length() +
                " audio=" + audioFile.length());
        } catch (Throwable error) {
            Log.e("CreatorStudioCodec", "FAIL " +
                errorMessage(error, "unknown codec error"), error);
        } finally {
            if (videoHandle != 0) abortMediaEncoder(videoHandle);
            if (audioHandle != 0) abortMediaEncoder(audioHandle);
            videoFile.delete();
            audioFile.delete();
        }
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
