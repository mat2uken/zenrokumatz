package com.example.dtvandroid;

import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.opengl.EGL14;
import android.opengl.EGLExt;
import android.opengl.GLES20;
import android.os.Handler;
import android.media.Image;
import android.media.ImageReader;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.io.BufferedInputStream;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.util.Arrays;
import java.util.Locale;

/**
 * Live TS -> hardware transcode -> native fMP4 sink.
 *
 * Reads an MPEG-TS stream (a torne FIFO or a file), demuxes the video PES
 * packets, transcodes them with the Amlogic VPU, and forwards the encoded
 * frames to the native fMP4 writer over a UNIX socket on tmpfs. Nothing is
 * written to the STB's flash.
 */
public final class LiveTsStreamer {
    private static final String TAG = "LiveTs";

    /** HAL_PIXEL_FORMAT_YCbCr_420_SP (NV12), what the vendor encoder accepts. */
    private static final int COLOR_FORMAT_NV12 = 0x15;


    public static class Config {
        /** TS source: a FIFO path or a regular file path. */
        public String tsPath = "/dev/shm/torne/clear.ts";
        /** loopback TCP port where the native fMP4 writer listens. */
        public int sinkPort = 40775;
        public int dstW = 1280;
        public int dstH = 720;
        public String codecName = "OMX.amlogic.video.encoder.avc";
        public String mime = "video/avc";
        /**
         * "avc" or "hevc". HEVC roughly halves the bitrate at the same quality
         * but is not playable in every browser, so it stays opt in.
         */
        public String codec = "avc";
        /** Optional debug tap for the transport bytes the demux reads. */
        public String debugDumpPath = "";
        public long debugDumpLimit = 0;
        public int bitrate = 5500000;
        /**
         * Rate control mode: "vbr" (default) or "cbr". CBR holds the bitrate
         * steady, which suits long recordings; VBR spends bits where the
         * picture needs them. The encoder advertises which modes it accepts.
         */
        public String bitrateMode = "vbr";
        public int frameRate = 30;
        /**
         * Optional encoded-frame ceiling. A box whose encoder cannot hold the
         * broadcast rate drops decoded frames above this rate instead of
         * letting the backlog push back onto the tuner. 0 keeps every frame.
         */
        public int fpsLimit = 0;
        /**
         * Adaptive frame ceiling: start at this rate, lower it one frame per
         * second while the render path or the ts-ring is under pressure, and
         * recover gradually. 0 disables adaptive control.
         */
        public int fpsTarget = 0;
        /** Lowest rate adaptive control may choose. */
        public int fpsFloor = 25;
        /** True when fpsTarget/fpsFloor should drive the frame-drop policy. */
        public boolean adaptiveFps = false;
        /**
         * Optional B-picture ceiling. The AVC fallback on some boxes emits a
         * corrupt stream when frames are skipped after decode, so those boxes
         * drop disposable B pictures before the decoder instead. 0 disables it.
         */
        public int bFps = 0;
        /** Keyframe interval in seconds. Every fragment starts on one. */
        public int iIntervalSec = 5;
        /** Stop after this many seconds of wall clock (0 = run until stopped). */
        public int maxSeconds = 0;
        /**
         * Feed the encoder through ByteBuffer (NV12) instead of an input
         * surface. The vendor HEVC component on some firmwares rejects the
         * gralloc format it is handed on the surface path.
         */
        public boolean byteBuffer;
        /** Feed the decoder into an ImageReader instead of a SurfaceTexture. */
        public boolean imageReader;
        /** Override the encoder colour format (0 = surface input as before). */
        public int colorFormat;
        /** Decoder component; the AOSP one outputs YUV to ByteBuffers. */
        public String decoderName = "OMX.amlogic.mpeg2.decoder.awesome";
        /**
         * MIME type of the transport's video stream, used to pick the decoder.
         *
         * <p>The OEM tuner on the PIX-SMB400 emits MPEG-2; the torne and DT-295
         * paths emit H.264. Resolving one fixed type made the H.264 boxes run
         * through the MPEG-2 decoder at about half the frame rate, so the
         * capture agent writes the right one into live.conf.
         */
        public String decoderMime = "video/mpeg2";
        /**
         * Adaptive sharpen applied in the GL bridge before encoding.
         * 0 disables it and keeps the plain blit. The filter is one extra
         * fragment shader, so it costs a few texture taps per pixel and no
         * extra memory.
         */
        public float sharpen = 0f;
        /**
         * Hand the encoder our own RGBA surface instead of the codec-created
         * one. The vendor HEVC component only accepts RGBA/RGBX gralloc
         * buffers in metadata mode, while the framework-created surface is
         * always YCbCr_420_888.
         */
        public boolean appSurface;
        /**
         * RAM ring in front of the demux, in megabytes.
         *
         * <p>The tuner must never be back-pressured by an encoder stall. On the
         * PIX-SMB400 a stall reaches the DMX ring and it starts dropping
         * transport packets, which shows up as continuity errors and as
         * unplayable audio. 16 MB is about ten seconds of the multiplex, the
         * same order as the X96 units' torne receive queue.
         */
        public int ringMb = 16;
        /**
         * Drop null packets (PID 0x1FFF) before they reach the demux.
         *
         * <p>This tuner pads its output to a fixed rate; the padding is ~45% of
         * the packets and carries nothing. Walking it cost the single-threaded
         * demux more than the box could spare, so the ring stayed full and the
         * DMX dropped real packets.
         */
        public boolean dropNulls = true;
        /**
         * How decoder outputs map to encoded frames: auto, drop or keep.
         *
         * <p>An interlaced source arrives as fields, half a frame apart on the
         * decoder's clock; "auto" encodes one buffer per frame by dropping
         * that second output. "drop" does the same by position, for decoders
         * that restamp the pair, and "keep" encodes every output, which is
         * what a progressive source needs.
         */
        public String fields = "auto";
        /**
         * Presentation clock handed to the codecs: "source" or "frame".
         *
         * <p>"source" passes the transport timestamps through, which keeps the
         * tracks drift free but is rejected by the Amlogic HEVC encoder (it
         * slows to half rate and sometimes stops producing). "frame" counts
         * frames on the 1001/30000 s grid instead, which is what the X96 units
         * have always been fed; it is drift free as long as every broadcast
         * frame ticks the counter once, including the ones dropped.
         */
        public String ptsMode = "source";
        /**
         * Timeout for the codec dequeues, in milliseconds.
         *
         * <p>0 keeps the loop moving: whatever is ready is collected now and
         * the rest next time round. A positive value waits, which paces the
         * loop at the codec's own rate; the Amlogic encoders need that pacing
         * and run at half rate without it.
         */
        public int deqMs = 0;
        /**
         * Flip the encoder's input surface to NV21 after EGL connects.
         *
         * <p>Only needed by the firmware whose patched vendor library converts
         * NV21; on the others the encoder accepts the default surface and
         * silently produces nothing when it is switched, which is why this is
         * opt-in (live.conf {@code nv21=1}).
         */
        public boolean nv21;
        /**
         * How many video PES may wait between the demux and the decoder.
         *
         * <p>1 means the decoder holds a single frame: every frame pays the
         * decoder's whole present latency because nothing overlaps. Deeper
         * values let the decoder work one frame ahead, which is what the X96
         * units need (measured at half rate with 1).
         */
        public int pesDepth = 1;
        /**
         * Timeout for the decoder's output dequeue, in milliseconds.
         *
         * <p>10000 (the historical value) blocks the loop until the decoder
         * hands over a frame, which serialises everything: one PES in, one
         * frame out, and the codec never works ahead. 0 returns immediately so
         * the demux can keep feeding and the codec's pipeline fills.
         */
        public int decOutMs = 10000;
    }

    public static class Stats {
        public long videoFrames;
        public long audioFrames;
        public long videoBytes;
        public long audioBytes;
        /* First sample timestamp per track, to report the head offset. */
        public long videoFirstPtsUs = -1;
        public long audioFirstPtsUs = -1;
        /* Audio frames that belonged ahead of the first encoded frame. */
        public long audioStartDropped;
        /* Times the audio clock was re-based after a broadcast clock reset. */
        public long audioRebases;
        /* Decoder outputs dropped because they repeat a frame timestamp. */
        public long decFieldRepeats;
        public long durationMs;
        public double fps;
        public boolean sentInit;
        public String message;
    }

    private LiveTsStreamer() {}

