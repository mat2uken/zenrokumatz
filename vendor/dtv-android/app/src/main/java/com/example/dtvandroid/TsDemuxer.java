package com.example.dtvandroid;

import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Locale;

/**
 * Minimal MPEG2-TS demuxer for Japanese DTV (single program extraction).
 * Parses PAT/PMT to find MPEG2 video (0x02) and AAC audio (0x0F/0x11) PIDs,
 * strips PES headers, writes raw video ES (.m2v) and audio ADTS (.aac).
 * Handles 188-byte packets, adaptation fields, continuity (ignores loss).
 */
public final class TsDemuxer {
    private static final String TAG = "TsDemux";

    private TsDemuxer() {}

    public static class Result {
        public int videoPid = -1;
        public int audioPid = -1;
        public int pmtPid = -1;
        public long videoBytes = 0;
        public long audioBytes = 0;
        public int packets = 0;
    }

    public static Result demux(String inTs, String outVideo, String outAudio) throws Exception {
        File f = new File(inTs);
        long len = f.length();
        if (len < 188) throw new java.io.IOException("too small TS");
        FileInputStream in = new FileInputStream(f);
        FileOutputStream vout = new FileOutputStream(outVideo);
        FileOutputStream aout = outAudio != null ? new FileOutputStream(outAudio) : null;
        try {
            byte[] pkt = new byte[188];
            int pmtPid = -1;
            int videoPid = -1, audioPid = -1;
            // PES reassembly state per PID (store pending PES header skip).
            // Simplified: strip PES header on PUSI packets, append rest directly.
            // This works because decoder tolerates chunked ES feeding.
            Result r = new Result();
            int packets = 0;
            long vBytes = 0, aBytes = 0;
            // First pass: find PAT/PMT quickly from first 2000 packets if needed.
            // Single-pass: parse PAT/PMT on the fly.
            while (true) {
                int off = 0;
                while (off < 188) {
                    int n = in.read(pkt, off, 188 - off);
                    if (n < 0) break;
                    off += n;
                }
                if (off < 188) break;
                packets++;
                if (pkt[0] != 0x47) {
                    // Resync: scan for next 0x47 (should not happen with clean file).
                    Log.w(TAG, "sync loss at packet " + packets);
                    continue;
                }
                // Drop transport-error packets (tuning start / signal glitch).
                if ((pkt[1] & 0x80) != 0) continue;
                int pid = ((pkt[1] & 0x1F) << 8) | (pkt[2] & 0xFF);
                int afc = (pkt[3] >> 4) & 0x3;
                boolean pusi = (pkt[1] & 0x40) != 0;
                int payloadOff = 4;
                if (afc == 2 || afc == 0) continue; // adaptation only, no payload
                if (afc == 3) {
                    int afl = pkt[4] & 0xFF;
                    payloadOff = 5 + afl;
                    if (payloadOff >= 188) continue;
                }
                int payloadLen = 188 - payloadOff;
                if (payloadLen <= 0) continue;

                if (pid == 0x00) {
                    // PAT: parse program -> PMT PID (skip TS header already).
                    // PAT payload may start with pointer_field when PUSI.
                    int p = payloadOff;
                    if (pusi) {
                        int pointer = pkt[p] & 0xFF;
                        p += 1 + pointer;
                    }
                    // section: table_id(1) + flags(2) + section_len(2) ... program loop
                    if (p + 12 <= 188) {
                        int sectionLen = ((pkt[p + 1] & 0x0F) << 8) | (pkt[p + 2] & 0xFF);
                        int progStart = p + 8;
                        int progEnd = p + 3 + sectionLen - 4; // minus CRC
                        for (int q = progStart; q + 4 <= progEnd && q + 4 <= 188; q += 4) {
                            int progNum = ((pkt[q] & 0xFF) << 8) | (pkt[q + 1] & 0xFF);
                            int pmt = ((pkt[q + 2] & 0x1F) << 8) | (pkt[q + 3] & 0xFF);
                            if (progNum != 0 && pmtPid < 0) {
                                pmtPid = pmt;
                                r.pmtPid = pmtPid;
                            }
                        }
                    }
                } else if (pmtPid >= 0 && pid == pmtPid) {
                    int p = payloadOff;
                    if (pusi) {
                        int pointer = pkt[p] & 0xFF;
                        p += 1 + pointer;
                    }
                    if (p + 12 <= 188) {
                        int sectionLen = ((pkt[p + 1] & 0x0F) << 8) | (pkt[p + 2] & 0xFF);
                        int progInfoLen = ((pkt[p + 10] & 0x0F) << 8) | (pkt[p + 11] & 0xFF);
                        int esStart = p + 12 + progInfoLen;
                        int esEnd = p + 3 + sectionLen - 4;
                        for (int q = esStart; q + 5 <= esEnd && q + 5 <= 188; ) {
                            int stype = pkt[q] & 0xFF;
                            int epub = ((pkt[q + 1] & 0x1F) << 8) | (pkt[q + 2] & 0xFF);
                            int esLen = ((pkt[q + 3] & 0x0F) << 8) | (pkt[q + 4] & 0xFF);
                            if (stype == 0x02 && videoPid < 0) videoPid = epub; // MPEG2 video
                            else if ((stype == 0x0F || stype == 0x11) && audioPid < 0) audioPid = epub; // AAC
                            else if (stype == 0x06 && false) { /* subtitle/data, ignore */ }
                            q += 5 + esLen;
                        }
                        r.videoPid = videoPid;
                        r.audioPid = audioPid;
                    }
                } else if (pid == videoPid && videoPid >= 0) {
                    int p = payloadOff;
                    int start = p;
                    int end = 188;
                    if (pusi) {
                        // PES header: 00 00 01 xx + 2B len + 1B flags + 1B flags + 1B hdrLen
                        if (p + 9 <= 188 && pkt[p] == 0 && pkt[p + 1] == 0 && pkt[p + 2] == 1) {
                            int hdrLen = pkt[p + 8] & 0xFF;
                            start = p + 9 + hdrLen;
                            if (start > 188) start = 188;
                        }
                    }
                    if (start < end) {
                        vout.write(pkt, start, end - start);
                        vBytes += (end - start);
                    }
                } else if (aout != null && pid == audioPid && audioPid >= 0) {
                    int p = payloadOff;
                    int start = p;
                    int end = 188;
                    if (pusi) {
                        if (p + 9 <= 188 && pkt[p] == 0 && pkt[p + 1] == 0 && pkt[p + 2] == 1) {
                            int hdrLen = pkt[p + 8] & 0xFF;
                            start = p + 9 + hdrLen;
                            if (start > 188) start = 188;
                        }
                    }
                    if (start < end) {
                        aout.write(pkt, start, end - start);
                        aBytes += (end - start);
                    }
                }
            }
            r.packets = packets;
            r.videoPid = videoPid;
            r.audioPid = audioPid;
            r.videoBytes = vBytes;
            r.audioBytes = aBytes;
            Log.i(TAG, String.format(Locale.US,
                    "demux packets=%d pmt=0x%x vpid=0x%x apid=0x%x v=%dB a=%dB",
                    packets, pmtPid, videoPid, audioPid, vBytes, aBytes));
            if (videoPid < 0) throw new java.io.IOException("no MPEG2 video PID found");
            return r;
        } finally {
            try { in.close(); } catch (Exception ignored) {}
            try { vout.close(); } catch (Exception ignored) {}
            if (aout != null) { try { aout.close(); } catch (Exception ignored) {} }
        }
    }
}
