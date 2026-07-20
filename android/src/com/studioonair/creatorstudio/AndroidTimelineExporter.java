package com.studioonair.creatorstudio;

import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.RectF;
import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.media.MediaMuxer;
import android.provider.MediaStore;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/** Bounded, cancellable timeline renderer backed by Android platform codecs. */
final class AndroidTimelineExporter {
    static final int PENDING = 0;
    static final int RUNNING = 1;
    static final int COMPLETED = 4;
    static final int FAILED = 5;
    static final int CANCELLED = 6;
    private static final int OUTPUT_SAMPLE_RATE = 48_000;
    private static final int OUTPUT_CHANNELS = 2;
    private static final int AUDIO_BLOCK_FRAMES = 1024;
    private static final int MAX_CLIPS = 512;
    private static final long MAX_DURATION_US = 24L * 60L * 60L * 1_000_000L;
    private static final AtomicLong nextHandle = new AtomicLong(1);
    private static final ConcurrentHashMap<Long, Session> sessions =
        new ConcurrentHashMap<>();

    private AndroidTimelineExporter() {}

    static long start(Context context, String json) {
        try {
            if (context == null) return 0;
            final Session session = new Session(new Spec(new JSONObject(json)));
            long handle;
            do {
                handle = nextHandle.getAndIncrement();
                if (handle <= 0) {
                    nextHandle.set(1);
                    handle = nextHandle.getAndIncrement();
                }
            } while (sessions.putIfAbsent(handle, session) != null);
            final File workRoot = new File(context.getCacheDir(),
                "timeline-export-work");
            session.start(handle, new File(workRoot, "session-" + handle));
            return handle;
        } catch (Throwable error) {
            Log.e("CreatorStudioExport", "Could not start timeline export", error);
            return 0;
        }
    }

    static int state(long handle) {
        final Session session = sessions.get(handle);
        return session == null ? FAILED : session.state;
    }

    static double progress(long handle) {
        final Session session = sessions.get(handle);
        return session == null ? 0.0 : session.progress;
    }

    static String error(long handle) {
        final Session session = sessions.get(handle);
        return session == null ? "Android timeline export handle is stale"
                               : session.error;
    }

    static void cancel(long handle) {
        final Session session = sessions.get(handle);
        if (session != null) session.cancel();
    }

    static void release(long handle) {
        final Session session = sessions.remove(handle);
        if (session == null) return;
        session.cancel();
        session.await();
    }

    static void cancelAll() {
        for (Session session : sessions.values()) session.cancel();
    }

    static void cleanupStaleWork(Context context) {
        if (context == null || !sessions.isEmpty()) return;
        deleteTree(new File(context.getCacheDir(), "timeline-export-work"));
    }

    private static final class Spec {
        final String destination;
        final int width;
        final int height;
        final int frameRateNumerator;
        final int frameRateDenominator;
        final int videoBitrate;
        final int audioBitrate;
        final long durationUs;
        final List<VisualClip> visuals;
        final List<AudioClip> audio;

        Spec(JSONObject json) throws Exception {
            if (json.getInt("version") != 1) {
                throw new IllegalArgumentException("Unsupported export manifest version");
            }
            destination = requiredPath(json.getString("destination"));
            width = positive(json.getInt("width"), 4096, "width");
            height = positive(json.getInt("height"), 4096, "height");
            if ((width & 1) != 0 || (height & 1) != 0) {
                throw new IllegalArgumentException("Export dimensions must be even");
            }
            frameRateNumerator = positive(json.getInt("frameRateNumerator"),
                240_000, "frame-rate numerator");
            frameRateDenominator = positive(json.getInt("frameRateDenominator"),
                100_000, "frame-rate denominator");
            final double framesPerSecond =
                (double) frameRateNumerator / frameRateDenominator;
            if (framesPerSecond < 1.0 || framesPerSecond > 120.0) {
                throw new IllegalArgumentException("Export frame rate is unsupported");
            }
            videoBitrate = positive(json.getInt("videoBitrate"), 100_000_000,
                "video bitrate");
            audioBitrate = positive(json.getInt("audioBitrate"), 1_000_000,
                "audio bitrate");
            durationUs = json.getLong("durationUs");
            if (durationUs <= 0 || durationUs > MAX_DURATION_US) {
                throw new IllegalArgumentException("Export duration is unsupported");
            }
            visuals = new ArrayList<>();
            final JSONArray visualRows = json.getJSONArray("visualClips");
            if (visualRows.length() > MAX_CLIPS) {
                throw new IllegalArgumentException("Too many visual clips");
            }
            for (int i = 0; i < visualRows.length(); ++i) {
                visuals.add(new VisualClip(visualRows.getJSONObject(i)));
            }
            Collections.sort(visuals, Comparator
                .comparingInt((VisualClip clip) -> clip.transform.zOrder)
                .thenComparingInt(clip -> clip.trackOrder)
                .thenComparingLong(clip -> clip.timelineStartUs)
                .thenComparing(clip -> clip.identity));
            audio = new ArrayList<>();
            final JSONArray audioRows = json.getJSONArray("audioClips");
            if (audioRows.length() > MAX_CLIPS) {
                throw new IllegalArgumentException("Too many audio clips");
            }
            for (int i = 0; i < audioRows.length(); ++i) {
                audio.add(new AudioClip(audioRows.getJSONObject(i)));
            }
        }

        int frameRate() {
            return Math.max(1, (int) Math.round(
                (double) frameRateNumerator / frameRateDenominator));
        }
    }

    private static class ClipTiming {
        final String path;
        final long sourceStartUs;
        final long timelineStartUs;
        final long durationUs;