    public static Stats run(Config cfg) {
        Stats stats = new Stats();
        if (cfg.byteBuffer) {
            /* No GL stage on this path: encode the decoder's native size. */
            cfg.dstW = 1440;
            cfg.dstH = 1080;
        }
        MediaCodec dec = null, enc = null;
        Surface decSurf = null, encSurf = null;
        SurfaceTexture st = null;
        HandlerThread cbThread = null;
        GlBridge.EglCore egl = null;
        /* Offscreen pair used by the sharpen pass; freed with the context. */
        int sharpTex = 0, sharpFbo = 0;
        InputStream in = null;
        Fmp4SinkClient sink = null;
        long t0 = System.currentTimeMillis();

        try {
            /*
             * Buffered on purpose: the parse loop asks for one 188 byte packet
             * at a time, which at 2.4MB/s is twelve thousand read() calls a
             * second. A quarter of the session's CPU went into those syscalls,
             * and every one of them is a chance to fall behind the tuner and
             * overflow its receive queue.
             */
            /*
             * bufferMb=0 reads the fifo directly. The ring exists to keep the
             * tuner from being back-pressured by a slow encoder; a box whose
             * tuner already buffers (the torne driver does) can skip it, which
             * is also the only way to tell a ring problem from a codec one.
             */
            InputStream fifo = new BufferedInputStream(
                    new FileInputStream(cfg.tsPath), 256 * 1024);
            in = cfg.ringMb > 0
                    ? new TsRingReader(fifo, Math.max(4, cfg.ringMb) * 1024 * 1024,
                                       cfg.dropNulls, cfg.debugDumpPath, cfg.debugDumpLimit)
                    : fifo;

            boolean encStarted = false;

            // ---- 1. Encoder ----
            if ("hevc".equalsIgnoreCase(cfg.codec)) {
                cfg.codecName = "OMX.amlogic.video.encoder.hevc";
                cfg.mime = "video/hevc";
            }
            /*
             * Component names differ per vendor (Amlogic on the X96 units,
             * HiSilicon on the PIX-SMB400). Resolve against what this box
             * actually exposes instead of assuming the configured name exists.
             */
            String encName = CodecCatalog.encoder(cfg.mime, cfg.codecName);
            if (encName == null) {
                stats.message = "NG: no encoder for " + cfg.mime;
                return stats;
            }
            cfg.codecName = encName;
            enc = newCodec(cfg.codecName);
            MediaFormat efmt = MediaFormat.createVideoFormat(cfg.mime, cfg.dstW, cfg.dstH);
            int colorFormat = cfg.byteBuffer ? COLOR_FORMAT_NV12
                    : (cfg.colorFormat != 0 ? cfg.colorFormat
                       : MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            efmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, colorFormat);
            efmt.setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate);
            efmt.setInteger(MediaFormat.KEY_FRAME_RATE, cfg.frameRate);
            efmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, cfg.iIntervalSec);
            // The Android 9 vendor Surface path uses NV21; older RGBA and
            // ByteBuffer probes retain their separately selected input layouts.
            boolean nv21Surface = cfg.nv21 && "video/hevc".equals(cfg.mime)
                    && !cfg.byteBuffer && !cfg.appSurface && cfg.colorFormat == 0
                    && android.os.Build.VERSION.SDK_INT == 28;
            if (nv21Surface) {
                // The vendor's NV21 Surface conversion uses BT.601 coefficients.
                efmt.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT601_PAL);
                efmt.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
                efmt.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO);
            }
            try {
                int mode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR;
                if ("cbr".equalsIgnoreCase(cfg.bitrateMode)) {
                    mode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR;
                }
                efmt.setInteger(MediaFormat.KEY_BITRATE_MODE, mode);
            } catch (Exception ignored) {}
            try {
                efmt.setInteger(MediaFormat.KEY_PRIORITY, 0);
            } catch (Exception ignored) {}
            try {
                efmt.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
            } catch (Exception ignored) {}
            /*
             * This SoC only offers the Baseline AVC encoder: asking for Main
             * fails with 0xffffffc3, and the framework exposes no quality knob
             * beyond the bitrate. Quality is therefore controlled by the
             * bitrate plus linear downscaling in the GL bridge.
             */
            enc.configure(efmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            if (!cfg.byteBuffer) {
                if (cfg.appSurface || ("video/hevc".equals(cfg.mime)
                        && "X96Max_V311_20190302-1355".equals(android.os.Build.DISPLAY))) {
                    /*
                     * Framework-created surface (correct usage flags) whose
                     * BufferQueue format is then flipped to RGBA_8888: the
                     * vendor HEVC component rejects YCbCr_420_888. On V311,
                     * the patched vendor library converts RGBA to NV21.
                     */
                    encSurf = enc.createInputSurface();
                    enc.start();
                    try {
                        int rc = NativeSurface.setFormat(encSurf, 1);
                        Log.i(TAG, "createInputSurface format -> RGBA rc=" + rc);
                    } catch (Throwable e) {
                        Log.w(TAG, "NativeSurface.setFormat: " + e);
                    }
                    encStarted = true;
                } else if (false) {
                    /*
                     * Persistent surface + hidden setBuffersFormat: the vendor
                     * HEVC component only accepts RGBA gralloc buffers, while
                     * the framework-created surface is always YCbCr_420_888.
                     */
                    Surface ps = MediaCodec.createPersistentInputSurface();
                    try {
                        enc.setInputSurface(ps);
                        Log.i(TAG, "using persistent surface for encoder input");
                    } catch (Exception e) {
                        Log.w(TAG, "setInputSurface rejected: " + e);
                    }
                    enc.start();
                    try {
                        int rc = NativeSurface.setFormat(ps, 1);
                        Log.i(TAG, "surface format -> RGBA_8888 rc=" + rc);
                    } catch (Throwable e) {
                        Log.w(TAG, "NativeSurface.setFormat: " + e);
                    }
                    encSurf = ps;
                    encStarted = true;
                } else {
                    encSurf = enc.createInputSurface();
                }
            }
            if (!encStarted) {
                enc.start();
            }
            // ---- 2. GLES bridge ----
            egl = cfg.byteBuffer
                    ? new GlBridge.EglCore(cfg.dstW, cfg.dstH)
                    : new GlBridge.EglCore(encSurf, 0, 0, cfg.appSurface);
            if (nv21Surface) {
                // EGL reconnects the window format; override it after connection.
                int rc = NativeSurface.setFormat(encSurf, android.graphics.ImageFormat.NV21);
                Log.i(TAG, "HEVC post-EGL NV21 rc=" + rc);
                if (rc != 0) throw new IllegalStateException("HEVC input format: " + rc);
            }
            /*
             * Ask for a sync frame only now: the input surface format is final
             * at this point. Doing it before the NV21 switch made the Amlogic
             * HEVC encoder negotiate its buffers against the old format, after
             * which it either ran at half rate or produced nothing at all.
             * The first picture is therefore an IDR, which the sink needs for
             * the opening fragment.
             */
            try {
                android.os.Bundle syncReq = new android.os.Bundle();
                syncReq.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
                enc.setParameters(syncReq);
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot request a sync frame: " + e);
            }
            int tex = GlBridge.createExternalTexture();
            /*
             * Sharpen runs as two passes: copy the OES frame into an RGBA
             * texture once, then read the taps from that texture. Sampling an
             * external texture five times per fragment costs tens of
             * milliseconds on some Mali drivers.
             */
            boolean sharp = cfg.sharpen > 0f;
            int srcW = 1440, srcH = 1080;
            int prog = egl.buildProgram();
            /*
             * ByteBuffer path: the EGL context renders into a pbuffer and the
             * frame is read back as RGBA, converted to NV12 and queued.
             */
            ImageReader reader = null;
            double swImgMs = 0, swConvMs = 0, swQueueMs = 0;
            int swFrames = 0;
            java.nio.ByteBuffer rgbaBuf = null;
            byte[] rgbaBytes = null;
            byte[] nv12 = null;
            if (cfg.byteBuffer) {
                rgbaBuf = java.nio.ByteBuffer.allocateDirect(cfg.dstW * cfg.dstH * 4);
                rgbaBytes = new byte[cfg.dstW * cfg.dstH * 4];
                nv12 = new byte[cfg.dstW * cfg.dstH * 3 / 2];
                rgbaBuf.order(java.nio.ByteOrder.nativeOrder());
            }
            int aPos = GLES20.glGetAttribLocation(prog, "aPosition");
            int aTex = GLES20.glGetAttribLocation(prog, "aTexCoord");
            int uTex = GLES20.glGetUniformLocation(prog, "sTexture");
            int uMtx = GLES20.glGetUniformLocation(prog, "uTexMatrix");
            int progSharp = -1;
            int fboTex = 0, fbo = 0;
            int aPos2 = -1, aTex2 = -1, uTex2 = -1, uSharp2 = -1, uTexel2 = -1;
            if (sharp) {
                progSharp = egl.buildSharpenProgram();
                aPos2 = GLES20.glGetAttribLocation(progSharp, "aPosition");
                aTex2 = GLES20.glGetAttribLocation(progSharp, "aTexCoord");
                uTex2 = GLES20.glGetUniformLocation(progSharp, "sTexture");
                uSharp2 = GLES20.glGetUniformLocation(progSharp, "uSharpen");
                uTexel2 = GLES20.glGetUniformLocation(progSharp, "uTexel");
                int[] t = new int[1];
                GLES20.glGenTextures(1, t, 0);
                fboTex = t[0];
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fboTex);
                GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, srcW, srcH,
                        0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                        GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                        GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                        GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D,
                        GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
                int[] f = new int[1];
                GLES20.glGenFramebuffers(1, f, 0);
                fbo = f[0];
                GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo);
                GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER,
                        GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, fboTex, 0);
                int status = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER);
                GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
                Log.i(TAG, "GL sharpen on, gain=" + cfg.sharpen
                        + " fbo=" + fbo + " status=0x" + Integer.toHexString(status));
                sharpTex = fboTex;
                sharpFbo = fbo;
            }
            FloatBuffer vb = GlBridge.floatBuf(new float[]{-1, -1, 1, -1, -1, 1, 1, 1});
            FloatBuffer tb = GlBridge.floatBuf(new float[]{0, 0, 1, 0, 0, 1, 1, 1});

            GlBridge.FrameCounter counter = null;
            if (!cfg.imageReader) {
                cbThread = new HandlerThread("live-tex-cb");
                cbThread.start();
                counter = new GlBridge.FrameCounter();
                st = new SurfaceTexture(tex);
                try {
                    st.setDefaultBufferSize(1440, 1080);
                } catch (Exception ignored) {}
                st.setOnFrameAvailableListener(counter, new Handler(cbThread.getLooper()));
                decSurf = new Surface(st);
            } else {
                /*
                 * No GL at all: the decoder writes YUV into an ImageReader and
                 * the planes are packed into NV12 for the ByteBuffer encoder.
                 * glReadPixels crashes the Mali driver on this firmware.
                 */
                /*
                 * The vendor decoder needs GRALLOC_USAGE_HW_VIDEO_DECODER on
                 * its output buffers; the public ImageReader factory uses
                 * usage 0, so try the hidden overload that takes one.
                 */
                reader = newReaderWithUsage(cfg.dstW, cfg.dstH,
                        android.graphics.ImageFormat.YV12, 3, 0x200L);
                if (reader == null) {
                    reader = ImageReader.newInstance(cfg.dstW, cfg.dstH,
                            android.graphics.ImageFormat.YV12, 3);
                    Log.w(TAG, "image reader without usage flags");
                }
                Log.i(TAG, "image reader surface " + cfg.dstW + "x" + cfg.dstH);
            }

            // ---- 3. Decoder ----
            String decName = CodecCatalog.decoder(cfg.decoderMime, cfg.decoderName);
            if (decName != null) cfg.decoderName = decName;
            dec = newCodec(cfg.decoderName);
            boolean swDecode = cfg.decoderName.toLowerCase().contains("google")
                    || cfg.decoderName.toLowerCase().contains("c2.android");
            /*
             * The format carries the same MIME as the component: telling an
             * H.264 decoder that it is being handed MPEG-2 leaves it on a
             * slow path (measured at half the frame rate on the X96 units).
             */
            MediaFormat dfmt = MediaFormat.createVideoFormat(cfg.decoderMime, 1440, 1080);
            if (swDecode) {
                /*
                 * Software MPEG-2 decoder: no surface, YUV lands in ByteBuffers
                 * which is exactly what the HEVC encoder wants.
                 */
                dfmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                        MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar);
                dec.configure(dfmt, null, null, 0);
            } else {
                dec.configure(dfmt, cfg.imageReader ? reader.getSurface() : decSurf, null, 0);
            }
            dec.start();
            Log.i(TAG, "decoder " + cfg.decoderName + " sw=" + swDecode);

            // ---- 4. Connect to the native sink ----
            sink = new Fmp4SinkClient(cfg.sinkPort);
            if (!sink.connect(20000)) {
                stats.message = "NG: sink connect timeout 127.0.0.1:" + cfg.sinkPort;
                return stats;
            }

            // ---- 5. Pipeline state ----
            byte[] pkt = new byte[188];
            byte[] pesBuf = new byte[1024 * 1024];
            int pesLen = 0;
            long pesPtsUs = -1;
            byte[] readyPes = null;
            long readyPesPtsUs = -1;
            java.util.ArrayDeque<Pes> pesQueue = new java.util.ArrayDeque<>();

            /* Audio: the broadcast carries AAC as ADTS inside the audio PES. */
            byte[] aPesBuf = new byte[256 * 1024];
            int aPesLen = 0;
            long aPesPtsUs = -1;
            /*
             * Audio and video carry timestamps from the same 90 kHz transport
             * clock, so an audio PES timestamp goes to the sink unchanged and
             * every sample keeps the presentation time it was broadcast with.
             * Re-anchoring the first packet to the encoder position shifted
             * the whole audio track by the pipeline lead and put A/V out of
             * sync, so only the start of the output timeline needs handling:
             * audio read before the first encoded frame is held back here and
             * released against that frame.
             */
            long firstVideoEncPtsUs = -1;
            long lastVideoEncPtsUs = -1;
            /*
             * "frame" clock: the transport timestamp of the frame that gets
             * slot 0. Audio is shifted by the same amount, so the two tracks
             * stay on one timeline without passing huge values to the codecs.
             */
            boolean frameClock = "frame".equalsIgnoreCase(cfg.ptsMode);
            /* Transport timestamp of the first video PES: the audio anchor for
             * the frame clock, whose own timestamps are a counter. */
            long videoSrcBaseUs = -1;
            long lastEncOut = 0;
            long lastEncMs = System.currentTimeMillis();
            AudioStartup audioStartup = new AudioStartup();
            Adts adtsCarry = new Adts();
            /* AAC-LC 48 kHz stereo is the ISDB-T default; ADTS confirms it. */
            AudioState audio = new AudioState();
            boolean audioWarned = false;

            /* EIT present/following drives the program boundary. */
            int serviceId = -1;
            long eitStartMs = 0;
            long eitDurationMs = 0;
            int eitServiceId = -1;

            MediaCodec.BufferInfo dbi = new MediaCodec.BufferInfo();
            MediaCodec.BufferInfo ebi = new MediaCodec.BufferInfo();

            int fedFrames = 0, decOut = 0, rendered = 0;
            /* PTS (µs) of the field waiting to be rendered into the encoder. */
            long pendingFramePtsUs = -1;
            /* Last decoder output timestamp, to spot the second field of a frame. */
            long lastDecPtsUs = -1;
            /* Frame-count accumulator for the optional fps ceiling. */
            double fpsAccumulator = Math.max(1, cfg.frameRate);
            /* Adaptive target, lowered on pressure and recovered gradually. */
            double effectiveFps = cfg.fpsTarget > 0
                    ? Math.min(cfg.fpsTarget, cfg.frameRate) : cfg.frameRate;
            long fpsAdaptedAt = 0;
            /* Where a frame's wall time goes; reported in the 5 s diag line. */
            long decWaitNs = 0, renderNs = 0, encWaitNs = 0;
            long decInNs = 0, sinkNs = 0, loopNs = 0, tLoop0 = 0;
            long texNs = 0, swapNs = 0, curNs = 0, drawNs = 0;
            /* Audio demux counters, surfaced through the 5 s diag line. */
            /* Per-PID packet counts plus continuity errors, to tell a demux
             * problem from packets lost upstream of the transcoder. */
            /*
             * A fifo is a byte stream, not a packet stream. A session can start
             * in the middle of a transport packet: the previous session stopped
             * part way through one, or the tuner restarted while a packet was
             * in flight. `have` is how much of the 188 byte window is filled,
             * and every block that does not start on a sync byte is slid one
             * byte at a time until it does.
             */
            int vPid = -1, pmtPid = -1, aPid = -1;
            long tsPackets = 0;
            int audioPesSeen = 0;
            long vPkts = 0, aPkts = 0, nullPkts = 0;
            int aCcErr = 0, vCcErr = 0;
            int aCcLast = -1, vCcLast = -1;
            int eitEventId = -1;
            long resyncPackets = 0;
            int have = 0;
            long firstByteMs = 0;
            long lastPesMs = 0;
            /*
             * Frame dropping. The encoder is the slowest stage, so a session
             * that cannot quite hold the broadcast rate used to push back on
             * the fifo until the tuner's receive queue overflowed and the
             * tuner restarted. A B picture costs the encoder work it cannot
             * do anyway and nothing references it, so dropping one keeps the
             * fifo drained at the input rate without touching the pictures
             * that follow.
             */
            boolean readyPesIsB = false;
            double dropCredit = 0;
            long droppedPes = 0;
            long workEmaMs = 0;
            long eitPackets = 0;
            int eitLastTable = -1;
            int eitLastRunning = -1;
            long lastDiagMs = System.currentTimeMillis();
            long demuxNanos = 0, codecNanos = 0, tCodecMark = 0;
            long lastDecOut = 0;
            long lastDecMs = System.currentTimeMillis();
            /*
             * When the first PES is fed. The decoder watchdog below measures
             * silence from here, not from the start of the session: a session
             * that opens before the tuner has locked would otherwise be
             * declared silent the moment its first frame arrives, and tearing
             * it down is what stops the fifo and kills the tuner with it.
             */
            long firstFeedMs = 0;
            long lastDiagFrames = 0;
            boolean tsEof = false, fedEos = false, decEos = false, encEos = false;
            boolean encEosSignalled = false;
            boolean initSent = false;
            byte[] sps = null, pps = null;

            long deadline = cfg.maxSeconds > 0
                    ? System.currentTimeMillis() + cfg.maxSeconds * 1000L
                    : Long.MAX_VALUE;

            while (System.currentTimeMillis() < deadline && !encEos) {
                long tDemux0 = System.nanoTime();
                if (tLoop0 != 0) loopNs += tDemux0 - tLoop0;
                tLoop0 = tDemux0;
                if (tCodecMark != 0) codecNanos += tDemux0 - tCodecMark;
                tCodecMark = 0;
                // ---- STEP A: Accumulate TS packets into a complete video PES ----
                // 1つのPESが完成するまで読み続ける（入力をまとめて処理できる）
                if (!tsEof && readyPes == null) {
                    while (readyPes == null && !tsEof) {
                        while (have < 188) {
                            int r = in.read(pkt, have, 188 - have);
                            if (r < 0) { tsEof = true; break; }
                            have += r;
                        }
                        if (tsEof) {
                            if (pesLen > 0) {
                                readyPes = new byte[pesLen];
                                System.arraycopy(pesBuf, 0, readyPes, 0, pesLen);
                                readyPesPtsUs = pesPtsUs;
                                pesLen = 0;
                            }
                            break;
                        }
                        /*
                         * Slide the window onto the next sync byte instead of
                         * dropping the whole block. A fifo that is a few bytes
                         * out of step rejects every block for ever, which is
                         * indistinguishable from a tuner that never delivers a
                         * picture: tsPackets stays at zero, no PES ever
                         * completes, and this loop spins without logging.
                         */
                        if (pkt[0] != 0x47) {
                            System.arraycopy(pkt, 1, pkt, 0, 187);
                            have = 187;
                            resyncPackets++;
                            continue;
                        }
                        have = 0;
                        long loopMs = System.currentTimeMillis();
                        if (firstByteMs == 0) firstByteMs = loopMs;
                        /*
                         * This inner loop is where a session spends all of its
                         * time, so the diagnostics have to be emitted from here
                         * too. The clock is "time since the last complete video
                         * PES", so a healthy session never trips it however
                         * long it runs; thirty seconds of input without one is
                         * a wedged session, not a slow one.
                         */
                        long sincePesMs = loopMs - (lastPesMs != 0 ? lastPesMs : firstByteMs);
                        if (sincePesMs >= 5000 && loopMs - lastDiagMs >= 5000) {
                            lastDiagMs = loopMs;
                            Log.i(TAG, "diag waiting for video PES: tsPackets=" + tsPackets
                                    + " resync=" + resyncPackets + " vPid=" + vPid
                                    + " aPid=" + aPid + " silent=" + (sincePesMs / 1000) + "s");
                        }
                        if (sincePesMs > 30000) {
                            stats.message = "NG:no video PES in "
                                    + (sincePesMs / 1000) + "s (tsPackets="
                                    + tsPackets + " resync=" + resyncPackets + ")";
                            return stats;
                        }
                        if ((pkt[1] & 0x80) != 0) continue; // TEI
                        tsPackets++;

                        int pid = ((pkt[1] & 0x1F) << 8) | (pkt[2] & 0xFF);
                        boolean pusi = (pkt[1] & 0x40) != 0;
                        int afc = (pkt[3] >> 4) & 0x3;
                        int pOff = 4;
                        if (afc == 2 || afc == 0) continue;
                        if (afc == 3) {
                            pOff = 5 + (pkt[4] & 0xFF);
                            if (pOff >= 188) continue;
                        }

                        /*
                         * Packet accounting for the two PIDs the session cares
                         * about. A continuity-counter jump on exactly one of them
                         * separates "the demux is dropping it" from "it never
                         * arrived".
                         */
                        if (pid == 0x1FFF) {
                            nullPkts++;
                            continue;
                        }
                        if (pid == vPid) {
                            vPkts++;
                            int cc = pkt[3] & 0x0F;
                            if (vCcLast >= 0 && ((vCcLast + 1) & 0x0F) != cc) vCcErr++;
                            vCcLast = cc;
                        } else if (pid == aPid) {
                            aPkts++;
                            int cc = pkt[3] & 0x0F;
                            if (aCcLast >= 0 && ((aCcLast + 1) & 0x0F) != cc) aCcErr++;
                            aCcLast = cc;
                        }

                        // PAT / PMT の探索
                        if (pid == 0) {
                            int p = pOff;
                            if (pusi) p += 1 + (pkt[p] & 0xFF);
                            if (p + 12 <= 188) {
                                int sLen = ((pkt[p+1] & 0x0F) << 8) | (pkt[p+2] & 0xFF);
                                for (int q = p + 8;
                                     q + 4 <= p + 3 + sLen - 4 && q + 4 <= 188;
                                     q += 4) {
                                    int progNum = ((pkt[q] & 0xFF) << 8) | (pkt[q+1] & 0xFF);
                                    if (progNum != 0 && pmtPid < 0) {
                                        pmtPid = ((pkt[q+2] & 0x1F) << 8) | (pkt[q+3] & 0xFF);
                                        /* On ISDB-T the programme number is the service id. */
                                        serviceId = progNum;
                                        Log.i(TAG, "service id discovered: " + serviceId);
                                    }
                                }
                            }
                            continue;
                        } else if (pmtPid >= 0 && pid == pmtPid) {
                            int p = pOff;
                            if (pusi) p += 1 + (pkt[p] & 0xFF);
                            if (p + 12 <= 188) {
                                int sLen = ((pkt[p+1] & 0x0F) << 8) | (pkt[p+2] & 0xFF);
                                int pInfoLen = ((pkt[p+10] & 0x0F) << 8) | (pkt[p+11] & 0xFF);
                                for (int q = p + 12 + pInfoLen;
                                     q + 5 <= p + 3 + sLen - 4 && q + 5 <= 188; ) {
                                    int stype = pkt[q] & 0xFF;
                                    int epub = ((pkt[q+1] & 0x1F) << 8) | (pkt[q+2] & 0xFF);
                                    int eLen = ((pkt[q+3] & 0x0F) << 8) | (pkt[q+4] & 0xFF);
                                    if (stype == 0x02 && vPid < 0) {
                                        vPid = epub;
                                        Log.i(TAG, "video PID discovered: " + vPid);
                                    }
                                    if (stype == 0x0F && aPid < 0) {
                                        aPid = epub;
                                        Log.i(TAG, "audio PID discovered (AAC ADTS): " + aPid);
                                    }
                                    if (stype == 0x11 && aPid < 0) {
                                        Log.w(TAG, "audio stream is LATM/LOAS (pid " + epub
                                                + "); AAC extraction is not implemented for it");
                                    }
                                    q += 5 + eLen;
                                }
                            }
                            continue;
                        }

                        if (pid == 0x12) {
                            eitPackets++;
                            if (pusi && pOff + 14 < 188) {
                                int p = pOff + 1 + (pkt[pOff] & 0xFF);
                                if (p + 14 < 188) {
                                    eitLastTable = pkt[p] & 0xFF;
                                }
                                if (p + 14 < 188 && (pkt[p] & 0xFF) == 0x4E) {
                                    int sectionLen = ((pkt[p+1] & 0x0F) << 8) | (pkt[p+2] & 0xFF);
                                    int svcId = ((pkt[p+3] & 0xFF) << 8) | (pkt[p+4] & 0xFF);
                                    /*
                                     * One transport stream carries the present/following
                                     * table of every service in it, so only the service
                                     * being recorded may drive the boundary.
                                     */
                                    if (serviceId >= 0 && svcId != serviceId) continue;
                                    /*
                                     * Present and following may arrive as separate
                                     * sections. Only section 0 describes the event
                                     * that is on air now.
                                     */
                                    if ((pkt[p + 6] & 0xFF) != 0) continue;
                                    int end = Math.min(p + 3 + sectionLen - 4, 188);
                                    int q = p + 14;
                                    if (q + 12 <= end) {
                                        int evId = ((pkt[q] & 0xFF) << 8) | (pkt[q+1] & 0xFF);
                                        int running = (pkt[q + 10] >> 5) & 0x7;
                                        eitLastRunning = running;
                                        /*
                                         * The first event of the present/following
                                         * section is the programme on air. Some
                                         * broadcasters leave running_status at 0, so
                                         * the event id alone decides the boundary.
                                         */
                                        if (evId != eitEventId) {
                                            eitEventId = evId;
                                            eitStartMs = eitTimeMs(pkt, q + 2);
                                            eitDurationMs = eitDurationToMs(pkt, q + 7);
                                            eitServiceId = svcId;
                                            Log.i(TAG, "EIT present event=" + evId
                                                    + " service=" + svcId
                                                    + " start=" + eitStartMs
                                                    + " dur=" + eitDurationMs);
                                            sink.sendBoundary(svcId, evId,
                                                    eitStartMs, eitDurationMs,
                                                    System.currentTimeMillis());
                                        }
                                    }
                                }
                            }
                            continue;
                        }

                        if (aPid >= 0 && pid == aPid) {
                            int p = pOff;
                            if (pusi) {
                                audioPesSeen++;
                                if (audioPesSeen < 6) {
                                    try {
                                        java.io.FileOutputStream f =
                                                new java.io.FileOutputStream(
                                                        "/dev/shm/pix/aac-pes.txt", true);
                                        StringBuilder sb = new StringBuilder();
                                        sb.append(audioPesSeen).append(" len=").append(aPesLen)
                                          .append(" pts=").append(aPesPtsUs).append(" first=");
                                        for (int k = 0; k < Math.min(16, aPesLen); k++) {
                                            sb.append(String.format("%02x", aPesBuf[k] & 0xFF));
                                        }
                                        sb.append('\n');
                                        f.write(sb.toString().getBytes());
                                        f.close();
                                    } catch (Exception ignored) {
                                    }
                                }
                                if (aPesLen > 0) {
                                    sendAdtsFrames(aPesBuf, aPesLen, aPesPtsUs,
                                            frameClock ? videoSrcBaseUs >= 0
                                                       : firstVideoEncPtsUs >= 0,
                                            0,
                                            frameClock && videoSrcBaseUs >= 0 ? -videoSrcBaseUs : 0,
                                            adtsCarry, audioStartup, sink, stats);
                                    aPesLen = 0;
                                }
                                if (!audioWarned) {
                                    int hdrAt = skipPesHeader(pkt, p);
                                    if (hdrAt < 188) {
                                        int b0 = pkt[hdrAt] & 0xFF;
                                        int b1 = pkt[hdrAt + 1] & 0xFF;
                                        if (b0 == 0x56 && (b1 & 0xE0) == 0xE0) {
                                            Log.w(TAG, "audio is LOAS/LATM; only ADTS is supported");
                                        }
                                        audioWarned = true;
                                    }
                                }
                                aPesPtsUs = parsePts(pkt, p);
                                p = skipPesHeader(pkt, p);
                            }
                            if (p < 188 && aPesLen + (188 - p) <= aPesBuf.length) {
                                System.arraycopy(pkt, p, aPesBuf, aPesLen, 188 - p);
                                aPesLen += (188 - p);
                            }
                            continue;
                        }

                        if (vPid >= 0 && pid == vPid) {
                            int p = pOff;
                            if (pusi) {
                                // The previous PES ends here: hand it over complete.
                                if (pesLen > 0) {
                                    byte[] done = new byte[pesLen];
                                    System.arraycopy(pesBuf, 0, done, 0, pesLen);
                                    if (videoSrcBaseUs < 0 && pesPtsUs >= 0) {
                                        videoSrcBaseUs = pesPtsUs;
                                    }
                                    boolean doneIsB = isBPicture(done);
                                    if (readyPes == null) {
                                        readyPes = done;
                                        readyPesPtsUs = pesPtsUs;
                                        readyPesIsB = doneIsB;
                                    } else if (cfg.pesDepth > 1
                                               && pesQueue.size() < cfg.pesDepth) {
                                        pesQueue.add(new Pes(done, pesPtsUs, doneIsB));
                                    } else {
                                        /* The decoder cannot take them any faster. */
                                        droppedPes++;
                                    }
                                    pesLen = 0;
                                    lastPesMs = System.currentTimeMillis();
                                }
                                pesPtsUs = parsePts(pkt, p);
                                p = skipPesHeader(pkt, p);
                            }
                            if (p < 188 && pesLen + (188 - p) <= pesBuf.length) {
                                System.arraycopy(pkt, p, pesBuf, pesLen, 188 - p);
                                pesLen += (188 - p);
                            }
                        }
                    }
                }

                long nowMs = System.currentTimeMillis();
                if (nowMs - lastDiagMs >= 5000) {
                    long spanMs = nowMs - lastDiagMs;
                    lastDiagMs = nowMs;
                    double outFps = lastDiagFrames > 0
                            ? (stats.videoFrames - lastDiagFrames) * 1000.0 / Math.max(1, spanMs)
                            : 0.0;
                    /* Head offset of the two tracks, in ms: 0 means A/V start together. */
                    long aLeadMs = (stats.audioFirstPtsUs >= 0 && stats.videoFirstPtsUs >= 0)
                            ? (stats.audioFirstPtsUs - stats.videoFirstPtsUs) / 1000 : -1;
                    String diag = "diag tsPackets=" + tsPackets
                            + " vPid=" + vPid + " aPid=" + aPid
                            + " fed=" + fedFrames + " dec=" + decOut
                            + " decRep=" + stats.decFieldRepeats
                            + " enc=" + stats.videoFrames
                            + " fps=" + String.format(Locale.US, "%.1f", outFps)
                            + " aud=" + stats.audioFrames
                            + " aPes=" + audioPesSeen
                            + " aSkip=" + stats.audioStartDropped
                            + " aRebase=" + stats.audioRebases
                            + " aLead=" + aLeadMs
                            + " ring=" + (in instanceof TsRingReader
                                    ? ((TsRingReader) in).pendingChunks() + "/"
                                      + ((TsRingReader) in).capacityChunks()
                                    : "-")
                            + " vPkts=" + vPkts
                            + " aPkts=" + aPkts
                            + " nulls=" + nullPkts
                            + " vCc=" + vCcErr
                            + " aCc=" + aCcErr
                            + " resync=" + resyncPackets
                            + " drop=" + droppedPes
                            + " demuxMs=" + (demuxNanos / 1000000)
                            + " codecMs=" + (codecNanos / 1000000)
                            + " decIn=" + (decInNs / 1000000)
                            + " sink=" + (sinkNs / 1000000)
                            + " loop=" + (loopNs / 1000000)
                            + " decW=" + (decWaitNs / 1000000)
                            + " ren=" + (renderNs / 1000000)
                            + " tex=" + (texNs / 1000000)
                            + " swap=" + (swapNs / 1000000)
                            + " cur=" + (curNs / 1000000)
                            + " draw=" + (drawNs / 1000000)
                            + " encW=" + (encWaitNs / 1000000)
                            + " work=" + workEmaMs
                            + " eitEvent=" + eitEventId;
                    Log.i(TAG, diag);
                    /*
                     * logd is stopped on the PIX-SMB400, so the diagnostic line
                     * is also appended to tmpfs. That is the only way to see the
                     * demux counters on that box without a serial console.
                     */
                    try {
                        /*
                         * The file lives in tmpfs and the box runs unattended
                         * for weeks, so keep one previous generation and cap
                         * the current one at about a day of lines.
                         */
                        java.io.File df = new java.io.File("/dev/shm/pix/diag.txt");
                        if (df.length() > 1024 * 1024) {
                            java.io.File prev = new java.io.File("/dev/shm/pix/diag.txt.1");
                            if (prev.exists()) prev.delete();
                            df.renameTo(prev);
                        }
                        java.io.FileWriter w = new java.io.FileWriter(df, true);
                        try {
                            w.write(System.currentTimeMillis() + " " + diag + "\n");
                        } finally {
                            w.close();
                        }
                    } catch (Exception ignored) {
                    }
                    lastDiagFrames = stats.videoFrames;
                    decWaitNs = 0;
                    renderNs = 0;
                    texNs = 0;
                    swapNs = 0;
                    curNs = 0;
                    drawNs = 0;
                    encWaitNs = 0;
                    decInNs = 0;
                    sinkNs = 0;
                    loopNs = 0;
                    demuxNanos = 0;
                    codecNanos = 0;
                }

                /*
                 * A session that is fed but never produces a picture is wedged:
                 * the single-instance MPEG2 decoder stayed bound to the previous
                 * session, which happens when a recording is restarted (channel
                 * or format change). Bail out so the outer loop retries with a
                 * fresh codec instead of blocking here until a reboot.
                 */
                if (decOut != lastDecOut) {
                    lastDecOut = decOut;
                    lastDecMs = nowMs;
                } else if (fedFrames > 10) {
                    /*
                     * Two very different situations look the same from here.
                     * A session that has never produced a frame is wedged on
                     * the previous owner of the single decoder, so give up
                     * quickly. A session that was working and then hit a slow
                     * patch only needs patience: tearing it down costs more
                     * than waiting, and the retry loop is what turned a brief
                     * hiccup into a restart cycle.
                     */
                    long silentMs = nowMs - Math.max(lastDecMs, firstFeedMs);
                    /*
                     * A cold decoder can take several seconds to hand out its
                     * first frame (the vendor one sometimes needs the stream's
                     * SPS before it starts). Giving up at 5 s made the caller
                     * switch codec for no reason, so both cases get 15 s.
                     */
                    long limitMs = 15000;
                    if (silentMs > limitMs) {
                        stats.message = "NG:decoder silent for " + (silentMs / 1000) + "s";
                        return stats;
                    }
                }

                /*
                 * The encoder can wedge the same way: it takes frames and
                 * never writes one out. Nothing downstream notices, the tuner
                 * ends up back-pressured and the ring overflows, so give it a
                 * bounded window and let the outer loop rebuild the session.
                 */
                if (stats.videoFrames != lastEncOut) {
                    lastEncOut = stats.videoFrames;
                    lastEncMs = nowMs;
                } else if (fedFrames > 30 && nowMs - lastEncMs > 15000) {
                    stats.message = "NG:encoder silent for "
                            + ((nowMs - lastEncMs) / 1000) + "s";
                    return stats;
                }

                long tDemuxEnd = System.nanoTime();
                demuxNanos += tDemuxEnd - tDemux0;
                tCodecMark = tDemuxEnd;

                // ---- STEP B: Feed one complete PES into the decoder ----
                // 入力バッファは実際にキューへ入れる時だけ取得する（漏らすと枯渇する）
                /*
                 * Keep feeding while the decoder has room. With one PES in
                 * flight the decoder can never work ahead: every frame pays its
                 * whole present latency, which on the X96 units is the
                 * difference between 15 and 30 fps. pesDepth > 1 lets a few
                 * frames wait in the queue instead.
                 */
                long workStartMs = System.currentTimeMillis();
                while (!fedEos) {
                    if (readyPes == null && tsEof) {
                        long tIn0 = System.nanoTime();
                        int idx = dec.dequeueInputBuffer(cfg.deqMs);
                        decInNs += System.nanoTime() - tIn0;
                        if (idx < 0) break;
                        dec.queueInputBuffer(idx, 0, 0, fedFrames * 33367L,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        fedEos = true;
                        break;
                    }
                    if (readyPes == null) break;
                    if (readyPesIsB && cfg.bFps > 0 && cfg.bFps < cfg.frameRate) {
                        /* Drop disposable B pictures before the decoder. The
                         * AVC fallback corrupts if the drop happens after it. */
                        dropCredit += (cfg.frameRate - cfg.bFps)
                                / (double) cfg.frameRate;
                    }
                    if (readyPesIsB && dropCredit >= 1.0) {
                        /*
                         * The last frames cost more than one frame period, so
                         * the encoder cannot hold the broadcast rate. Skipping
                         * this B picture keeps the reader moving at the input
                         * rate, which is what stops the tuner's receive queue
                         * from filling up and ending the session with a restart.
                         */
                        dropCredit -= 1.0;
                        droppedPes++;
                        /* The presentation clock keeps running. */
                        rendered++;
                        readyPes = null;
                    } else {
                        long tIn0 = System.nanoTime();
                        int idx = dec.dequeueInputBuffer(cfg.deqMs);
                        decInNs += System.nanoTime() - tIn0;
                        if (idx < 0) break;
                        ByteBuffer buf = dec.getInputBuffer(idx);
                        if (buf == null) break;
                        buf.clear();
                        buf.put(readyPes);
                        /*
                         * In frame mode the decoder is fed the same small
                         * counter as the encoder: this SoC's codecs slow down
                         * when handed transport timestamps, and the old X96
                         * build only ever gave them the counter.
                         */
                        long pts = frameClock
                                ? fedFrames * 100100L / 3
                                : (readyPesPtsUs >= 0 ? readyPesPtsUs
                                                      : (fedFrames * 33367L));
                        dec.queueInputBuffer(idx, 0, readyPes.length, pts, 0);
                        fedFrames++;
                        if (firstFeedMs == 0) firstFeedMs = System.currentTimeMillis();
                        readyPes = null;
                    }
                    Pes next = pesQueue.poll();
                    if (next != null) {
                        readyPes = next.data;
                        readyPesPtsUs = next.pts;
                        readyPesIsB = next.isB;
                    }
                }

                // ---- STEP C: Drain the decoder and render through EGL ----
                long tDec0 = System.nanoTime();
                int didx = dec.dequeueOutputBuffer(dbi, cfg.decOutMs);
                decWaitNs += System.nanoTime() - tDec0;
                if (didx >= 0) {
                    decOut++;
                    /*
                     * Interlaced sources arrive as fields: this decoder hands
                     * out one buffer per field, so a 29.97 fps broadcast
                     * produces ~59.94 outputs per second, each half a frame
                     * apart (16.7 ms on the unit here). Encoding both made the
                     * video timeline run at twice real speed (measured: the
                     * video PTS span was 2.01x the audio span) and left the
                     * encoder no budget for realtime. "auto" keeps one buffer
                     * per frame by dropping any output that steps less than
                     * three quarters of a frame after the previous one; "drop"
                     * does it by position for decoders that restamp the pairs,
                     * and "keep" encodes every output for progressive sources.
                     */
                    long decPtsUs = dbi.presentationTimeUs;
                    long decDeltaUs = lastDecPtsUs >= 0 ? decPtsUs - lastDecPtsUs : -1;
                    lastDecPtsUs = decPtsUs;
                    long frameUs = 1000000L / Math.max(1, cfg.frameRate);
                    boolean secondField = decDeltaUs >= 0 && decDeltaUs < frameUs * 3 / 4;
                    if (secondField) stats.decFieldRepeats++;
                    boolean keepField;
                    if ("keep".equalsIgnoreCase(cfg.fields)) {
                        keepField = true;
                    } else if ("drop".equalsIgnoreCase(cfg.fields)) {
                        keepField = (decOut & 1) == 1;
                    } else {
                        keepField = !secondField;
                    }
                    if ((dbi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        decEos = true;
                    }
                    if (cfg.decoderName.toLowerCase().contains("google")
                            || cfg.decoderName.toLowerCase().contains("c2.android")) {
                        long swT0 = System.nanoTime();
                        Image img = dec.getOutputImage(didx);
                        long swT1 = System.nanoTime();
                        if (img != null && keepField) {
                            try {
                                YuvPacker.imageToNv12(img, nv12, cfg.dstW, cfg.dstH);
                            } finally {
                                img.close();
                            }
                            long swT2 = System.nanoTime();
                            long ptsUs = rendered * 1000000L / cfg.frameRate;
                            int bidx = enc.dequeueInputBuffer(0);
                            if (bidx >= 0) {
                                ByteBuffer bin = enc.getInputBuffer(bidx);
                                if (bin != null) {
                                    bin.clear();
                                    bin.put(nv12, 0, nv12.length);
                                    enc.queueInputBuffer(bidx, 0, nv12.length, ptsUs, 0);
                                }
                            }
                            long swT3 = System.nanoTime();
                            swImgMs += (swT1 - swT0) / 1000000.0;
                            swConvMs += (swT2 - swT1) / 1000000.0;
                            swQueueMs += (swT3 - swT2) / 1000000.0;
                            swFrames++;
                            if (swFrames % 5 == 0) {
                                Log.i(TAG, String.format(java.util.Locale.US,
                                        "sw timing avg: getImage=%.1fms convert=%.1fms queue=%.1fms",
                                        swImgMs / swFrames, swConvMs / swFrames, swQueueMs / swFrames));
                            }
                            rendered++;
                        }
                        dec.releaseOutputBuffer(didx, false);
                    } else if (keepField) {
                        /* The decoder echoes our input PTS, now in microseconds. */
                        pendingFramePtsUs = dbi.presentationTimeUs;
                        dec.releaseOutputBuffer(didx, true);
                    } else {
                        dec.releaseOutputBuffer(didx, false);
                    }
                    if (false) {
                        Image img = reader.acquireLatestImage();
                        if (img != null && keepField) {
                            try {
                                YuvPacker.imageToNv12(img, nv12, cfg.dstW, cfg.dstH);
                            } finally {
                                img.close();
                            }
                            long ptsUs = rendered * 1000000L / cfg.frameRate;
                            int bidx = enc.dequeueInputBuffer(0);
                            if (bidx >= 0) {
                                ByteBuffer bin = enc.getInputBuffer(bidx);
                                if (bin != null) {
                                    bin.clear();
                                    bin.put(nv12, 0, nv12.length);
                                    enc.queueInputBuffer(bidx, 0, nv12.length, ptsUs, 0);
                                }
                            }
                            rendered++;
                        }
                    } else if (counter.await(pendingFramePtsUs >= 0 ? 1000 : 0)) {
                        long tRen0 = System.nanoTime();
                        st.updateTexImage();
                        if (cfg.adaptiveFps && cfg.fpsTarget > 0 && cfg.fpsFloor > 0) {
                            long adaptNowMs = System.currentTimeMillis();
                            if (fpsAdaptedAt == 0) fpsAdaptedAt = adaptNowMs;
                            if (adaptNowMs - fpsAdaptedAt >= 1000) {
                                double periodMs = 1000.0 / Math.max(1, cfg.fpsTarget);
                                boolean ringPressure = in instanceof TsRingReader
                                        && ((TsRingReader) in).pendingChunks() * 2
                                        >= Math.max(1, ((TsRingReader) in).capacityChunks());
                                boolean renderPressure = workEmaMs > periodMs * 0.95;
                                if (ringPressure || renderPressure) {
                                    effectiveFps = Math.max(cfg.fpsFloor, effectiveFps - 1.0);
                                } else {
                                    effectiveFps = Math.min(cfg.fpsTarget, effectiveFps + 0.25);
                                }
                                fpsAdaptedAt = adaptNowMs;
                            }
                            if (effectiveFps < cfg.frameRate) {
                                fpsAccumulator += effectiveFps;
                                if (fpsAccumulator < cfg.frameRate) {
                                    pendingFramePtsUs = -1;
                                    rendered++;
                                    continue;
                                }
                                fpsAccumulator -= cfg.frameRate;
                            }
                        } else if (cfg.fpsLimit > 0 && cfg.fpsLimit < cfg.frameRate) {
                            fpsAccumulator += cfg.fpsLimit;
                            if (fpsAccumulator < cfg.frameRate) {
                                /* The encoder cannot hold the source rate:
                                 * consume the decoded frame without drawing
                                 * or encoding it. 30 -> 25 keeps five of six. */
                                pendingFramePtsUs = -1;
                                rendered++;
                                continue;
                            }
                            fpsAccumulator -= cfg.frameRate;
                        }
                        float[] mtx = new float[16];
                        st.getTransformMatrix(mtx);
                        long tTex = System.nanoTime();
                        texNs += tTex - tRen0;
                        egl.makeCurrent();
                        long tCur = System.nanoTime();
                        curNs += tCur - tTex;
                        if (sharp) {
                            /* Pass 1 renders into the offscreen texture. */
                            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo);
                            GLES20.glViewport(0, 0, srcW, srcH);
                        } else {
                            GLES20.glViewport(0, 0, cfg.dstW, cfg.dstH);
                        }
                        /*
                         * No glClear: the full screen quad covers the whole
                         * viewport, and the extra pass costs real time at
                         * 1080p.
                         */
                        GLES20.glUseProgram(prog);
                        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
                        GLES20.glBindTexture(GlBridge.glExtOes(), tex);
                        GLES20.glUniform1i(uTex, 0);
                        GLES20.glUniformMatrix4fv(uMtx, 1, false, mtx, 0);
                        GLES20.glEnableVertexAttribArray(aPos);
                        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, vb);
                        GLES20.glEnableVertexAttribArray(aTex);
                        GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, tb);
                        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                        GLES20.glDisableVertexAttribArray(aPos);
                        GLES20.glDisableVertexAttribArray(aTex);
                        if (sharp) {
                            /*
                             * The draw above went to the offscreen texture;
                             * this pass sharpens it onto the encoder surface.
                             */
                            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
                            GLES20.glViewport(0, 0, cfg.dstW, cfg.dstH);
                            GLES20.glUseProgram(progSharp);
                            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
                            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fboTex);
                            GLES20.glUniform1i(uTex2, 0);
                            GLES20.glUniform1f(uSharp2, cfg.sharpen);
                            GLES20.glUniform2f(uTexel2, 1.0f / srcW, 1.0f / srcH);
                            GLES20.glEnableVertexAttribArray(aPos2);
                            GLES20.glVertexAttribPointer(aPos2, 2, GLES20.GL_FLOAT, false, 0, vb);
                            GLES20.glEnableVertexAttribArray(aTex2);
                            GLES20.glVertexAttribPointer(aTex2, 2, GLES20.GL_FLOAT, false, 0, tb);
                            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                            GLES20.glDisableVertexAttribArray(aPos2);
                            GLES20.glDisableVertexAttribArray(aTex2);
                        }
                        long tDraw = System.nanoTime();
                        drawNs += tDraw - tCur;
                        long ptsUs;
                        if (frameClock) {
                            /* 1001/30000 s per frame, truncated to microseconds. */
                            ptsUs = rendered * 100100L / 3;
                        } else {
                            ptsUs = pendingFramePtsUs >= 0
                                    ? pendingFramePtsUs
                                    : rendered * 1000000L / cfg.frameRate;
                        }
                        pendingFramePtsUs = -1;
                        if (cfg.byteBuffer) {
                            GLES20.glReadPixels(0, 0, cfg.dstW, cfg.dstH, GLES20.GL_RGBA,
                                    GLES20.GL_UNSIGNED_BYTE, rgbaBuf);
                            rgbaBuf.position(0);
                            rgbaBuf.get(rgbaBytes, 0, rgbaBytes.length);
                            Nv12Converter.rgbaToNv12(rgbaBytes, cfg.dstW, cfg.dstH, nv12);
                            int bidx = enc.dequeueInputBuffer(0);
                            if (bidx >= 0) {
                                ByteBuffer bin = enc.getInputBuffer(bidx);
                                if (bin != null) {
                                    bin.clear();
                                    bin.put(nv12, 0, nv12.length);
                                    enc.queueInputBuffer(bidx, 0, nv12.length, ptsUs, 0);
                                }
                            }
                        } else {
                            EGLExt.eglPresentationTimeANDROID(egl.dpy, egl.surf, ptsUs * 1000);
                            long tSwap = System.nanoTime();
                            EGL14.eglSwapBuffers(egl.dpy, egl.surf);
                            swapNs += System.nanoTime() - tSwap;
                        }
                        rendered++;
                        renderNs += System.nanoTime() - tRen0;
                        /*
                         * How long this frame took from feed to swap. Waiting
                         * for the decoder or for the encoder's input queue
                         * both show up here, and both mean the same thing: the
                         * pipeline owes the tuner time it will not get back
                         * unless a frame is skipped.
                         */
                        long workMs = System.currentTimeMillis() - workStartMs;
                        workEmaMs = workEmaMs == 0 ? workMs : (workEmaMs * 7 + workMs) / 8;
                        long targetMs = 1000L / Math.max(1, cfg.frameRate);
                        if (workEmaMs > targetMs + 2) {
                            dropCredit += (workEmaMs - targetMs) / (double) workEmaMs;
                        } else if (dropCredit > 0) {
                            dropCredit = Math.max(0, dropCredit - 0.05);
                        }
                    }
                    if (decEos && !encEosSignalled) {
                        try { enc.signalEndOfInputStream(); } catch (Exception ignored) {}
                        encEosSignalled = true;
                    }
                }

                // ---- STEP D: Drain the encoder and forward to the sink ----
                long tEnc0 = System.nanoTime();
                /*
                 * Do not wait for the encoder here: it produced a frame every
                 * ~48 ms and blocking on it serialised the whole loop. Whatever
                 * is ready now is drained now; the rest is picked up on the
                 * following iterations.
                 */
                int eidx = enc.dequeueOutputBuffer(ebi, cfg.deqMs);
                encWaitNs += System.nanoTime() - tEnc0;
                if (eidx >= 0) {
                    if ((ebi.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                        ByteBuffer cfgBuf = enc.getOutputBuffer(eidx);
                        if (cfgBuf != null) {
                            cfgBuf.position(ebi.offset);
                            cfgBuf.limit(ebi.offset + ebi.size);
                            byte[] csd = new byte[ebi.size];
                            cfgBuf.get(csd);
                            if ("hevc".equalsIgnoreCase(cfg.codec)) {
                                /* csd-0 is the HEVC decoder configuration record. */
                                sps = csd;
                                pps = new byte[0];
                                Log.i(TAG, "csd parsed as HVCC, size=" + csd.length);
                            } else {
                                int[] split = splitAvcc(csd);
                                if (split != null) {
                                    sps = Arrays.copyOfRange(csd, 0, split[0]);
                                    pps = Arrays.copyOfRange(csd, split[0],
                                            split[0] + split[1]);
                                    Log.i(TAG, "csd parsed as avcC sps=" + split[0]
                                            + " pps=" + split[1]);
                                } else {
                                    byte[][] annexb = splitAnnexBCsds(csd);
                                    if (annexb != null && annexb[0].length >= 8
                                            && annexb[1].length >= 4) {
                                        sps = stripStartCode(annexb[0]);
                                        pps = stripStartCode(annexb[1]);
                                        Log.i(TAG, "csd parsed as Annex-B sps=" + sps.length
                                                + " pps=" + pps.length);
                                    } else {
                                        /* The buffer can hold a partial PPS; the
                                           output format carries the full pair. */
                                        Log.w(TAG, "csd buffer incomplete, size=" + csd.length);
                                    }
                                }
                            }
                        }
                        enc.releaseOutputBuffer(eidx, false);
                    } else {
                        ByteBuffer outBuf = enc.getOutputBuffer(eidx);
                        if (outBuf != null && ebi.size > 0) {
                            outBuf.position(ebi.offset);
                            outBuf.limit(ebi.offset + ebi.size);
                            byte[] data = new byte[ebi.size];
                            outBuf.get(data);

                            if (!initSent && sps != null && pps != null) {
                                boolean hevc = "hevc".equalsIgnoreCase(cfg.codec);
                                sink.sendInit(cfg.dstW, cfg.dstH, sps, pps,
                                        audio.sampleRate, audio.channels, audio.asc,
                                        hevc);
                                initSent = true;
                                stats.sentInit = true;
                            }

                            boolean key = (ebi.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                            long ptsUs = (long) ebi.presentationTimeUs;
                            long durationUs = 1000000L / cfg.frameRate;
                            if (firstVideoEncPtsUs < 0) {
                                firstVideoEncPtsUs = ptsUs;
                                stats.videoFirstPtsUs = ptsUs;
                            }
                            /* The encoder's own clock is kept as is. Subtracting
                             * a per-session origin looked tidy but split the two
                             * tracks on boxes whose encoder clock and broadcast
                             * clock start from different bases: the picture then
                             * began hours after the sound. The muxer already
                             * rebases both tracks on the common minimum. */
                            lastVideoEncPtsUs = ptsUs;
                            long tSink0 = System.nanoTime();
                            sink.sendVideo(data, ptsUs, durationUs, key);
                            sinkNs += System.nanoTime() - tSink0;

                            stats.videoFrames++;
                            stats.videoBytes += ebi.size;
                            /*
                             * The first frame fixes where the output starts, so
                             * the audio read before it can be released now.
                             */
                            if (initSent && !audioStartup.flushed) {
                                long tAud0 = System.nanoTime();
                                audioStartup.flush(0,
                                        frameClock && videoSrcBaseUs >= 0 ? -videoSrcBaseUs : 0,
                                        sink, stats);
                                sinkNs += System.nanoTime() - tAud0;
                            }
                        }
                        enc.releaseOutputBuffer(eidx, false);
                        if ((ebi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            encEos = true;
                        }
                    }
                } else if (eidx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat nf = enc.getOutputFormat();
                    Log.i(TAG, "enc format=" + nf);
                    if (sps == null || pps == null) {
                        byte[][] fromFormat = csdsFromFormat(nf);
                        if (fromFormat != null) {
                            sps = stripStartCode(fromFormat[0]);
                            pps = stripStartCode(fromFormat[1]);
                            Log.i(TAG, "csd from format sps=" + sps.length
                                    + " pps=" + pps.length);
                        }
                    }
                }
            }

            if (aPesLen > 0) {
                try {
                    sendAdtsFrames(aPesBuf, aPesLen, aPesPtsUs,
                            frameClock ? videoSrcBaseUs >= 0 : firstVideoEncPtsUs >= 0,
                            0,
                            frameClock && videoSrcBaseUs >= 0 ? -videoSrcBaseUs : 0,
                            adtsCarry, audioStartup, sink, stats);
                } catch (IOException ignored) {
                    // The sink may already be gone; nothing to recover here.
                }
                aPesLen = 0;
            }

            long dtMs = System.currentTimeMillis() - t0;
            stats.durationMs = dtMs;
            stats.fps = stats.videoFrames * 1000.0 / Math.max(1, dtMs);
            stats.message = String.format(Locale.US,
                    "LIVE-TS video=%d audio=%d vBytes=%d aBytes=%d time=%dms fps=%.1f init=%b",
                    stats.videoFrames, stats.audioFrames, stats.videoBytes, stats.audioBytes,
                    dtMs, stats.fps, stats.sentInit);
            return stats;

        } catch (Exception e) {
            Throwable cause = e;
            while (cause.getCause() != null && cause.getCause() != cause) {
                cause = cause.getCause();
            }
            stats.message = "NG:" + cause.getClass().getSimpleName()
                    + ":" + cause.getMessage();
            Log.e(TAG, "failure", e);
            return stats;
        } finally {
            if (sink != null) {
                try { sink.sendFlush(); } catch (IOException ignored) {}
                sink.close();
            }
            /*
             * release() matters as much as stop(): this SoC allows a single
             * MPEG2 decoder instance, and a component that was only stopped
             * keeps the hardware until the garbage collector finalises it.
             * Without the release a session restart (channel change, format
             * change) leaves the next decoder with no hardware, which shows up
             * as "fed > 0, dec = 0" until the box is rebooted.
             */
            try { if (dec != null) dec.stop(); } catch (Exception ignored) {}
            try { if (dec != null) dec.release(); } catch (Exception ignored) {}
            try { if (enc != null) enc.stop(); } catch (Exception ignored) {}
            try { if (enc != null) enc.release(); } catch (Exception ignored) {}
            if (sharpFbo != 0) {
                try {
                    int[] f = {sharpFbo};
                    GLES20.glDeleteFramebuffers(1, f, 0);
                } catch (Exception ignored) {}
                sharpFbo = 0;
            }
            if (sharpTex != 0) {
                try {
                    int[] t = {sharpTex};
                    GLES20.glDeleteTextures(1, t, 0);
                } catch (Exception ignored) {}
                sharpTex = 0;
            }
            try { if (egl != null) egl.release(); } catch (Exception ignored) {}
            /*
             * The decoder renders into a Surface built on a SurfaceTexture, and
             * the encoder owns another Surface. Neither is a codec, so neither
             * is freed by stop()/release() above; without releasing them every
             * session leaks a graphic buffer producer. After a few dozen
             * session restarts the decoder still accepts input but never gets a
             * buffer to write into, which looked like "fed > 0, dec = 0" and
             * needed a reboot to clear.
             */
            try { if (decSurf != null) decSurf.release(); } catch (Exception ignored) {}
            try { if (st != null) st.release(); } catch (Exception ignored) {}
            /*
             * The surface callback thread is created per session and used to
             * receive onFrameAvailable. Leaving it running leaks one thread per
             * session (measured 28 "live-tex-cb" threads on a box that had been
             * restarting for an hour, against 3 on a steady one); once the
             * process runs out of resources the decoder starts, accepts input
             * and never produces a frame.
             */
            try { if (cbThread != null) cbThread.quitSafely(); } catch (Exception ignored) {}
            try { if (encSurf != null) encSurf.release(); } catch (Exception ignored) {}
            try { if (in != null) in.close(); } catch (Exception ignored) {}
        }
    }

    /**
     * ADTS bytes that continue into the next PES.
     *
     * <p>On the X96 feeds one audio access unit is often split across two PES
     * packets. Scanning each PES on its own then finds almost nothing (measured
     * 13.6 frames/s instead of 46.9), so the tail is carried over and the frame
     * is completed with the next PES. The PTS rule follows the spec: a PES
     * timestamp refers to the first access unit that starts inside that PES.
     */
    private static final class Adts {
        byte[] pending = new byte[0];
        long pendingPtsUs = -1;
        /* Last timestamp handed to the sink: the container rejects a track that
         * steps backwards, and the audio clock is re-based mid-session. */
        long lastSentPtsUs = Long.MIN_VALUE;
        /* Correction applied to every later frame so the sound continues from
         * where it stopped instead of restarting at zero. */
        long biasAdjustUs;
    }

    /** One video PES waiting for a free decoder input buffer. */
    private static final class Pes {
        final byte[] data;
        final long pts;
        final boolean isB;

        Pes(byte[] data, long pts, boolean isB) {
            this.data = data;
            this.pts = pts;
            this.isB = isB;
        }
    }

    /** AAC configuration used to build the fMP4 audio track. */
    static final class AudioState {
        int sampleRate = 48000;
        int channels = 2;
        byte[] asc = new byte[]{0x11, (byte) 0x90};
    }

    /** AAC frame duration at 48 kHz (1024 samples). */
    private static final int AUDIO_FRAME_US = (int) (1024L * 1000000L / 48000);

    /**
     * Audio that arrived before the first encoded video frame.
     *
     * Render and encode run behind the demuxer, so a few hundred milliseconds
     * of audio are read before the output timeline has a start. Those frames
     * used to be dropped, which left the stream starting with video only; they
     * are kept here and released once the first frame fixes the timeline.
     * Frames whose timestamp lands ahead of that point are dropped, because
     * there is no picture they could be paired with.
     */
    private static final class AudioStartup {
        /* ~5.5 s at 48 kHz, far more than the pipeline lead ever needs. */
        private static final int MAX_FRAMES = 256;
        private final java.util.ArrayList<byte[]> frames = new java.util.ArrayList<>();
        private final java.util.ArrayList<Long> pts = new java.util.ArrayList<>();
        boolean flushed;

        void add(byte[] frame, long ptsUs) {
            if (frames.size() >= MAX_FRAMES) {
                frames.remove(0);
                pts.remove(0);
            }
            frames.add(frame);
            pts.add(ptsUs);
        }

        void flush(long thresholdPts, long biasUs, Fmp4SinkClient sink, Stats stats)
                throws IOException {
            flushed = true;
            for (int i = 0; i < frames.size(); i++) {
                long p = pts.get(i) + biasUs;
                if (p < thresholdPts) {
                    stats.audioStartDropped++;
                    continue;
                }
                sink.sendAudio(frames.get(i), p, AUDIO_FRAME_US);
                if (stats.audioFirstPtsUs < 0) stats.audioFirstPtsUs = p;
                stats.audioFrames++;
                stats.audioBytes += frames.get(i).length;
            }
            frames.clear();
            pts.clear();
        }
    }

    /**
     * Extracts AAC frames from an ADTS byte run and forwards them to the sink.
     * The ADTS header is stripped; the writer expects raw AAC access units.
     */
    private static void sendAdtsFrames(byte[] buf, int len, long ptsUs, boolean ready,
                                       long thresholdPts, long biasUs, Adts carry,
                                       AudioStartup startup, Fmp4SinkClient sink, Stats stats)
            throws IOException {
        if (stats.audioFrames < 5) {
            try {
                java.io.FileOutputStream f =
                        new java.io.FileOutputStream("/dev/shm/pix/aac-len.txt", true);
                f.write((len + "\n").getBytes());
                f.close();
            } catch (Exception ignored) {
            }
        }
        int prefix = carry.pending.length;
        byte[] bytes;
        if (prefix > 0) {
            bytes = new byte[prefix + len];
            System.arraycopy(carry.pending, 0, bytes, 0, prefix);
            System.arraycopy(buf, 0, bytes, prefix, len);
        } else {
            bytes = java.util.Arrays.copyOf(buf, len);
        }
        long nextPtsUs = ptsUs;
        long pendingPtsUs = prefix > 0 ? carry.pendingPtsUs : -1;
        int off = 0;
        while (off + 7 <= bytes.length) {
            if ((bytes[off] & 0xFF) != 0xFF || (bytes[off + 1] & 0xF6) != 0xF0) {
                off++;
                continue;
            }
            /* AAC-LC 48 kHz stereo is what the sink declares; anything else is
             * a sync-like sequence, not a frame this track can carry. */
            int profile = ((bytes[off + 2] & 0xFF) >> 6) + 1;
            int frequency = (bytes[off + 2] >> 2) & 15;
            int channels = ((bytes[off + 2] & 1) << 2) | ((bytes[off + 3] >> 6) & 3);
            if (profile != 2 || frequency != 3 || channels != 2) {
                off++;
                continue;
            }
            int b1 = bytes[off + 1] & 0xFF;
            int b3 = bytes[off + 3] & 0xFF;
            int b4 = bytes[off + 4] & 0xFF;
            int b5 = bytes[off + 5] & 0xFF;
            int hdr = ((b1 & 1) != 0) ? 7 : 9;
            int frameLen = ((b3 & 0x03) << 11) | (b4 << 3) | (b5 >> 5);
            if (frameLen < hdr) {
                off++;
                continue;
            }
            if (off + frameLen > bytes.length) break; /* wait for the next PES */
            int payload = frameLen - hdr;
            if (stats.audioFrames < 5) {
                try {
                    java.io.FileOutputStream f =
                            new java.io.FileOutputStream("/dev/shm/pix/aac-frame.txt", true);
                    f.write((off + " " + frameLen + " " + hdr + " "
                            + String.format("%02x%02x%02x%02x",
                                bytes[off] & 0xFF, bytes[off + 1] & 0xFF,
                                bytes[off + 2] & 0xFF, bytes[off + 3] & 0xFF)
                            + "\n").getBytes());
                    f.close();
                } catch (Exception ignored) {
                }
            }
            long framePts = off < prefix ? pendingPtsUs : nextPtsUs;
            if (payload > 0 && framePts >= 0) {
                byte[] frame = new byte[payload];
                System.arraycopy(bytes, off + hdr, frame, 0, payload);
                long outPts = framePts + biasUs + carry.biasAdjustUs;
                if (!ready) {
                    /* The first encoded frame has not fixed the timeline yet. */
                    startup.add(frame, framePts);
                } else {
                    if (carry.lastSentPtsUs == Long.MIN_VALUE && outPts < thresholdPts) {
                        /* The first sound of the session sits behind the picture,
                         * which happens when the broadcast clock and the encoder
                         * clock start from different bases. Dropping it silenced
                         * the head of the recording (42 seconds on the NHK box),
                         * so move the audio origin instead: nothing is lost and
                         * the two tracks still begin together. */
                        carry.biasAdjustUs += thresholdPts - outPts;
                        outPts = thresholdPts;
                        Log.w(TAG, "audio head aligned by " + carry.biasAdjustUs + " us");
                    }
                    if (carry.lastSentPtsUs != Long.MIN_VALUE && outPts <= carry.lastSentPtsUs
                            && stats.audioFrames > 0) {
                        /* The broadcast clock restarted, so this frame sits behind
                         * the one already sent. Shifting the rest of the stream
                         * keeps the sound continuous and never moves the audio
                         * timeline away from the video, which re-basing the shared
                         * origin did: that left audio at zero and video twelve
                         * hours later inside one container. */
                        carry.biasAdjustUs += carry.lastSentPtsUs + AUDIO_FRAME_US - outPts;
                        outPts = carry.lastSentPtsUs + AUDIO_FRAME_US;
                        stats.audioRebases++;
                        Log.w(TAG, "audio clock re-based by " + carry.biasAdjustUs + " us");
                    }
                    carry.lastSentPtsUs = outPts;
                    sink.sendAudio(frame, outPts, AUDIO_FRAME_US);
                    if (stats.audioFirstPtsUs < 0) stats.audioFirstPtsUs = outPts;
                    stats.audioFrames++;
                    stats.audioBytes += payload;
                }
                if (off >= prefix) nextPtsUs += AUDIO_FRAME_US;
            }
            off += frameLen;
        }
        carry.pendingPtsUs = off < prefix ? pendingPtsUs : nextPtsUs;
        carry.pending = java.util.Arrays.copyOfRange(bytes, off, bytes.length);
    }

    /** Converts the EIT 5-byte start time (MJD plus BCD) to epoch milliseconds. */
    static long eitTimeMs(byte[] pkt, int p) {
        int mjd = ((pkt[p] & 0xFF) << 8) | (pkt[p + 1] & 0xFF);
        int hh = bcd(pkt[p + 2]);
        int mm = bcd(pkt[p + 3]);
        int ss = bcd(pkt[p + 4]);
        return mjdToEpochMs(mjd, hh, mm, ss);
    }

    /** Converts the EIT 3-byte duration (BCD) to milliseconds. */
    static long eitDurationToMs(byte[] pkt, int p) {
        int hh = bcd(pkt[p]);
        int mm = bcd(pkt[p + 1]);
        int ss = bcd(pkt[p + 2]);
        return ((long) hh * 3600L + (long) mm * 60L + (long) ss) * 1000L;
    }

    private static int bcd(byte b) {
        return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
    }

    /** MJD plus Japan standard time of day to epoch milliseconds. */
    private static long mjdToEpochMs(int mjd, int hh, int mm, int ss) {
        int yp = (int) ((mjd - 15078.2) / 365.25);
        int mp = (int) ((mjd - 14956.1 - (int) (yp * 365.25)) / 30.6001);
        int d = mjd - 14956 - (int) (yp * 365.25) - (int) (mp * 30.6001);
        int k = (mp == 14 || mp == 15) ? 1 : 0;
        int year = yp + k + 1900;
        int month = mp - 1 - k * 12;
        java.util.Calendar cal = java.util.Calendar.getInstance(
                java.util.TimeZone.getTimeZone("Asia/Tokyo"));
        cal.clear();
        cal.set(year, month - 1, d, hh, mm, ss);
        return cal.getTimeInMillis();
    }

    static long parsePts(byte[] pkt, int p) {
        if (p + 14 <= 188 && pkt[p] == 0 && pkt[p+1] == 0 && pkt[p+2] == 1) {
            int flags = pkt[p+7] & 0xFF;
            if (((flags >> 6) & 0x3) >= 2) {
                long b0 = pkt[p+9] & 0xFF;
                long b1 = pkt[p+10] & 0xFF;
                long b2 = pkt[p+11] & 0xFF;
                long b3 = pkt[p+12] & 0xFF;
                long b4 = pkt[p+13] & 0xFF;
                long pts = (((b0 & 0x0E) << 29) |
                            (b1 << 22) |
                            ((b2 & 0xFE) << 14) |
                            (b3 << 7) |
                            ((b4 & 0xFE) >> 1));
                return pts * 1000L / 90L; // 90 kHz -> microseconds
            }
        }
        return -1;
    }

    static int skipPesHeader(byte[] pkt, int p) {
        if (p + 9 <= 188 && pkt[p] == 0 && pkt[p+1] == 0 && pkt[p+2] == 1) {
            int hdrLen = pkt[p+8] & 0xFF;
            int next = p + 9 + hdrLen;
            return Math.min(next, 188);
        }
        return p;
    }

    /**
     * True when this PES carries an MPEG2 B picture.
     *
     * <p>B pictures are the only frames nothing else references, so they are
     * the only ones that can be dropped without corrupting the pictures that
     * follow. The picture header (00 00 01 00) is the first start code in the
     * payload for every frame we have seen, but an I picture starts with a
     * sequence header first, so the scan covers the first 64 bytes.
     */
    static boolean isBPicture(byte[] pes) {
        int limit = Math.min(pes.length - 6, 64);
        for (int i = 0; i <= limit; i++) {
            if (pes[i] == 0 && pes[i+1] == 0 && pes[i+2] == 1 && pes[i+3] == 0) {
                /* temporal_reference(10) then picture_coding_type(3). */
                return ((pes[i+5] >> 3) & 0x07) == 3;
            }
        }
        return false;
    }

    /**
     * Reads SPS and PPS from a MediaFormat that carries csd-0 and csd-1.
     * Android's AVC encoder reports csd-0=SPS and csd-1=PPS in Annex-B form.
     */
    private static byte[][] csdsFromFormat(MediaFormat nf) {
        try {
            ByteBuffer s0 = nf.getByteBuffer("csd-0");
            ByteBuffer s1 = nf.getByteBuffer("csd-1");
            if (s0 == null || s1 == null) return null;
            byte[] sps = new byte[s0.remaining()];
            s0.get(sps);
            byte[] pps = new byte[s1.remaining()];
            s1.get(pps);
            if (sps.length == 0 || pps.length == 0) return null;
            return new byte[][]{sps, pps};
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Splits an Annex-B buffer into its SPS and PPS NAL units.
     * Used when the encoder reports the codec config as csd-0 only.
     */
    private static byte[][] splitAnnexBCsds(byte[] csd) {
        int spsStart = -1, spsEnd = -1, ppsStart = -1, ppsEnd = -1;
        int at = 0;
        while (at + 3 < csd.length) {
            int hdr = 0;
            if (csd[at] == 0 && csd[at+1] == 0 && csd[at+2] == 1) {
                hdr = 3;
            } else if (at + 4 < csd.length
                    && csd[at] == 0 && csd[at+1] == 0 && csd[at+2] == 0 && csd[at+3] == 1) {
                hdr = 4;
            }
            if (hdr == 0) { at++; continue; }
            int start = at + hdr;
            int next = start;
            while (next + 3 < csd.length) {
                boolean found = (csd[next] == 0 && csd[next+1] == 0 && csd[next+2] == 1)
                        || (next + 4 < csd.length && csd[next] == 0 && csd[next+1] == 0
                            && csd[next+2] == 0 && csd[next+3] == 1);
                if (found) break;
                next++;
            }
            int end = Math.min(next, csd.length);
            if (start < end) {
                int type = csd[start] & 0x1F;
                if (type == 7 && spsStart < 0) { spsStart = start; spsEnd = end; }
                else if (type == 8 && ppsStart < 0) { ppsStart = start; ppsEnd = end; }
            }
            at = end;
        }
        if (spsStart < 0 || ppsStart < 0) return null;
        return new byte[][]{
                Arrays.copyOfRange(csd, spsStart, spsEnd),
                Arrays.copyOfRange(csd, ppsStart, ppsEnd)};
    }

    /** Splits an AVCC decoder configuration into SPS length and PPS length. */
    /** Removes a leading Annex-B start code; the writer stores raw NAL units. */
    private static byte[] stripStartCode(byte[] nal) {
        if (nal.length > 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
            return Arrays.copyOfRange(nal, 4, nal.length);
        }
        if (nal.length > 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
            return Arrays.copyOfRange(nal, 3, nal.length);
        }
        return nal;
    }

    private static int[] splitAvcc(byte[] csd) {
        if (csd.length < 8) return null;
        int numSps = csd[5] & 0x1F;
        if (numSps < 1) return null;
        int p = 6;
        int spsLen = ((csd[p] & 0xFF) << 8) | (csd[p + 1] & 0xFF);
        p += 2 + spsLen;
        if (p + 1 >= csd.length) return null;
        int numPps = csd[p] & 0xFF;
        if (numPps < 1) return null;
        p += 1;
        int ppsLen = ((csd[p] & 0xFF) << 8) | (csd[p + 1] & 0xFF);
        return new int[]{spsLen, ppsLen};
    }

    /** ImageReader.newInstance(w, h, fmt, max, usage) via reflection. */
    private static ImageReader newReaderWithUsage(int w, int h, int fmt, int max, long usage) {
        try {
            java.lang.reflect.Method m = ImageReader.class.getMethod(
                    "newInstance", int.class, int.class, int.class, int.class, long.class);
            Object r = m.invoke(null, w, h, fmt, max, usage);
            Log.i(TAG, "image reader created with usage=0x" + Long.toHexString(usage));
            return (ImageReader) r;
        } catch (Throwable t) {
            Log.i(TAG, "hidden ImageReader usage overload unavailable: " + t);
            return null;
        }
    }

    private static MediaCodec newCodec(String name) throws Exception {
        Exception last = null;
        for (int attempt = 0; attempt < 10; attempt++) {
            try {
                java.lang.reflect.Method m =
                        MediaCodec.class.getMethod("createByCodecName", String.class);
                return (MediaCodec) m.invoke(null, name);
            } catch (Exception ex) {
                last = ex;
                try {
                    Thread.sleep(1500);
                } catch (InterruptedException ignored) {}
            }
        }
        throw last;
    }
}