        ClipTiming(JSONObject json) throws Exception {
            path = requiredPath(json.getString("path"));
            sourceStartUs = nonNegative(json.getLong("sourceStartUs"),
                "source start");
            timelineStartUs = nonNegative(json.getLong("timelineStartUs"),
                "timeline start");
            durationUs = json.getLong("durationUs");
            if (durationUs <= 0 || durationUs > MAX_DURATION_US ||
                sourceStartUs > MAX_DURATION_US - durationUs ||
                timelineStartUs > MAX_DURATION_US - durationUs) {
                throw new IllegalArgumentException("Clip duration is unsupported");
            }
        }

        boolean active(long timelineUs) {
            return timelineUs >= timelineStartUs &&
                timelineUs < timelineStartUs + durationUs;
        }
    }

    private static final class Transform {
        final double x;
        final double y;
        final double width;
        final double height;
        final double scaleX;
        final double scaleY;
        final double rotation;
        final double cropLeft;
        final double cropTop;
        final double cropRight;
        final double cropBottom;
        final double opacity;
        final int zOrder;

        Transform(JSONObject json) throws Exception {
            x = json.getDouble("x");
            y = json.getDouble("y");
            width = json.getDouble("width");
            height = json.getDouble("height");
            scaleX = json.getDouble("scaleX");
            scaleY = json.getDouble("scaleY");
            rotation = json.getDouble("rotation");
            cropLeft = json.getDouble("cropLeft");
            cropTop = json.getDouble("cropTop");
            cropRight = json.getDouble("cropRight");
            cropBottom = json.getDouble("cropBottom");
            opacity = json.getDouble("opacity");
            zOrder = json.getInt("zOrder");
            if (!finite(x) || !finite(y) || !finite(width) || !finite(height) ||
                !finite(scaleX) || !finite(scaleY) || !finite(rotation) ||
                !finite(cropLeft) || !finite(cropTop) || !finite(cropRight) ||
                !finite(cropBottom) || !finite(opacity) || width <= 0.0 ||
                height <= 0.0 || scaleX <= 0.0 || scaleY <= 0.0 ||
                opacity < 0.0 || opacity > 1.0 || cropLeft < 0.0 ||
                cropTop < 0.0 || cropRight < 0.0 || cropBottom < 0.0 ||
                cropLeft + cropRight >= 1.0 || cropTop + cropBottom >= 1.0) {
                throw new IllegalArgumentException("Visual transform is invalid");
            }
        }
    }

    private static final class VisualClip extends ClipTiming {
        final boolean video;
        final String identity;
        final int trackOrder;
        final Transform transform;
        private MediaMetadataRetriever retriever;
        private Bitmap image;

        VisualClip(JSONObject json) throws Exception {
            super(json);
            final String kind = json.getString("kind");
            if (!"video".equals(kind) && !"image".equals(kind)) {
                throw new IllegalArgumentException("Visual clip kind is invalid");
            }
            video = "video".equals(kind);
            identity = json.getString("identity");
            trackOrder = json.getInt("trackOrder");
            transform = new Transform(json.getJSONObject("transform"));
        }

        void open() throws Exception {
            if (video) {
                retriever = new MediaMetadataRetriever();
                retriever.setDataSource(path);
                final String duration = retriever.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_DURATION);
                if (duration == null ||
                    Long.parseLong(duration) * 1_000L + 100_000L <
                        sourceStartUs + durationUs) {
                    throw new IllegalStateException(
                        "Video clip ends before its requested source range " + identity);
                }
            } else {
                image = BitmapFactory.decodeFile(path);
                if (image == null) {
                    throw new IllegalStateException("Could not decode generated image " + identity);
                }
            }
        }

        Bitmap frame(long timelineUs) {
            if (!video) return image;
            final long sourceUs = sourceStartUs + (timelineUs - timelineStartUs);
            return retriever.getFrameAtTime(sourceUs,
                MediaMetadataRetriever.OPTION_CLOSEST);
        }

        void close() {
            if (retriever != null) {
                try { retriever.release(); } catch (Throwable ignored) {}
                retriever = null;
            }
            if (image != null) {
                image.recycle();
                image = null;
            }
        }
    }

    private static final class AudioClip extends ClipTiming {
        final double gainDb;
        final long fadeInUs;
        final long fadeOutUs;

        AudioClip(JSONObject json) throws Exception {
            super(json);
            gainDb = json.getDouble("gainDb");
            fadeInUs = nonNegative(json.getLong("fadeInUs"), "fade in");
            fadeOutUs = nonNegative(json.getLong("fadeOutUs"), "fade out");
            if (!finite(gainDb) || fadeInUs + fadeOutUs > durationUs) {
                throw new IllegalArgumentException("Audio envelope is invalid");
            }
        }

        double factor(long localUs) {
            double value = Math.pow(10.0, gainDb / 20.0);
            if (fadeInUs > 0 && localUs < fadeInUs) {
                value *= Math.max(0.0, (double) localUs / fadeInUs);
            }
            final long remaining = durationUs - localUs;
            if (fadeOutUs > 0 && remaining < fadeOutUs) {
                value *= Math.max(0.0, (double) remaining / fadeOutUs);
            }
            return value;
        }
    }

    private static final class Cancelled extends Exception {}

    private static final class Session implements Runnable {
        final Spec spec;
        final AtomicBoolean cancelled = new AtomicBoolean(false);
        volatile int state = PENDING;
        volatile double progress;
        volatile String error = "";
        private Thread worker;
        private File videoFile;
        private File audioFile;
        private File workDirectory;
        private final List<File> decodedFiles = new ArrayList<>();

        Session(Spec spec) { this.spec = spec; }

        void start(long handle, File workDirectory) {
            this.workDirectory = workDirectory;
            if (!workDirectory.mkdirs() && !workDirectory.isDirectory()) {
                error = "Android export work directory is unavailable";
                state = FAILED;
                return;
            }
            worker = new Thread(this, "CreatorStudioExport-" + handle);
            worker.start();
        }

        void cancel() { cancelled.set(true); }

        void await() {
            final Thread thread = worker;
            if (thread == null || thread == Thread.currentThread()) return;
            try { thread.join(5_000); } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }

        void checkCancelled() throws Cancelled {
            if (cancelled.get() || Thread.currentThread().isInterrupted()) {
                throw new Cancelled();
            }
        }

        @Override public void run() {
            final File destination = new File(spec.destination);
            videoFile = new File(workDirectory, "video.mp4");
            audioFile = new File(workDirectory, "audio.m4a");
            delete(destination);
            delete(videoFile);
            delete(audioFile);
            state = RUNNING;
            try {
                renderVideo();
                checkCancelled();
                renderAudio();
                checkCancelled();
                remux(videoFile, audioFile, destination);
                checkCancelled();
                validate(destination);
                progress = 1.0;
                state = COMPLETED;
            } catch (Cancelled stopped) {
                error = "Android timeline export cancelled";
                state = CANCELLED;
                delete(destination);
            } catch (Throwable failure) {
                error = message(failure, "Android timeline export failed");
                Log.e("CreatorStudioExport", error, failure);
                state = FAILED;
                delete(destination);
            } finally {
                delete(videoFile);
                delete(audioFile);
                for (File file : decodedFiles) delete(file);
                deleteTree(workDirectory);
            }
        }

        private void renderVideo() throws Exception {
            final MediaFormat format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC, spec.width, spec.height);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                android.media.MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            format.setInteger(MediaFormat.KEY_BIT_RATE, spec.videoBitrate);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, spec.frameRate());
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            final CreatorStudioActivity.MediaSegmentEncoder encoder =
                new CreatorStudioActivity.MediaSegmentEncoder(
                    videoFile.getAbsolutePath(), format, true, spec.width,
                    spec.height, 0, 0);
            final Bitmap canvasBitmap = Bitmap.createBitmap(
                spec.width, spec.height, Bitmap.Config.ARGB_8888);
            final Canvas canvas = new Canvas(canvasBitmap);
            final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG |
                                          Paint.FILTER_BITMAP_FLAG);
            final ByteBuffer bgra = ByteBuffer.allocateDirect(
                spec.width * spec.height * 4).order(ByteOrder.nativeOrder());
            final int[] row = new int[spec.width];
            boolean finished = false;
            try {
                for (VisualClip clip : spec.visuals) {
                    checkCancelled();
                    clip.open();
                }
                final long frameCount = Math.max(1L,
                    divideCeil(spec.durationUs * spec.frameRateNumerator,
                               1_000_000L * spec.frameRateDenominator));
                for (long frame = 0; frame < frameCount; ++frame) {
                    checkCancelled();
                    final long timelineUs = frame * 1_000_000L *
                        spec.frameRateDenominator / spec.frameRateNumerator;
                    canvas.drawColor(Color.BLACK, PorterDuff.Mode.SRC);
                    for (VisualClip clip : spec.visuals) {
                        if (!clip.active(timelineUs)) continue;
                        final Bitmap source = clip.frame(timelineUs);
                        if (source == null) {
                            throw new IllegalStateException(
                                "Could not decode visual clip " + clip.identity);
                        }
                        drawVisual(canvas, paint, source, clip.transform,
                                   spec.width, spec.height);
                        if (clip.video) source.recycle();
                    }
                    bgra.clear();
                    for (int y = 0; y < spec.height; ++y) {
                        canvasBitmap.getPixels(row, 0, spec.width, 0, y,
                                               spec.width, 1);
                        for (int pixel : row) {
                            bgra.put((byte) Color.blue(pixel));
                            bgra.put((byte) Color.green(pixel));
                            bgra.put((byte) Color.red(pixel));
                            bgra.put((byte) Color.alpha(pixel));
                        }
                    }
                    encoder.writeVideo(bgra, timelineUs);
                    progress = Math.min(0.60,
                        0.60 * (frame + 1.0) / frameCount);
                }
                encoder.finish();
                finished = true;
            } finally {
                for (VisualClip clip : spec.visuals) clip.close();
                canvasBitmap.recycle();
                if (!finished) encoder.abort();
            }
        }

        private void renderAudio() throws Exception {
            final List<DecodedAudio> decoded = new ArrayList<>();
            try {
                for (int index = 0; index < spec.audio.size(); ++index) {
                    checkCancelled();
                    final File file = new File(workDirectory,
                        "decoded-" + index + ".pcm");
                    decodedFiles.add(file);
                    decoded.add(decode(spec.audio.get(index), file));
                    progress = 0.60 + 0.10 * (index + 1.0) /
                        Math.max(1, spec.audio.size());
                }
                final MediaFormat format = MediaFormat.createAudioFormat(
                    MediaFormat.MIMETYPE_AUDIO_AAC, OUTPUT_SAMPLE_RATE,
                    OUTPUT_CHANNELS);
                format.setInteger(MediaFormat.KEY_AAC_PROFILE,
                    android.media.MediaCodecInfo.CodecProfileLevel.AACObjectLC);
                format.setInteger(MediaFormat.KEY_BIT_RATE, spec.audioBitrate);
                format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 64 * 1024);
                final CreatorStudioActivity.MediaSegmentEncoder encoder =
                    new CreatorStudioActivity.MediaSegmentEncoder(
                        audioFile.getAbsolutePath(), format, false, 0, 0,
                        OUTPUT_SAMPLE_RATE, OUTPUT_CHANNELS);
                boolean finished = false;
                try {
                    final long totalFrames = Math.max(1L,
                        divideCeil(spec.durationUs * OUTPUT_SAMPLE_RATE, 1_000_000L));
                    final ByteBuffer pcm = ByteBuffer.allocateDirect(
                        AUDIO_BLOCK_FRAMES * OUTPUT_CHANNELS * 2)
                        .order(ByteOrder.nativeOrder());
                    long firstFrame = 0;
                    while (firstFrame < totalFrames) {
                        checkCancelled();
                        final int frames = (int) Math.min(AUDIO_BLOCK_FRAMES,
                            totalFrames - firstFrame);
                        pcm.clear();
                        for (int within = 0; within < frames; ++within) {
                            final long outputFrame = firstFrame + within;
                            final long timelineUs = outputFrame * 1_000_000L /
                                OUTPUT_SAMPLE_RATE;
                            double left = 0.0;
                            double right = 0.0;
                            for (int index = 0; index < spec.audio.size(); ++index) {
                                final AudioClip clip = spec.audio.get(index);
                                if (!clip.active(timelineUs)) continue;
                                final long localUs = timelineUs - clip.timelineStartUs;
                                final double sourceFrame =
                                    (double) localUs * decoded.get(index).sampleRate /
                                    1_000_000.0;
                                final double factor = clip.factor(localUs);
                                left += decoded.get(index).sample(sourceFrame, 0) * factor;
                                right += decoded.get(index).sample(sourceFrame, 1) * factor;
                            }
                            pcm.putShort(toPcm16(left));
                            pcm.putShort(toPcm16(right));
                        }
                        encoder.writeAudio(pcm, frames * OUTPUT_CHANNELS,
                            firstFrame * 1_000_000L / OUTPUT_SAMPLE_RATE);
                        firstFrame += frames;
                        progress = 0.70 + 0.15 * firstFrame / totalFrames;
                    }
                    encoder.finish();
                    finished = true;
                } finally {
                    if (!finished) encoder.abort();
                }
            } finally {
                for (DecodedAudio value : decoded) value.close();
            }
        }

        private DecodedAudio decode(AudioClip clip, File outputFile)
                throws Exception {
            delete(outputFile);
            final MediaExtractor extractor = new MediaExtractor();
            MediaCodec decoder = null;
            PcmWriter writer = null;
            try {
                extractor.setDataSource(clip.path);
                final int track = findTrack(extractor, "audio/");
                if (track < 0) {
                    throw new IllegalStateException("Audible clip has no decodable audio track");
                }
                extractor.selectTrack(track);
                extractor.seekTo(clip.sourceStartUs,
                    MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
                final MediaFormat inputFormat = extractor.getTrackFormat(track);
                final String mime = inputFormat.getString(MediaFormat.KEY_MIME);
                if (mime == null) throw new IllegalStateException("Audio MIME is missing");
                decoder = MediaCodec.createDecoderByType(mime);
                decoder.configure(inputFormat, null, null, 0);
                decoder.start();
                writer = new PcmWriter(outputFile);
                final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                boolean inputDone = false;
                boolean outputDone = false;
                int sampleRate = 0;
                int channels = 0;
                int encoding = AudioFormat.ENCODING_PCM_16BIT;
                long writtenFrames = 0;
                long targetFrames = 0;
                while (!outputDone) {
                    checkCancelled();
                    if (!inputDone) {
                        final int inputIndex = decoder.dequeueInputBuffer(10_000);
                        if (inputIndex >= 0) {
                            final ByteBuffer input = decoder.getInputBuffer(inputIndex);
                            if (input == null) throw new IllegalStateException("Audio decoder input missing");
                            input.clear();
                            final long sampleTime = extractor.getSampleTime();
                            if (sampleTime < 0 ||
                                sampleTime >= clip.sourceStartUs + clip.durationUs) {
                                decoder.queueInputBuffer(inputIndex, 0, 0,
                                    Math.max(0, clip.sourceStartUs + clip.durationUs),
                                    MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                inputDone = true;
                            } else {
                                final int bytes = extractor.readSampleData(input, 0);
                                if (bytes < 0) {
                                    decoder.queueInputBuffer(inputIndex, 0, 0,
                                        Math.max(0, sampleTime),
                                        MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                    inputDone = true;
                                } else {
                                    decoder.queueInputBuffer(inputIndex, 0, bytes,
                                        sampleTime, 0);
                                    extractor.advance();
                                }
                            }
                        }
                    }
                    final int outputIndex = decoder.dequeueOutputBuffer(info, 10_000);
                    if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        final MediaFormat decodedFormat = decoder.getOutputFormat();
                        sampleRate = decodedFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                        channels = decodedFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                        if (decodedFormat.containsKey(MediaFormat.KEY_PCM_ENCODING)) {
                            encoding = decodedFormat.getInteger(MediaFormat.KEY_PCM_ENCODING);
                        }
                        if (sampleRate <= 0 || sampleRate > 384_000 ||
                            channels <= 0 || channels > 2 ||
                            (encoding != AudioFormat.ENCODING_PCM_16BIT &&
                             encoding != AudioFormat.ENCODING_PCM_FLOAT)) {
                            throw new IllegalStateException("Decoded PCM format is unsupported");
                        }
                        targetFrames = divideCeil(clip.durationUs * sampleRate,
                                                  1_000_000L);
                    } else if (outputIndex >= 0) {
                        if (sampleRate == 0) {
                            throw new IllegalStateException("Audio decoder format is unavailable");
                        }
                        final ByteBuffer output = decoder.getOutputBuffer(outputIndex);
                        if (info.size > 0 && output != null) {
                            output.order(ByteOrder.nativeOrder());
                            final int bytesPerSample =
                                encoding == AudioFormat.ENCODING_PCM_FLOAT ? 4 : 2;
                            final int frames = info.size / (bytesPerSample * channels);
                            for (int frame = 0; frame < frames; ++frame) {
                                final long sampleUs = info.presentationTimeUs +
                                    frame * 1_000_000L / sampleRate;
                                if (sampleUs < clip.sourceStartUs ||
                                    sampleUs >= clip.sourceStartUs + clip.durationUs) continue;
                                final long expected = Math.max(0,
                                    (sampleUs - clip.sourceStartUs) * sampleRate /
                                    1_000_000L);
                                while (writtenFrames < Math.min(expected, targetFrames)) {
                                    writer.write(0.0f, 0.0f);
                                    ++writtenFrames;
                                }
                                if (writtenFrames >= targetFrames || expected < writtenFrames) continue;
                                final int base = info.offset +
                                    frame * bytesPerSample * channels;
                                final float left = decodedSample(output, base,
                                    encoding);
                                final float right = channels == 1 ? left
                                    : decodedSample(output, base + bytesPerSample,
                                                    encoding);
                                writer.write(left, right);
                                ++writtenFrames;
                            }
                        }
                        outputDone = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                        decoder.releaseOutputBuffer(outputIndex, false);
                    }
                }
                final long toleratedTailFrames = Math.max(2_048L, sampleRate / 20L);
                if (targetFrames - writtenFrames > toleratedTailFrames) {
                    throw new IllegalStateException(
                        "Audio clip ends before its requested source range");
                }
                while (writtenFrames < targetFrames) {
                    writer.write(0.0f, 0.0f);
                    ++writtenFrames;
                }
                writer.close();
                writer = null;
                return new DecodedAudio(outputFile, sampleRate, writtenFrames);
            } finally {
                if (writer != null) writer.close();
                if (decoder != null) {
                    try { decoder.stop(); } catch (Throwable ignored) {}
                    try { decoder.release(); } catch (Throwable ignored) {}
                }
                extractor.release();
            }
        }

        private void remux(File video, File audio, File destination)
                throws Exception {
            final MediaExtractor videoExtractor = new MediaExtractor();
            final MediaExtractor audioExtractor = new MediaExtractor();
            MediaMuxer muxer = null;
            boolean started = false;
            try {
                videoExtractor.setDataSource(video.getAbsolutePath());
                audioExtractor.setDataSource(audio.getAbsolutePath());
                final int sourceVideo = findTrack(videoExtractor, "video/");
                final int sourceAudio = findTrack(audioExtractor, "audio/");
                if (sourceVideo < 0 || sourceAudio < 0) {
                    throw new IllegalStateException(
                        "Encoded export tracks are incomplete; video=" +
                        trackSummary(videoExtractor) + "; audio=" +
                        trackSummary(audioExtractor) + "; bytes=" +
                        video.length() + "/" + audio.length());
                }
                muxer = new MediaMuxer(destination.getAbsolutePath(),
                    MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
                final int targetVideo = muxer.addTrack(
                    videoExtractor.getTrackFormat(sourceVideo));
                final int targetAudio = muxer.addTrack(
                    audioExtractor.getTrackFormat(sourceAudio));
                muxer.start();
                started = true;
                copyTrack(videoExtractor, sourceVideo, muxer, targetVideo, 0.85, 0.91);
                copyTrack(audioExtractor, sourceAudio, muxer, targetAudio, 0.91, 0.98);
            } finally {
                if (muxer != null) {
                    if (started) {
                        try { muxer.stop(); } catch (RuntimeException stopError) {
                            delete(destination);
                            throw stopError;
                        }
                    }
                    muxer.release();
                }
                videoExtractor.release();
                audioExtractor.release();
            }
        }

        private void copyTrack(MediaExtractor extractor, int sourceTrack,
                               MediaMuxer muxer, int targetTrack,
                               double startProgress, double endProgress)
                throws Exception {
            extractor.selectTrack(sourceTrack);
            final MediaFormat format = extractor.getTrackFormat(sourceTrack);
            int capacity = 1024 * 1024;
            if (format.containsKey(MediaFormat.KEY_MAX_INPUT_SIZE)) {
                capacity = Math.max(capacity,
                    Math.min(16 * 1024 * 1024,
                             format.getInteger(MediaFormat.KEY_MAX_INPUT_SIZE)));
            }
            final ByteBuffer buffer = ByteBuffer.allocateDirect(capacity);
            final MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (true) {
                checkCancelled();
                buffer.clear();
                final int bytes = extractor.readSampleData(buffer, 0);
                if (bytes < 0) break;
                info.offset = 0;
                info.size = bytes;
                info.presentationTimeUs = extractor.getSampleTime();
                info.flags = extractor.getSampleFlags();
                muxer.writeSampleData(targetTrack, buffer, info);
                progress = startProgress + (endProgress - startProgress) *
                    Math.min(1.0, (double) Math.max(0, info.presentationTimeUs) /
                                      spec.durationUs);
                extractor.advance();
            }
        }

        private void validate(File destination) throws Exception {
            if (!destination.isFile() || destination.length() <= 0) {
                throw new IllegalStateException("MediaMuxer produced an empty export");
            }
            final MediaExtractor extractor = new MediaExtractor();
            try {
                extractor.setDataSource(destination.getAbsolutePath());
                final int video = findTrack(extractor, "video/");
                final int audio = findTrack(extractor, "audio/");
                if (video < 0 || audio < 0) {
                    throw new IllegalStateException("Export MP4 is missing video or audio");
                }
                final MediaFormat videoFormat = extractor.getTrackFormat(video);
                final MediaFormat audioFormat = extractor.getTrackFormat(audio);
                if (!MediaFormat.MIMETYPE_VIDEO_AVC.equals(
                        videoFormat.getString(MediaFormat.KEY_MIME)) ||
                    videoFormat.getInteger(MediaFormat.KEY_WIDTH) != spec.width ||
                    videoFormat.getInteger(MediaFormat.KEY_HEIGHT) != spec.height ||
                    !MediaFormat.MIMETYPE_AUDIO_AAC.equals(
                        audioFormat.getString(MediaFormat.KEY_MIME)) ||
                    audioFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE) !=
                        OUTPUT_SAMPLE_RATE ||
                    audioFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT) !=
                        OUTPUT_CHANNELS) {
                    throw new IllegalStateException("Export MP4 profile validation failed");
                }
            } finally {
                extractor.release();
            }
        }
    }

    private static final class PcmWriter {
        private final OutputStream output;
        private final ByteBuffer buffer = ByteBuffer.allocate(64 * 1024)
            .order(ByteOrder.LITTLE_ENDIAN);

        PcmWriter(File file) throws Exception {
            output = new BufferedOutputStream(new FileOutputStream(file));
        }

        void write(float left, float right) throws Exception {
            if (buffer.remaining() < 8) flush();
            buffer.putFloat(left);
            buffer.putFloat(right);
        }

        void flush() throws Exception {
            output.write(buffer.array(), 0, buffer.position());
            buffer.clear();
        }

        void close() throws Exception {
            flush();
            output.close();
        }
    }

    private static final class DecodedAudio {
        final int sampleRate;
        final long frames;
        private final RandomAccessFile input;
        private final byte[] bytes = new byte[4096 * 8];
        private final ByteBuffer values = ByteBuffer.wrap(bytes)
            .order(ByteOrder.LITTLE_ENDIAN);
        private long windowStart = -1;
        private int windowFrames;

        DecodedAudio(File file, int sampleRate, long frames) throws Exception {
            this.sampleRate = sampleRate;
            this.frames = frames;
            input = new RandomAccessFile(file, "r");
        }

        float sample(double position, int channel) throws Exception {
            if (position < 0.0 || position >= frames) return 0.0f;
            final long left = (long) Math.floor(position);
            final long right = Math.min(frames - 1, left + 1);
            final double fraction = position - left;
            final float first = sampleAt(left, channel);
            final float second = sampleAt(right, channel);
            return (float) (first + (second - first) * fraction);
        }

        private float sampleAt(long frame, int channel) throws Exception {
            if (frame < windowStart || frame >= windowStart + windowFrames) {
                windowStart = (frame / 4096) * 4096;
                input.seek(windowStart * 8);
                final int wanted = (int) Math.min(bytes.length,
                    Math.max(0L, (frames - windowStart) * 8));
                int read = 0;
                while (read < wanted) {
                    final int amount = input.read(bytes, read, wanted - read);
                    if (amount < 0) break;
                    read += amount;
                }
                windowFrames = read / 8;
                values.position(0);
            }
            final int offset = (int) (frame - windowStart) * 8 + channel * 4;
            return values.getFloat(offset);
        }

        void close() {
            try { input.close(); } catch (Throwable ignored) {}
        }
    }

    private static void drawVisual(Canvas canvas, Paint paint, Bitmap source,
                                   Transform transform, int canvasWidth,
                                   int canvasHeight) {
        final int left = Math.max(0,
            Math.min(source.getWidth() - 1,
                (int) Math.floor(transform.cropLeft * source.getWidth())));
        final int top = Math.max(0,
            Math.min(source.getHeight() - 1,
                (int) Math.floor(transform.cropTop * source.getHeight())));
        final int right = Math.max(left + 1,
            Math.min(source.getWidth(),
                (int) Math.ceil((1.0 - transform.cropRight) * source.getWidth())));
        final int bottom = Math.max(top + 1,
            Math.min(source.getHeight(),
                (int) Math.ceil((1.0 - transform.cropBottom) * source.getHeight())));
        final float unscaledWidth = (float) (transform.width * canvasWidth);
        final float unscaledHeight = (float) (transform.height * canvasHeight);
        final float centerX = (float) (transform.x * canvasWidth +
                                       unscaledWidth * 0.5);
        final float centerY = (float) (transform.y * canvasHeight +
                                       unscaledHeight * 0.5);
        final float width = (float) (unscaledWidth * transform.scaleX);
        final float height = (float) (unscaledHeight * transform.scaleY);
        final RectF destination = new RectF(centerX - width * 0.5f,
            centerY - height * 0.5f, centerX + width * 0.5f,
            centerY + height * 0.5f);
        paint.setAlpha((int) Math.round(transform.opacity * 255.0));
        final int save = canvas.save();
        canvas.rotate((float) transform.rotation, centerX, centerY);
        canvas.drawBitmap(source, new Rect(left, top, right, bottom),
                          destination, paint);
        canvas.restoreToCount(save);
    }

    private static int findTrack(MediaExtractor extractor, String prefix) {
        for (int index = 0; index < extractor.getTrackCount(); ++index) {
            final String mime = extractor.getTrackFormat(index).getString(
                MediaFormat.KEY_MIME);
            if (mime != null && mime.startsWith(prefix)) return index;
        }
        return -1;
    }

    private static String trackSummary(MediaExtractor extractor) {
        final StringBuilder value = new StringBuilder();
        for (int index = 0; index < extractor.getTrackCount(); ++index) {
            if (index > 0) value.append(',');
            value.append(extractor.getTrackFormat(index).getString(
                MediaFormat.KEY_MIME));
        }
        return value.length() == 0 ? "none" : value.toString();
    }

    private static float decodedSample(ByteBuffer buffer, int offset,
                                       int encoding) {
        if (encoding == AudioFormat.ENCODING_PCM_FLOAT) {
            return Math.max(-1.0f, Math.min(1.0f, buffer.getFloat(offset)));
        }
        return buffer.getShort(offset) / 32768.0f;
    }

    private static short toPcm16(double sample) {
        return (short) Math.round(Math.max(-1.0, Math.min(1.0, sample)) * 32767.0);
    }

    private static long divideCeil(long value, long divisor) {
        if (value < 0 || divisor <= 0 || value > Long.MAX_VALUE - divisor + 1) {
            throw new IllegalArgumentException("Timeline arithmetic overflow");
        }
        return (value + divisor - 1) / divisor;
    }

    private static int positive(int value, int maximum, String name) {
        if (value <= 0 || value > maximum) {
            throw new IllegalArgumentException("Export " + name + " is invalid");
        }
        return value;
    }

    private static long nonNegative(long value, String name) {
        if (value < 0) throw new IllegalArgumentException(name + " is negative");
        return value;
    }

    private static boolean finite(double value) {
        return !Double.isNaN(value) && !Double.isInfinite(value);
    }

    private static String requiredPath(String path) {
        if (path == null || path.isEmpty() || !new File(path).isAbsolute()) {
            throw new IllegalArgumentException("Export path is invalid");
        }
        return path;
    }

    private static String message(Throwable error, String fallback) {
        final String value = error == null ? null : error.getMessage();
        return value == null || value.isEmpty() ? fallback : value;
    }

    private static void delete(File file) {
        if (file != null && file.exists() && !file.delete()) {
            Log.w("CreatorStudioExport", "Could not remove " + file.getAbsolutePath());
        }
    }

    private static void deleteTree(File root) {
        if (root == null || !root.exists()) return;
        final File[] children = root.listFiles();
        if (children != null) {
            for (File child : children) {
                if (child.isDirectory()) deleteTree(child);
                else delete(child);
            }
        }
        delete(root);
    }

    static void runSelfTest(Context context) {
        final File root = new File(context.getCacheDir(), "timeline-export-self-test");
        root.mkdirs();
        final File video = new File(root, "source.mp4");
        final File image = new File(root, "source.png");
        final File audio = new File(root, "source.m4a");
        final File destination = new File(root, "result.mp4");
        final File cancelledDestination = new File(root, "cancelled.mp4");
        long videoHandle = 0;
        long audioHandle = 0;
        long exportHandle = 0;
        long cancelHandle = 0;
        Uri scopedUri = null;
        try {
            videoHandle = CreatorStudioActivity.createVideoEncoder(
                video.getAbsolutePath(), 64, 64, 256_000, 30);
            if (videoHandle <= 0) {
                throw new IllegalStateException("Self-test H.264 unavailable");
            }
            final ByteBuffer bgra = ByteBuffer.allocateDirect(64 * 64 * 4);
            for (int frame = 0; frame < 3; ++frame) {
                bgra.clear();
                for (int pixel = 0; pixel < 64 * 64; ++pixel) {
                    bgra.put((byte) (32 + frame * 16));
                    bgra.put((byte) 64);
                    bgra.put((byte) 192);
                    bgra.put((byte) 255);
                }
                final String written =
                    CreatorStudioActivity.writeVideoEncoderFrame(
                        videoHandle, bgra, frame * 33_333L);
                if (!written.isEmpty()) throw new IllegalStateException(written);
            }
            final String videoFinished =
                CreatorStudioActivity.finishMediaEncoder(videoHandle);
            videoHandle = 0;
            if (!videoFinished.isEmpty()) {
                throw new IllegalStateException(videoFinished);
            }

            final Bitmap bitmap = Bitmap.createBitmap(64, 64,
                Bitmap.Config.ARGB_8888);
            bitmap.eraseColor(Color.rgb(224, 96, 32));
            try (FileOutputStream stream = new FileOutputStream(image)) {
                if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)) {
                    throw new IllegalStateException("Self-test PNG creation failed");
                }
            }
            bitmap.recycle();
            audioHandle = CreatorStudioActivity.createAudioEncoder(
                audio.getAbsolutePath(), OUTPUT_SAMPLE_RATE, OUTPUT_CHANNELS,
                96_000);
            if (audioHandle <= 0) throw new IllegalStateException("Self-test AAC unavailable");
            final int frames = 4_800;
            final ByteBuffer pcm = ByteBuffer.allocateDirect(frames * 4)
                .order(ByteOrder.nativeOrder());
            for (int frame = 0; frame < frames; ++frame) {
                final short sample = (short) Math.round(
                    Math.sin(frame * 2.0 * Math.PI * 440.0 / OUTPUT_SAMPLE_RATE) *
                    4_000.0);
                pcm.putShort(sample);
                pcm.putShort(sample);
            }
            final String written = CreatorStudioActivity.writeAudioEncoderSamples(
                audioHandle, pcm, frames * 2, 0);
            if (!written.isEmpty()) throw new IllegalStateException(written);
            final String finished = CreatorStudioActivity.finishMediaEncoder(audioHandle);
            audioHandle = 0;
            if (!finished.isEmpty()) throw new IllegalStateException(finished);

            final JSONObject fullFrame = new JSONObject()
                .put("x", 0.0).put("y", 0.0).put("width", 1.0)
                .put("height", 1.0).put("scaleX", 1.0).put("scaleY", 1.0)
                .put("rotation", 0.0).put("cropLeft", 0.0)
                .put("cropTop", 0.0).put("cropRight", 0.0)
                .put("cropBottom", 0.0).put("opacity", 1.0).put("zOrder", 0);
            final JSONObject videoVisual = new JSONObject()
                .put("path", video.getAbsolutePath()).put("kind", "video")
                .put("identity", "self-test-video").put("trackOrder", 0)
                .put("sourceStartUs", 0).put("timelineStartUs", 0)
                .put("durationUs", 100_000).put("transform", fullFrame);
            final JSONObject overlayTransform = new JSONObject(fullFrame.toString())
                .put("x", 0.50).put("y", 0.0).put("width", 0.50)
                .put("height", 0.50).put("rotation", 5.0)
                .put("opacity", 0.85).put("zOrder", 1);
            final JSONObject imageVisual = new JSONObject()
                .put("path", image.getAbsolutePath()).put("kind", "image")
                .put("identity", "self-test-overlay").put("trackOrder", 1)
                .put("sourceStartUs", 0).put("timelineStartUs", 0)
                .put("durationUs", 100_000).put("transform", overlayTransform);
            final JSONObject audible = new JSONObject()
                .put("path", audio.getAbsolutePath()).put("sourceStartUs", 0)
                .put("timelineStartUs", 0).put("durationUs", 100_000)
                .put("gainDb", 0.0).put("fadeInUs", 0).put("fadeOutUs", 0);
            final JSONObject spec = new JSONObject()
                .put("version", 1).put("destination", destination.getAbsolutePath())
                .put("width", 64).put("height", 64)
                .put("frameRateNumerator", 30).put("frameRateDenominator", 1)
                .put("videoBitrate", 256_000).put("audioBitrate", 96_000)
                .put("durationUs", 100_000)
                .put("visualClips", new JSONArray().put(videoVisual)
                    .put(imageVisual))
                .put("audioClips", new JSONArray().put(audible));
            exportHandle = start(context, spec.toString());
            if (exportHandle <= 0) throw new IllegalStateException("Self-test export did not start");
            final long deadline = System.currentTimeMillis() + 30_000;
            while (state(exportHandle) == PENDING || state(exportHandle) == RUNNING) {
                if (System.currentTimeMillis() >= deadline) {
                    throw new IllegalStateException("Self-test export timed out");
                }
                Thread.sleep(10);
            }
            if (state(exportHandle) != COMPLETED || destination.length() <= 0) {
                throw new IllegalStateException(error(exportHandle));
            }
            final MediaMetadataRetriever rendered = new MediaMetadataRetriever();
            try {
                rendered.setDataSource(destination.getAbsolutePath());
                final Bitmap frame = rendered.getFrameAtTime(0,
                    MediaMetadataRetriever.OPTION_CLOSEST);
                if (frame == null || frame.getPixel(48, 16) == Color.BLACK) {
                    throw new IllegalStateException(
                        "Self-test visual composition was not encoded");
                }
                frame.recycle();
            } finally {
                rendered.release();
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                final ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME,
                    "creator-studio-self-test-" + System.currentTimeMillis() + ".mp4");
                values.put(MediaStore.MediaColumns.MIME_TYPE, "video/mp4");
                values.put(MediaStore.MediaColumns.RELATIVE_PATH,
                    "Movies/CreatorStudioSelfTest");
                scopedUri = context.getContentResolver().insert(
                    MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);
                if (scopedUri == null) {
                    throw new IllegalStateException("Self-test scoped URI was not created");
                }
                final String published = CreatorStudioActivity.publishExport(
                    destination.getAbsolutePath(), scopedUri.toString(), true);
                if (!published.isEmpty()) {
                    throw new IllegalStateException(published);
                }
                try (InputStream input = context.getContentResolver()
                        .openInputStream(scopedUri)) {
                    if (input == null || input.read() < 0) {
                        throw new IllegalStateException(
                            "Self-test scoped export is empty");
                    }
                }
            }

            final JSONObject cancelSpec = new JSONObject(spec.toString())
                .put("destination", cancelledDestination.getAbsolutePath())
                .put("durationUs", 10_000_000);
            final JSONObject cancelImage = new JSONObject(imageVisual.toString())
                .put("durationUs", 10_000_000);
            cancelSpec.put("visualClips", new JSONArray().put(cancelImage));
            cancelSpec.getJSONArray("audioClips").getJSONObject(0)
                .put("durationUs", 10_000_000);
            cancelHandle = start(context, cancelSpec.toString());
            if (cancelHandle <= 0) {
                throw new IllegalStateException("Self-test cancellation did not start");
            }
            cancel(cancelHandle);
            final long cancelDeadline = System.currentTimeMillis() + 5_000;
            while (state(cancelHandle) == PENDING || state(cancelHandle) == RUNNING) {
                if (System.currentTimeMillis() >= cancelDeadline) {
                    throw new IllegalStateException("Self-test cancellation timed out");
                }
                Thread.sleep(10);
            }
            if (state(cancelHandle) != CANCELLED || cancelledDestination.exists()) {
                throw new IllegalStateException("Self-test cancellation leaked output");
            }
            Log.i("CreatorStudioExport", "PASS bytes=" + destination.length() +
                " video+overlay+audio+scoped+cancellation");
        } catch (Throwable failure) {
            Log.e("CreatorStudioExport", "FAIL " + message(failure,
                "unknown timeline export error"), failure);
        } finally {
            if (videoHandle > 0) CreatorStudioActivity.abortMediaEncoder(videoHandle);
            if (audioHandle > 0) CreatorStudioActivity.abortMediaEncoder(audioHandle);
            if (exportHandle > 0) release(exportHandle);
            if (cancelHandle > 0) release(cancelHandle);
            if (scopedUri != null) {
                try { context.getContentResolver().delete(scopedUri, null, null); }
                catch (Throwable ignored) {}
            }
            delete(cancelledDestination);
            delete(destination);
            delete(audio);
            delete(image);
            delete(video);
            delete(root);
        }
    }
}
